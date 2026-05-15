/*
 * lwipopts.h — lwIP configuration for KlaussCPU with FreeRTOS (NO_SYS=0).
 *
 * Placed in freertos/ so it shadows lwip_port/lwipopts.h when the freertos/
 * directory is first in the compiler's include path.  The bare-metal builds
 * in runtime/ continue to use lwip_port/lwipopts.h unchanged.
 */

#ifndef LWIP_LWIPOPTS_H
#define LWIP_LWIPOPTS_H

/* ── Compile-time workarounds ───────────────────────────────────────────── */
#define LWIP_NO_UNISTD_H            1
/* Tell lwIP where to find errno constants (picolibc provides them). */
#define LWIP_ERRNO_INCLUDE          <errno.h>
int rand(void);
#define LWIP_RAND()                 ((u32_t)rand())

/* ── Core mode — OS-aware ───────────────────────────────────────────────── */
#define NO_SYS                      0
#define SYS_LIGHTWEIGHT_PROT        1   /* use sys_arch_protect/unprotect */
#define LWIP_NETCONN                1
#define LWIP_SOCKET                 1
#define LWIP_COMPAT_SOCKETS         1   /* socket() etc. without lwip_ prefix */
#define LWIP_POSIX_SOCKETS_IO_NAMES 0   /* avoid clashing with picolibc names */

/* ── tcpip thread ───────────────────────────────────────────────────────── */
#define TCPIP_THREAD_NAME           "TCPIP"
#define TCPIP_THREAD_STACKSIZE      512     /* 512 × 8 B = 4 KB */
#define TCPIP_THREAD_PRIO           6       /* one below max (7) */
#define TCPIP_MBOX_SIZE             16

/* ── Default thread / mbox sizes (netconn / socket internals) ───────────── */
#define DEFAULT_THREAD_STACKSIZE    256     /* 256 × 8 B = 2 KB */
#define DEFAULT_THREAD_PRIO         2
#define DEFAULT_RAW_RECVMBOX_SIZE   8
#define DEFAULT_UDP_RECVMBOX_SIZE   8
#define DEFAULT_TCP_RECVMBOX_SIZE   16
#define DEFAULT_ACCEPTMBOX_SIZE     4

/* ── Memory ─────────────────────────────────────────────────────────────── */
#define MEM_ALIGNMENT               8
#define MEM_SIZE                    (48 * 1024)   /* lwIP heap */

/* ── Protocols ──────────────────────────────────────────────────────────── */
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

/* ── Pool counts ────────────────────────────────────────────────────────── */
#define MEMP_NUM_PBUF               16
#define MEMP_NUM_TCP_PCB            8
#define MEMP_NUM_TCP_PCB_LISTEN     4
#define MEMP_NUM_TCP_SEG            24
#define MEMP_NUM_SYS_TIMEOUT        24
#define MEMP_NUM_NETCONN            8
#define MEMP_NUM_NETBUF             8
#define MEMP_NUM_TCPIP_MSG_API      16
#define MEMP_NUM_TCPIP_MSG_INPKT    16
#define PBUF_POOL_SIZE              16
#define PBUF_POOL_BUFSIZE           1600

/* ── TCP ────────────────────────────────────────────────────────────────── */
#define TCP_MSS                     1460
#define TCP_WND                     (4 * TCP_MSS)
#define TCP_SND_BUF                 (4 * TCP_MSS)
#define TCP_QUEUE_OOSEQ             0

/* ── ARP ────────────────────────────────────────────────────────────────── */
#define ARP_TABLE_SIZE              8
#define ARP_MAXAGE                  300

/* ── Netif ──────────────────────────────────────────────────────────────── */
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_LOOPBACK         0
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1

/* ── Checksum — algorithm 1 (byte reads, correct for KlaussCPU) ────────── */
#define LWIP_CHKSUM_ALGORITHM       1
#define CHECKSUM_GEN_IP             1
#define CHECKSUM_GEN_UDP            1
#define CHECKSUM_GEN_TCP            1
#define CHECKSUM_CHECK_IP           1
#define CHECKSUM_CHECK_UDP          1
#define CHECKSUM_CHECK_TCP          1

/* ── SNTP ───────────────────────────────────────────────────────────────── */
#define SNTP_SERVER_DNS             1
#define SNTP_UPDATE_DELAY           60000
void app_sntp_set_time(unsigned int sec);
#define SNTP_SET_SYSTEM_TIME(sec)   app_sntp_set_time(sec)

/* ── Stats / debug ──────────────────────────────────────────────────────── */
#define LWIP_STATS                  0
#define LWIP_STATS_DISPLAY          0
#define LWIP_DBG_MIN_LEVEL          LWIP_DBG_LEVEL_ALL
#define LWIP_DBG_TYPES_ON           LWIP_DBG_OFF

#endif /* LWIP_LWIPOPTS_H */
