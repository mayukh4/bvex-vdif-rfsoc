% mc_setup.m -- bootstrap Model Composer / System Generator + CASPER libs for headless batch.
% Mirrors what `model_composer` (xmcStart) + mlib_devel/startup.m do interactively.
disp('=== mc_setup: adding Model Composer / SysGen paths ===');
addpath('/tools/Xilinx/Model_Composer/2021.1/lib/lnx64.o/Ubuntu');
addpath('/tools/Xilinx/Model_Composer/2021.1/lib/lnx64.o');
addpath('/tools/Xilinx/Model_Composer/2021.1/simulink');
addpath('/tools/Xilinx/Model_Composer/2021.1/doc/help/xmc');
addpath('/tools/Xilinx/Vivado/2021.1/lib/lnx64.o/matlab');
addpath('/tools/Xilinx/Vivado/2021.1/scripts/sysgen/matlab');
addpath('/tools/Xilinx/Vivado/2021.1/scripts/sysgen/matlab/plugins/compilation');
javaaddpath('/tools/Xilinx/Vivado/2021.1/lib/classes/sysgen.jar');
try
    xmcStart;
    disp('=== mc_setup: xmcStart OK ===');
catch e
    disp(['=== mc_setup: xmcStart FAILED: ' e.message]);
end
% Run the CASPER startup.m (adds casper_library/xps_library/jasper_library, load_system libs)
setenv('CASPER_STARTUP_DIR', pwd);
old = pwd;
mlib = getenv('MLIB_DEVEL_PATH');
if isempty(mlib), mlib = fullfile(getenv('HOME'), 'mlib_devel'); end
cd(mlib);
try
    startup;
    disp('=== mc_setup: casper startup OK ===');
catch e
    disp(['=== mc_setup: casper startup FAILED: ' e.message]);
end
cd(old);
disp('=== mc_setup: done ===');
