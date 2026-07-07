#!/usr/bin/env python3
"""Parse DiFX SWIN output (bvex_1.difx/DIFX_*) and validate the zero-baseline result.

Expectations for identical-data zero-baseline (BV, BW = same file):
  * autocorr (baseline 257=BV*BV, 514=BW*BW): real, positive, sane noise bandpass
  * cross (258=BV*BW): amplitude == autocorr level (correlation coefficient ~1), phase ~0
"""
import glob, struct, sys
import numpy as np

SYNC = 0xFF00FF00
files = sorted(glob.glob("bvex_1.difx/DIFX_*"))
if not files:
    sys.exit("no DIFX_ output found")

recs = {}
for fn in files:
    buf = open(fn, "rb").read()
    off = 0
    while off + 74 <= len(buf):
        sync, ver = struct.unpack_from("<II", buf, off)
        if sync != SYNC:
            off += 1
            continue
        (bl, mjd) = struct.unpack_from("<ii", buf, off + 8)
        (sec,) = struct.unpack_from("<d", buf, off + 16)
        (cfg, src, freq) = struct.unpack_from("<iii", buf, off + 24)
        pol = buf[off + 36:off + 38].decode()
        (psrbin,) = struct.unpack_from("<i", buf, off + 38)
        (weight,) = struct.unpack_from("<d", buf, off + 42)
        # uvw 3 doubles then vis data
        vis_off = off + 42 + 8 + 24
        # find nchan: scan to next sync or EOF
        nxt = buf.find(struct.pack("<I", SYNC), vis_off)
        end = nxt if nxt != -1 else len(buf)
        nchan = (end - vis_off) // 8
        v = np.frombuffer(buf, dtype=np.complex64, count=nchan, offset=vis_off)
        recs.setdefault((bl, pol), []).append((mjd, sec, weight, v.copy()))
        off = end

print("records by (baseline, pol):")
for k in sorted(recs):
    print("  bl=%d pol=%s : %d integrations, nchan=%d" % (k[0], k[1], len(recs[k]), recs[k][0][3].size))

def avg(k):
    return np.mean([r[3] for r in recs[k]], axis=0)

ac1 = ac2 = xc = None
for (bl, pol) in recs:
    if bl == 257: ac1 = avg((bl, pol))
    elif bl == 514: ac2 = avg((bl, pol))
    elif bl == 258: xc = avg((bl, pol))

if ac1 is not None:
    a = np.abs(ac1)
    print("\nBV autocorr: mean=%.4g  std/mean=%.3f  min=%.4g  max=%.4g" % (a.mean(), a.std()/a.mean(), a.min(), a.max()))
    print("  imag/real ratio (should be ~0): %.4g" % (np.abs(ac1.imag).mean() / np.abs(ac1.real).mean()))
if ac2 is not None:
    a = np.abs(ac2)
    print("BW autocorr: mean=%.4g  std/mean=%.3f" % (a.mean(), a.std()/a.mean()))
if xc is not None and ac1 is not None and ac2 is not None:
    denom = np.sqrt(np.abs(ac1.real) * np.abs(ac2.real))
    coh = np.abs(xc) / np.where(denom > 0, denom, 1)
    ph = np.degrees(np.angle(xc))
    print("\nZERO-BASELINE cross-corr (BV x BW, same data):")
    print("  correlation coefficient: mean=%.4f  (expect ~1.0)" % coh.mean())
    print("  phase: mean=%.2f deg  rms=%.2f deg  (expect ~0)" % (ph.mean(), ph.std()))
    ok = coh.mean() > 0.95 and abs(ph.mean()) < 1 and ph.std() < 5
    print("  VERDICT: %s" % ("PASS - DiFX coherently correlates the data" if ok else "CHECK - see numbers above"))

# save spectra for the report
try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(1, 3 if xc is not None else 1, figsize=(14, 3.6))
    ax = np.atleast_1d(ax)
    fmhz = np.linspace(3072, 3072 + 1024, ac1.size, endpoint=False)
    ax[0].plot(fmhz, np.abs(ac1)); ax[0].set_title("BV autocorr bandpass"); ax[0].set_xlabel("MHz")
    if xc is not None:
        ax[1].plot(fmhz, coh); ax[1].set_ylim(0, 1.1); ax[1].set_title("zero-baseline corr coeff"); ax[1].set_xlabel("MHz")
        ax[2].plot(fmhz, ph, ".", ms=2); ax[2].set_ylim(-180, 180); ax[2].set_title("cross phase (deg)"); ax[2].set_xlabel("MHz")
    plt.tight_layout(); plt.savefig("difx_zerobaseline.png", dpi=110)
    print("\nsaved difx_zerobaseline.png")
except Exception as e:
    print("plot skipped:", e)
