/*---------------------------------------------------------------------------/
/  FatFs configuration for KlaussCPU — KlaussCPU FPGA, picolibc, no OS
/---------------------------------------------------------------------------*/

#define FFCONF_DEF  80386   /* must match ff.h */

/*--- Function Configurations -----------------------------------------------*/

#define FF_FS_READONLY  0       /* 0: read+write, 1: read-only */
#define FF_FS_MINIMIZE  0       /* 0: all functions enabled */
#define FF_USE_FIND     0       /* no f_findfirst/f_findnext */
#define FF_USE_MKFS     0       /* no f_mkfs — format on host */
#define FF_USE_FASTSEEK 0
#define FF_USE_EXPAND   0
#define FF_USE_CHMOD    0
#define FF_USE_LABEL    0
#define FF_USE_FORWARD  0
#define FF_USE_STRFUNC  0
#define FF_PRINT_LLI    0
#define FF_PRINT_FLOAT  0
#define FF_STRF_ENCODE  0

/*--- Locale / Namespace Configurations -------------------------------------*/

#define FF_CODE_PAGE    437     /* US English; ~3 KB code-page table */

/* LFN enabled (stack-based buffer, no ffunicode.c needed for FF_CODE_PAGE=437).
   FF_MAX_LFN=64 → 130 bytes of stack per file op, negligible on 32 KB stacks. */
#define FF_USE_LFN          1
#define FF_MAX_LFN          64
#define FF_LFN_UNICODE      0
#define FF_LFN_BUF          64
#define FF_SFN_BUF          12
#define FF_FS_RPATH         0   /* no relative-path support */

/*--- Drive / Volume Configurations -----------------------------------------*/

#define FF_VOLUMES          1
#define FF_STR_VOLUME_ID    0
#define FF_VOLUME_STRS      "SD"
#define FF_MULTI_PARTITION  0
#define FF_MIN_SS           512
#define FF_MAX_SS           512
#define FF_LBA64            0   /* 32-bit LBA — SD cards ≤ 2 TB */
#define FF_MIN_GPT          0x10000000UL
#define FF_USE_TRIM         0

/*--- System Configurations -------------------------------------------------*/

#define FF_FS_TINY          0   /* full buffer per file */
#define FF_FS_EXFAT         0   /* no exFAT */
#define FF_FS_NORTC         1   /* no RTC; use get_fattime() stub */
#define FF_NORTC_MON        5
#define FF_NORTC_MDAY       6
#define FF_NORTC_YEAR       2026
#define FF_FS_NOFSINFO      0
#define FF_FS_LOCK          0   /* no file-locking (single-threaded) */
#define FF_FS_REENTRANT     0   /* no mutex (no OS) */
#define FF_SYNC_t           void *
