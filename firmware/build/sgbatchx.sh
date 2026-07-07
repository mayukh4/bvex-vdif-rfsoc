#!/bin/bash
# Display-backed CASPER/System Generator MATLAB launcher (via Xvfb).
# Design-entry (black-box config, addClkCEPair) needs a real display; -nodisplay fails.
# Usage: sgbatchx.sh <workdir> <full_path_to_target.m>
set -u
WORKDIR="${1:?need workdir}"
TARGET="${2:?need target .m full path}"
MLIB="${MLIB_DEVEL_PATH:-$HOME/mlib_devel}"

cd "$MLIB"
# shellcheck disable=SC1091
source ./startsg.local
if [ -f "$COMPOSER_PATH/settings64.sh" ]; then
  source "$COMPOSER_PATH/settings64.sh" >/dev/null 2>&1 || true
fi
export MLIB_DEVEL_PATH="$MLIB"
export CASPER_STARTUP_DIR="$WORKDIR"
export PATH="$MATLAB_PATH/bin:$PATH"
export LD_LIBRARY_PATH="${HOME}/.XILINX_MATLAB_RUNTIME/XMC_2021.1_R2021a/Ubuntu/16:/tools/Xilinx/Model_Composer/2021.1/lib/lnx64.o/Ubuntu:/tools/Xilinx/Model_Composer/2021.1/lib/lnx64.o:/tools/Xilinx/Vivado/2021.1/lib/lnx64.o:/tools/Xilinx/Vivado/2021.1/lib/lnx64.o/Default:${LD_LIBRARY_PATH:-}"

MC_SETUP="$WORKDIR/mc_setup.m"
# -nodesktop with a virtual display (NOT -nodisplay) so SysGen's GUI/clock context is active.
exec xvfb-run -a -s "-screen 0 1600x1200x24" \
  matlab -nodesktop -nosplash -r \
  "try, run('$MC_SETUP'); cd('$WORKDIR'); run('$TARGET'); catch e, disp(['FATAL: ' e.message]); end; exit"
