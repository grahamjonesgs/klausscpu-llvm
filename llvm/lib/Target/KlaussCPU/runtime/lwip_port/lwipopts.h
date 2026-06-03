// lwipopts.h — lwIP configuration for KlaussCPU (NO_SYS=1, bare-metal).

#ifndef LWIP_LWIPOPTS_H
#define LWIP_LWIPOPTS_H

// ── Compile-time workarounds ──────────────────────────────────────────────
// cc.h includes <limits.h> + defines SSIZE_MAX before lwIP arch.h runs.
// Prevents lwIP from doing 'typedef int ssize_t' which conflicts with picolibc.
#define LWIP_NO_UNISTD_H            1

// ── Random number source (required by dns.c for transaction IDs) ─────────
// Forward-declare picolibc's rand() without pulling in <stdlib.h> here.
int rand(void);
#define LWIP_RAND() ((u32_t)rand())

// ── Core mode ─────────────────────────────────────────────────────────────
#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0   // safe: timer ISR never calls lwIP
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0

// ── Memory ────────────────────────────────────────────────────────────────
// FK_Data_8 (R_KLAUSSCPU_ABS64) is now implemented in KlaussCPUAsmBackend.cpp
// and lld/ELF/Arch/KlaussCPU.cpp, so lwIP's const pool descriptor structs
// in .rodata have their pointer fields correctly resolved by the linker.
// MEMP_MEM_MALLOC is NOT needed.
#define MEM_ALIGNMENT               8       // 64-bit pointer width on KlaussCPU
#define MEM_SIZE                    (128 * 1024)

// ── Protocols ─────────────────────────────────────────────────────────────
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DHCP                   1
#define LWIP_AUTOIP                 0
#define LWIP_IGMP                   0
#define LWIP_DNS                    1

// ── Pool counts (used by memp_sizes[] even with MEMP_MEM_MALLOC=1) ────────
#define MEMP_NUM_PBUF               32
#define MEMP_NUM_TCP_PCB            4
#define MEMP_NUM_TCP_PCB_LISTEN     2
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_SYS_TIMEOUT        24   /* DNS retries + NTP + TCP + DHCP */
#define PBUF_POOL_SIZE              32
#define PBUF_POOL_BUFSIZE           1600

// ── TCP ───────────────────────────────────────────────────────────────────
#define TCP_MSS                     1460
// The LiteEth MAC has exactly TWO RX slots.  Each received frame takes the core
// ~1 ms to process, far longer than the ~120 us wire spacing, so any burst
// larger than the ring overflows and the surplus frames are dropped *in
// hardware* (invisible to lwIP's link.drop) — the sender then recovers each loss
// via a ~270 ms retransmit timeout, collapsing throughput.  Advertising a window
// of exactly 2*MSS caps the sender at two in-flight segments = the two ring
// slots, so nothing is ever dropped.  Immediate ACKs (tcp_ack_now in
// netboot_recv) reopen the window the instant a slot drains, keeping it pipelined
// at the board's true processing rate.  Do NOT raise this without deeper RX slots.
#define TCP_WND                     (2 * TCP_MSS)
#define TCP_SND_BUF                 (4 * TCP_MSS)
// Queue out-of-order segments instead of discarding everything past a hole, so
// an occasional ring-overflow drop costs one retransmit rather than the whole
// in-flight window (which made large transfers collapse as cwnd grew).
#define TCP_QUEUE_OOSEQ             1

// ── ARP ───────────────────────────────────────────────────────────────────
#define ARP_TABLE_SIZE              8
#define ARP_MAXAGE                  300

// ── Netif ─────────────────────────────────────────────────────────────────
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_LOOPBACK         0
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1

// ── Checksum algorithm ────────────────────────────────────────────────────
// Algorithm 2 (default) uses u16_t* reads (LDIDX16) which on KlaussCPU
// read bytes in big-endian order, accumulating SBE instead of SLE.  The
// resulting stored checksum is byte-swapped.  Algorithm 1 uses explicit
// LDIDX8 byte reads (known-correct) and applies lwip_htons(), which gives
// the right result on KlaussCPU regardless of LDIDX16 byte order.
#define LWIP_CHKSUM_ALGORITHM       1

// ── Checksums: software only ──────────────────────────────────────────────
// Keep GEN_* on so our outgoing ACKs/data carry valid checksums (the host
// validates them).  Skip CHECK_* on RX: byte-wise verification of every
// received segment costs ~1 ms/frame on this core and is redundant — the
// LiteEth MAC drops frames failing the Ethernet FCS, and netboot's own
// image_checksum catches any end-to-end corruption of the loaded program.
#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_GEN_TCP            1
#define CHECKSUM_CHECK_IP           0
#define CHECKSUM_CHECK_UDP          0
#define CHECKSUM_CHECK_TCP          0

// ── SNTP app ──────────────────────────────────────────────────────────────
// SNTP_SERVER_DNS=1: sntp_setservername() accepts a hostname string.
// SNTP_UPDATE_DELAY: re-sync interval in ms (minimum 60000 per RFC).
// SNTP_SET_SYSTEM_TIME: called by the SNTP module on each successful sync.
// Forward-declare with unsigned int (= u32_t on KlaussCPU) to avoid
// pulling in lwIP type headers before they are defined.
#define SNTP_SERVER_DNS             1
#define SNTP_UPDATE_DELAY           60000
void app_sntp_set_time(unsigned int sec);
#define SNTP_SET_SYSTEM_TIME(sec)   app_sntp_set_time(sec)

// ── Stats / debug ─────────────────────────────────────────────────────────
#define LWIP_STATS                  0
#define LWIP_STATS_DISPLAY          0
#define LWIP_DBG_MIN_LEVEL          LWIP_DBG_LEVEL_ALL
#define LWIP_DBG_TYPES_ON           LWIP_DBG_OFF

#endif // LWIP_LWIPOPTS_H
