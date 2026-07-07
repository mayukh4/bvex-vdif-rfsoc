#!/usr/bin/env python3
"""
config.example.py -- site configuration for the mmBVEX RFSoC VDIF bring-up + recorder.

Copy this file to config.py and edit the values for your setup:

    cp config.example.py config.py

config.py is gitignored (it holds your host-specific addresses). init_fpga.py and
record_supervisor.py both import it. Any IP octet shown as 'xx' below is a placeholder
you MUST replace with your real value.
"""
import os
import glob

# --- repo-relative paths (leave as-is; resolved from this file's location) ---
_HERE = os.path.dirname(os.path.abspath(__file__))            # .../host
REPO_ROOT = os.path.dirname(_HERE)
BITSTREAM_DIR = os.path.join(REPO_ROOT, 'firmware', 'bitstream')
CLOCK_DIR = os.path.join(REPO_ROOT, 'clocks')

# --- RFSoC board (KATCP control link over the management network) ---
BOARD_IP = '172.20.4.xx'             # RFSoC control (KATCP) IP

# --- bitstream: auto-select the NEWEST built .fpg in firmware/bitstream/ ---
_FPGS = sorted(glob.glob(os.path.join(BITSTREAM_DIR, 'rfsoc_vdif_v0_4_dev_*.fpg')))
FPG_FILE = _FPGS[-1] if _FPGS else os.path.join(
    BITSTREAM_DIR, 'rfsoc_vdif_v0_4_dev_2026-06-24_1521.fpg')

# --- clock register files (10 MHz external-reference chain; live in clocks/) ---
LMK_CLK = 'rfsoc4x2_lmk_CLKin0_extref_10M_PL_128M_LMXREF_256M.txt'
LMX_CLK = 'rfsoc4x2_lmx_inputref_256M_outputref_512M.txt'

# --- host 100GbE NIC (the port cabled to the FPGA QSFP) ---
NIC        = 'enp1s0f0np0'           # your 100GbE interface (see: ethtool)
NIC_MTU    = 9000                    # jumbo frames for the 8032-byte VDIF frames
MANAGE_NIC = True                    # bring the NIC up at start / down at exit (needs sudo)
HOST_IP    = '10.17.16.xx'           # IP assigned to the NIC (recorder side)

# --- 100GbE data-plane addressing (separate from the management network) ---
FABRIC_PORT = 4000                   # FPGA source/fabric UDP port
DEST_PORT   = FABRIC_PORT + 1        # VDIF destination UDP port (4001)
DEST_IP     = '10.17.16.xx'          # recorder NIC IP (== HOST_IP)
DEST_MAC    = 0xb83fd2e472fe         # recorder NIC MAC (b8:3f:d2:e4:72:fe)
FPGA_IP     = '10.17.16.xx'          # the FPGA's own 100GbE IP

# --- VDIF header values (single thread = inp2 / ADC2) ---
STATION_ID  = 0                      # inp2_st_id
NUM_CH_LOG2 = 0                      # inp2_num_ch (log2 #chan; 0 => 1 channel)
THREAD_ID   = 0                      # inp2_th_id

# --- requant control (runtime software registers; defaults = adaptive Van Vleck) ---
REQUANT_ALPHA_SHIFT     = 12         # EMA smoothing shift for the RMS estimator
REQUANT_THRESH_OVERRIDE = 0          # manual 2-bit threshold magnitude (used iff override=1)
REQUANT_USE_OVERRIDE    = 0          # 0 = adaptive thresholds (normal); 1 = manual
SHOW_REQUANT_POWER      = True       # include rq2_pow_{i,q} readback in the status line

# --- recorder: NVMe mounts (filled sequentially) and CPU-core pinning ---
DISKS    = '/mnt/vlbi0,/mnt/vlbi1,/mnt/vlbi2'
CAP_CORE = 2                         # capture / fragment-reassembly thread
WR_CORE  = 3                         # writer thread
IRQ_CORE = 1                         # NIC IRQ / NAPI core

# --- python interpreter that has casperfpga (used by record.sh) ---
CFPGA_PY = 'python3'                 # e.g. /home/USER/casper/cfpga_venv/bin/python

# --- monitor ---
STATUS_PERIOD = 1.0                  # seconds between live status lines
