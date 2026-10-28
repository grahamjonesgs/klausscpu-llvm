// A global that --gc-sections removed keeps its DIE, and lld writes the -1
// tombstone (0xffffffff) for its address (DW_OP_addr, or the .debug_addr
// entry of DW_OP_addrx): KlaussCPU's default, which -z dead-reloc-in-nonalloc
// gives on other targets.  lldb must treat the variable as not in the program
// instead of reading address 0xffffffff.

// REQUIRES: lld
// RUN: %clang --target=klausscpu-unknown-elf -g -gdwarf-4 -O1 \
// RUN:   -ffunction-sections -fdata-sections -c %s -o %t4.o
// RUN: ld.lld --gc-sections -e main %t4.o -o %t4
// RUN: llvm-dwarfdump --name=dropped %t4 | FileCheck --check-prefix=DWARF4 %s
// RUN: not %lldb %t4 -b -o "target variable kept" -o "target variable dropped" \
// RUN:   2>&1 | FileCheck %s
// RUN: %clang --target=klausscpu-unknown-elf -g -gdwarf-5 -O1 \
// RUN:   -ffunction-sections -fdata-sections -c %s -o %t5.o
// RUN: ld.lld --gc-sections -e main %t5.o -o %t5
// RUN: llvm-dwarfdump --debug-addr %t5 | FileCheck --check-prefix=DWARF5 %s
// RUN: not %lldb %t5 -b -o "target variable kept" -o "target variable dropped" \
// RUN:   2>&1 | FileCheck %s

// DWARF4: DW_AT_name ("dropped")
// DWARF4: DW_AT_location (DW_OP_addr 0xffffffff)
// DWARF5: 0xffffffff

// CHECK: (lldb) target variable kept
// CHECK-NEXT: (long) kept = 7
// CHECK: (lldb) target variable dropped
// CHECK-NEXT: error: can't find global variable 'dropped'

long kept = 7;
long dropped = 5;

int main(void) { return (int)kept; }
