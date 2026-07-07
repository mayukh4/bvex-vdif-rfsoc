-- sim_counter_stub.vhd : faithful behavioral stand-ins for the three Xilinx
-- c_counter_binary_v12_0 IP cores used by the framer read FSM (i3/i4/i5).
-- Per their .xci: UP counter, Increment=1, SINIT (synchronous init to 0), no
-- SCLR/SSET/LOAD/threshold.  Xilinx priority is SINIT > CE.  Widths: i3=64, i4=16,
-- i5=24.  Only the IP cores are substituted; the real xlcounter_free wrapper and
-- the real framer logic are used unchanged.

library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;

entity rfsoc_vdif_v0_4_dev_c_counter_binary_v12_0_i3 is
  port (clk : in std_logic; ce : in std_logic; SINIT : in std_logic;
        q : out std_logic_vector(63 downto 0));
end entity;
architecture beh of rfsoc_vdif_v0_4_dev_c_counter_binary_v12_0_i3 is
  signal cnt : unsigned(63 downto 0) := (others => '0');
begin
  process(clk) begin
    if rising_edge(clk) then
      if SINIT = '1' then cnt <= (others => '0');
      elsif ce = '1' then cnt <= cnt + 1; end if;
    end if;
  end process;
  q <= std_logic_vector(cnt);
end architecture;


library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;

entity rfsoc_vdif_v0_4_dev_c_counter_binary_v12_0_i4 is
  port (clk : in std_logic; ce : in std_logic; SINIT : in std_logic;
        q : out std_logic_vector(15 downto 0));
end entity;
architecture beh of rfsoc_vdif_v0_4_dev_c_counter_binary_v12_0_i4 is
  signal cnt : unsigned(15 downto 0) := (others => '0');
begin
  process(clk) begin
    if rising_edge(clk) then
      if SINIT = '1' then cnt <= (others => '0');
      elsif ce = '1' then cnt <= cnt + 1; end if;
    end if;
  end process;
  q <= std_logic_vector(cnt);
end architecture;


library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;

entity rfsoc_vdif_v0_4_dev_c_counter_binary_v12_0_i5 is
  port (clk : in std_logic; ce : in std_logic; SINIT : in std_logic;
        q : out std_logic_vector(23 downto 0));
end entity;
architecture beh of rfsoc_vdif_v0_4_dev_c_counter_binary_v12_0_i5 is
  signal cnt : unsigned(23 downto 0) := (others => '0');
begin
  process(clk) begin
    if rising_edge(clk) then
      if SINIT = '1' then cnt <= (others => '0');
      elsif ce = '1' then cnt <= cnt + 1; end if;
    end if;
  end process;
  q <= std_logic_vector(cnt);
end architecture;
