#!/usr/bin/env python3
"""Generate bvex.vex / bvex.v2d / bvex.filelist for the mmBVEX autocorr test (run in ~/bvex_vdif)."""
import math, os

# Queen's/Kingston approx geodetic -> ECEF (fine for a noise autocorrelation)
lat, lon, h = math.radians(44.2253), math.radians(-76.4951), 100.0
a, f = 6378137.0, 1 / 298.257223563
e2 = f * (2 - f)
N = a / math.sqrt(1 - e2 * math.sin(lat) ** 2)
X = (N + h) * math.cos(lat) * math.cos(lon)
Y = (N + h) * math.cos(lat) * math.sin(lon)
Z = (N * (1 - e2) + h) * math.sin(lat)

MJD0 = 61217
T0 = "2026y177d20h05m07s"  # recording start, m5time-confirmed
start_mjd = MJD0 + (20 * 3600 + 5 * 60 + 7) / 86400.0
stop_mjd = MJD0 + (20 * 3600 + 5 * 60 + 10) / 86400.0

vex = f"""VEX_rev = 1.5;
$GLOBAL;
    ref $EXPER = BVEX01;
    ref $EOP = EOP61217;
$EXPER;
  def BVEX01;
    exper_name = BVEX01;
    exper_description = "mmBVEX 2-bit complex VDIF autocorrelation test";
    PI_name = "Mayukh Bagchi";
    target_correlator = DiFX;
  enddef;
$MODE;
  def BVEXMODE;
    ref $FREQ = FREQ2048 :Bv;
    ref $BBC = BBC01 :Bv;
    ref $IF = IF01 :Bv;
    ref $TRACKS = VDIFD_FMT :Bv;
  enddef;
$STATION;
  def Bv;
    ref $SITE = KINGSTON;
    ref $ANTENNA = BV_ANT;
    ref $DAS = RFSOC;
  enddef;
$ANTENNA;
  def BV_ANT;
    axis_type = az : el;
    antenna_motion = az : 30.0 deg/min : 2 sec;
    antenna_motion = el : 30.0 deg/min : 2 sec;
    axis_offset = 0.0 m;
  enddef;
$DAS;
  def RFSOC;
    record_transport_type = File;
    electronics_rack_type = none;
    number_drives = 1;
    headstack = 1 : : 0;
    tape_motion = adaptive : 0 min : 0 min : 0 sec;
  enddef;
$SITE;
  def KINGSTON;
    site_type = fixed;
    site_name = KINGSTON;
    site_ID = Bv;
    site_position = {X:14.3f} m : {Y:14.3f} m : {Z:14.3f} m;
    occupation_code = 00000000;
  enddef;
$SOURCE;
  def 3C273;
    source_name = 3C273;
    ra = 12h29m06.6997s; dec = 02d03'08.598"; ref_coord_frame = J2000;
  enddef;
$FREQ;
  def FREQ2048;
    chan_def = : 3072.00 MHz : U : 1024.00 MHz : &CH01 : &BBC01 : &NOCAL;
    sample_rate = 2048.0 Ms/sec;
  enddef;
$BBC;
  def BBC01;
    BBC_assign = &BBC01 : 1 : &IF_A;
  enddef;
$IF;
  def IF01;
    if_def = &IF_A : A : R : 2048.0 MHz : U;
  enddef;
$TRACKS;
  def VDIFD_FMT;
    track_frame_format = VDIFD/8032/2;
  enddef;
$SCHED;
  scan No0001;
    start = {T0};
    mode = BVEXMODE;
    source = 3C273;
    station = Bv : 0 sec : 3 sec : 0 GB : : : 1;
  endscan;
$EOP;
  def EOP61217;
    TAI-UTC = 37 sec;
    A1-TAI = 0.0 sec;
    eop_ref_epoch = 2026y175d00h00m00s;
    num_eop_points = 5;
    eop_interval = 24 hr;
    ut1-utc = 0.0 sec : 0.0 sec : 0.0 sec : 0.0 sec : 0.0 sec;
    x_wobble = 0.0 asec : 0.0 asec : 0.0 asec : 0.0 asec : 0.0 asec;
    y_wobble = 0.0 asec : 0.0 asec : 0.0 asec : 0.0 asec : 0.0 asec;
  enddef;
$CLOCK;
  def Bv;
    clock_early = {T0} : 0.0 usec : {T0} : 0.0;
  enddef;
"""
with open("bvex.vex", "w") as fh:
    fh.write(vex)

vpath = os.path.abspath("bvex_20260626t200507Z_th0.vdif")
with open("bvex.filelist", "w") as fh:
    fh.write("%s %.9f %.9f\n" % (vpath, start_mjd, stop_mjd))

v2d = f"""vex = bvex.vex
mjdStart = {start_mjd:.9f}
mjdStop  = {stop_mjd:.9f}
minLength = 1
minSubarray = 1
singleScan = True

ANTENNA Bv
{{
    filelist = bvex.filelist
    format = VDIFD/8032/2
    phaseCalInt = 0
    toneSelection = none
}}

SETUP default
{{
    tInt = 1.0
    FFTSpecRes = 4.0
    specRes = 4.0
}}
"""
with open("bvex.v2d", "w") as fh:
    fh.write(v2d)

print("wrote bvex.vex bvex.v2d bvex.filelist")
print("station XYZ = %.3f %.3f %.3f" % (X, Y, Z))
