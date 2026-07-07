% build_frontend.m -- run the CASPER jasper frontend (Simulink compile + SysGen
% HDL netlist generation + Vivado project). Validates the design is build-ready
% (this is the gating step the full build runs before the long Vivado P&R).
warning off all;
W=getenv('WORKDIR'); if isempty(W), W=pwd; end; addpath(W);
MDL='rfsoc_vdif_v0_4_dev';
R='/tmp/build_frontend.txt'; fid=fopen(R,'w'); pr=@(varargin)fprintf(fid,varargin{:});
pr('starting jasper_frontend for %s ...\n', MDL);
try
  bc = jasper_frontend([W '/' MDL '.slx']);
  pr('\nJASPER_FRONTEND OK\n');
  pr('backend build cmd: %s\n', bc);
catch e
  pr('\nJASPER_FRONTEND ERROR: %s\n', e.message);
  for i=1:numel(e.stack), pr('   at %s line %d\n', e.stack(i).name, e.stack(i).line); end
end
pr('\nBUILD_FRONTEND DONE\n'); fclose(fid); disp('BUILD_FRONTEND DONE'); type(R);
