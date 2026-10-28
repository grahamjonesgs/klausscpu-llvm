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
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/TargetParser/KlaussCPUABI.h"
#include "llvm/TargetParser/Triple.h"

using namespace lldb;
using namespace lldb_private;
namespace KlaussCPUABI = llvm::KlaussCPUABI;

LLDB_PLUGIN_DEFINE_ADV(ABISysV_klausscpu, ABIKlaussCPU)

namespace {
// DWARF register numbers: a GPR's is its number.  The PC shares LLVM's RA
// column, so the CFI return-address rule restores the PC.
constexpr uint32_t dwarf_fp = KlaussCPUABI::FPReg;
constexpr uint32_t dwarf_sp = KlaussCPUABI::DwarfSP;
constexpr uint32_t dwarf_pc = KlaussCPUABI::DwarfRA;

// CFA offsets (KlaussCPUABI.h): the return address is at CFA-8, the caller's
// r15 at CFA-16, and r15 = CFA-16 once the prologue has run.
constexpr int RetAddrSize = KlaussCPUABI::RetAddrSize;
constexpr int FPToCFAOffset = KlaussCPUABI::FPToCFAOffset;

llvm::StringRef gprName(unsigned N) { return KlaussCPUABI::GPRNames[N]; }
} // namespace

ABISP ABISysV_klausscpu::CreateInstance(ProcessSP process_sp,
                                        const ArchSpec &arch) {
  if (arch.GetTriple().getArch() != llvm::Triple::klausscpu)
    return ABISP();
  return ABISP(
      new ABISysV_klausscpu(std::move(process_sp), MakeMCRegisterInfo(arch)));
}

uint32_t ABISysV_klausscpu::GetGenericNum(llvm::StringRef name) {
  if (name == "pc")
    return LLDB_REGNUM_GENERIC_PC;
  if (name == "sp")
    return LLDB_REGNUM_GENERIC_SP;
  if (name == "flags")
    return LLDB_REGNUM_GENERIC_FLAGS;
  if (name == gprName(KlaussCPUABI::FPReg))
    return LLDB_REGNUM_GENERIC_FP;
  static_assert(KlaussCPUABI::NumArgGPRs <=
                LLDB_REGNUM_GENERIC_ARG8 - LLDB_REGNUM_GENERIC_ARG1 + 1);
  for (unsigned I = 0; I < KlaussCPUABI::NumArgGPRs; ++I)
    if (name == gprName(KlaussCPUABI::ArgGPRs[I]))
      return LLDB_REGNUM_GENERIC_ARG1 + I;
  return LLDB_INVALID_REGNUM;
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
  // Callee-saved: r4-r7 (CSR_KlaussCPU), the frame pointer r15, and SP.
  llvm::StringRef name = reg_info->name;
  if (name == "sp" || name == gprName(KlaussCPUABI::FPReg))
    return false;
  return llvm::none_of(KlaussCPUABI::CalleeSavedGPRs,
                       [&](unsigned N) { return name == gprName(N); });
}

UnwindPlanSP ABISysV_klausscpu::CreateFunctionEntryUnwindPlan() {
  // At the first instruction CALL has just pushed the return address:
  // CFA = SP+8, PC = [CFA-8], caller's SP = CFA.
  UnwindPlan::Row row;
  row.GetCFAValue().SetIsRegisterPlusOffset(dwarf_sp, RetAddrSize);
  row.SetRegisterLocationToAtCFAPlusOffset(dwarf_pc, -RetAddrSize, true);
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
  row.GetCFAValue().SetIsRegisterPlusOffset(dwarf_fp, FPToCFAOffset);
  row.SetRegisterLocationToAtCFAPlusOffset(dwarf_pc, -RetAddrSize, true);
  row.SetRegisterLocationToAtCFAPlusOffset(dwarf_fp, -FPToCFAOffset, true);
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
  // 64 bits come back in r12; 128-bit values in r12 (low) and r11 (high).  An
  // aggregate (struct, union, class, complex, GNU vector) of up to 16 bytes
  // comes back the same way, its bytes in memory order; a larger one through
  // a hidden pointer that is not returned, so it cannot be shown.  (As in the
  // AArch64 plugin, a C++ class that is returned in memory because it is not
  // trivially copyable is not told apart.)
  if (!type)
    return {};
  const uint32_t flags = type.GetTypeInfo();
  const bool is_scalar =
      flags & (eTypeIsScalar | eTypeIsPointer | eTypeIsEnumeration);
  const bool is_aggregate = flags & (eTypeIsStructUnion | eTypeIsClass |
                                     eTypeIsComplex | eTypeIsVector);
  if (!is_scalar && !is_aggregate)
    return {};
  const uint64_t size =
      llvm::expectedToOptional(type.GetByteSize(&thread)).value_or(0);
  const uint64_t max_size = is_scalar
                                ? 2 * KlaussCPUABI::SlotSize
                                : KlaussCPUABI::MaxDirectAggregateSize;
  if (size == 0 || size > max_size)
    return {};

  RegisterContextSP reg_ctx = thread.GetRegisterContext();
  if (!reg_ctx)
    return {};
  const RegisterInfo *lo =
      reg_ctx->GetRegisterInfoByName(gprName(KlaussCPUABI::RetGPRs[0]));
  const RegisterInfo *hi =
      reg_ctx->GetRegisterInfoByName(gprName(KlaussCPUABI::RetGPRs[1]));
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
  // The caller relies on the upper bits (ABI.md): a 32-bit integer comes
  // back sign-extended even when it is unsigned, a narrower one extended by
  // its signedness.
  if (num_bytes < 8 && (num_bytes == 4 || is_signed))
    raw = llvm::SignExtend64(raw, num_bytes * 8);
  RegisterContext &reg_ctx = *frame_sp->GetThread()->GetRegisterContext();
  llvm::StringRef ret_reg = gprName(KlaussCPUABI::RetGPRs[0]);
  if (!reg_ctx.WriteRegisterFromUnsigned(reg_ctx.GetRegisterInfoByName(ret_reg),
                                         raw))
    return Status::FromErrorStringWithFormatv("failed to write {0}", ret_reg);
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
