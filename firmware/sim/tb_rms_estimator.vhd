library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;
use std.env.all;

entity tb_rms_estimator is end tb_rms_estimator;

architecture sim of tb_rms_estimator is
  signal clk : std_logic := '0';
  signal ce  : std_logic := '1';
  signal rst : std_logic := '1';
  signal data_in : std_logic_vector(127 downto 0) := (others=>'0');
  signal thresh_upper, thresh_lower : std_logic_vector(15 downto 0);
  signal power_est : std_logic_vector(31 downto 0);
  signal alpha_shift : std_logic_vector(3 downto 0) := x"4";
  signal thresh_override : std_logic_vector(15 downto 0) := (others=>'0');
  signal use_override : std_logic := '0';
  constant TCK : time := 10 ns;
  signal done : boolean := false;
begin
  dut: entity work.rms_estimator
    port map(clk=>clk, ce=>ce, rst=>rst, data_in=>data_in,
             thresh_upper=>thresh_upper, thresh_lower=>thresh_lower,
             power_est=>power_est, alpha_shift=>alpha_shift,
             thresh_override=>thresh_override, use_override=>use_override);

  clkp: process begin
    while not done loop clk<='0'; wait for TCK/2; clk<='1'; wait for TCK/2; end loop;
    wait;
  end process;

  stim: process
    variable tu : integer;
    variable tl : integer;
  begin
    -- all 8 samples = +1000  -> abs_sum = 8000, steady mean_abs = 1000
    for i in 0 to 7 loop
      data_in(i*16+15 downto i*16) <= std_logic_vector(to_signed(1000,16));
    end loop;
    rst<='1'; wait for 5*TCK; rst<='0';

    -- let EMA converge (alpha=4 -> tau ~16 samples)
    wait for 700*TCK;
    tu := to_integer(signed(thresh_upper));
    tl := to_integer(signed(thresh_lower));
    report "TB_RMS adaptive: thresh_upper=" & integer'image(tu) &
           " thresh_lower=" & integer'image(tl);
    -- Van Vleck scale fix: thr = m_abs*1.23047 = 1000+250-15-3 = 1232 (0.9817 sigma)
    -- (was 1.21875 -> 1219 = 0.9725 sigma, ~1% low)
    assert (tu >= 1228 and tu <= 1236)
      report "TB_RMS FAIL: thresh_upper out of range (expect ~1232)" severity error;
    assert (tl <= -1228 and tl >= -1236)
      report "TB_RMS FAIL: thresh_lower out of range (expect ~-1232)" severity error;

    -- override mode
    use_override<='1'; thresh_override<=std_logic_vector(to_signed(400,16));
    wait for 10*TCK;
    tu := to_integer(signed(thresh_upper));
    tl := to_integer(signed(thresh_lower));
    report "TB_RMS override: thresh_upper=" & integer'image(tu) &
           " thresh_lower=" & integer'image(tl);
    assert (tu = 400)  report "TB_RMS FAIL: override upper /= 400" severity error;
    assert (tl = -400) report "TB_RMS FAIL: override lower /= -400" severity error;

    report "TB_RMS COMPLETE";
    done<=true;
    stop;
    wait;
  end process;
end sim;
