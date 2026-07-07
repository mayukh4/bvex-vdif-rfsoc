#!/bin/bash
# record.sh -- THE single command to record VDIF to the SSDs.
#
#   ./record.sh                # record until Ctrl-C (rolls files across the configured disks)
#   ./record.sh --secs 300     # record 5 minutes, then stop cleanly
#   ./record.sh --secs-per-file 30
#
# It does EVERYTHING end to end:
#   1. tunes the NIC for the AF_XDP no-drop path (ring/queue/striding/IRQ/napi/governor),
#   2. brings the RFSoC + 100GbE link up (programs FPGA, clocks, ADC, QSFP, PPS arm),
#   3. captures the stream gaplessly with the AF_XDP backend (COPY, IRQ-driven),
#   4. writes flat .vdif files (+ JSON sidecars) sequentially across the configured NVMe mounts,
#   5. prints one health line per second, and on Ctrl-C finalizes the file, turns the
#      QSFP off and brings the NIC down.
#
# Interpreter, NVMe mounts and CPU cores come from host/config.py (copy config.example.py
# first). Healthy line = "fps=128000 ... gaps=0 ... time=OK ... [OK]". Needs root (re-execs
# under sudo). Output: /mnt/vlbiX/bvex_<UTC>_th0.vdif  (+ .vdif.json sidecars).
cd "$(dirname "$0")" || exit 1
if [ ! -f config.py ]; then
    echo "config.py not found. Copy the template and edit it for your site:" >&2
    echo "    cp config.example.py config.py" >&2
    exit 1
fi
PY="$(python3 -c 'import config; print(config.CFPGA_PY)' 2>/dev/null || echo python3)"
exec sudo "$PY" record_supervisor.py "$@"
