function rms_estimator_config(this_block)
  % CASPER/System Generator Black Box config for rms_estimator.vhd
  % Mirrors the proven wizard-generated pattern (see dynamic_requantizer_config):
  % clk/ce are registered ONLY after input rates are known, using the DETECTED
  % rate -- NOT a hardcoded rate during the copy/load callback (which causes
  % "addClkCEInternal: Tried to add clock with no model settings for domain").

  this_block.setTopLevelLanguage('VHDL');
  this_block.setEntityName('rms_estimator');

  % SysGen assumes a combinational feed-through for black boxes; harmless here.
  this_block.tagAsCombinational;

  % --- Simulink ports (every entity port except clk/ce) ---
  this_block.addSimulinkInport('rst');
  this_block.addSimulinkInport('data_in');
  this_block.addSimulinkInport('alpha_shift');
  this_block.addSimulinkInport('thresh_override');
  this_block.addSimulinkInport('use_override');

  this_block.addSimulinkOutport('thresh_upper');
  this_block.addSimulinkOutport('thresh_lower');
  this_block.addSimulinkOutport('power_est');

  % --- Output types (must be declared; nothing upstream defines them) ---
  this_block.port('thresh_upper').setType('UFix_16_0');
  this_block.port('thresh_lower').setType('UFix_16_0');
  this_block.port('power_est').setType('UFix_32_0');

  % -----------------------------
  if (this_block.inputTypesKnown)
    % std_logic (single-bit) inputs must NOT be treated as 1-element vectors.
    this_block.port('rst').useHDLVector(false);
    this_block.port('use_override').useHDLVector(false);

    if (this_block.port('rst').width ~= 1)
      this_block.setError('Input data type for port "rst" must have width=1.');
    end
    if (this_block.port('data_in').width ~= 128)
      this_block.setError('Input data type for port "data_in" must have width=128.');
    end
    if (this_block.port('alpha_shift').width ~= 4)
      this_block.setError('Input data type for port "alpha_shift" must have width=4.');
    end
    if (this_block.port('thresh_override').width ~= 16)
      this_block.setError('Input data type for port "thresh_override" must have width=16.');
    end
    if (this_block.port('use_override').width ~= 1)
      this_block.setError('Input data type for port "use_override" must have width=1.');
    end
  end  % if(inputTypesKnown)
  % -----------------------------

  % -----------------------------
  if (this_block.inputRatesKnown)
    setup_as_single_rate(this_block,'clk','ce')
  end  % if(inputRatesKnown)
  % -----------------------------

  this_block.addGeneric('C_S_AXI_DATA_WIDTH','integer','32');

  this_block.addFile('rms_estimator.vhd');

return;

% ------------------------------------------------------------
function setup_as_single_rate(block,clkname,cename)
  inputRates = block.inputRates;
  uniqueInputRates = unique(inputRates);
  if (length(uniqueInputRates)==1 & uniqueInputRates(1)==Inf)
    block.addError('The inputs to this block cannot all be constant.');
    return;
  end
  if (uniqueInputRates(end) == Inf)
     hasConstantInput = true;
     uniqueInputRates = uniqueInputRates(1:end-1);
  end
  if (length(uniqueInputRates) ~= 1)
    block.addError('The inputs to this block must run at a single rate.');
    return;
  end
  theInputRate = uniqueInputRates(1);
  for i = 1:block.numSimulinkOutports
     block.outport(i).setRate(theInputRate);
  end
  block.addClkCEPair(clkname,cename,theInputRate);
  return;
% ------------------------------------------------------------
