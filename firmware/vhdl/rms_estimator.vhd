library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;

-- RMS Estimator for adaptive 2-bit requantization
-- Computes running mean absolute deviation of 8 x 16-bit signed samples,
-- derives Van Vleck optimal thresholds at +/-0.9816 sigma.
-- Uses only shift/add operations — zero DSP slices.

entity rms_estimator is
    Generic (
        C_S_AXI_DATA_WIDTH : integer := 32
    );
    Port (
        clk             : in  std_logic;
        ce              : in  std_logic := '1';
        rst             : in  std_logic;
        -- 8 x 16-bit signed samples from RFDC
        data_in         : in  std_logic_vector(127 downto 0);
        -- Adaptive thresholds (signed 16-bit)
        thresh_upper    : out std_logic_vector(15 downto 0);
        thresh_lower    : out std_logic_vector(15 downto 0);
        -- Software-readable power estimate
        power_est       : out std_logic_vector(31 downto 0);
        -- Software register inputs
        alpha_shift     : in  std_logic_vector(3 downto 0);
        thresh_override : in  std_logic_vector(15 downto 0);
        use_override    : in  std_logic
    );
end rms_estimator;

architecture Behavioral of rms_estimator is

    constant SAMPLE_WIDTH : integer := 16;
    constant NUM_SAMPLES  : integer := 8;

    -- Absolute value sum needs up to 8 * 32767 = 262136, fits in 19 bits
    signal abs_sum : unsigned(18 downto 0);

    -- EMA accumulator — wide enough for large alpha_shift values
    -- Max accumulator value ~ abs_sum_max * 2^alpha_shift_max = 262136 * 2^15 ~ 34 bits
    signal mean_abs_acc : unsigned(34 downto 0) := (others => '0');

    -- Per-sample mean absolute value (after dividing by 2^(alpha+3))
    signal mean_abs : unsigned(15 downto 0);

    -- Computed threshold
    signal threshold : unsigned(15 downto 0);

    -- Pipeline registers
    signal abs_sum_reg   : unsigned(18 downto 0) := (others => '0');
    signal thresh_u_reg  : std_logic_vector(15 downto 0) := (others => '0');
    signal thresh_l_reg  : std_logic_vector(15 downto 0) := (others => '0');
    signal power_reg     : std_logic_vector(31 downto 0) := (others => '0');

    -- Alpha as integer for shifting
    signal alpha_int : integer range 0 to 15;

    -- Xilinx attributes
    attribute use_dsp : string;
    attribute use_dsp of Behavioral : architecture is "no";

begin

    alpha_int <= to_integer(unsigned(alpha_shift));

    ---------------------------------------------------------------------------
    -- Stage 1: Compute sum of absolute values for 8 samples (combinational)
    ---------------------------------------------------------------------------
    process(data_in)
        variable sample : signed(SAMPLE_WIDTH-1 downto 0);
        variable abs_val : unsigned(SAMPLE_WIDTH-1 downto 0);
        variable acc : unsigned(18 downto 0);
    begin
        acc := (others => '0');
        for i in 0 to NUM_SAMPLES-1 loop
            sample := signed(data_in((i+1)*SAMPLE_WIDTH-1 downto i*SAMPLE_WIDTH));
            if sample < 0 then
                abs_val := unsigned(-sample);
            else
                abs_val := unsigned(sample);
            end if;
            acc := acc + resize(abs_val, 19);
        end loop;
        abs_sum <= acc;
    end process;

    ---------------------------------------------------------------------------
    -- Stage 2: EMA accumulator and threshold computation (sequential)
    ---------------------------------------------------------------------------
    process(clk)
        variable decay   : unsigned(34 downto 0);
        variable new_acc : unsigned(34 downto 0);
        variable m_abs   : unsigned(15 downto 0);
        variable thresh  : unsigned(15 downto 0);
    begin
        if rising_edge(clk) then
            if rst = '1' then
                abs_sum_reg  <= (others => '0');
                mean_abs_acc <= (others => '0');
                thresh_u_reg <= (others => '0');
                thresh_l_reg <= (others => '0');
                power_reg    <= (others => '0');
            elsif ce = '1' then
                -- Register the abs_sum for pipelining
                abs_sum_reg <= abs_sum;

                -- EMA update: acc <= acc - (acc >> alpha) + abs_sum
                decay   := shift_right(mean_abs_acc, alpha_int);
                new_acc := mean_abs_acc - decay + resize(abs_sum_reg, 35);
                mean_abs_acc <= new_acc;

                -- Per-sample mean absolute: acc >> (alpha + 3)
                -- alpha+3 can be at most 18, fits in shift
                m_abs := resize(shift_right(new_acc, alpha_int + 3), 16);
                mean_abs <= m_abs;

                -- Threshold = mean_abs * 1.23047 (shift approximation of 0.9816 sigma)
                -- MAD = 0.7979*sigma, so thr/MAD = 0.9816/0.7979 = 1.2302; the previous
                -- factor 1.21875 gave only 0.9725 sigma (thresholds ~1% low -> outer 2-bit
                -- levels over-filled). 1 + 1/4 - 1/64 - 1/256 = 1.23047 -> 0.98169 sigma.
                -- = mean_abs + (mean_abs >> 2) - (mean_abs >> 6) - (mean_abs >> 8)
                thresh := m_abs
                        + shift_right(m_abs, 2)
                        - shift_right(m_abs, 6)
                        - shift_right(m_abs, 8);
                threshold <= thresh;

                -- Output threshold or override
                if use_override = '1' then
                    thresh_u_reg <= thresh_override;
                    -- Negate for lower threshold
                    thresh_l_reg <= std_logic_vector(-signed(thresh_override));
                else
                    thresh_u_reg <= std_logic_vector(signed(resize(thresh, 16)));
                    thresh_l_reg <= std_logic_vector(-signed(resize(thresh, 16)));
                end if;

                -- Power estimate for software monitoring (accumulator value)
                power_reg <= std_logic_vector(resize(shift_right(new_acc, alpha_int), 32));
            end if;
        end if;
    end process;

    -- Output assignments
    thresh_upper <= thresh_u_reg;
    thresh_lower <= thresh_l_reg;
    power_est    <= power_reg;

end Behavioral;
