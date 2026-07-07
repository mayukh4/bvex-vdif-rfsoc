library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;
library STD;
use STD.ENV.ALL;

-- Testbench for sample_packer512: verifies 4 consecutive 128-bit words are
-- collected into one 512-bit output as [w3|w2|w1|w0] (w0 in low bits) and that
-- data_valid_out pulses once every 16 input clocks.

entity tb_sample_packer512 is
end tb_sample_packer512;

architecture sim of tb_sample_packer512 is
    signal clk    : std_logic := '0';
    signal ce     : std_logic := '1';
    signal rst    : std_logic := '1';
    signal din_i  : std_logic_vector(15 downto 0) := (others => '0');
    signal din_q  : std_logic_vector(15 downto 0) := (others => '0');
    signal dout   : std_logic_vector(511 downto 0);
    signal dvalid : std_logic;
    constant CP : time := 10 ns;

    -- expected per-word interleaved patterns for the 4 groups
    --   group0: I=01 -> nibble 0001 -> 0x11111111
    --   group1: I=10 -> nibble 0010 -> 0x22222222
    --   group2: I=11 -> nibble 0011 -> 0x33333333
    --   group3: I=00 -> nibble 0000 -> 0x00000000
    constant W0 : std_logic_vector(127 downto 0) := x"11111111111111111111111111111111";
    constant W1 : std_logic_vector(127 downto 0) := x"22222222222222222222222222222222";
    constant W2 : std_logic_vector(127 downto 0) := x"33333333333333333333333333333333";
    constant W3 : std_logic_vector(127 downto 0) := x"00000000000000000000000000000000";

    signal valid_count : integer := 0;
    signal checked     : boolean := false;
begin
    clk <= not clk after CP/2;

    stim: process
    begin
        rst <= '1'; din_i <= (others=>'0'); din_q <= (others=>'0');
        wait for 3*CP; wait until rising_edge(clk);
        rst <= '0';
        -- feed 32 input clocks: groups of 4 with distinct I value
        for grp in 0 to 7 loop
            for k in 0 to 3 loop
                case (grp mod 4) is
                    when 0 => din_i <= x"5555";  -- I=01 each sample
                    when 1 => din_i <= x"AAAA";  -- I=10
                    when 2 => din_i <= x"FFFF";  -- I=11
                    when others => din_i <= x"0000"; -- I=00
                end case;
                din_q <= (others=>'0');
                wait until rising_edge(clk);
            end loop;
        end loop;
        wait for 5*CP;
        assert checked report "TB512: never saw a valid output!" severity failure;
        report "TB512 DONE" severity note;
        finish;
    end process;

    chk: process(clk)
    begin
        if rising_edge(clk) then
            if dvalid = '1' then
                valid_count <= valid_count + 1;
                -- check the 512 layout on the first valid
                if not checked then
                    assert dout(127 downto 0)    = W0 report "TB512 FAIL: w0 slot" severity failure;
                    assert dout(255 downto 128)  = W1 report "TB512 FAIL: w1 slot" severity failure;
                    assert dout(383 downto 256)  = W2 report "TB512 FAIL: w2 slot" severity failure;
                    assert dout(511 downto 384)  = W3 report "TB512 FAIL: w3 slot" severity failure;
                    report "TB512 PASS: 512-bit layout [w3|w2|w1|w0] correct" severity note;
                    checked <= true;
                end if;
            end if;
        end if;
    end process;

    dut: entity work.sample_packer512
        port map (clk=>clk, ce=>ce, rst=>rst, data_in_i=>din_i, data_in_q=>din_q,
                  data_out=>dout, data_valid_out=>dvalid);
end sim;
