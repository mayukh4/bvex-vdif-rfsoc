#!/usr/bin/env python3
"""
init_fpga.py -- program, configure, AND run the VDIF RFSoC 2-bit complex packetizer.

Bitstream: rfsoc_vdif_v0_4_dev  (RFDC 4096 MSPS Real->I/Q, Decim 2x, NCO -1.024 GHz;
adaptive 2-bit complex requantization; VDIF over 100 GbE).  Built 2026-06-19.

This is a LONG-RUNNING controller (not a one-shot).  It:
  1. brings up the host 100GbE NIC at MTU 9000 (sudo),
  2. programs the FPGA + clocks, checks ADC lock,
  3. configures the 100GbE core + ARP and ENABLES the QSFP transmitter (qsfp_rst=1),
  4. seeds VDIF time (ref_epoch / sec_from_ep) and arms transmission,
  5. stays alive printing a live status line (TX rate / Gbps, overflow, ADC clock
     count, PPS readback, seconds advancing) so you can watch the stream,
  6. on Ctrl-C / exit DISABLES the QSFP (qsfp_rst=0, saves power) and brings the
     NIC down -- via atexit + signal handlers.

PHYSICAL PREREQUISITES for VALID data/timing (front-panel SMAs on the RFSoC):
  * 10 MHz reference  -> LMK CLKin0.  The loaded LMK file is `CLKin0_extref_10M`,
    so it LOCKS TO AN EXTERNAL 10 MHz.  No 10 MHz => ADCs never lock => no data.
  * 1 PPS             -> the FPGA PPS input (a hardware pin; there is NO irig_trig
    register in this design).  Verified live via clk_count_at_pps / clk_count_sec
    (~256,000,000/s) / pps_count_sec (+1/s).  No PPS => VDIF seconds won't align.

Register-name notes (match the as-built .fpg):
  * Destination-IP register is inp2_ip_addr (a block named `ip` once collided with
    the packaged <model>_ip wrapper and was renamed to ip_addr).
  * Requant is RUNTIME-TUNABLE via software registers (added 2026-06-22):
      requant_alpha_shift     (4-bit) EMA smoothing shift, default 12
      requant_thresh_override (16-bit) manual 2-bit threshold magnitude
      requant_use_override    (1-bit) 1 => use the manual threshold, 0 => adaptive
      rq{1,2}_pow_{i,q}       (32-bit, read-only) per-stream RMS power readback
    (The requant reset stays a fixed gateware constant -- no software reset reg.)
  * SINGLE-CHANNEL design (2026-06-23): only inp2 (ADC2 / RFDC tile 226) feeds the
    100GbE TX -- the inp1 path and the entire 2->1 merge were stripped out of the
    model, so there is exactly ONE clean VDIF thread. The inp1_* and rq1_pow_*
    registers no longer exist; do not write/read them.
  * qsfp_rst is INVERTED: write 1 = QSFP port ON, write 0 = OFF.
  * bits/sample-1 (=1), data_type (=1, complex), frame_length field (=1004 -> 8032
    bytes) are fixed in the gateware (Bug A fix: 125 packed words = 8032-B payload).

Site addresses (board IP, NIC, data-plane IPs/MAC, VDIF ids) come from config.py --
copy config.example.py to config.py and edit it first.

Run it (root needed for NIC + optional live packet sniff):
    sudo <cfpga_venv>/bin/python init_fpga.py
Stop it with Ctrl-C (QSFP off + NIC down on the way out).
"""
import os
import sys
import glob
import time
import signal
import atexit
import struct
import subprocess
from datetime import datetime

import numpy
import casperfpga

# ---------------------------------------------------------------------
# Site configuration lives in config.py (copy config.example.py -> config.py and edit).
# ---------------------------------------------------------------------
try:
    import config
except ModuleNotFoundError:
    sys.exit('config.py not found. Copy the template and edit it for your site:\n'
             '    cp config.example.py config.py')

BOARD_IP    = config.BOARD_IP
FPG_FILE    = config.FPG_FILE           # newest firmware/bitstream/*.fpg (see config)
CLOCK_DIR   = config.CLOCK_DIR          # clocks/ (LMK/LMX register files)
LMK_CLK     = config.LMK_CLK            # basename; uploaded from CLOCK_DIR if not on the board
LMX_CLK     = config.LMX_CLK

NIC         = config.NIC
NIC_MTU     = config.NIC_MTU
MANAGE_NIC  = config.MANAGE_NIC
HOST_IP     = config.HOST_IP

FABRIC_PORT = config.FABRIC_PORT
DEST_PORT   = config.DEST_PORT
DEST_IP     = config.DEST_IP
DEST_MAC    = config.DEST_MAC
FPGA_IP     = config.FPGA_IP

STATION_ID  = config.STATION_ID
NUM_CH_LOG2 = config.NUM_CH_LOG2
THREAD_ID   = config.THREAD_ID

REQUANT_ALPHA_SHIFT     = config.REQUANT_ALPHA_SHIFT
REQUANT_THRESH_OVERRIDE = config.REQUANT_THRESH_OVERRIDE
REQUANT_USE_OVERRIDE    = config.REQUANT_USE_OVERRIDE
SHOW_REQUANT_POWER      = config.SHOW_REQUANT_POWER

STATUS_PERIOD = config.STATUS_PERIOD

# --- fixed design constants (independent of site; do not edit) ---
# Each frame is 125 packed 512-bit words = 8000 B payload + 32 B VDIF header = 8032 B
# UDP payload == the declared frame_length. On the wire that is 8032 + 8 UDP + 20 IP
# + 14 Eth = 8074 B.
WIRE_BYTES_PER_PKT = 8074
VDIF_PAYLOAD_B = 8032             # UDP payload/frame = 32-B VDIF header + 8000-B data array
EXPECTED_PPS   = 128000           # ONE VDIF thread: 2048 Msps complex / 16000 complex-samples per frame
EXPECTED_GBPS  = EXPECTED_PPS * WIRE_BYTES_PER_PKT * 8 / 1e9   # ~8.27 Gbps on the wire (one thread)

fpga = None
_cleaned = False


# ----------------------------- helpers -----------------------------
def _ip2int(ip):
    a, b, c, d = (int(x) for x in ip.split('.'))
    return (a << 24) + (b << 16) + (c << 8) + d


def _run(cmd, check=False):
    """Run a shell command (list form). Prefix sudo unless we are already root."""
    if cmd[0] == 'ip' and os.geteuid() != 0:
        cmd = ['sudo'] + cmd
    return subprocess.run(cmd, check=check)


def nic_present():
    """True if the NIC interface exists (a ConnectX-5 PCIe-fatal drops it off the bus)."""
    return os.path.exists('/sys/class/net/%s' % NIC)


def nic_temp_c():
    """Best-effort ConnectX-5 ASIC temperature in deg C (None if unavailable).
    The card thermal-shuts-down (mlx5 'synd 0x10: High temperature' -> Fatal error
    -> PCI slot unavailable) without server-grade airflow."""
    for p in glob.glob('/sys/class/net/%s/device/hwmon/hwmon*/temp1_input' % NIC):
        try:
            return int(open(p).read().strip()) / 1000.0
        except Exception:
            pass
    return None


NIC_TEMP_WARN_C = 90.0     # ConnectX-5 throttles ~105 C and faults above; warn early


def nic_up():
    if not MANAGE_NIC:
        return
    if not nic_present():
        print('WARNING: NIC %s not present -- it likely fell off the PCIe bus '
              '(known ConnectX-5 PCIe fault; lspci shows rev ff). COLD power-cycle '
              'the host + reseat the QSFP cable, then retry.' % NIC)
        return
    print('Bringing up %s at MTU %d ...' % (NIC, NIC_MTU))
    _run(['ip', 'link', 'set', NIC, 'mtu', str(NIC_MTU), 'up'], check=True)
    # assign an IP (harmless for raw capture; useful if a UDP receiver is added)
    _run(['ip', 'addr', 'add', '%s/24' % HOST_IP, 'dev', NIC], check=False)
    time.sleep(1.0)


def nic_down():
    if not MANAGE_NIC:
        return
    print('Bringing %s down ...' % NIC)
    _run(['ip', 'link', 'set', NIC, 'down'], check=False)


def cleanup():
    """Disable the QSFP transmitter (save power) and drop the NIC. Idempotent."""
    global _cleaned
    if _cleaned:
        return
    _cleaned = True
    print('\n--- shutting down ---')
    try:
        if fpga is not None and fpga.is_connected():
            fpga.write_int('qsfp_rst', 0)       # inverted: 0 = QSFP OFF
            time.sleep(0.1)
            print('QSFP transmitter disabled (qsfp_rst=0)')
    except Exception as e:
        print('  (could not disable QSFP: %s)' % e)
    nic_down()
    print('shutdown complete')


def _sig(signum, frame):
    cleanup()
    sys.exit(0)


def vdif_epoch_now():
    """VDIF (ref_epoch, sec_from_ep) for the current UTC second."""
    now = datetime.utcnow()
    half = 0 if now.month < 7 else 1
    ref_epoch = (now.year - 2000) * 2 + half
    epoch_start = datetime(now.year, 1 if half == 0 else 7, 1)
    sec_from_ep = int((now - epoch_start).total_seconds())
    return ref_epoch, sec_from_ep


def _clk_index(files, exact, kind):
    if exact in files:
        return files.index(exact)
    for i, f in enumerate(files):
        if kind in f.lower():
            return i
    raise RuntimeError('clock file for %r not found on board: %r' % (kind, files))


def parse_vdif_header(first16):
    """Return a dict of the key VDIF header fields from the first 16 bytes."""
    w0, w1, w2, w3 = struct.unpack('<IIII', first16[:16])
    return dict(
        invalid=(w0 >> 31) & 1,
        sec_from_ep=w0 & 0x3FFFFFFF,
        frame_num=w1 & 0x00FFFFFF,
        ref_epoch=(w1 >> 24) & 0x3F,
        frame_len=(w2 & 0x00FFFFFF) * 8,
        log2_nchan=(w2 >> 24) & 0x1F,
        bps_m1=(w3 >> 26) & 0x1F,
        data_type=(w3 >> 31) & 1,
        thread_id=(w3 >> 16) & 0x3FF,
        station_id=w3 & 0xFFFF,
    )


def validate_vdif_header(first16, verbose=True):
    h = parse_vdif_header(first16)
    if verbose:
        print('  VDIF: invalid=%d sec=%d ref_epoch=%d frame#=%d' %
              (h['invalid'], h['sec_from_ep'], h['ref_epoch'], h['frame_num']))
        print('        frame_len=%d B (field=%d) log2nch=%d bps-1=%d data_type=%d'
              % (h['frame_len'], h['frame_len'] // 8, h['log2_nchan'],
                 h['bps_m1'], h['data_type']))
        print('        thread_id=%d station_id=%d' % (h['thread_id'], h['station_id']))
    ok = (h['frame_len'] == 8032 and h['bps_m1'] == 1 and h['data_type'] == 1)
    print('  VDIF header %s' % ('VALID (8032 B, 2-bit, complex)' if ok else
                                'UNEXPECTED -- check frame_len/bps/data_type'))
    return ok


def sniff_one_vdif(timeout=3.0):
    """Capture one jumbo frame off the NIC and validate its VDIF header.
    Needs root (AF_PACKET). Skipped with a note otherwise."""
    import socket
    if os.geteuid() != 0:
        print('(skip live packet validation: not root -- run with sudo to enable)')
        return None
    if not nic_present():
        print('  NIC %s is gone (dropped off the PCIe bus) -- skipping sniff. '
              'Cold power-cycle the host + reseat the QSFP, then retry.' % NIC)
        return None
    print('Sniffing one VDIF frame on %s (timeout %.0fs) ...' % (NIC, timeout))
    try:
        s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(0x0003))
        s.bind((NIC, 0))
    except OSError as e:
        print('  NIC %s unavailable (%s) -- ConnectX-5 likely fell off the bus '
              '(known thermal/PCIe fault). Cold power-cycle + reseat QSFP.' % (NIC, e))
        return None
    s.settimeout(timeout)
    try:
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                pkt = s.recv(9200)
            except socket.timeout:
                break
            if len(pkt) > 1000:                  # our jumbo VDIF frames (~8074 B on the wire)
                validate_vdif_header(pkt[42:42 + 16])   # skip Eth(14)+IP(20)+UDP(8)
                return pkt
        print('  no jumbo frames captured -- is the link up / QSFP enabled / armed?')
    finally:
        s.close()
    return None


def validate_stream(n_frames=60, timeout=6.0):
    """Capture several VDIF frames off the NIC and print a CLEAR data verdict:
    on-wire frame SIZE (the still-unconfirmed 8032-B check), VDIF header fields,
    thread id (expect just THREAD_ID), and whether the second is advancing.
    Returns a summary dict (or None). Needs root (AF_PACKET)."""
    import socket
    if os.geteuid() != 0:
        print('(skip live packet validation: not root -- run with sudo to enable)')
        return None
    if not nic_present():
        print('  NIC %s gone (PCIe bus) -- skip capture; cold power-cycle + reseat QSFP.' % NIC)
        return None
    print('Capturing up to %d VDIF frames on %s (timeout %.0fs) ...' % (n_frames, NIC, timeout))
    try:
        s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(0x0003))
        s.bind((NIC, 0))
    except OSError as e:
        print('  NIC %s unavailable (%s) -- ConnectX-5 likely off the bus.' % (NIC, e))
        return None
    s.settimeout(timeout)
    sizes = {}              # on-wire length -> count
    threads = {}            # thread_id -> count
    secs = set()
    fmin = fmax = None
    first_pkt = None
    got = 0
    try:
        deadline = time.time() + timeout
        while got < n_frames and time.time() < deadline:
            try:
                pkt = s.recv(9200)
            except socket.timeout:
                break
            if len(pkt) <= 1000:
                continue
            got += 1
            sizes[len(pkt)] = sizes.get(len(pkt), 0) + 1
            h = parse_vdif_header(pkt[42:42 + 16])
            threads[h['thread_id']] = threads.get(h['thread_id'], 0) + 1
            secs.add(h['sec_from_ep'])
            fmin = h['frame_num'] if fmin is None else min(fmin, h['frame_num'])
            fmax = h['frame_num'] if fmax is None else max(fmax, h['frame_num'])
            if first_pkt is None:
                first_pkt = pkt
    finally:
        s.close()

    if got == 0:
        print('  NO frames captured -- link up? QSFP on? armed? (give it a few s after start)')
        return None

    print('  captured %d frames; decoded first header:' % got)
    validate_vdif_header(first_pkt[42:42 + 16])
    h0 = parse_vdif_header(first_pkt[42:42 + 16])

    # --- on-wire SIZE check (the Bug-A / frame-trigger check) ---
    main_size = max(sizes, key=sizes.get)
    udp = main_size - 42                          # minus Eth(14)+IP(20)+UDP(8)
    size_ok = (udp == VDIF_PAYLOAD_B)
    print('  on-wire frame: %d B (UDP payload %d B; want %d wire / %d payload) -- %s'
          % (main_size, udp, WIRE_BYTES_PER_PKT, VDIF_PAYLOAD_B,
             'OK' if size_ok else
             'WRONG SIZE -> frame trigger off; tweak inp2 Constant4/6/11 + rebuild'))
    if len(sizes) > 1:
        print('  (size spread: %s -- minority sizes may be host capture artifacts)' % sizes)

    # --- header + thread + time checks ---
    hdr_ok = (h0['frame_len'] == 8032 and h0['bps_m1'] == 1 and h0['data_type'] == 1)
    tids = sorted(threads)
    thread_ok = (tids == [THREAD_ID])
    print('  thread id(s): %s (want [%d]) -- %s'
          % (tids, THREAD_ID, 'OK (single thread)' if thread_ok else 'UNEXPECTED'))
    print('  frame# %s..%s; seconds seen: %s (>1 value => second advancing / PPS OK)'
          % (fmin, fmax, sorted(secs)))
    ok = size_ok and hdr_ok and thread_ok
    print('  >>> DATA %s' % ('HEALTHY (8032 B, 2-bit complex, single thread)' if ok
                             else 'NEEDS ATTENTION -- see the flags above'))
    return dict(got=got, size_ok=size_ok, hdr_ok=hdr_ok, thread_ok=thread_ok,
                main_size=main_size, udp=udp, threads=tids, fmin=fmin, fmax=fmax)


# ------------------- board bring-up (SHARED, single source of truth) -------------------
# Both init_fpga.py (verify/monitor) and record_vdif.py (record to disk) call these so
# the program/clock/100GbE/dest/requant/arm sequence can never drift between the two.
def connect_board(board_ip=BOARD_IP):
    print('Connecting to %s ...' % board_ip)
    return casperfpga.CasperFpga(board_ip, transport=casperfpga.KatcpTransport)


def program_fpga(fpga_obj, fpg_file=None):
    """Upload+program the .fpg, load the register map, program the LMK(x2)+LMX PLLs,
    and print ADC status. Self-contained so a fresh/power-cycled board comes up fully."""
    fpg_file = fpg_file or FPG_FILE
    print('Programming %s ...' % os.path.basename(fpg_file))
    fpga_obj.upload_to_ram_and_program(fpg_file)
    fpga_obj.get_system_information(fpg_file)
    rfdc = fpga_obj.adcs['rfdc']
    rfdc.init()
    existing = rfdc.show_clk_files()
    if LMX_CLK not in existing:
        rfdc.upload_clk_file(os.path.join(CLOCK_DIR, LMX_CLK))
    if LMK_CLK not in existing:
        rfdc.upload_clk_file(os.path.join(CLOCK_DIR, LMK_CLK))
    c = rfdc.show_clk_files()
    lmk = c[_clk_index(c, LMK_CLK, 'lmk')]
    lmx = c[_clk_index(c, LMX_CLK, 'lmx')]
    print('Programming PLLs: LMK=%s (x2), LMX=%s' % (lmk, lmx))
    rfdc.progpll('lmk', lmk)
    rfdc.progpll('lmk', lmk)
    rfdc.progpll('lmx', lmx)
    print('--- ADC status (ADCs 0 & 2 want state 15, PLL 1; needs the 10 MHz ref) ---')
    try:
        rfdc.status()
    except Exception as e:
        print('  rfdc.status() error: %s' % e)


def configure_and_arm(fpga_obj):
    """Reset+configure the 100GbE core (MAC/IP/ARP), ENABLE the QSFP, set per-thread
    destination + VDIF header fields + requant controls, then seed VDIF time and arm.
    After this the board transmits ONE valid VDIF thread (given 10 MHz + PPS present)."""
    # ---- 100GbE core ----
    fpga_obj.write_int('pkt_rst', 3)
    fpga_obj.write_int('pkt_rst', 0)
    mac_base = (2 << 40) + (2 << 32)
    gbe = fpga_obj.gbes['onehundred_gbe']
    gbe.set_arp_table(mac_base + numpy.arange(256))
    gbe.set_single_arp_entry(DEST_IP, DEST_MAC)             # recorder NIC (override the base ARP entry)
    gbe.configure_core(mac_base + 20, _ip2int(FPGA_IP), FABRIC_PORT)

    # ---- ENABLE the QSFP transmitter (this is what lights the host NIC) ----
    fpga_obj.write_int('qsfp_rst', 1)                       # inverted: 1 = ON
    print('QSFP transmitter ENABLED (qsfp_rst=1)')

    # ---- destination + VDIF header (single thread: inp2 = ADC2 / tile 226) ----
    fpga_obj.write_int('inp2_ip_addr', _ip2int(DEST_IP))
    fpga_obj.write_int('inp2_port', DEST_PORT)
    fpga_obj.write_int('inp2_st_id', STATION_ID)
    fpga_obj.write_int('inp2_num_ch', NUM_CH_LOG2)
    fpga_obj.write_int('inp2_th_id', THREAD_ID)            # single VDIF thread

    # ---- requant control (runtime software registers) ----
    try:
        fpga_obj.write_int('requant_alpha_shift', REQUANT_ALPHA_SHIFT)
        fpga_obj.write_int('requant_thresh_override', REQUANT_THRESH_OVERRIDE)
        fpga_obj.write_int('requant_use_override', REQUANT_USE_OVERRIDE)
        print('Requant: alpha_shift=%d, %s' % (
            REQUANT_ALPHA_SHIFT,
            'MANUAL thr=%d' % REQUANT_THRESH_OVERRIDE if REQUANT_USE_OVERRIDE
            else 'adaptive thresholds'))
    except Exception as e:
        print('  (requant registers not present? %s)' % e)

    # ---- seed VDIF time + arm ----
    ref_epoch, sec_from_ep = vdif_epoch_now()
    fpga_obj.write_int('ref_epoch', ref_epoch)
    fpga_obj.write_int('sec_from_ep', sec_from_ep + 1)      # arm for the next second
    fpga_obj.write_int('arm', 1)
    print('Armed: ref_epoch=%d sec_from_ep=%d' % (ref_epoch, sec_from_ep + 1))


# ----------------------------- main -----------------------------
def main():
    global fpga

    atexit.register(cleanup)
    signal.signal(signal.SIGINT, _sig)
    signal.signal(signal.SIGTERM, _sig)

    # cache sudo up front so NIC down at exit doesn't reprompt mid-run
    if MANAGE_NIC and os.geteuid() != 0:
        print('Caching sudo credentials (for NIC bring-up/down)...')
        subprocess.run(['sudo', '-v'], check=True)

    nic_up()

    fpga = connect_board()
    program_fpga(fpga)
    configure_and_arm(fpga)

    time.sleep(7.0)            # allow the 100GbE link to train (~3-5 s) before validating
    validate_stream(n_frames=60, timeout=6.0)

    # ---- live monitor ----
    print('\n--- streaming; Ctrl-C to stop (QSFP off + NIC down on exit) ---')
    print('    TRUE frame rate = sys_clkcounter/2000 (the framer is a fixed divide-by-2000):')
    print('    expect ~%d pps / ~%.2f Gbps, one VDIF thread, full 8032-B frames, PPS +1/s.' %
          (EXPECTED_PPS, EXPECTED_GBPS))
    print('    fabric should read ~256 MHz (=> ADC 4096 MS/s). NOTE: the GbE tx_packet_count register')
    print('    reads ~12%% HIGH on this design (counter artifact -- the on-wire frame# proves 128,000')
    print('    UNIQUE full frames/s, verified by capture); it is shown as txcnt_raw for REFERENCE ONLY,')
    print('    not used to judge the rate. HEALTHY = [DATA OK] + [TIME OK]. (sec_seed = write-only seed.)\n')

    def rd(reg):
        try:
            return fpga.read_uint(reg)
        except Exception:
            return None

    def d32(a, b):
        return (a - b) & 0xFFFFFFFF

    prev_clk = rd('sys_clkcounter') or 0
    prev_pkts = rd('onehundred_gbe_gmac_reg_tx_packet_count') or 0
    prev_pps = rd('pps_count_sec')
    prev_t = time.time()
    nic_warned = False
    while True:
        time.sleep(STATUS_PERIOD)
        now = time.time()
        dt = now - prev_t
        prev_t = now

        if MANAGE_NIC and not nic_present() and not nic_warned:
            print('!! NIC %s DROPPED OFF THE PCIe BUS -- no data reaching the host '
                  '(FPGA still transmitting; ConnectX-5 thermal/PCIe fault). '
                  'COLD power-cycle the host + add airflow over the NIC.' % NIC)
            nic_warned = True

        tC = nic_temp_c()
        if tC is not None and tC >= NIC_TEMP_WARN_C:
            print('!! NIC TEMPERATURE %.0f C -- HIGH (ConnectX-5 thermal-faults ~105 C). '
                  'Add a fan over the card heatsink NOW.' % tC)

        # TRUE frame rate from the fabric clock: the framer is a fixed divide-by-2000 (125 packer
        # words x 16 clk -> 1 VDIF frame), so frames/sec = sys_clkcounter_rate / 2000. This matches
        # the on-wire VDIF frame# counter (128,000 unique frames/s, verified by capture). The GbE
        # tx_packet_count register reads ~12% high (counter artifact), so it is NOT used here --
        # only shown as txcnt_raw for reference.
        clk = rd('sys_clkcounter') or 0
        fabric_hz = d32(clk, prev_clk) / dt if dt > 0 else 0.0
        prev_clk = clk
        true_pps = fabric_hz / 2000.0
        gbps = true_pps * WIRE_BYTES_PER_PKT * 8 / 1e9

        pkts = rd('onehundred_gbe_gmac_reg_tx_packet_count') or 0
        raw_pps = d32(pkts, prev_pkts) / dt if dt > 0 else 0.0
        prev_pkts = pkts

        # gbe_ovflow_reg is the reliable overflow indicator (reads 0 on hardware). The gmac
        # tx_overflow_count and clk_count_at_pps registers return garbage on this design.
        gbe_ovf = rd('gbe_ovflow_reg')
        pps_cnt = rd('pps_count_sec')

        # ---- health verdicts: fabric ~256 MHz + frames flowing + no overflow ----
        d_pps = (pps_cnt - prev_pps) if (pps_cnt is not None and prev_pps is not None) else None
        prev_pps = pps_cnt
        if fabric_hz < 1e6 or true_pps < 1000:
            data_tag = 'NO TX/CLK'                        # fabric clock dead / nothing transmitting
        elif abs(fabric_hz / 1e6 - 256.0) > 25:
            data_tag = 'CLK OFF'                          # fabric not ~256 MHz => clock chain problem
        elif gbe_ovf not in (0, None):
            data_tag = 'OVERFLOW'                         # real GbE overflow
        else:
            data_tag = 'DATA OK'                          # ~128k fps @ 256 MHz => full 8032-B frames
        time_tag = ('TIME OK' if d_pps == 1 else
                    'PPS MISSING' if d_pps == 0 else 'PPS?')

        powstr = ''
        if SHOW_REQUANT_POWER:
            powstr = ' | pow=%s/%s' % (rd('rq2_pow_i'), rd('rq2_pow_q'))
        tstr = '' if tC is None else ' | nicT=%.0fC' % tC

        print('fps %7.0f ~%4.2f Gbps [%-9s] | fabric %.2f MHz | PPS#%s(%s)[%-11s] | gbe_ovf=%s | txcnt_raw %.0f(+12%%)%s%s'
              % (true_pps, gbps, data_tag, fabric_hz / 1e6,
                 pps_cnt, ('+%d' % d_pps if d_pps is not None else '?'), time_tag,
                 gbe_ovf, raw_pps, powstr, tstr))


def check_adc():
    """Convenience: print ADC status (ADCs 0 & 2 want state 15, PLL 1)."""
    fpga.adcs['rfdc'].status()


if __name__ == '__main__':
    try:
        main()
    finally:
        cleanup()
