// lwipopts.h — lwIP configuration for KlaussCPU (NO_SYS=1, bare-metal).

#ifndef LWIP_LWIPOPTS_H
#define LWIP_LWIPOPTS_H

// ── Compile-time workarounds ──────────────────────────────────────────────
// cc.h includes <limits.h> + defines SSIZE_MAX before lwIP arch.h runs.
// Prevents lwIP from doing 'typedef int ssize_t' which conflicts with picolibc.
#define LWIP_NO_UNISTD_H            1

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
#define MEM_SIZE                    (32 * 1024)

// ── Protocols ─────────────────────────────────────────────────────────────
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_DHCP                   0   // static IP; enable when FK_Data_8 fixed
#define LWIP_AUTOIP                 0
#define LWIP_IGMP                   0
#define LWIP_DNS                    0

// ── Pool counts (used by memp_sizes[] even with MEMP_MEM_MALLOC=1) ────────
#define MEMP_NUM_PBUF               8
#define MEMP_NUM_TCP_PCB            4
#define MEMP_NUM_TCP_PCB_LISTEN     2
#define MEMP_NUM_TCP_SEG            16
#define MEMP_NUM_SYS_TIMEOUT        16
#define PBUF_POOL_SIZE              8
#define PBUF_POOL_BUFSIZE           1600

// ── TCP ───────────────────────────────────────────────────────────────────
#define TCP_MSS                     1460
#define TCP_WND                     (4 * TCP_MSS)
#define TCP_SND_BUF                 (4 * TCP_MSS)
#define TCP_QUEUE_OOSEQ             0

// ── ARP ───────────────────────────────────────────────────────────────────
#define ARP_TABLE_SIZE              8
#define ARP_MAXAGE                  300

// ── Netif ─────────────────────────────────────────────────────────────────
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_LOOPBACK         0
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1

// ── Checksums: software only ──────────────────────────────────────────────
#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_GEN_TCP            1
#define CHECKSUM_CHECK_IP           1
#define CHECKSUM_CHECK_UDP          1
#define CHECKSUM_CHECK_TCP          1

// ── Stats / debug ─────────────────────────────────────────────────────────
#define LWIP_STATS                  0
#define LWIP_STATS_DISPLAY          0
#define LWIP_DBG_MIN_LEVEL          LWIP_DBG_LEVEL_ALL
#define LWIP_DBG_TYPES_ON           LWIP_DBG_OFF

#endif // LWIP_LWIPOPTS_H
