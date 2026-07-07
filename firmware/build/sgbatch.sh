#!/bin/bash
# Headless CASPER/System Generator MATLAB batch launcher.
# Usage: sgbatch.sh <workdir> <full_path_to_target.m>
# Replicates the startsg + model_composer environment, then runs:
#   run(mc_setup.m);  run(<target.m>)
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

# System Generator (xmcStart) needs the Xilinx MATLAB runtime + Model Composer/Vivado libs
# on LD_LIBRARY_PATH (settings64.sh alone misses the XMC runtime -> libfourier.so not found).
export LD_LIBRARY_PATH="${HOME}/.XILINX_MATLAB_RUNTIME/XMC_2021.1_R2021a/Ubuntu/16:/tools/Xilinx/Model_Composer/2021.1/lib/lnx64.o/Ubuntu:/tools/Xilinx/Model_Composer/2021.1/lib/lnx64.o:/tools/Xilinx/Vivado/2021.1/lib/lnx64.o:/tools/Xilinx/Vivado/2021.1/lib/lnx64.o/Default:${LD_LIBRARY_PATH:-}"

MC_SETUP="$WORKDIR/mc_setup.m"
exec matlab -nodisplay -nosplash -batch \
  "run('$MC_SETUP'); cd('$WORKDIR'); run('$TARGET')"
