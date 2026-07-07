-- sim_fifo_stub.vhd : faithful behavioral stand-ins for the two Xilinx
-- fifo_generator IP cores used by the framer FIFOs (dat1-4, header).  Only the
-- IP CORES are substituted; the REAL generated xlfifogen_u wrapper and the REAL
-- generated framer logic (trigger, counters, read FSM) are used unchanged, so
-- this exercises the actual gateware that the .fpg was built from.
--
-- Both cores are STANDARD (non-FWFT) common-clock synchronous FIFOs per the .xci:
--   i0 : data_width=128, depth=256, data_count_width=8   (dat1-4)   <- trigger FIFO
--   i1 : data_width=256, depth= 16, data_count_width=4   (header)
-- Standard-FIFO semantics: assert rd_en (when not empty) -> dout valid on the NEXT
-- clock edge; data_count = exact occupancy (during the writes-only fill phase this
-- is monotonic, so an exact "==123" trigger is guaranteed to be hit -- the precise
-- question Bug A turns on).

library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;

entity rfsoc_vdif_v0_4_dev_fifo_generator_i0 is
  port (
    clk        : in  std_logic;
    din        : in  std_logic_vector(127 downto 0);
    wr_en      : in  std_logic;
    rd_en      : in  std_logic;
    dout       : out std_logic_vector(127 downto 0);
    full       : out std_logic;
    empty      : out std_logic;
    data_count : out std_logic_vector(7 downto 0)
  );
end entity;

architecture beh of rfsoc_vdif_v0_4_dev_fifo_generator_i0 is
  constant DEPTH : integer := 256;
  type mem_t is array(0 to DEPTH-1) of std_logic_vector(127 downto 0);
  signal mem  : mem_t;
  signal wptr : integer range 0 to DEPTH-1 := 0;
  signal rptr : integer range 0 to DEPTH-1 := 0;
  signal cnt  : integer range 0 to DEPTH   := 0;
begin
  process(clk)
    variable do_wr, do_rd : boolean;
  begin
    if rising_edge(clk) then
      do_wr := (wr_en = '1') and (cnt < DEPTH);
      do_rd := (rd_en = '1') and (cnt > 0);
      if do_wr then
        mem(wptr) <= din;
        wptr <= (wptr + 1) mod DEPTH;
      end if;
      if do_rd then
        dout <= mem(rptr);            -- standard FIFO: data appears next cycle
        rptr <= (rptr + 1) mod DEPTH;
      end if;
      if do_wr and not do_rd then
        cnt <= cnt + 1;
      elsif do_rd and not do_wr then
        cnt <= cnt - 1;
      end if;
    end if;
  end process;
  data_count <= std_logic_vector(to_unsigned(cnt, 8));
  empty <= '1' when cnt = 0     else '0';
  full  <= '1' when cnt = DEPTH else '0';
end architecture;


library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;

entity rfsoc_vdif_v0_4_dev_fifo_generator_i1 is
  port (
    clk        : in  std_logic;
    din        : in  std_logic_vector(255 downto 0);
    wr_en      : in  std_logic;
    rd_en      : in  std_logic;
    dout       : out std_logic_vector(255 downto 0);
    full       : out std_logic;
    empty      : out std_logic;
    data_count : out std_logic_vector(3 downto 0)
  );
end entity;

architecture beh of rfsoc_vdif_v0_4_dev_fifo_generator_i1 is
  constant DEPTH : integer := 16;
  type mem_t is array(0 to DEPTH-1) of std_logic_vector(255 downto 0);
  signal mem  : mem_t;
  signal wptr : integer range 0 to DEPTH-1 := 0;
  signal rptr : integer range 0 to DEPTH-1 := 0;
  signal cnt  : integer range 0 to DEPTH   := 0;
begin
  process(clk)
    variable do_wr, do_rd : boolean;
  begin
    if rising_edge(clk) then
      do_wr := (wr_en = '1') and (cnt < DEPTH);
      do_rd := (rd_en = '1') and (cnt > 0);
      if do_wr then
        mem(wptr) <= din;
        wptr <= (wptr + 1) mod DEPTH;
      end if;
      if do_rd then
        dout <= mem(rptr);
        rptr <= (rptr + 1) mod DEPTH;
      end if;
      if do_wr and not do_rd then
        cnt <= cnt + 1;
      elsif do_rd and not do_wr then
        cnt <= cnt - 1;
      end if;
    end if;
  end process;
  data_count <= std_logic_vector(to_unsigned(cnt, 4));
  empty <= '1' when cnt = 0     else '0';
  full  <= '1' when cnt = DEPTH else '0';
end architecture;
