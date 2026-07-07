#!/bin/bash
# tune_nic.sh [iface] [irq_core] -- REQUIRED NIC tuning for sustained no-drop capture.
#
# Root cause of drops on the real ConnectX-5: the default RX ring is tiny (1024) and a
# single UDP flow lands on ONE RSS queue whose softirq shares a core with the (unpinned)
# capture thread -> the NIC RX ring overflows (ethtool -S rx_out_of_buffer climbs) and
# frames are lost BEFORE AF_PACKET. This fixes all three: bigger ring, one deterministic
# queue, and the NIC IRQ pinned to its own core (capture/writer get their own cores via
# the recorder's --cap-core/--wr-core). Run as root. Settings persist until reboot.
#
#   sudo ./tune_nic.sh enp1s0f0np0 1
set -u
IFACE="${1:-enp1s0f0np0}"
IRQ_CORE="${2:-1}"          # pin the NIC IRQ here; keep capture/writer on OTHER cores
MODE="${3:-xdp}"            # xdp (AF_XDP recorder, default) | udp (legacy kernel path)

echo "== tuning $IFACE (IRQ -> core $IRQ_CORE) =="
ip link set "$IFACE" up mtu 9000 2>/dev/null && echo "  link up, MTU 9000"

# one RX queue for the single VDIF flow => one IRQ we can pin deterministically
ethtool -L "$IFACE" combined 1 2>/dev/null && echo "  combined queues -> 1" \
  || echo "  (could not set combined=1; will pin all NIC IRQs instead)"
# max out the RX ring (1024 -> 8192 => ~64 ms of buffer at 128k pps, was ~8 ms)
ethtool -G "$IFACE" rx 8192 2>/dev/null && echo "  RX ring -> 8192"
# no coalescing of our fixed-size frames
ethtool -K "$IFACE" gro off lro off 2>/dev/null && echo "  GRO/LRO -> off"

# kernel softirq / socket ceilings
sysctl -qw net.core.rmem_max=536870912
sysctl -qw net.core.netdev_max_backlog=300000
sysctl -qw net.core.netdev_budget=3000
sysctl -qw net.core.netdev_budget_usecs=8000
echo "  sysctl: rmem_max=512MiB backlog=300000 netdev_budget=3000"

# lock CPU clocks high (default 'powersave' downclocks the capture/softirq cores)
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$g" 2>/dev/null; done
echo "  CPU governor -> performance"

# stop irqbalance (else it migrates the NIC IRQ off our chosen core) and pin IRQs
systemctl stop irqbalance 2>/dev/null && echo "  irqbalance stopped"
n=0
for irq in $(grep -iE "$IFACE|mlx5" /proc/interrupts | awk -F: '{print $1}' | tr -d ' '); do
  echo "$IRQ_CORE" > /proc/irq/$irq/smp_affinity_list 2>/dev/null && n=$((n+1))
done
echo "  pinned $n NIC IRQ(s) -> core $IRQ_CORE"

# ---- AF_XDP (--rx xdp) specific NIC tuning ------------------------------------------
# These break the single-flow kernel receive ceiling for the zero-copy recorder.
if [ "$MODE" = "xdp" ]; then
  # mlx5 striding-RQ + jumbo (MTU>3498) + XDP multi-buffer can corrupt the 2nd fragment
  # (known bug, kernel 6.x); turn striding RQ OFF so multi-buffer frags are intact.
  ethtool --set-priv-flags "$IFACE" rx_striding_rq off 2>/dev/null \
    && echo "  rx_striding_rq -> off (XDP multi-buffer safe)" \
    || echo "  (could not toggle rx_striding_rq)"
  # IRQ-driven (default): let hardware IRQs drive NAPI on the pinned IRQ core so the
  # per-packet copy-to-UMEM runs THERE, leaving the capture core free to reassemble +
  # write -- the two-core split is what gives zero-gap capture in COPY mode on this NIC.
  # (napi_defer/gro_flush deferral is for busy-poll mode; keep it OFF here.)
  echo 0 > /sys/class/net/"$IFACE"/napi_defer_hard_irqs 2>/dev/null
  echo 0 > /sys/class/net/"$IFACE"/gro_flush_timeout    2>/dev/null
  echo "  napi_defer_hard_irqs=0 gro_flush_timeout=0 (IRQ-driven NAPI on core $IRQ_CORE)"
else
  # legacy kernel UDP path prefers striding RQ ON (off made it worse there)
  ethtool --set-priv-flags "$IFACE" rx_striding_rq on 2>/dev/null \
    && echo "  rx_striding_rq -> on (legacy udp path)"
fi

echo "== verify (mode=$MODE) =="
echo -n "  RX ring: "; ethtool -g "$IFACE" 2>/dev/null | awk '/Current hardware settings/{f=1} f&&/^RX:/{print $2; exit}'
echo -n "  queues : "; ethtool -l "$IFACE" 2>/dev/null | awk '/Current hardware settings/{f=1} f&&/Combined:/{print $2; exit}'
echo -n "  striding_rq: "; ethtool --show-priv-flags "$IFACE" 2>/dev/null | awk '/rx_striding_rq/{print $3; exit}'
echo "  -> now run the recorder (AF_XDP) with capture/writer on OTHER cores, e.g.:"
echo "     record_supervisor.py --rx xdp --cap-core 2 --wr-core 3"
