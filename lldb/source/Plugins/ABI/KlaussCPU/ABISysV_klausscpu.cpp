//===-- ABISysV_klausscpu.cpp ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ABISysV_klausscpu.h"

#include "lldb/Core/PluginManager.h"
#include "lldb/Symbol/UnwindPlan.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/Thread.h"
#include "lldb/Utility/DataBufferHeap.h"
#include "lldb/Utility/DataExtractor.h"
#include "lldb/Utility/RegisterValue.h"
#include "lldb/ValueObject/ValueObjectConstResult.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/TargetParser/Triple.h"

using namespace lldb;
using namespace lldb_private;

LLDB_PLUGIN_DEFINE_ADV(ABISysV_klausscpu, ABIKlaussCPU)

namespace {
// DWARF register numbers (KlaussCPURegisterInfo.td).  The PC shares number 17
// with LLVM's RA column, so the CFI return-address rule restores the PC.
enum : uint32_t {
  dwarf_r0 = 0,
  dwarf_r15 = 15,
  dwarf_sp = 16,
  dwarf_pc = 17,
};
} // namespace

ABISP ABISysV_klausscpu::CreateInstance(ProcessSP process_sp,
                                        const ArchSpec &arch) {
  if (arch.GetTriple().getArch() != llvm::Triple::klausscpu)
    return ABISP();
  return ABISP(
      new ABISysV_klausscpu(std::move(process_sp), MakeMCRegisterInfo(arch)));
}

uint32_t ABISysV_klausscpu::GetGenericNum(llvm::StringRef name) {
  return llvm::StringSwitch<uint32_t>(name)
      .Case("pc", LLDB_REGNUM_GENERIC_PC)
      .Case("sp", LLDB_REGNUM_GENERIC_SP)
      .Case("r15", LLDB_REGNUM_GENERIC_FP)
      .Case("flags", LLDB_REGNUM_GENERIC_FLAGS)
      .Case("r0", LLDB_REGNUM_GENERIC_ARG1)
      .Case("r1", LLDB_REGNUM_GENERIC_ARG2)
      .Case("r2", LLDB_REGNUM_GENERIC_ARG3)
      .Case("r3", LLDB_REGNUM_GENERIC_ARG4)
      .Default(LLDB_INVALID_REGNUM);
}

std::string ABISysV_klausscpu::GetMCName(std::string name) {
  // LLVM models the return-address column as the DWARF-only register RA; on
  // the target that value is the PC.
  if (name == "pc")
    return "ra";
  return name;
}

bool ABISysV_klausscpu::RegisterIsVolatile(const RegisterInfo *reg_info) {
  if (!reg_info)
    return true;
  // Callee-saved: r4-r7, the frame pointer r15, and SP.
  return !llvm::StringSwitch<bool>(reg_info->name)
              .Cases({"r4", "r5", "r6", "r7", "r15", "sp"}, true)
              .Default(false);
}

UnwindPlanSP ABISysV_klausscpu::CreateFunctionEntryUnwindPlan() {
  // At the first instruction CALL has just pushed the return address:
  // CFA = SP+8, PC = [CFA-8], caller's SP = CFA.
  UnwindPlan::Row row;
  row.GetCFAValue().SetIsRegisterPlusOffset(dwarf_sp, 8);
  row.SetRegisterLocationToAtCFAPlusOffset(dwarf_pc, -8, true);
  row.SetRegisterLocationToIsCFAPlusOffset(dwarf_sp, 0, true);

  auto plan_sp = std::make_shared<UnwindPlan>(eRegisterKindDWARF);
  plan_sp->AppendRow(std::move(row));
  plan_sp->SetSourceName("klausscpu at-func-entry default");
  plan_sp->SetSourcedFromCompiler(eLazyBoolNo);
  return plan_sp;
}

UnwindPlanSP ABISysV_klausscpu::CreateDefaultUnwindPlan() {
  // Every function keeps a frame pointer: after the prologue r15 points at
  // the saved caller r15, with the return address above it.
  UnwindPlan::Row row;
  row.GetCFAValue().SetIsRegisterPlusOffset(dwarf_r15, 16);
  row.SetRegisterLocationToAtCFAPlusOffset(dwarf_pc, -8, true);
  row.SetRegisterLocationToAtCFAPlusOffset(dwarf_r15, -16, true);
  row.SetRegisterLocationToIsCFAPlusOffset(dwarf_sp, 0, true);

  auto plan_sp = std::make_shared<UnwindPlan>(eRegisterKindDWARF);
  plan_sp->AppendRow(std::move(row));
  plan_sp->SetSourceName("klausscpu frame-pointer default");
  plan_sp->SetSourcedFromCompiler(eLazyBoolNo);
  plan_sp->SetUnwindPlanValidAtAllInstructions(eLazyBoolNo);
  return plan_sp;
}

ValueObjectSP
ABISysV_klausscpu::GetReturnValueObjectImpl(Thread &thread,
                                            CompilerType &type) const {
  // Scalars (integers, enums, pointers, and soft-float floating point) up to
  // 64 bits come back in r12; 128-bit values in r12 (low) and r11 (high).
  if (!type)
    return {};
  const uint32_t flags = type.GetTypeInfo();
  if (!(flags & (eTypeIsScalar | eTypeIsPointer | eTypeIsEnumeration)))
    return {};
  const uint64_t size =
      llvm::expectedToOptional(type.GetByteSize(&thread)).value_or(0);
  if (size == 0 || size > 16)
    return {};

  RegisterContextSP reg_ctx = thread.GetRegisterContext();
  if (!reg_ctx)
    return {};
  const RegisterInfo *lo = reg_ctx->GetRegisterInfoByName("r12");
  const RegisterInfo *hi = reg_ctx->GetRegisterInfoByName("r11");
  if (!lo || (size > 8 && !hi))
    return {};

  uint64_t words[2] = {reg_ctx->ReadRegisterAsUnsigned(lo, 0), 0};
  if (size > 8)
    words[1] = reg_ctx->ReadRegisterAsUnsigned(hi, 0);

  auto buf = std::make_shared<DataBufferHeap>(size, 0);
  for (uint64_t i = 0; i < size; ++i)
    buf->GetBytes()[i] = uint8_t(words[i / 8] >> (8 * (i % 8)));
  ProcessSP process_sp = thread.GetProcess();
  return ValueObjectConstResult::Create(
      &thread, type, ConstString(""), buf, eByteOrderLittle,
      process_sp ? process_sp->GetAddressByteSize() : 4);
}

Status ABISysV_klausscpu::SetReturnValueObject(StackFrameSP &frame_sp,
                                               ValueObjectSP &new_value_sp) {
  if (!new_value_sp)
    return Status::FromErrorString("empty value object for return value");
  CompilerType type = new_value_sp->GetCompilerType();
  bool is_signed = false;
  if (!type.IsIntegerOrEnumerationType(is_signed) && !type.IsPointerType())
    return Status::FromErrorString(
        "only integer and pointer return values can be set");

  DataExtractor data;
  Status error;
  size_t num_bytes = new_value_sp->GetData(data, error);
  if (error.Fail())
    return error;
  if (num_bytes > 8)
    return Status::FromErrorString("return values wider than 64 bits "
                                   "cannot be set");
  offset_t offset = 0;
  uint64_t raw = data.GetMaxU64(&offset, num_bytes);
  RegisterContext &reg_ctx = *frame_sp->GetThread()->GetRegisterContext();
  if (!reg_ctx.WriteRegisterFromUnsigned(reg_ctx.GetRegisterInfoByName("r12"),
                                         raw))
    return Status::FromErrorString("failed to write r12");
  return Status();
}

void ABISysV_klausscpu::Initialize() {
  PluginManager::RegisterPlugin(GetPluginNameStatic(),
                                "System V ABI for KlaussCPU targets",
                                CreateInstance);
}

void ABISysV_klausscpu::Terminate() {
  PluginManager::UnregisterPlugin(CreateInstance);
}
