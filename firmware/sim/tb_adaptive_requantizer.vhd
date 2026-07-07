library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;
use std.env.all;

entity tb_adaptive_requantizer is end tb_adaptive_requantizer;

architecture sim of tb_adaptive_requantizer is
  signal clk : std_logic := '0';
  signal ce  : std_logic := '1';
  signal rst : std_logic := '1';
  signal data_in : std_logic_vector(127 downto 0) := (others=>'0');
  signal thresh_upper : std_logic_vector(15 downto 0) := std_logic_vector(to_signed(1000,16));
  signal thresh_lower : std_logic_vector(15 downto 0) := std_logic_vector(to_signed(-1000,16));
  signal data_out : std_logic_vector(15 downto 0);
  constant TCK : time := 10 ns;
  signal done : boolean := false;

  -- sample pattern per index 0..7: 2000,500,-500,-2000 repeated
  type iarr is array(0 to 7) of integer;
  constant SAMP : iarr := (2000,500,-500,-2000,2000,500,-500,-2000);
  -- expected 2-bit codes: 2000>1000 ->11 ; 500>=0 ->10 ; -500>-1000 ->01 ; -2000 ->00
  -- per sample i bits[2i+1:2i]; pattern -> 0x1B1B
begin
  dut: entity work.adaptive_requantizer
    port map(clk=>clk, ce=>ce, rst=>rst, data_in=>data_in,
             thresh_upper=>thresh_upper, thresh_lower=>thresh_lower, data_out=>data_out);

  clkp: process begin
    while not done loop clk<='0'; wait for TCK/2; clk<='1'; wait for TCK/2; end loop;
    wait;
  end process;

  stim: process
  begin
    for i in 0 to 7 loop
      data_in(i*16+15 downto i*16) <= std_logic_vector(to_signed(SAMP(i),16));
    end loop;
    rst<='1'; wait for 5*TCK; rst<='0';
    -- 2-cycle internal latency; wait a few clocks
    wait for 6*TCK;
    report "TB_REQ data_out=0x" & to_hstring(data_out);
    assert (data_out = x"1B1B")
      report "TB_REQ FAIL: expected 0x1B1B got 0x" & to_hstring(data_out) severity error;

    report "TB_REQ COMPLETE";
    done<=true;
    stop;
    wait;
  end process;
end sim;
