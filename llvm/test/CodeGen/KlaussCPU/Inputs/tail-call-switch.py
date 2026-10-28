"""Write IR for fuse-cmp-br-tail-size.ll: a fusable compare (a < 0) whose branch
jumps over a dense switch of N cases, each case block nothing but a tail call.

Usage: tail-call-switch.py N direct|indirect

direct:   case K is `tail call @fK(%x)`, a PseudoTAIL (`tail fK`).
indirect: every case is `tail call %fp(%x)`, a PseudoTAILIndirect (`jmpr`);
          the test turns tail merging off so that the cases stay apart.
The jump table holds the case addresses, so every case block is one tail
jump and the branch's distance is about 4 * N bytes.
"""
import sys

n = int(sys.argv[1])
indirect = sys.argv[2] == "indirect"
out = []
if not indirect:
    out += ["declare i64 @f%d(i64)" % k for k in range(n)]
out.append("declare i64 @far(i64)")
out.append("define i64 @t(i64 %x, ptr %fp, i64 %a, i64 %i) {")
out.append("entry:")
out.append("  %c = icmp slt i64 %a, 0")
out.append("  br i1 %c, label %farbb, label %sw")
out.append("sw:")
out.append("  switch i64 %i, label %def [")
out += ["    i64 %d, label %%c%d" % (k, k) for k in range(n)]
out.append("  ]")
for k in range(n):
    callee = "%fp" if indirect else "@f%d" % k
    out.append("c%d:" % k)
    out.append("  %%r%d = tail call i64 %s(i64 %%x)" % (k, callee))
    out.append("  ret i64 %%r%d" % k)
out.append("def:")
out.append("  ret i64 0")
out.append("farbb:")
out.append("  %rf = tail call i64 @far(i64 %x)")
out.append("  ret i64 %rf")
out.append("}")
print("\n".join(out))
