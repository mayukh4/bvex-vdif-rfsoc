# bvex-vdif-rfsoc

RFSoC 4x2 VDIF packetizer and no-drop recorder for the mmBVEX millimeter-wave VLBI experiment.

The RFSoC digitizes a 2-4 GHz IF, requantizes it to 2-bit complex (Van Vleck optimal), frames it as
VDIF, and streams it over 100GbE as a single UDP flow. A C recorder using AF_XDP kernel-bypass writes
the full stream to NVMe SSDs with zero dropped frames, in a flat `.vdif` layout that DiFX reads
directly.

Signal chain:

```
RFSoC ADC (4096 MSPS real -> I/Q, decim 2x, NCO -1.024 GHz)
  -> 2-bit I/Q requantizer (adaptive Van Vleck thresholds)
  -> VDIF framer (8032-byte frames, single thread, PPS-locked)
  -> 100GbE UDP (one flow, ~8.3 Gbps)
  -> AF_XDP recorder (no-drop) -> flat .vdif on NVMe
  -> DiFX correlation
```

Numbers, hardware-validated: 128000 frames/s, 8032-byte VDIF frames, 8074 bytes on the wire,
~8.3 Gbps, single thread id 0, PPS-locked, `gap_frames=0`.

![Top-level model](docs/images/vdif_packetizer_toplevel.png)

## Repository layout

```
firmware/     Simulink/CASPER model, VHDL blocks, build harness, testbenches, compiled bitstream
  rfsoc_vdif_v0_4_dev.slx           current single-channel model
  vhdl/                             the 3 instantiated blocks + their *_config.m
  build/                            headless build harness (mc_setup, sgbatch, sgbatchx)
  sim/                              xsim testbenches + stubs
  bitstream/                        rfsoc_vdif_v0_4_dev_2026-06-24_1521.fpg + .dtbo (as-built)
clocks/       LMK04828 / LMX2594 register files (10 MHz external reference chain) + TICS Pro sources
host/         everything that runs on the recording host: board bring-up + AF_XDP recorder
  config.example.py                site config template (copy to config.py)
  init_fpga.py                     program + clock + arm the board, live health monitor
  record_supervisor.py             bring-up + launch the C recorder + merged telemetry
  record.sh                        the single command to record
  vdif_recorder.c                  the C capture engine (AF_XDP / raw / udp backends)
  xsk_redirect.bpf.c               XDP frags-aware redirect program
  Makefile  build_libxdp.sh  tune_nic.sh  vdif_gen.c  veth_selftest.sh
analysis/     decode + validation notebook and DiFX-prep scripts (run on a separate workstation)
docs/         model_overview.pdf and block-diagram screenshots
```

## Prerequisites

Hardware:
- Xilinx RFSoC 4x2 board (`xczu48dr`), reachable over a management network (KATCP).
- 10 MHz reference into the LMK CLKin0 front-panel SMA. Without it the ADCs never lock.
- 1 PPS into the FPGA PPS front-panel pin. Without it VDIF seconds do not align.
- 100GbE from the FPGA QSFP to the recording host NIC (tested: Mellanox ConnectX-5).
- Recording host with NVMe SSDs mounted (default `/mnt/vlbi0`, `/mnt/vlbi1`, `/mnt/vlbi2`).

Software:
- Board bring-up: `casperfpga` (Python 3 venv).
- Recorder: Linux, gcc, clang, libelf, zlib. libxdp + a modern libbpf are built from source by
  `host/build_libxdp.sh`.
- Analysis: Python 3 with the packages in `analysis/requirements.txt` (`baseband`, `astropy`, ...).
- Firmware rebuild (optional): MATLAB R2021a + Xilinx Vivado/Model Composer 2021.1 + `mlib_devel`.

## Quick start (record end to end)

On the recording host:

```
git clone <this-repo> bvex-vdif-rfsoc
cd bvex-vdif-rfsoc/host
cp config.example.py config.py         # then edit config.py (see below)
./build_libxdp.sh                      # one-time: build libxdp + libbpf under xdpbuild/
make                                   # builds vdif_recorder (with AF_XDP) + vdif_gen
sudo ./record.sh --secs 60             # record 60 s, or omit --secs to run until Ctrl-C
```

Edit `config.py` before the first run:
- `BOARD_IP`: the RFSoC KATCP address (replace the `xx` octet).
- `NIC`: your 100GbE interface (`ethtool`, `ip link`).
- `HOST_IP`, `DEST_IP`, `FPGA_IP`: data-plane addresses (replace the `xx` octets). `DEST_IP` = `HOST_IP`.
- `DEST_MAC`: the recording NIC MAC.
- `CFPGA_PY`: path to the python that has casperfpga (e.g. a cfpga venv).
- `DISKS`, `CAP_CORE`, `WR_CORE`, `IRQ_CORE`: NVMe mounts and CPU-core pinning.

A healthy monitor line:

```
REC fps=128000 drop(k/s)=0/0 gaps=0 time=OK | BOARD 256.0MHz PPS+1 pow=1660/89 nicT=73C | disk0 free=1.80T bvex_2026...th0.vdif [OK]
```

`fps=128000`, `gaps=0`, `time=OK`, tag `[OK]` means the full stream is being recorded losslessly.
On Ctrl-C the recorder finalizes the current file, disables the QSFP, and drops the NIC.

Output files: `/mnt/vlbiX/bvex_<UTC>_th0.vdif` (flat 8032-byte VDIF frames, no network wrapper) plus a
`.vdif.json` sidecar per file (start time, frame counts, gaps, disk). Files roll every
`--secs-per-file` seconds (default 10, about 10.3 GB) and fill the disks in order.

## Board bring-up only (no recording)

To program, clock, arm, and watch the stream without writing to disk:

```
cd host
sudo "$(python3 -c 'import config; print(config.CFPGA_PY)')" init_fpga.py
```

It programs the `.fpg`, uploads and programs the LMK (twice) and LMX PLLs, checks ADC lock, configures
the 100GbE core and ARP, enables the QSFP, seeds VDIF time, arms on the next PPS edge, then prints a
health line each second. Run this OR `record.sh`, not both.

## Register map (as-built, operator-facing)

Full list: `strings firmware/bitstream/*.fpg | grep -i inp2`. The ones `init_fpga.py` writes or reads:

| Register | R/W | Meaning |
| --- | --- | --- |
| `inp2_ip_addr` | W | destination IP (named `ip_addr`; a block named `ip` collides with the IP wrapper) |
| `inp2_port` | W | destination UDP port |
| `inp2_th_id` / `inp2_st_id` / `inp2_num_ch` | W | VDIF thread id / station id / log2(#chan) |
| `ref_epoch` / `sec_from_ep` | W | VDIF time seed (write-only seed; the gateware PPS counter holds the live second) |
| `arm` | W | latch the time seed and start transmission on the next PPS edge |
| `qsfp_rst` | W | QSFP TX enable, INVERTED: write 1 = ON, 0 = OFF |
| `pkt_rst` / `lpmode` | W | 100GbE core reset (write 3 then 0) / QSFP low-power pin |
| `requant_alpha_shift` | W | RMS estimator EMA smoothing shift (default 12) |
| `requant_thresh_override` / `requant_use_override` | W | manual 2-bit threshold / 1 = use manual, 0 = adaptive |
| `rq2_pow_i` / `rq2_pow_q` | R | per-stream RMS power readback |
| `sys_clkcounter` | R | fabric clock counter; true frame rate = `sys_clkcounter` rate / 2000 |
| `pps_count_sec` | R | increments +1 per second when PPS is present |

Fixed in gateware (not registers): bits/sample-1 = 1, data_type = 1 (complex), frame_length field =
1004 (8032-byte frames). The design is single-channel: only `inp2` (ADC2) exists; there are no
`inp1_*` or `rq1_*` registers.

Note on rates: `onehundred_gbe_gmac_reg_tx_packet_count` reads about 12 percent high on this design (a
GbE status-counter artifact). The true rate is `sys_clkcounter` rate / 2000 = 128000, confirmed by the
on-wire VDIF frame numbers. The monitor shows the true rate and labels the raw counter as reference only.

## AF_XDP recorder (how it works)

`vdif_recorder.c` captures with AF_XDP (XSK sockets), which bypasses the kernel socket layer and breaks
the single-flow receive ceiling that caps ordinary UDP sockets at roughly half the required rate. Key
implementation points, each hardware-validated:

- COPY mode, not zero-copy. This ConnectX-5 supports single-buffer zero-copy and copy-mode
  multi-buffer, but not zero-copy multi-buffer (`XDP_ZEROCOPY | XDP_USE_SG` returns `EOPNOTSUPP`).
  COPY mode still bypasses the socket layer and reaches line rate. `--xdp-zc` is kept for future NICs.
- Own frags-aware XDP program. On x86-64 a UMEM chunk is capped at the 4096-byte page in both aligned
  and unaligned mode, so the 8074-byte jumbo frame arrives as 2-3 `XDP_PKT_CONTD` fragments. At MTU
  9000 the mlx5 driver only attaches a frags-aware program, and libxdp 1.5.8's default program fails to
  attach in COPY mode here. The fix is `xsk_redirect.bpf.c` (`SEC("xdp.frags")`), loaded via libbpf and
  attached once; the recorder reassembles the fragments into each full frame. `bad_frames=0` proves the
  reassembly is byte-perfect.
- IRQ-driven two-core split, not busy-poll. In COPY mode the per-packet copy into UMEM runs in NAPI.
  Pinning NAPI and reassembly on one core leaves a small `rx_ring_full` drop rate. Letting hardware
  IRQs drive NAPI on the IRQ core while a separate capture core only reassembles and hands off gives
  `gap_frames=0`. `tune_nic.sh <nic> <irq_core> xdp` sets this up (ring 8192, one combined queue,
  `rx_striding_rq off`, `napi_defer_hard_irqs=0`, GRO/LRO off, IRQ affinity, performance governor).
  `record_supervisor.py` runs it automatically.
- Tight rings (UMEM 256 MiB, fill 16384, rx 8192), pre-faulted with `memset`. Larger rings hurt cache
  locality and drop the consumer below line rate.
- O_DIRECT slab writes. `8032 * 128 = 251 * 4096`, so 128 frames is exactly 251 pages and every slab is
  page- and frame-aligned. Frames are written wrapper-stripped, so the file is flat VDIF.
- Three independent loss counters, all reported: kernel `PACKET_STATISTICS` / XDP statistics,
  ring-to-disk `spsc_drops`, and end-to-end `gap_frames` from the VDIF frame number. Healthy = all zero.

Backends other than AF_XDP exist for testing: `--rx raw` (AF_PACKET v3), `--rx udp`. Off-hardware
self-test without the FPGA: `sudo ./veth_selftest.sh` (veth pair + `vdif_gen` at full rate, checks
zero drops and byte-exact files).

libxdp build note: `build_libxdp.sh` builds xdp-tools v1.5.8 with `FORCE_SUBDIR_LIBBPF=1`. That flag is
essential; without it configure links the system libbpf 0.5, which cannot set the XDP frags flag.

## Copy recordings to the analysis workstation

```
rsync -av --progress /mnt/vlbi0/bvex_*.vdif /mnt/vlbi0/bvex_*.vdif.json user@workstation:/data/bvex/
```

## Analysis and DiFX

On a workstation (not the recorder):

```
cd analysis
python3 -m venv venv && source venv/bin/activate
pip install -r requirements.txt
jupyter lab analyze_vdif.ipynb
```

`analyze_vdif.ipynb` opens a `.vdif` file (or the bundled `sample/haystack_test.vdif`), validates every
header, checks frame-number continuity against the sidecar, de-interleaves the 2-bit I/Q, plots the Van
Vleck histograms (target 16.4 / 33.6 / 33.6 / 16.4 percent), and computes the bandpass. DiFX prep:
`gen_vex.py` writes a `.vex` / `.v2d` / filelist (`format=VDIF/8032/2`), `run_mpifx.sh` launches
`mpifxcorr`, and `parse_difx.py` reads the SWIN output.

## Firmware

The compiled bitstream in `firmware/bitstream/` runs as-is; rebuilding is only needed to change the
design. The model is `firmware/rfsoc_vdif_v0_4_dev.slx`; the block diagram is in `docs/model_overview.pdf`
and `docs/images/`. The three custom VHDL blocks (each xsim-tested, bound as CASPER black boxes via
their `*_config.m`):

- `rms_estimator.vhd`: mean-absolute-deviation RMS estimator with EMA smoothing, derives the +/-0.9816
  sigma 2-bit thresholds using shift and add only (no DSP).
- `adaptive_requantizer.vhd`: 8-channel parallel 2-bit quantizer using those thresholds, VDIF
  offset-binary output.
- `sample_packer512.vhd`: interleaves and packs 16 clocks of 2-bit complex samples into one 512-bit
  word (`data_valid_out` pulses 1 in 16). This width is the rate-reduction mechanism.

Rebuild (toolchain: MATLAB R2021a + Vivado/Model Composer 2021.1 + `mlib_devel`):

```
# frontend (Simulink compile + HDL netlist), headless under Xvfb:
firmware/build/sgbatchx.sh <workdir> <run_jasper_frontend.m>
# backend (Vivado synth/P&R -> .fpg):
python $MLIB/jasper_library/exec_flow.py -m rfsoc_vdif_v0_4_dev.slx \
    --middleware --backend --software --vitis
```

`mc_setup.m` sets the Model Composer / SysGen paths. `sgbatch.sh` runs a MATLAB batch job;
`sgbatchx.sh` wraps it in Xvfb for model operations that pop SysGen dialogs. The black-box `*_config.m`
files use the deferred-clock pattern so they bind headless.

## Clocks

LMK first (programmed twice), then LMX. The files in `clocks/` are uploaded to the board by
`init_fpga.py` if not already present, then selected by name:
- `rfsoc4x2_lmk_CLKin0_extref_10M_PL_128M_LMXREF_256M.txt`: locks to the external 10 MHz, produces the
  128 MHz PL clock and the 256 MHz LMX reference.
- `rfsoc4x2_lmx_inputref_256M_outputref_512M.txt`: 256 MHz in, 512 MHz out (the ADC sample clock ref).

The `.tcs` files are the TICS Pro sources for regenerating the register dumps.

## Troubleshooting

- `config.py not found`: `cp config.example.py config.py` in `host/` and edit it.
- No frames captured: give the 100GbE link about 7 s to train after the QSFP enables; confirm 10 MHz
  and PPS are connected; check `ethtool <nic>` shows a partner.
- `fps` far from 128000 or fabric not near 256 MHz: clock chain problem (10 MHz reference missing or the
  wrong LMK/LMX file loaded).
- `--rx xdp` errors at runtime: libxdp was not built. Run `./build_libxdp.sh`, then `make`.
- NIC drops off the PCIe bus / high NIC temperature: the ConnectX-5 needs airflow over its heatsink;
  cold power-cycle the host and reseat the QSFP.

## License

MIT. See `LICENSE`.
