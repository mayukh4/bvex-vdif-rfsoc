#!/bin/bash
# veth_selftest.sh -- prove vdif_recorder works WITHOUT the FPGA.
# Creates a cross-namespace veth wire, blasts synthetic VDIF at full rate through it,
# records to the real XFS SSDs (O_DIRECT path), and verifies zero drops + byte-exact
# files + sidecars. Then a stop-on-disk-full check. Run as root (sudo).
set -u
WD="$(cd "$(dirname "$0")" && pwd)"    # run from this repo's host/ directory
cd "$WD" || exit 1
D0=/mnt/vlbi0/_rectest; D1=/mnt/vlbi1/_rectest
REC=/tmp/rec_selftest.json

cleanup(){
  pkill -f 'vdif_recorder --iface veth_rx' 2>/dev/null
  ip netns del ns_tx 2>/dev/null
  ip link del veth_rx 2>/dev/null
  rm -rf "$D0" "$D1"
}
trap cleanup EXIT
cleanup   # clean any prior state

# --- cross-namespace veth at MTU 9000 (a real wire AF_PACKET can see) ---
ip netns add ns_tx
ip link add veth_tx type veth peer name veth_rx
ip link set veth_tx netns ns_tx
ip netns exec ns_tx ip addr add 10.17.16.60/24 dev veth_tx
ip netns exec ns_tx ip link set veth_tx up mtu 9000
ip netns exec ns_tx ip link set lo up
ip addr add 10.17.16.1/24 dev veth_rx
ip link set veth_rx up mtu 9000
mkdir -p "$D0" "$D1"

echo "================ RUN A: 12 s full rate, NO gaps -> expect ZERO drops ================"
./vdif_recorder --iface veth_rx --dport 4001 --disks "$D0,$D1" \
   --secs-per-file 2 --no-align --status "$REC" > /tmp/recA.log 2>&1 &
RECPID=$!
sleep 1
ip netns exec ns_tx ./vdif_gen --dst 10.17.16.1 --port 4001 --secs 12 > /tmp/genA.log 2>&1
sleep 2
kill -INT $RECPID 2>/dev/null; wait $RECPID 2>/dev/null

echo "--- recorder final stats ---"; grep -E 'final|fps=' /tmp/recA.log | tail -4
echo "--- generator ---"; tail -1 /tmp/genA.log
echo "--- files ---"; ls -la "$D0" "$D1" 2>/dev/null | grep -E 'vdif$|partial$' | head
echo "--- byte-exactness + counters ---"
python3 - "$D0" "$D1" "$REC" <<'PY'
import os,sys,json,glob
dirs=sys.argv[1:3]; rec=sys.argv[3]
ok=True; nf=0; tot=0; gaps_sum=0
for d in dirs:
    for vf in sorted(glob.glob(d+'/*.vdif')):
        sc=vf+'.json'
        if not os.path.exists(sc): print("  NO SIDECAR:",vf); ok=False; continue
        m=json.load(open(sc)); sz=os.path.getsize(vf); exp=m['frame_count']*8032
        nf+=1; tot+=m['frame_count']
        good=(sz==exp==m['bytes'])
        ok=ok and good
        print("  %-34s frames=%-8d size=%-12d exp=%-12d %s"%(os.path.basename(vf),m['frame_count'],sz,exp,"OK" if good else "MISMATCH"))
parts=glob.glob(dirs[0]+'/*.partial')+glob.glob(dirs[1]+'/*.partial')
s=json.load(open(rec)) if os.path.exists(rec) else {}
print("  ---")
print("  files=%d  total_frames=%d  leftover .partial=%d"%(nf,tot,len(parts)))
print("  status.json: state=%s kernel_drops=%s spsc_drops=%s gap_frames=%s bad=%s frames_seen=%s frames_written=%s"
      %(s.get('state'),s.get('kernel_drops'),s.get('spsc_drops'),s.get('gap_frames'),
        s.get('bad_frames'),s.get('frames_seen'),s.get('frames_written')))
zero = (s.get('kernel_drops')==0 and s.get('spsc_drops')==0 and s.get('gap_frames')==0)
print("  ===> RUN A %s (byte-exact=%s, zero-drop=%s)"%("PASS" if (ok and zero and nf>0 and not parts) else "CHECK",ok,zero))
PY

echo ""
echo "================ RUN B: stop-on-disk-full (min-free huge) -> expect STOPPED_DISK_FULL ================"
rm -rf "$D0" "$D1"; mkdir -p "$D0" "$D1"
./vdif_recorder --iface veth_rx --dport 4001 --disks "$D0,$D1" \
   --min-free-gb 99999 --no-align --status "$REC" > /tmp/recB.log 2>&1 &
RECPID=$!
sleep 1
ip netns exec ns_tx ./vdif_gen --dst 10.17.16.1 --port 4001 --secs 3 > /dev/null 2>&1
sleep 1
if kill -0 $RECPID 2>/dev/null; then kill -INT $RECPID 2>/dev/null; fi
wait $RECPID 2>/dev/null
echo "--- status.json state + alarm ---"; grep -E '"state"|"alarm"' "$REC"
echo "--- recorder log ---"; grep -iE 'alarm|disk' /tmp/recB.log | tail -3
python3 - "$REC" <<'PY'
import sys,json,os
s=json.load(open(sys.argv[1]))
print("  ===> RUN B %s (state=%s)"%("PASS" if s.get('state')=='STOPPED_DISK_FULL' else "CHECK",s.get('state')))
PY
echo ""
echo "================ SELF-TEST DONE (teardown on exit) ================"
