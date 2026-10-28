"""Print the Rust target spec's atomic widths and write IR that uses them.

Usage: rust-spec-atomics.py SPEC.json OUT.ll

Prints `rust atomics min M max W cas BOOL` and writes functions that use the
narrowest and widest atomic the spec offers (load, store, RMW, cmpxchg) and
an RMW twice as wide, for rust-target-spec.test to compile with llc.
"""
import json
import sys

spec = json.load(open(sys.argv[1]))
w = spec["max-atomic-width"]
m = spec.get("min-atomic-width", 8)
print("rust atomics min %d max %d cas %s" % (m, w, str(spec["atomic-cas"]).lower()))


def fn(name, n, body):
    return "define i%d @%s(ptr %%p, i%d %%v) {\n%s\n  ret i%d %%r\n}\n" % (
        n, name, n, body, n)


ir = [
    fn("load_min", m, "  %%r = load atomic i%d, ptr %%p seq_cst, align %d" % (m, m // 8)),
    fn("store_max", w, "  store atomic i%d %%v, ptr %%p seq_cst, align %d\n"
                       "  %%r = add i%d %%v, 1" % (w, w // 8, w)),
    fn("load_max", w, "  %%r = load atomic i%d, ptr %%p seq_cst, align %d" % (w, w // 8)),
    fn("rmw_max", w, "  %%r = atomicrmw add ptr %%p, i%d %%v seq_cst" % w),
    fn("cas_max", w, "  %%x = cmpxchg ptr %%p, i%d 0, i%d %%v seq_cst seq_cst\n"
                     "  %%r = extractvalue {i%d, i1} %%x, 0" % (w, w, w)),
    fn("rmw_over", 2 * w, "  %%r = atomicrmw add ptr %%p, i%d %%v seq_cst" % (2 * w)),
]
with open(sys.argv[2], "w") as f:
    f.write("\n".join(ir))
