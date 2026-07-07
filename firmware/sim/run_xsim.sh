#!/bin/bash
# Compile + simulate the four custom VHDL blocks with Vivado xsim (license-free).
source /tools/Xilinx/Vivado/2021.1/settings64.sh >/dev/null 2>&1
cd /tmp/vdif_tb || { echo "no test dir"; exit 1; }
rm -rf xsim.dir *.log *.jou *.pb webtalk* .Xil 2>/dev/null
PASS=0; FAIL=0

run_one () {
  u=$1
  echo "==================== $u ===================="
  xvhdl -2008 ${u}.vhd tb_${u}.vhd > ${u}_compile.log 2>&1
  if grep -qE "ERROR:" ${u}_compile.log; then
    echo "COMPILE ERROR:"; grep -E "ERROR:" ${u}_compile.log | head; FAIL=$((FAIL+1)); return
  fi
  xelab tb_${u} -s sim_${u} > ${u}_elab.log 2>&1
  if grep -qE "ERROR:|Fatal" ${u}_elab.log; then
    echo "ELAB ERROR:"; grep -E "ERROR:|Fatal" ${u}_elab.log | head; FAIL=$((FAIL+1)); return
  fi
  xsim sim_${u} -R > ${u}_sim.log 2>&1
  echo "--- sim output ---"
  grep -iE "TB_|FAIL|COMPLETE" ${u}_sim.log
  if grep -qi "FAIL" ${u}_sim.log; then
    echo "RESULT: FAIL"; FAIL=$((FAIL+1))
  elif grep -qi "COMPLETE" ${u}_sim.log; then
    echo "RESULT: PASS"; PASS=$((PASS+1))
  else
    echo "RESULT: UNKNOWN (no COMPLETE marker)"; FAIL=$((FAIL+1))
  fi
}

for u in rms_estimator adaptive_requantizer sample_packer frame_counter; do
  run_one "$u"
done
echo "==================== SUMMARY: PASS=$PASS FAIL=$FAIL ===================="
