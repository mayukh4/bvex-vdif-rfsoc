#!/usr/bin/env python3
"""record_supervisor.py -- bring the RFSoC VDIF board up and run the C recorder.

This is the operator-facing launcher for the no-drop VDIF recorder. It:
  1. brings up the host 100GbE NIC (MTU 9000) and programs+arms the FPGA, reusing
     the EXACT bring-up sequence from init_fpga.py (single source of truth),
  2. launches the compiled C engine `vdif_recorder` (AF_PACKET v3 -> flat .vdif on
     the NVMe SSDs),
  3. runs a lightweight ~1 Hz monitor that MERGES board-register health (fabric
     clock, PPS, requant power, NIC temp) with the recorder's status.json, printing
     ONE concise console line and writing ONE merged status file for a future
     telemetry downlink,
  4. on Ctrl-C / exit: stops the recorder cleanly (SIGTERM), then disables the QSFP
     and drops the NIC via init_fpga.cleanup().

The C engine does the data path (capture + disk); this supervisor stays OUT of the
data path -- it only reads board registers and a small JSON file.

Physical prereqs (same as init_fpga.py): front-panel 10 MHz -> LMK CLKin0 and 1 PPS
-> the FPGA PPS pin, or the ADCs never lock and the VDIF seconds never align.

Site addresses and the NVMe mounts / CPU cores come from config.py (copy
config.example.py to config.py first). Normally launched via ./record.sh.

Usage (root needed for NIC + AF_PACKET + O_DIRECT):
    sudo <cfpga_venv>/bin/python record_supervisor.py [--secs N] \\
         [--disks /mnt/vlbi0,/mnt/vlbi1,/mnt/vlbi2] [--secs-per-file 10] \\
         [--cap-core C] [--wr-core W]
Stop with Ctrl-C. Run THIS *or* init_fpga.py, not both (both drive the QSFP/NIC).
"""
import os
import sys
import json
import time
import signal
import atexit
import argparse
import subprocess

import config                   # site config (NVMe mounts, CPU cores)
import init_fpga as vd          # board bring-up + telemetry helpers (single source of truth)

_HERE = os.path.dirname(os.path.abspath(__file__))

# --- defaults (override on the command line; base values come from config.py) ---
RECORDER_BIN   = os.path.join(_HERE, 'vdif_recorder')
REC_STATUS     = '/tmp/vdif_recorder_status.json'        # written by the C engine
MERGED_STATUS  = '/tmp/vdif_supervisor_status.json'      # written here, for downlink
DISKS_DEFAULT  = config.DISKS
SETTLE_S       = 8              # let the 100GbE link/FEC train before capturing

proc = None                    # the vdif_recorder subprocess


def stop_recorder():
    """SIGTERM the C recorder and wait for a clean finalize (SIGKILL as a backstop)."""
    global proc
    if proc is None:
        return
    if proc.poll() is None:
        print('Stopping recorder (SIGTERM)...')
        try:
            proc.send_signal(signal.SIGINT)        # same handler as Ctrl-C in the C engine
        except Exception:
            pass
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            print('  recorder did not exit; SIGKILL')
            proc.kill()
    proc = None


def _cleanup():
    """Stop the recorder, then hand off to init_fpga.cleanup() (QSFP off + NIC down)."""
    stop_recorder()
    try:
        vd.cleanup()
    except Exception as e:
        print('  (cleanup: %s)' % e)


def _sig(signum, frame):
    _cleanup()
    sys.exit(0)


def read_rec_status(path):
    """Read the recorder's status.json (best-effort; it is rewritten atomically)."""
    try:
        with open(path) as f:
            return json.load(f)
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser(description='RFSoC VDIF recorder supervisor')
    ap.add_argument('--secs', type=int, default=0,
                    help='record for N seconds then stop (0 = until Ctrl-C)')
    ap.add_argument('--disks', default=DISKS_DEFAULT,
                    help='comma-separated NVMe mounts, filled sequentially')
    ap.add_argument('--secs-per-file', type=int, default=10, help='file roll-over period (s)')
    ap.add_argument('--cap-core', type=int, default=config.CAP_CORE, help='pin capture thread to this CPU')
    ap.add_argument('--wr-core', type=int, default=config.WR_CORE, help='pin writer thread to this CPU')
    ap.add_argument('--status', default=REC_STATUS, help='recorder status.json path')
    ap.add_argument('--merged-status', default=MERGED_STATUS, help='merged status output path')
    ap.add_argument('--log', default='/tmp/vdif_recorder.log',
                    help='recorder stdout/stderr log (keeps the console clean)')
    ap.add_argument('--buffered', action='store_true', help='disable O_DIRECT in the recorder')
    ap.add_argument('--rx', default='xdp', choices=['xdp', 'udp', 'raw', 'ring'],
                    help='capture backend (default xdp = AF_XDP zero-copy, no-drop)')
    ap.add_argument('--xdp-copy', action='store_true',
                    help='AF_XDP: force XDP_COPY (skip zero-copy)')
    ap.add_argument('--xdp-no-busy', action='store_true',
                    help='AF_XDP: IRQ-driven (NAPI on the IRQ core, processing on cap-core)')
    ap.add_argument('--irq-core', type=int, default=config.IRQ_CORE, help='pin the NIC IRQ to this CPU')
    ap.add_argument('--no-tune', action='store_true',
                    help='skip the automatic tune_nic.sh (NIC ring/queue/IRQ/striding/governor)')
    args = ap.parse_args()

    if not os.path.exists(RECORDER_BIN):
        sys.exit('ERROR: %s not built. Run `make` on this host first.' % RECORDER_BIN)
    if vd.MANAGE_NIC and os.geteuid() != 0:
        sys.exit('Run as root (NIC + AF_PACKET + O_DIRECT):\n'
                 '  sudo %s record_supervisor.py' % sys.executable)

    atexit.register(_cleanup)
    signal.signal(signal.SIGINT, _sig)
    signal.signal(signal.SIGTERM, _sig)

    # ---- NIC tuning for the AF_XDP no-drop path (ring 8192, combined 1, rx_striding_rq
    #      off, IRQ->irq_core, napi_defer=0, governor) -- the validated zero-gap recipe ----
    if args.rx == 'xdp' and not args.no_tune:
        tune = os.path.join(_HERE, 'tune_nic.sh')
        if os.path.exists(tune):
            print('Tuning NIC: %s %s %d xdp' % (tune, vd.NIC, args.irq_core), flush=True)
            subprocess.run(['bash', tune, vd.NIC, str(args.irq_core), 'xdp'], check=False)
        else:
            print('WARNING: %s not found -- run NIC tuning manually for no-drop capture' % tune)

    # ---- board + NIC bring-up (identical to init_fpga.py) ----
    vd.nic_up()
    fpga = vd.connect_board()
    vd.fpga = fpga              # so init_fpga.cleanup() can disable the QSFP on exit
    vd.program_fpga(fpga)
    vd.configure_and_arm(fpga)
    print('Settling %d s for the 100GbE link/FEC to train ...' % SETTLE_S, flush=True)
    time.sleep(SETTLE_S)

    # ---- launch the C recorder ----
    cmd = [RECORDER_BIN,
           '--iface', vd.NIC,
           '--dport', str(vd.DEST_PORT),
           '--disks', args.disks,
           '--secs-per-file', str(args.secs_per_file),
           '--rx', args.rx,
           '--status', args.status]
    if args.rx == 'xdp' and args.xdp_copy:
        cmd += ['--xdp-copy']
    if args.rx == 'xdp' and args.xdp_no_busy:
        cmd += ['--xdp-no-busy']
    if args.cap_core >= 0:
        cmd += ['--cap-core', str(args.cap_core)]
    if args.wr_core >= 0:
        cmd += ['--wr-core', str(args.wr_core)]
    if args.buffered:
        cmd += ['--buffered']
    if args.secs > 0:
        cmd += ['--run-secs', str(args.secs)]
    print('Launching recorder: %s' % ' '.join(cmd), flush=True)
    print('  (recorder stdout/stderr -> %s; console shows the merged line only)' % args.log)
    global proc
    _logf = open(args.log, 'ab', buffering=0)
    proc = subprocess.Popen(cmd, stdout=_logf, stderr=_logf)

    # ---- lightweight merged monitor (~1 Hz) ----
    print('\n--- recording; Ctrl-C to stop (recorder finalized, QSFP off, NIC down on exit) ---')
    print('    REC = capture/disk health (from the C engine);  BOARD = FPGA register health.')
    print('    Healthy = fps~128000, drops/gaps 0, fabric ~256 MHz, PPS +1/s, time=OK.\n')

    def rd(reg):
        try:
            return fpga.read_uint(reg)
        except Exception:
            return None

    def d32(a, b):
        return (a - b) & 0xFFFFFFFF

    prev_clk = rd('sys_clkcounter') or 0
    prev_pps = rd('pps_count_sec')
    prev_t = time.time()
    nic_warned = False
    prev_gaps = 0          # gap_frames is the ground-truth loss; alarm only when it GROWS
    prev_kdrop = 0

    while True:
        time.sleep(1.0)
        now = time.time()
        dt = now - prev_t
        prev_t = now

        # ----- board-register health -----
        clk = rd('sys_clkcounter') or 0
        fabric_hz = d32(clk, prev_clk) / dt if dt > 0 else 0.0
        prev_clk = clk
        true_pps = fabric_hz / 2000.0                  # framer is a fixed divide-by-2000
        pps = rd('pps_count_sec')
        d_pps = (pps - prev_pps) if (pps is not None and prev_pps is not None) else None
        prev_pps = pps
        powi, powq = rd('rq2_pow_i'), rd('rq2_pow_q')
        nicT = vd.nic_temp_c()

        # ----- recorder health (status.json) -----
        rs = read_rec_status(args.status) or {}
        rstate = rs.get('state', '??')
        fps    = rs.get('fps_capture', 0)
        kdrop  = rs.get('kernel_drops', 0)
        sdrop  = rs.get('spsc_drops', 0)
        gaps   = rs.get('gap_frames', 0)
        time_ok = rs.get('time_ok', False)
        cur_disk = rs.get('cur_disk', 0)
        freeb  = rs.get('free_bytes', [])
        cur_file = os.path.basename(rs.get('cur_file', '') or '(none)')
        alarm  = rs.get('alarm', '')
        free_t = (freeb[cur_disk] / 1e12) if (freeb and cur_disk < len(freeb)) else 0.0

        # ----- NIC presence / thermal warnings -----
        if vd.MANAGE_NIC and not vd.nic_present() and not nic_warned:
            print('!! NIC %s DROPPED OFF THE PCIe BUS -- ConnectX-5 thermal/PCIe fault. '
                  'COLD power-cycle the host + airflow over the NIC.' % vd.NIC)
            nic_warned = True
        if nicT is not None and nicT >= vd.NIC_TEMP_WARN_C:
            print('!! NIC %.0f C -- HIGH (faults ~105 C). Add airflow NOW.' % nicT)

        # ----- one concise merged line -----
        # gap_frames is the authoritative end-to-end loss (frame# discontinuity); a few
        # kernel_drops at startup (pre-frame#0 link junk) are benign. Alarm only when the
        # loss counters are GROWING, not on a static startup baseline.
        d_gaps  = gaps  - prev_gaps;  prev_gaps  = gaps
        d_kdrop = kdrop - prev_kdrop; prev_kdrop = kdrop
        tag = 'OK'
        if rstate != 'RUNNING':                tag = rstate
        elif fps < 1000:                       tag = 'NO-DATA'
        elif d_gaps > 0 or sdrop > 0:          tag = 'DROPS!'      # real, ongoing loss
        elif d_kdrop > 0:                      tag = 'kdrop'       # NIC/XSK drops growing (watch)
        elif abs(fabric_hz/1e6 - 256.0) > 25:  tag = 'CLK?'
        elif not time_ok or d_pps != 1:        tag = 'TIME?'
        print('REC fps=%-7d drop(k/s)=%s/%s gaps=%s time=%s | BOARD %.1fMHz PPS%s pow=%s/%s nicT=%s '
              '| disk%d free=%.2fT %s [%s]%s'
              % (fps, kdrop, sdrop, gaps, 'OK' if time_ok else '--',
                 fabric_hz/1e6, ('+%d' % d_pps if d_pps is not None else '?'),
                 powi, powq, ('%.0fC' % nicT) if nicT is not None else 'NA',
                 cur_disk, free_t, cur_file, tag,
                 ('  ALARM: %s' % alarm) if alarm else ''))
        sys.stdout.flush()

        # ----- merged status for downlink (atomic rewrite) -----
        merged = dict(ts_unix=int(now), verdict=tag,
                      board=dict(fabric_mhz=round(fabric_hz/1e6, 3), true_pps=round(true_pps),
                                 pps_delta=d_pps, pow_i=powi, pow_q=powq, nic_tempC=nicT,
                                 nic_present=vd.nic_present()),
                      recorder=rs)
        try:
            tmp = args.merged_status + '.tmp'
            with open(tmp, 'w') as f:
                json.dump(merged, f, indent=2)
            os.replace(tmp, args.merged_status)
        except Exception:
            pass

        # ----- exit conditions -----
        if proc.poll() is not None:
            print('\nRecorder exited (code %s).' % proc.returncode)
            break
        if rstate.startswith('STOPPED'):
            print('\nRecorder reported state=%s -- stopping.' % rstate)
            break


if __name__ == '__main__':
    try:
        main()
    finally:
        _cleanup()
