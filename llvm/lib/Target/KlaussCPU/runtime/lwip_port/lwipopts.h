// lwipopts.h — lwIP configuration for KlaussCPU (NO_SYS=1, bare-metal).

#ifndef LWIP_LWIPOPTS_H
#define LWIP_LWIPOPTS_H

// ── Core mode ─────────────────────────────────────────────────────────────

// cc.h includes <limits.h> so SSIZE_MAX is defined before lwIP's arch.h is
// processed.  Tell lwIP not to include <unistd.h> again — picolibc's stdio.h
// will define ssize_t correctly when it is first pulled in.
#define LWIP_NO_UNISTD_H            1

#define NO_SYS                      1
// With NO_SYS=1 lwIP runs entirely in one task; the timer ISR never calls
// lwIP functions, so there is no re-entrancy risk.  Keep PROT=0 to avoid
// the sys_prot_t forward-declaration problem in sys.h when NO_SYS=1.
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0

// ── Memory ────────────────────────────────────────────────────────────────
// lwIP manages its own heap (mem_malloc/mem_free) separate from picolibc's.
// 32 KB covers several simultaneous TCP connections comfortably.

#define MEM_ALIGNMENT               8       // 64-bit aligned (KlaussCPU)
#define MEM_SIZE                    (32 * 1024)

// pbuf pool: each entry holds one Ethernet MTU frame (1500 B payload +
// headers); 8 slots lets us queue a handful of frames before dropping.
#define PBUF_POOL_SIZE              8
#define PBUF_POOL_BUFSIZE           1536    // ≥ ETH MTU (1500) + headers

// ── Protocols ─────────────────────────────────────────────────────────────

#define LWIP_ARP                    1
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ICMP                   1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DHCP                   1
#define LWIP_AUTOIP                 0
#define LWIP_IGMP                   0
#define LWIP_DNS                    0   // enable later when needed

// ── TCP tuning ────────────────────────────────────────────────────────────

#define TCP_MSS                     1460
#define TCP_WND                     (4 * TCP_MSS)
#define TCP_SND_BUF                 (4 * TCP_MSS)
#define TCP_QUEUE_OOSEQ             0   // drop out-of-order segments (saves RAM)
#define MEMP_NUM_TCP_SEG            16
#define MEMP_NUM_TCP_PCB            4   // max simultaneous TCP connections

// ── ARP ───────────────────────────────────────────────────────────────────

#define ARP_TABLE_SIZE              8
#define ARP_MAXAGE                  300  // seconds

// ── Netif ─────────────────────────────────────────────────────────────────

#define LWIP_NETIF_LOOPBACK         0
#define LWIP_NETIF_STATUS_CALLBACK  1   // called when IP assigned / removed
#define LWIP_NETIF_LINK_CALLBACK    1   // called on link up / down

// ── DHCP ──────────────────────────────────────────────────────────────────

#define DHCP_DOES_ARP_CHECK         0   // skip ARP conflict detection (speed)

// ── Stats: disable for production, flip on to debug ───────────────────────

#define LWIP_STATS                  0
#define LWIP_STATS_DISPLAY          0

// ── Checksum: computed in software (no hardware offload) ─────────────────

#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_GEN_TCP            1
#define CHECKSUM_CHECK_IP           1
#define CHECKSUM_CHECK_UDP          1
#define CHECKSUM_CHECK_TCP          1

// ── Debug: set LWIP_DBG_ON to enable category-specific traces ─────────────

#define LWIP_DBG_MIN_LEVEL          LWIP_DBG_LEVEL_ALL
#define LWIP_DBG_TYPES_ON           LWIP_DBG_OFF   // flip to LWIP_DBG_ON to trace

#endif // LWIP_LWIPOPTS_H
