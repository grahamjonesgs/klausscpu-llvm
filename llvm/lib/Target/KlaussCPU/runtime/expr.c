/* expr.c — Recursive-descent expression evaluator for KlaussCPU.
 *
 * Compile via Makefile:  make expr.elf  &&  make expr.bin
 *
 * Tests deep recursion, char parsing, large if/else ladders (no jump tables
 * on KlaussCPU), and integer arithmetic with mixed precedence.
 *
 * Grammar:
 *     expr   := term   (('+' | '-') term)*
 *     term   := factor (('*' | '/' | '%') factor)*
 *     factor := '-' factor                (unary minus)
 *             | '(' expr ')'              (parenthesized sub-expression)
 *             | digit+                    (non-negative integer literal)
 */

extern void print_str(char *s);
extern void print_int(long long n);
extern void print_hex_prefix(long long val);
extern void newline(void);
extern int  strlen(char *s);

/* ── Globals ──────────────────────────────────────────────────────────────── */

static const char *g_src;   /* current input pointer */
static int         g_pass;
static int         g_fail;
static int         g_err;   /* set if parser hits a syntax error */

/* ── Lexer helpers ────────────────────────────────────────────────────────── */

static void skip_ws(void) {
    while (*g_src == ' ' || *g_src == '\t')
        g_src++;
}

static int peek(void) {
    skip_ws();
    return (int)(unsigned char)*g_src;
}

static int eat(int expected) {
    skip_ws();
    if ((int)(unsigned char)*g_src == expected) {
        g_src++;
        return 1;
    }
    return 0;
}

/* ── Parser ───────────────────────────────────────────────────────────────── */

static long long parse_expr(void);

static long long parse_factor(void) {
    skip_ws();
    int c = (int)(unsigned char)*g_src;

    if (c == '-') {
        g_src++;
        return -parse_factor();
    }

    if (c == '+') {
        g_src++;
        return parse_factor();
    }

    if (c == '(') {
        g_src++;
        long long v = parse_expr();
        if (!eat(')')) { g_err = 1; return 0; }
        return v;
    }

    if (c >= '0' && c <= '9') {
        long long v = 0;
        while (*g_src >= '0' && *g_src <= '9') {
            v = v * 10 + (*g_src - '0');
            g_src++;
        }
        return v;
    }

    g_err = 1;
    return 0;
}

static long long parse_term(void) {
    long long v = parse_factor();
    while (!g_err) {
        int c = peek();
        if (c == '*') {
            g_src++;
            v = v * parse_factor();
        } else if (c == '/') {
            g_src++;
            long long r = parse_factor();
            if (r == 0) { g_err = 1; return 0; }
            v = v / r;
        } else if (c == '%') {
            g_src++;
            long long r = parse_factor();
            if (r == 0) { g_err = 1; return 0; }
            v = v % r;
        } else {
            break;
        }
    }
    return v;
}

static long long parse_expr(void) {
    long long v = parse_term();
    while (!g_err) {
        int c = peek();
        if (c == '+') {
            g_src++;
            v = v + parse_term();
        } else if (c == '-') {
            g_src++;
            v = v - parse_term();
        } else {
            break;
        }
    }
    return v;
}

/* ── Test harness ─────────────────────────────────────────────────────────── */

static long long evaluate(const char *src) {
    g_src = src;
    g_err = 0;
    long long v = parse_expr();
    skip_ws();
    if (*g_src != '\0') g_err = 1;     /* trailing garbage */
    return v;
}

static void check(const char *src, long long expected) {
    long long got = evaluate((const char *)src);
    print_str("[");
    print_str((char *)src);
    print_str("] = ");
    print_int(got);
    if (!g_err && got == expected) {
        print_str("  PASS");
        g_pass++;
    } else {
        print_str("  FAIL exp=");
        print_int(expected);
        if (g_err) print_str(" (parse error)");
        g_fail++;
    }
    newline();
}

static void check_err(const char *src) {
    (void)evaluate((const char *)src);
    print_str("[");
    print_str((char *)src);
    print_str("]");
    if (g_err) {
        print_str("  PASS (rejected as expected)");
        g_pass++;
    } else {
        print_str("  FAIL (should have rejected)");
        g_fail++;
    }
    newline();
}

int main(void) {
    g_pass = 0;
    g_fail = 0;

    print_str("=== Expression Evaluator ===");
    newline();

    /* Basics */
    check("0", 0);
    check("42", 42);
    check("1+2", 3);
    check("10-3", 7);
    check("4*5", 20);
    check("20/4", 5);
    check("17%5", 2);

    /* Precedence */
    check("1+2*3", 7);
    check("2*3+1", 7);
    check("10-2*3", 4);
    check("100/10/2", 5);
    check("100/(10/2)", 20);

    /* Parens & unary */
    check("(1+2)*3", 9);
    check("-5+3", -2);
    check("-(5+3)", -8);
    check("--5", 5);
    check("---5", -5);
    check("3*-4", -12);
    check("-(-(-(7)))", -7);

    /* Whitespace handling */
    check("  1  +  2  ", 3);
    check("\t10\t*\t10\t", 100);

    /* Deep nesting — exercises recursion depth */
    check("(((((((1+2)*3)+4)*5)+6)*7)+8)", 668);
    check("1+(2+(3+(4+(5+(6+(7+(8+9)))))))", 45);

    /* Large values fit in long long but not int */
    check("1000000*1000000", 1000000000000LL);
    check("123456*789012", 97408265472LL);

    /* Mixed precedence chains */
    check("2+3*4-5*6+7", -9);
    check("100-50+25-12+6-3", 66);
    check("2*3*4+5*6*7+8*9", 304);

    /* Error cases */
    check_err("");
    check_err("1+");
    check_err("(1+2");
    check_err("1+2)");
    check_err("1/0");          /* division by zero */
    check_err("1++");
    check_err("foo");

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
