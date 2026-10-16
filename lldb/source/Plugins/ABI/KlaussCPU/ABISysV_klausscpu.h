//===-- ABISysV_klausscpu.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SOURCE_PLUGINS_ABI_KLAUSSCPU_ABISYSV_KLAUSSCPU_H
#define LLDB_SOURCE_PLUGINS_ABI_KLAUSSCPU_ABISYSV_KLAUSSCPU_H

#include "lldb/Target/ABI.h"
#include "lldb/lldb-private.h"

// KlaussCPU calling convention (see llvm/lib/Target/KlaussCPU/CLAUDE.md):
// arguments r0-r3, return value r12 (r12:r11 for 128-bit), callee-saved
// r4-r7 and r15 (frame pointer), 32-bit SP and PC.  CALL pushes the 64-bit
// return address, so at function entry CFA = SP+8 and the return address is
// at CFA-8; after the prologue (ENTER or PUSH r15; GETSP r15) CFA = r15+16
// with the caller's r15 at CFA-16.  The PC has DWARF number 17, which is the
// CFI return-address column.
class ABISysV_klausscpu : public lldb_private::MCBasedABI {
public:
  ~ABISysV_klausscpu() override = default;

  size_t GetRedZoneSize() const override { return 0; }

  bool PrepareTrivialCall(lldb_private::Thread &thread, lldb::addr_t sp,
                          lldb::addr_t functionAddress,
                          lldb::addr_t returnAddress,
                          llvm::ArrayRef<lldb::addr_t> args) const override {
    // Expression evaluation that calls into the target is not supported.
    return false;
  }

  bool GetArgumentValues(lldb_private::Thread &thread,
                         lldb_private::ValueList &values) const override {
    return false;
  }

  lldb_private::Status
  SetReturnValueObject(lldb::StackFrameSP &frame_sp,
                       lldb::ValueObjectSP &new_value) override;

  lldb::UnwindPlanSP CreateFunctionEntryUnwindPlan() override;

  lldb::UnwindPlanSP CreateDefaultUnwindPlan() override;

  bool RegisterIsVolatile(const lldb_private::RegisterInfo *reg_info) override;

  bool CallFrameAddressIsValid(lldb::addr_t cfa) override {
    // The stack is 8-byte aligned and lives in 32-bit memory.
    return cfa != 0 && (cfa & 7) == 0 && cfa <= UINT32_MAX;
  }

  bool CodeAddressIsValid(lldb::addr_t pc) override {
    return (pc & 3) == 0 && pc <= UINT32_MAX;
  }

  // An interrupt frame saves {flags, INT_MASK, PC} in one 64-bit word; only
  // the low 32 bits are the code address.
  lldb::addr_t FixCodeAddress(lldb::addr_t pc) override {
    return pc & UINT32_MAX;
  }
  lldb::addr_t FixDataAddress(lldb::addr_t addr) override {
    return addr & UINT32_MAX;
  }

  bool GetPointerReturnRegister(const char *&name) override {
    name = "r12";
    return true;
  }

  static void Initialize();

  static void Terminate();

  static lldb::ABISP CreateInstance(lldb::ProcessSP process_sp,
                                    const lldb_private::ArchSpec &arch);

  static llvm::StringRef GetPluginNameStatic() { return "sysv-klausscpu"; }

  llvm::StringRef GetPluginName() override { return GetPluginNameStatic(); }

protected:
  lldb::ValueObjectSP
  GetReturnValueObjectImpl(lldb_private::Thread &thread,
                           lldb_private::CompilerType &type) const override;

  uint32_t GetGenericNum(llvm::StringRef reg) override;

  std::string GetMCName(std::string reg) override;

private:
  using lldb_private::MCBasedABI::MCBasedABI;
};

#endif // LLDB_SOURCE_PLUGINS_ABI_KLAUSSCPU_ABISYSV_KLAUSSCPU_H
