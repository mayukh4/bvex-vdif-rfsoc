#!/usr/bin/env python3
"""analyze_pcap.py <capture.pcap> -- offline analysis of a recorded VDIF capture.

Reads the pcap (Python is fine OFFLINE -- it's not in the live data path), classifies
every frame, and reports: on-wire size histogram (trustworthy since tcpdump never
truncates), Ethertype mix, per-thread VDIF counts (definitively answers 'is inp1/
thread-0 streaming?'), per-second frame# progression + max (tests the 192-B-short
theory: max frame# ~ 128000*125/122 = 131148), declared frame_length vs actual UDP
payload, and the 2-bit Van Vleck level distribution. Also extracts a clean .vdif.
"""
import sys, struct, collections

pcap = sys.argv[1]
VDIF_OUT = sys.argv[2] if len(sys.argv) > 2 else None     # optional .vdif extract
VDIF_CAP = 200000                                          # cap frames written to .vdif

f = open(pcap, 'rb')
magic = f.read(4)
if magic in (b'\xd4\xc3\xb2\xa1', b'\x4d\x3c\xb2\xa1'):
    en = '<'
elif magic in (b'\xa1\xb2\xc3\xd4', b'\xa1\xb2\x3c\x4d'):
    en = '>'
else:
    print('not a pcap (magic %r)' % magic); sys.exit(1)
f.read(20)                                                 # rest of global header

size_hist = collections.Counter()
etype_hist = collections.Counter()
payload_len = collections.Counter()
flen_field = collections.Counter()
dt_set = collections.Counter()
bps_set = collections.Counter()
thread_cnt = collections.Counter()
persec = collections.defaultdict(lambda: collections.defaultdict(lambda: [1 << 30, -1, 0]))  # thread->sec->[min,max,count]
total = valid = nonip = 0
levels = collections.Counter(); lvl_n = 0
vdif_fh = open(VDIF_OUT, 'wb') if VDIF_OUT else None
vdif_written = 0

while True:
    hdr = f.read(16)
    if len(hdr) < 16:
        break
    _, _, incl, orig = struct.unpack(en + 'IIII', hdr)
    data = f.read(incl)
    if len(data) < incl:
        break
    total += 1
    size_hist[orig] += 1
    if len(data) < 42:
        nonip += 1
        continue
    etype = struct.unpack('>H', data[12:14])[0]
    etype_hist[etype] += 1
    # require IPv4 + UDP + dst port 4001 to call it a real VDIF frame
    if etype != 0x0800 or data[23] != 17:
        nonip += 1
        continue
    dport = struct.unpack('>H', data[36:38])[0]
    if dport != 4001:
        nonip += 1
        continue
    w0, w1, w2, w3 = struct.unpack_from('<IIII', data, 42)
    th = (w3 >> 16) & 0x3FF
    sec = w0 & 0x3FFFFFFF
    fn = w1 & 0xFFFFFF
    valid += 1
    thread_cnt[th] += 1
    payload_len[orig - 42] += 1
    flen_field[(w2 & 0xFFFFFF) * 8] += 1
    dt_set[(w3 >> 31) & 1] += 1
    bps_set[(w3 >> 26) & 0x1F] += 1
    rec = persec[th][sec]
    if fn < rec[0]:
        rec[0] = fn
    if fn > rec[1]:
        rec[1] = fn
    rec[2] += 1
    if lvl_n < 1600000 and valid <= 1000:          # Van Vleck on first 1000 valid frames
        for b in data[74:]:
            levels[b & 3] += 1; levels[(b >> 2) & 3] += 1
            levels[(b >> 4) & 3] += 1; levels[(b >> 6) & 3] += 1; lvl_n += 4
    if vdif_fh and vdif_written < VDIF_CAP:
        vdif_fh.write(data[42:])                    # the VDIF frame = UDP payload
        vdif_written += 1

if vdif_fh:
    vdif_fh.close()

print('=== capture summary: %s ===' % pcap)
print('total frames        : %d' % total)
print('valid VDIF (UDP:4001): %d' % valid)
print('non-IP/UDP/other     : %d' % nonip)
print('\non-wire size histogram (bytes:count, top 8):')
for sz, c in size_hist.most_common(8):
    print('   %6d : %d' % (sz, c))
print('\nEthertype histogram (top 6): %s' %
      {('0x%04x' % k): v for k, v in etype_hist.most_common(6)})
print('\nVALID-frame fields:')
print('   UDP payload bytes : %s' % dict(payload_len.most_common(5)))
print('   frame_length field: %s' % dict(flen_field.most_common(5)))
print('   data_type         : %s' % dict(dt_set))
print('   bps-1             : %s' % dict(bps_set))
print('\nper-thread VDIF counts: %s' % dict(thread_cnt.most_common(10)))
print('  (inp1 => thread 0, inp2 => thread 1)')

for th in sorted(persec):
    if thread_cnt[th] < 50:               # skip junk threads from any stray malformed frames
        continue
    secs = sorted(persec[th])
    print('\nthread %d: %d seconds seen %s' % (th, len(secs), secs[:6]))
    for s in secs:
        mn, mx, c = persec[th][s]
        print('   sec %d: %d frames, frame# %d..%d (span %d; expect max ~131148 if 192B-short)'
              % (s, c, mn, mx, mx - mn + 1))

print('\n2-bit symbol-level distribution (first 1000 valid frames):')
if lvl_n:
    for lv in range(4):
        print('   level %d: %6.2f %%' % (lv, 100.0 * levels[lv] / lvl_n))
    print('   (ideal Van Vleck ~ 16.4 / 33.6 / 33.6 / 16.4 %)')
if VDIF_OUT:
    print('\nwrote %d VDIF frames -> %s' % (vdif_written, VDIF_OUT))
print('\nanalysis complete')
