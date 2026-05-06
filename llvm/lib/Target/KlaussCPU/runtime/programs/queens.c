/* queens.c — N-queens backtracker for KlaussCPU.
 *
 * Compile via Makefile:  make queens.elf  &&  make queens.bin
 *
 * Tests deep recursion (up to N levels), array indexing, and tight inner
 * loops with conditional early-out — exactly the shape of a backtracking
 * search.  Counts solutions for N=1..8 and verifies them against the known
 * sequence [1, 0, 0, 2, 10, 4, 40, 92] (OEIS A000170).
 *
 * Also runs a "show one solution for N=8" demo by printing the row index of
 * each queen in the first solution found.
 */

#include <stdio.h>

static int g_pass;
static int g_fail;

static void check(char *name, int ok) {
    printf("%s%s\n", name, ok ? "PASS" : "FAIL");
    if (ok) g_pass++; else g_fail++;
}

/* ── N-queens count ───────────────────────────────────────────────────────── */

/* Returns the number of legal placements that complete the board, given
 * the columns chosen for rows 0..row-1 in `cols[]`. */
static int count_solutions(int n, int row, int *cols) {
    if (row == n) return 1;

    int total = 0;
    for (int c = 0; c < n; c++) {
        int legal = 1;
        for (int r = 0; r < row; r++) {
            int dr = row - r;
            int dc = c - cols[r];
            if (dc < 0) dc = -dc;
            if (cols[r] == c || dc == dr) {
                legal = 0;
                break;
            }
        }
        if (legal) {
            cols[row] = c;
            total += count_solutions(n, row + 1, cols);
        }
    }
    return total;
}

/* Find the lexicographically-first solution for N=n; columns written into
 * cols[] on success, return 1.  Return 0 on failure. */
static int find_first(int n, int row, int *cols) {
    if (row == n) return 1;

    for (int c = 0; c < n; c++) {
        int legal = 1;
        for (int r = 0; r < row; r++) {
            int dr = row - r;
            int dc = c - cols[r];
            if (dc < 0) dc = -dc;
            if (cols[r] == c || dc == dr) {
                legal = 0;
                break;
            }
        }
        if (legal) {
            cols[row] = c;
            if (find_first(n, row + 1, cols)) return 1;
        }
    }
    return 0;
}

/* Verify that cols[] is a legal n-queens solution. */
static int verify(int n, int *cols) {
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (cols[i] == cols[j]) return 0;
            int dc = cols[j] - cols[i];
            if (dc < 0) dc = -dc;
            int dr = j - i;
            if (dc == dr) return 0;
        }
    }
    return 1;
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    g_pass = 0;
    g_fail = 0;

    printf("=== N-Queens Backtracker ===\n");

    /* Known solution counts for N=1..8 (OEIS A000170). */
    static const int expected[] = { 1, 0, 0, 2, 10, 4, 40, 92 };

    int cols[12];

    for (int n = 1; n <= 8; n++) {
        int got = count_solutions(n, 0, cols);
        if (got == expected[n - 1]) {
            printf("N=%d: %d  PASS\n", n, got);
            g_pass++;
        } else {
            printf("N=%d: %d  FAIL exp=%d\n", n, got, expected[n - 1]);
            g_fail++;
        }
    }

    /* Display the first N=8 solution. */
    {
        int cols8[8];
        int found = find_first(8, 0, cols8);
        check("Find first N=8:   ", found);

        if (found) {
            printf("    cols=[");
            for (int i = 0; i < 8; i++) {
                printf("%d", cols8[i]);
                if (i < 7) printf(",");
            }
            printf("]\n");

            check("Verify legal:     ", verify(8, cols8));

            /* Print the board (row-major; '.' empty, 'Q' queen). */
            for (int r = 0; r < 8; r++) {
                printf("    ");
                for (int c = 0; c < 8; c++)
                    printf("%s", cols8[r] == c ? "Q " : ". ");
                printf("\n");
            }
        }
    }

    /* Quick stress: count for N=9, expected 352 (still cheap, < 1M leaves). */
    {
        int got9 = count_solutions(9, 0, cols);
        check("N=9 count==352:   ", got9 == 352);
    }

    printf("\nResults: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
