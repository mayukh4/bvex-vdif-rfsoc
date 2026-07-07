function sample_packer512_config(this_block)
  % CASPER/System Generator Black Box config for sample_packer512.vhd
  % Wizard-pattern (deferred clk/ce). Output is 512-bit (4 collected 128-bit words),
  % data_valid_out pulses 1-in-16 clocks.

  this_block.setTopLevelLanguage('VHDL');
  this_block.setEntityName('sample_packer512');

  this_block.tagAsCombinational;

  this_block.addSimulinkInport('rst');
  this_block.addSimulinkInport('data_in_i');
  this_block.addSimulinkInport('data_in_q');

  this_block.addSimulinkOutport('data_out');
  this_block.addSimulinkOutport('data_valid_out');

  this_block.port('data_out').setType('UFix_512_0');
  this_block.port('data_valid_out').setType('UFix_1_0');
  this_block.port('data_valid_out').useHDLVector(false);

  if (this_block.inputTypesKnown)
    this_block.port('rst').useHDLVector(false);
    if (this_block.port('rst').width ~= 1)
      this_block.setError('Input data type for port "rst" must have width=1.');
    end
    if (this_block.port('data_in_i').width ~= 16)
      this_block.setError('Input data type for port "data_in_i" must have width=16.');
    end
    if (this_block.port('data_in_q').width ~= 16)
      this_block.setError('Input data type for port "data_in_q" must have width=16.');
    end
  end  % if(inputTypesKnown)

  if (this_block.inputRatesKnown)
    setup_as_single_rate(this_block,'clk','ce')
  end  % if(inputRatesKnown)

  this_block.addGeneric('C_S_AXI_DATA_WIDTH','integer','32');

  this_block.addFile('sample_packer512.vhd');

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
