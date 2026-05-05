/* bst.c — Binary search tree workout for KlaussCPU.
 *
 * Compile via Makefile:  make bst.elf  &&  make bst.bin
 *
 * Tests pointer chasing, malloc/free patterns, recursion with pointer args,
 * and the libc heap allocator (malloc/free/coalesce).
 *
 * Operations:
 *   - bst_insert: recursive insert
 *   - bst_find:   recursive search
 *   - bst_walk:   in-order traversal into a flat array (verifies sorted)
 *   - bst_remove: recursive delete (in-order successor for two-child nodes)
 *   - bst_free:   post-order destructor
 */

extern void  print_str(char *s);
extern void  print_int(long long n);
extern void  newline(void);
extern void *malloc(unsigned long long size);
extern void  free(void *ptr);
extern int   heap_words_used(void);
extern void  leds(unsigned long long val);
extern void  seg7(unsigned long long val);

/* ── Tree ─────────────────────────────────────────────────────────────────── */

struct node {
    long long     key;
    struct node *l;
    struct node *r;
};

static int g_pass;
static int g_fail;

static struct node *new_node(long long k) {
    struct node *n = (struct node *)malloc(sizeof(struct node));
    if (!n) return 0;
    n->key = k;
    n->l   = 0;
    n->r   = 0;
    return n;
}

static struct node *bst_insert(struct node *root, long long k) {
    if (!root) return new_node(k);
    if (k < root->key) {
        root->l = bst_insert(root->l, k);
    } else if (k > root->key) {
        root->r = bst_insert(root->r, k);
    }
    /* duplicates ignored */
    return root;
}

static int bst_find(struct node *root, long long k) {
    if (!root) return 0;
    if (k == root->key) return 1;
    if (k <  root->key) return bst_find(root->l, k);
    return bst_find(root->r, k);
}

/* In-order walk into out[]; returns updated idx (next free slot).
 * noinline: prevents the -O1 inter-procedural optimizer from devirtualising
 * the `out` pointer, which would hide writes to main.out from the caller's
 * alias analysis and cause wrong CSE of out[0] vs out[29] in main. */
__attribute__((noinline))
static int bst_walk(struct node *root, long long *out, int idx, int cap) {
    if (!root) return idx;
    idx = bst_walk(root->l, out, idx, cap);
    if (idx < cap) out[idx] = root->key;
    return bst_walk(root->r, out, idx + 1, cap);
}

static struct node *bst_min_node(struct node *root) {
    while (root && root->l) root = root->l;
    return root;
}

static struct node *bst_remove(struct node *root, long long k) {
    if (!root) return 0;
    if (k < root->key) {
        root->l = bst_remove(root->l, k);
    } else if (k > root->key) {
        root->r = bst_remove(root->r, k);
    } else {
        /* Found node to delete. */
        if (!root->l) {
            struct node *r = root->r;
            free(root);
            return r;
        }
        if (!root->r) {
            struct node *l = root->l;
            free(root);
            return l;
        }
        /* Two children: copy in-order successor's key, then delete successor. */
        struct node *succ = bst_min_node(root->r);
        root->key = succ->key;
        root->r   = bst_remove(root->r, succ->key);
    }
    return root;
}

static void bst_free(struct node *root) {
    if (!root) return;
    bst_free(root->l);
    bst_free(root->r);
    free(root);
}

static int bst_height(struct node *root) {
    if (!root) return 0;
    int hl = bst_height(root->l);
    int hr = bst_height(root->r);
    return 1 + (hl > hr ? hl : hr);
}

static int bst_count(struct node *root) {
    if (!root) return 0;
    return 1 + bst_count(root->l) + bst_count(root->r);
}

/* ── Test helpers ─────────────────────────────────────────────────────────── */

static void check(char *name, int ok) {
    print_str(name);
    if (ok) { print_str("PASS"); g_pass++; }
    else    { print_str("FAIL"); g_fail++; }
    newline();
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    g_pass = 0;
    g_fail = 0;

    print_str("=== Binary Search Tree ===");
    newline();
    leds(0x0001);  /* CP1: entered main */

    /* Build a tree with mixed insertion order. */
    static const long long keys[] = {
        50, 30, 70, 20, 40, 60, 80, 10, 25, 35, 45, 55, 65, 75, 85,
        5, 15, 22, 28, 33, 38, 42, 48, 100, -10, -20, 0, 99, 1, 2
    };
    int nkeys = (int)(sizeof(keys) / sizeof(keys[0]));   /* 30 */

    struct node *root = 0;
    for (int i = 0; i < nkeys; i++) {
        seg7(i);
        root = bst_insert(root, keys[i]);
    }

    leds(0x0002);  /* CP2: insert loop done */

    /* T1: count matches */
    check("T1  count==30:    ", bst_count(root) == 30);
    leds(0x0003);  /* CP3: T1 done */

    /* T2: every inserted key found */
    int all_found = 1;
    for (int i = 0; i < nkeys; i++)
        if (!bst_find(root, keys[i])) { all_found = 0; break; }
    check("T2  find all:     ", all_found);

    /* T3: never-inserted keys absent */
    int none_extra = 1;
    long long absent[] = {1000, -1000, 7, 11, 13, 17, 19, 23, 27, 29, 31, 37, 41};
    int nabsent = (int)(sizeof(absent) / sizeof(absent[0]));
    for (int i = 0; i < nabsent; i++)
        if (bst_find(root, absent[i])) { none_extra = 0; break; }
    check("T3  miss unknown: ", none_extra);

    /* T4: in-order walk yields sorted, deduplicated keys */
    static long long out[64];
    int idx = bst_walk(root, out, 0, 64);
    int sorted_ok = (idx == 30);
    for (int i = 0; i + 1 < 30 && sorted_ok; i++)
        if (out[i + 1] <= out[i]) sorted_ok = 0;
    check("T4  in-order:     ", sorted_ok);
    /* Use local variables to force correct loads — the compiler
     * otherwise CSEs out[29] back to out[0] after devirtualising bst_walk. */
    {
        long long min_v = out[0];
        long long max_v = out[29];
        print_str("    min="); print_int(min_v);
        print_str("  max=");   print_int(max_v);
        print_str("  height="); print_int(bst_height(root));
        newline();
    }

    /* T5: delete a leaf, an internal-with-one-child, and an internal-with-two */
    root = bst_remove(root, 5);     /* leaf */
    root = bst_remove(root, 25);    /* one or two children */
    root = bst_remove(root, 30);    /* internal, both children */
    root = bst_remove(root, 70);    /* internal, both children */
    int after_count = bst_count(root);
    check("T5a count==26:    ", after_count == 26);

    int still_present_ok = bst_find(root, 50) && bst_find(root, 40)
                        && bst_find(root, 60) && bst_find(root, 80);
    check("T5b survivors:    ", still_present_ok);

    int gone_ok = !bst_find(root, 5)  && !bst_find(root, 25)
               && !bst_find(root, 30) && !bst_find(root, 70);
    check("T5c removed:      ", gone_ok);

    /* T6: walk again — still sorted, contains exactly the surviving keys */
    idx = bst_walk(root, out, 0, 64);
    int re_sorted = (idx == 26);
    for (int i = 0; i + 1 < 26 && re_sorted; i++)
        if (out[i + 1] <= out[i]) re_sorted = 0;
    check("T6  re-walk sort: ", re_sorted);

    /* T7: deleting absent key leaves tree unchanged */
    int hgt_before = bst_height(root);
    root = bst_remove(root, 9999);
    root = bst_remove(root, -9999);
    check("T7  delete absent:", bst_count(root) == 26 &&
                                bst_height(root) == hgt_before);

    /* T8: free tree, verify heap empty */
    bst_free(root);
    root = 0;
    check("T8  free→empty:   ", heap_words_used() == 0);

    /* T9: re-build a tiny tree and tear down to exercise reuse */
    for (int i = 0; i < 5; i++) {
        struct node *t = 0;
        t = bst_insert(t, 100);
        t = bst_insert(t,  50);
        t = bst_insert(t, 150);
        bst_free(t);
    }
    check("T9  realloc cycle:", heap_words_used() == 0);

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
