/*
 * test_sd.c — KlaussCPU SD card / FatFs comprehensive test
 *
 * Tests (in order):
 *   1.  Mount + free-space query
 *   2.  Create a subdirectory
 *   3.  Create / write / read / verify a text file inside that dir
 *   4.  f_stat — check reported file size matches bytes written
 *   5.  Append to an existing file and verify combined content
 *   6.  f_lseek — random-access read from middle of a file
 *   7.  Rename a file
 *   8.  Multiple small files (10 × write + verify)
 *   9.  16 KiB sequential write / read / pattern verify
 *  10.  Remove all test files then the test directory (clean-up)
 *
 * Success: 7-seg = "50D0000" (sod = "passed"), LEDs = 0xFFFF
 * Failure: 7-seg = 0x000000FF, LEDs = fail-count
 */

#include <stdio.h>
#include <string.h>
#include "ff.h"
#include "mmio.h"

/* ── bookkeeping ──────────────────────────────────────────────────────────── */

static int g_pass, g_fail;

static void pass(const char *name) {
    g_pass++;
    printf("  PASS  %s\n", name);
}
static void fail(const char *name, const char *reason) {
    g_fail++;
    printf("  FAIL  %s  (%s)\n", name, reason);
}
#define CHECK(name, cond, reason) do { if (cond) pass(name); else { fail(name, reason); return; } } while (0)
#define CHECK_CONT(name, cond, reason) do { if (cond) pass(name); else fail(name, reason); } while (0)

/* ── globals ──────────────────────────────────────────────────────────────── */

static FATFS  fs;
static FIL    fil;
static FILINFO fno;

/* ── 1. mount + free space ────────────────────────────────────────────────── */

static void test_mount(void) {
    printf("\n--- 1. Mount + free space ---\n");
    FRESULT fr = f_mount(&fs, "0:", 1);
    CHECK("mount", fr == FR_OK, "f_mount failed");

    DWORD fre_clust;
    FATFS *fsp = &fs;
    fr = f_getfree("0:", &fre_clust, &fsp);
    CHECK_CONT("getfree", fr == FR_OK, "f_getfree failed");
    if (fr == FR_OK) {
        DWORD fre_kb = fre_clust * fs.csize / 2;
        printf("  free: %lu KiB\n", (unsigned long)fre_kb);
    }
}

/* ── 2. create subdirectory ───────────────────────────────────────────────── */

static void test_mkdir(void) {
    printf("\n--- 2. Create directory ---\n");
    /* Remove first in case left over from a previous run. */
    f_unlink("0:/SDTEST");
    FRESULT fr = f_mkdir("0:/SDTEST");
    /* FR_EXIST is fine — directory already existed (e.g. previous run cleanup failed). */
    CHECK("mkdir", fr == FR_OK || fr == FR_EXIST, "f_mkdir failed");
}

/* ── 3. write + read text file ────────────────────────────────────────────── */

#define TEXT_PAYLOAD "KlaussCPU SD card test file\nLine two of the test.\n"

static void test_text_file(void) {
    printf("\n--- 3. Write / read text file ---\n");
    FRESULT fr = f_open(&fil, "0:/SDTEST/TEXT.TXT", FA_WRITE | FA_CREATE_ALWAYS);
    CHECK("text open-write", fr == FR_OK, "f_open write failed");

    UINT bw;
    fr = f_write(&fil, TEXT_PAYLOAD, (UINT)strlen(TEXT_PAYLOAD), &bw);
    f_sync(&fil);
    f_close(&fil);
    CHECK("text write", fr == FR_OK && bw == (UINT)strlen(TEXT_PAYLOAD), "f_write failed");

    fr = f_open(&fil, "0:/SDTEST/TEXT.TXT", FA_READ);
    CHECK("text open-read", fr == FR_OK, "f_open read failed");

    char buf[128];
    UINT br;
    fr = f_read(&fil, buf, sizeof(buf) - 1, &br);
    f_close(&fil);
    buf[br] = '\0';
    CHECK("text read", fr == FR_OK && br == (UINT)strlen(TEXT_PAYLOAD) &&
          strcmp(buf, TEXT_PAYLOAD) == 0, "content mismatch");
}

/* ── 4. f_stat ────────────────────────────────────────────────────────────── */

static void test_stat(void) {
    printf("\n--- 4. f_stat ---\n");
    FRESULT fr = f_stat("0:/SDTEST/TEXT.TXT", &fno);
    CHECK("stat", fr == FR_OK && fno.fsize == (FSIZE_t)strlen(TEXT_PAYLOAD),
          "wrong size");
    if (fr == FR_OK)
        printf("  size: %lu bytes\n", (unsigned long)fno.fsize);
}

/* ── 5. append ────────────────────────────────────────────────────────────── */

#define APPEND_PAYLOAD "Appended line.\n"

static void test_append(void) {
    printf("\n--- 5. Append ---\n");
    FRESULT fr = f_open(&fil, "0:/SDTEST/TEXT.TXT", FA_WRITE | FA_OPEN_APPEND);
    CHECK("append open", fr == FR_OK, "f_open append failed");

    UINT bw;
    fr = f_write(&fil, APPEND_PAYLOAD, (UINT)strlen(APPEND_PAYLOAD), &bw);
    f_sync(&fil);
    f_close(&fil);
    CHECK("append write", fr == FR_OK && bw == (UINT)strlen(APPEND_PAYLOAD),
          "f_write failed");

    /* Verify combined size. */
    size_t expected = strlen(TEXT_PAYLOAD) + strlen(APPEND_PAYLOAD);
    fr = f_stat("0:/SDTEST/TEXT.TXT", &fno);
    CHECK_CONT("append size", fr == FR_OK && fno.fsize == (FSIZE_t)expected,
               "size wrong after append");
}

/* ── 6. f_lseek (random access) ───────────────────────────────────────────── */

static void test_seek(void) {
    printf("\n--- 6. Seek ---\n");
    FRESULT fr = f_open(&fil, "0:/SDTEST/TEXT.TXT", FA_READ);
    CHECK("seek open", fr == FR_OK, "f_open failed");

    /* Seek to byte 10 and read 5 bytes — should be "CPU S" from "KlaussCPU SD" */
    fr = f_lseek(&fil, 10);
    char sbuf[8];
    UINT br;
    if (fr == FR_OK) fr = f_read(&fil, sbuf, 5, &br);
    f_close(&fil);
    sbuf[br < 5 ? br : 5] = '\0';
    printf("  bytes[10..14]: \"%s\"\n", sbuf);
    CHECK("seek read", fr == FR_OK && br == 5 && strncmp(sbuf, "SD ca", 5) == 0,
          "unexpected bytes at offset 10");
}

/* ── 7. rename ────────────────────────────────────────────────────────────── */

static void test_rename(void) {
    printf("\n--- 7. Rename ---\n");
    FRESULT fr = f_rename("0:/SDTEST/TEXT.TXT", "0:/SDTEST/RENAMED.TXT");
    CHECK("rename", fr == FR_OK, "f_rename failed");

    /* Old name must be gone, new name must exist. */
    int old_gone = (f_stat("0:/SDTEST/TEXT.TXT",    &fno) == FR_NO_FILE);
    int new_here = (f_stat("0:/SDTEST/RENAMED.TXT", &fno) == FR_OK);
    CHECK_CONT("rename verify", old_gone && new_here, "stat check failed");

    /* Rename back so later clean-up is consistent. */
    f_rename("0:/SDTEST/RENAMED.TXT", "0:/SDTEST/TEXT.TXT");
}

/* ── 8. multiple small files ──────────────────────────────────────────────── */

#define N_SMALL 10

static void test_multi_file(void) {
    printf("\n--- 8. %d small files ---\n", N_SMALL);
    char path[32], buf[32];
    int all_ok = 1;

    for (int i = 0; i < N_SMALL; i++) {
        /* Build 8.3 name: FILE0.TXT .. FILE9.TXT */
        path[0] = '0'; path[1] = ':'; path[2] = '/';
        path[3] = 'S'; path[4] = 'D'; path[5] = 'T'; path[6] = 'E'; path[7] = 'S'; path[8] = 'T'; path[9] = '/';
        path[10] = 'F'; path[11] = 'I'; path[12] = 'L'; path[13] = 'E';
        path[14] = (char)('0' + i);
        path[15] = '.'; path[16] = 'T'; path[17] = 'X'; path[18] = 'T'; path[19] = '\0';

        /* Write: "file N" */
        FRESULT fr = f_open(&fil, path, FA_WRITE | FA_CREATE_ALWAYS);
        if (fr != FR_OK) { all_ok = 0; continue; }
        buf[0] = 'f'; buf[1] = 'i'; buf[2] = 'l'; buf[3] = 'e'; buf[4] = ' ';
        buf[5] = (char)('0' + i); buf[6] = '\0';
        UINT bw;
        f_write(&fil, buf, 6, &bw);
        f_sync(&fil);
        f_close(&fil);

        /* Read back and verify. */
        char rbuf[16]; UINT br;
        fr = f_open(&fil, path, FA_READ);
        if (fr != FR_OK) { all_ok = 0; continue; }
        f_read(&fil, rbuf, sizeof(rbuf) - 1, &br);
        f_close(&fil);
        rbuf[br] = '\0';
        if (strcmp(rbuf, buf) != 0) all_ok = 0;
    }
    CHECK_CONT("multi-file write/read", all_ok, "one or more files failed");
}

/* ── 9. 16 KiB sequential pattern ────────────────────────────────────────── */

#define LARGE_SZ 16384

static uint8_t g_wbuf[LARGE_SZ];
static uint8_t g_rbuf[LARGE_SZ];

static void test_large(void) {
    printf("\n--- 9. 16 KiB pattern ---\n");
    for (int i = 0; i < LARGE_SZ; i++) g_wbuf[i] = (uint8_t)(i & 0xFF);

    FRESULT fr = f_open(&fil, "0:/SDTEST/BIG.BIN", FA_WRITE | FA_CREATE_ALWAYS);
    CHECK("large open-write", fr == FR_OK, "f_open failed");

    UINT bw;
    fr = f_write(&fil, g_wbuf, LARGE_SZ, &bw);
    f_sync(&fil);
    f_close(&fil);
    CHECK("large write", fr == FR_OK && bw == LARGE_SZ, "f_write failed");

    fr = f_open(&fil, "0:/SDTEST/BIG.BIN", FA_READ);
    CHECK("large open-read", fr == FR_OK, "f_open failed");

    UINT br;
    fr = f_read(&fil, g_rbuf, LARGE_SZ, &br);
    f_close(&fil);
    int match = (fr == FR_OK) && (br == LARGE_SZ) &&
                (memcmp(g_wbuf, g_rbuf, LARGE_SZ) == 0);
    printf("  wrote %u, read %u, %s\n", (unsigned)bw, (unsigned)br,
           match ? "match" : "MISMATCH");
    CHECK("large verify", match, "pattern mismatch");
}

/* ── 10. clean-up ─────────────────────────────────────────────────────────── */

/* Delete every file inside a directory by scanning it. */
static void purge_dir(const char *dirpath) {
    DIR d;
    FILINFO fi;
    char path[64];
    if (f_opendir(&d, dirpath) != FR_OK) return;
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0] != '\0') {
        int i = 0;
        while (dirpath[i]) { path[i] = dirpath[i]; i++; }
        path[i++] = '/';
        int j = 0;
        while (fi.fname[j]) { path[i++] = fi.fname[j++]; }
        path[i] = '\0';
        f_unlink(path);
    }
    f_closedir(&d);
}

static void test_cleanup(void) {
    printf("\n--- 10. Clean-up ---\n");
    purge_dir("0:/SDTEST");
    FRESULT fr = f_unlink("0:/SDTEST");
    CHECK("cleanup rmdir", fr == FR_OK, "f_unlink dir failed");
    CHECK_CONT("cleanup verify", f_stat("0:/SDTEST", &fno) == FR_NO_FILE,
               "directory still present");
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    g_pass = g_fail = 0;
    printf("=== KlaussCPU SD card test ===\n");

    test_mount();
    test_mkdir();
    test_text_file();
    test_stat();
    test_append();
    test_seek();
    test_rename();
    test_multi_file();
    test_large();
    test_cleanup();

    printf("\n=== Results: %d pass, %d fail ===\n", g_pass, g_fail);

    if (g_fail == 0) {
        REG_SEG_ALL = 0x50D00000u;   /* "50d0" ≈ "SOD" passed */
        REG_LEDS    = 0xFFFF;
    } else {
        REG_SEG_ALL = 0x000000FFu;
        REG_LEDS    = (uint32_t)g_fail;
    }
    return g_fail;
}
