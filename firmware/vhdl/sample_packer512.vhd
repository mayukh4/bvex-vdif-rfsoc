library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;

-- 512-bit complex sample packer for VDIF I/Q data (rate-reduced 100GbE feed).
--
-- Identical interleave/packing convention to the verified 128-bit sample_packer,
-- but collects FOUR consecutive 128-bit words (w0,w1,w2,w3) into one 512-bit word
-- and emits it with data_valid_out pulsing once every 16 input clocks.
--
-- Output layout (clean, w0 in the low 128 bits):
--   data_out(127:0)   = w0   (oldest 128-bit word)
--   data_out(255:128) = w1
--   data_out(383:256) = w2
--   data_out(511:384) = w3   (newest 128-bit word)
-- Each w_k uses the EXACT composition of the verified 128-bit packer
--   ( pack_reg(95:0) & interleaved ), so per-word byte order is unchanged.
--
-- This lets inp1's existing round-robin/concat framer be fed by a parallel load
-- (dat1<-w0, dat2<-w1, dat3<-w2, dat4<-w3) instead of the per-clock round-robin,
-- giving the exact same frame structure at 1/4 the rate.

entity sample_packer512 is
    Generic (
        C_S_AXI_DATA_WIDTH : integer := 32
    );
    Port (
        clk            : in  std_logic;
        ce             : in  std_logic := '1';
        rst            : in  std_logic;
        data_in_i      : in  std_logic_vector(15 downto 0);  -- 8 x 2-bit I
        data_in_q      : in  std_logic_vector(15 downto 0);  -- 8 x 2-bit Q
        data_out       : out std_logic_vector(511 downto 0); -- 4 packed 128-bit words
        data_valid_out : out std_logic                       -- pulses 1-in-16 clocks
    );
end sample_packer512;

architecture Behavioral of sample_packer512 is

    signal interleaved  : std_logic_vector(31 downto 0);

    signal icnt         : unsigned(1 downto 0) := (others => '0');  -- inner 0..3 (32->128)
    signal ocnt         : unsigned(1 downto 0) := (others => '0');  -- outer 0..3 (128->512)

    signal pack_reg     : std_logic_vector(127 downto 0) := (others => '0'); -- inner accum
    signal collect_reg  : std_logic_vector(383 downto 0) := (others => '0'); -- holds w0,w1,w2

    signal data_out_reg : std_logic_vector(511 downto 0) := (others => '0');
    signal valid_reg    : std_logic := '0';

    attribute use_dsp : string;
    attribute use_dsp of Behavioral : architecture is "no";

begin

    -- Interleave I and Q exactly as the verified 128-bit packer
    gen_interleave: for i in 0 to 7 generate
        interleaved(i*4+1 downto i*4)   <= data_in_i(i*2+1 downto i*2);  -- I sample
        interleaved(i*4+3 downto i*4+2) <= data_in_q(i*2+1 downto i*2);  -- Q sample
    end generate;

    process(clk)
        variable word128 : std_logic_vector(127 downto 0);
    begin
        if rising_edge(clk) then
            if rst = '1' then
                icnt         <= (others => '0');
                ocnt         <= (others => '0');
                pack_reg     <= (others => '0');
                collect_reg  <= (others => '0');
                data_out_reg <= (others => '0');
                valid_reg    <= '0';
            elsif ce = '1' then
                valid_reg <= '0';

                -- inner accumulation (same as verified packer)
                pack_reg(to_integer(icnt)*32 + 31 downto to_integer(icnt)*32) <= interleaved;

                if icnt = 3 then
                    -- a 128-bit word completes this cycle (verified composition)
                    word128 := pack_reg(95 downto 0) & interleaved;
                    icnt <= (others => '0');

                    if ocnt = 3 then
                        -- 512-bit word complete: w0,w1,w2 in collect_reg, w3 = word128
                        data_out_reg <= word128 & collect_reg(383 downto 0);  -- [w3|w2|w1|w0]
                        valid_reg    <= '1';
                        ocnt         <= (others => '0');
                    else
                        -- store w_ocnt into collect_reg
                        collect_reg(to_integer(ocnt)*128 + 127 downto to_integer(ocnt)*128) <= word128;
                        ocnt <= ocnt + 1;
                    end if;
                else
                    icnt <= icnt + 1;
                end if;
            end if;
        end if;
    end process;

    data_out       <= data_out_reg;
    data_valid_out <= valid_reg;

end Behavioral;
