library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;

-- Adaptive 2-bit requantizer for VDIF
-- Takes 8 x 16-bit signed samples and external thresholds from rms_estimator,
-- outputs 8 x 2-bit offset binary values (VDIF standard encoding).
-- Encoding: 00 = most negative, 01 = slightly negative,
--           10 = slightly positive, 11 = most positive
--
-- This block is instantiated once per I and once per Q stream.
-- The sample_packer handles the complex interleaving downstream.

entity adaptive_requantizer is
    Generic (
        C_S_AXI_DATA_WIDTH : integer := 32
    );
    Port (
        clk          : in  std_logic;
        ce           : in  std_logic := '1';
        rst          : in  std_logic;
        -- Input: 8 x 16-bit signed samples from RFDC (I or Q stream)
        data_in      : in  std_logic_vector(127 downto 0);
        -- Thresholds from rms_estimator (signed 16-bit)
        thresh_upper : in  std_logic_vector(15 downto 0);
        thresh_lower : in  std_logic_vector(15 downto 0);
        -- Output: 8 x 2-bit offset binary samples
        data_out     : out std_logic_vector(15 downto 0)
    );
end adaptive_requantizer;

architecture Behavioral of adaptive_requantizer is

    constant SAMPLE_WIDTH : integer := 16;
    constant NUM_SAMPLES  : integer := 8;

    -- Pipeline registers
    signal data_in_reg  : std_logic_vector(127 downto 0);
    signal data_out_reg : std_logic_vector(15 downto 0);
    signal thresh_u_reg : signed(SAMPLE_WIDTH-1 downto 0);
    signal thresh_l_reg : signed(SAMPLE_WIDTH-1 downto 0);

    -- Xilinx attributes
    attribute use_dsp : string;
    attribute use_dsp of Behavioral : architecture is "no";

begin

    process(clk)
        variable current_sample  : signed(SAMPLE_WIDTH-1 downto 0);
        variable quantized_value : std_logic_vector(1 downto 0);
    begin
        if rising_edge(clk) then
            if rst = '1' then
                data_out_reg <= (others => '0');
                data_in_reg  <= (others => '0');
                thresh_u_reg <= (others => '0');
                thresh_l_reg <= (others => '0');
            elsif ce = '1' then
                -- Register inputs
                data_in_reg  <= data_in;
                thresh_u_reg <= signed(thresh_upper);
                thresh_l_reg <= signed(thresh_lower);

                -- Quantize all 8 samples
                for i in 0 to NUM_SAMPLES-1 loop
                    current_sample := signed(
                        data_in_reg((i+1)*SAMPLE_WIDTH-1 downto i*SAMPLE_WIDTH));

                    -- 2-bit quantization with offset binary encoding (VDIF standard)
                    if current_sample > thresh_u_reg then
                        quantized_value := "11";  -- most positive
                    elsif current_sample >= 0 then
                        quantized_value := "10";  -- slightly positive
                    elsif current_sample > thresh_l_reg then
                        quantized_value := "01";  -- slightly negative
                    else
                        quantized_value := "00";  -- most negative
                    end if;

                    -- Pack: sample 0 in bits [1:0], sample 1 in [3:2], etc.
                    data_out_reg(i*2+1 downto i*2) <= quantized_value;
                end loop;
            end if;
        end if;
    end process;

    data_out <= data_out_reg;

end Behavioral;
