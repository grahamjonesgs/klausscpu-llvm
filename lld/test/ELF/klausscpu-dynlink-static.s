# REQUIRES: klausscpu
## Linux -fPIC / -fPIE objects linked statically (ABI.md "Dynamic linking",
## "What stays the same"): lld fills the 8-byte GOT entries itself (the
## address zero-extended, 0 for an undefined weak symbol), calls through
## %plt or CALL18 go straight to the function, and there is no dynamic
## section, interpreter or 64 KiB page default.

# RUN: llvm-mc -triple=klausscpu-unknown-linux-musl -filetype=obj %s -o %t.o
# RUN: ld.lld -static %t.o --defsym=high=0x80001000 -o %t
# RUN: llvm-readelf -S -l %t | FileCheck %s
# RUN: llvm-objdump -d --no-show-raw-insn --no-leading-addr %t | FileCheck %s --check-prefix=DIS
# RUN: llvm-objdump -s -j .got %t | FileCheck %s --check-prefix=GOT

# CHECK-NOT: .dynamic
# CHECK-NOT: .plt
# CHECK:     .got PROGBITS 00012130 000130 000018 00 WA 0 0 8
# CHECK-NOT: .dynamic
# CHECK-NOT: .plt
# CHECK-NOT: INTERP
# CHECK:     LOAD {{.*}} 0x1000
# CHECK-NOT: INTERP
# CHECK-NOT: DYNAMIC

# DIS:      <_start>:
# DIS-NEXT:   leapc r1, 0x12130
# DIS-NEXT:   ldidx64 r1, r1, 0
# DIS-NEXT:   leapc r2, 0x12140
# DIS-NEXT:   ldidx64 r2, r2, 0
# DIS-NEXT:   leapc r3, 0x12138
# DIS-NEXT:   ldidx64 r3, r3, 0
# DIS-NEXT:   callrel 0x1112c <f>
# DIS-NEXT:   callrel 0x1112c <f>
# DIS-NEXT:   jmprel 0x1112c <f>
# DIS-EMPTY:
# DIS-NEXT: <f>:

## f, weak (0), high (0x80001000: above 2 GiB, zero-extended).
# GOT:      12130 2c110100 00000000 00000000 00000000
# GOT-NEXT: 12140 00100080 00000000

        .text
        .globl _start, f
        .weak weak
_start:
        leapc   r1, %gotpcrel(f)
        ldidx64 r1, r1, 0
        leapc   r2, %gotpcrel(high)
        ldidx64 r2, r2, 0
        leapc   r3, %gotpcrel(weak)
        ldidx64 r3, r3, 0
        callrel f
        callrel %plt(f)
        tail    %plt(f)
f:      ret
