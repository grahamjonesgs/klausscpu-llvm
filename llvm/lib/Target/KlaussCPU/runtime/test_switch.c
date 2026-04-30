/* test_switch.c — exercises switch/case → jump table dispatch.
 *
 * Compile via Makefile:  make test_switch.elf  &&  make test_switch.bin
 *
 * KlaussCPU's LLVM backend lowers BR_JT to a 4-byte EK_Custom32 table
 * (each entry is an absolute target address with R_KLAUSSCPU_ABS32),
 * loads the entry with MEMGET32/LDIDX32, and dispatches via JMPR_R.
 * This file pokes that whole pipeline:
 *
 *   - dense-key switch (consecutive small ints, the obvious jump-table case)
 *   - char-key switch (offset key, still dense)
 *   - sparse switch with default branch (LLVM may keep this as if/else, but
 *     the harness still verifies behaviour)
 *
 * Run output should print "PASS" for every test then a results summary.
 */

extern void print_str(char *s);
extern void print_int(long long n);
extern void newline(void);

static int g_pass;
static int g_fail;

static void check(char *name, int ok) {
    print_str(name);
    print_str(ok ? "PASS" : "FAIL");
    newline();
    if (ok) g_pass++; else g_fail++;
}

/* ── T1: dense int switch ─────────────────────────────────────────────────── */

static int classify(int n) {
    switch (n) {
    case 0:  return 100;
    case 1:  return 101;
    case 2:  return 102;
    case 3:  return 103;
    case 4:  return 104;
    case 5:  return 105;
    case 6:  return 106;
    case 7:  return 107;
    case 8:  return 108;
    case 9:  return 109;
    default: return -1;
    }
}

/* ── T2: char-key switch with side effect ─────────────────────────────────── */

static int op_apply(char op, int a, int b) {
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/': return (b == 0) ? 0 : a / b;
    case '%': return (b == 0) ? 0 : a % b;
    case '&': return a & b;
    case '|': return a | b;
    case '^': return a ^ b;
    case '<': return a << (b & 31);
    case '>': return a >> (b & 31);
    default:  return -1;
    }
}

/* ── T3: switch inside a loop, with fallthrough ───────────────────────────── */

static int score(char *s) {
    int total = 0;
    for (int i = 0; s[i] != '\0'; ++i) {
        switch (s[i]) {
        case 'a': case 'e': case 'i': case 'o': case 'u':
            total += 1;
            break;
        case 'A': case 'E': case 'I': case 'O': case 'U':
            total += 2;
            break;
        case ' ':
        case '\t':
            break;
        default:
            total += 3;
            break;
        }
    }
    return total;
}

/* ── T4: sparse switch (LLVM may keep as if/else; verify behaviour) ───────── */

static int sparse(int k) {
    switch (k) {
    case  1:    return 11;
    case 17:    return 217;
    case 99:    return 9999;
    case 1000:  return 100000;
    case -5:    return -55;
    default:    return 0;
    }
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    g_pass = 0;
    g_fail = 0;

    print_str("=== Switch / Jump-Table Test ===");
    newline();

    /* T1: dense int switch — exercise every case + the default. */
    {
        int ok = 1;
        for (int i = 0; i < 10; ++i)
            ok = ok && (classify(i) == 100 + i);
        ok = ok && (classify(10)  == -1);
        ok = ok && (classify(-1)  == -1);
        ok = ok && (classify(99)  == -1);
        check("T1 classify(0..9): ", ok);
    }

    /* T2: char-key switch */
    {
        int ok = 1;
        ok = ok && (op_apply('+', 7, 3) == 10);
        ok = ok && (op_apply('-', 7, 3) ==  4);
        ok = ok && (op_apply('*', 7, 3) == 21);
        ok = ok && (op_apply('/', 7, 3) ==  2);
        ok = ok && (op_apply('%', 7, 3) ==  1);
        ok = ok && (op_apply('&', 0xF0, 0x0F) == 0x00);
        ok = ok && (op_apply('|', 0xF0, 0x0F) == 0xFF);
        ok = ok && (op_apply('^', 0xFF, 0x0F) == 0xF0);
        ok = ok && (op_apply('<', 1, 4) == 16);
        ok = ok && (op_apply('>', 64, 2) == 16);
        ok = ok && (op_apply('?', 1, 2) == -1);   /* default */
        check("T2 op_apply chars: ", ok);
    }

    /* T3: switch inside a loop with fallthrough cases */
    {
        int ok = 1;
        ok = ok && (score("hello world") == 1+3+3+3+1+0+3+1+3+3+1);
        ok = ok && (score("AEIOU")       == 2+2+2+2+2);
        ok = ok && (score("aeiou")       == 1+1+1+1+1);
        ok = ok && (score("")            == 0);
        ok = ok && (score("   ")         == 0);
        ok = ok && (score("xyz")         == 9);
        check("T3 score loop sw:  ", ok);
    }

    /* T4: sparse keys */
    {
        int ok = 1;
        ok = ok && (sparse(   1) ==     11);
        ok = ok && (sparse(  17) ==    217);
        ok = ok && (sparse(  99) ==   9999);
        ok = ok && (sparse(1000) == 100000);
        ok = ok && (sparse(  -5) ==    -55);
        ok = ok && (sparse(   0) ==      0);
        ok = ok && (sparse(   2) ==      0);
        check("T4 sparse switch:  ", ok);
    }

    /* Summary */
    newline();
    print_str("Results: ");
    print_int(g_pass);
    print_str(" pass, ");
    print_int(g_fail);
    print_str(" fail");
    newline();

    return g_fail;
}
