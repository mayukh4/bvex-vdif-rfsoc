function adaptive_requantizer_config(this_block)
  % CASPER/System Generator Black Box config for adaptive_requantizer.vhd
  % Mirrors the proven wizard-generated pattern (see dynamic_requantizer_config):
  % clk/ce registered only after rates are known (deferred), using the DETECTED
  % rate -- never a hardcoded rate during copy/load (avoids the addClkCEInternal
  % "no model settings for domain" error).

  this_block.setTopLevelLanguage('VHDL');
  this_block.setEntityName('adaptive_requantizer');

  this_block.tagAsCombinational;

  % --- Simulink ports (every entity port except clk/ce) ---
  this_block.addSimulinkInport('rst');
  this_block.addSimulinkInport('data_in');
  this_block.addSimulinkInport('thresh_upper');
  this_block.addSimulinkInport('thresh_lower');

  this_block.addSimulinkOutport('data_out');

  % --- Output type (declared; nothing upstream defines it) ---
  this_block.port('data_out').setType('UFix_16_0');

  % -----------------------------
  if (this_block.inputTypesKnown)
    this_block.port('rst').useHDLVector(false);

    if (this_block.port('rst').width ~= 1)
      this_block.setError('Input data type for port "rst" must have width=1.');
    end
    if (this_block.port('data_in').width ~= 128)
      this_block.setError('Input data type for port "data_in" must have width=128.');
    end
    if (this_block.port('thresh_upper').width ~= 16)
      this_block.setError('Input data type for port "thresh_upper" must have width=16.');
    end
    if (this_block.port('thresh_lower').width ~= 16)
      this_block.setError('Input data type for port "thresh_lower" must have width=16.');
    end
  end  % if(inputTypesKnown)
  % -----------------------------

  % -----------------------------
  if (this_block.inputRatesKnown)
    setup_as_single_rate(this_block,'clk','ce')
  end  % if(inputRatesKnown)
  % -----------------------------

  this_block.addGeneric('C_S_AXI_DATA_WIDTH','integer','32');

  this_block.addFile('adaptive_requantizer.vhd');

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
