//===- KlaussCPUABITest.cpp - KlaussCPUABI.h against TableGen -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// llvm/TargetParser/KlaussCPUABI.h repeats facts the backend's TableGen files
// define (register names and DWARF numbers, the calling convention's
// registers, instruction encodings) for clang, lld and lldb.  Check that the
// two agree.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPURegisterInfo.h"
#include "KlaussCPUTargetMachine.h"
#include "MCTargetDesc/KlaussCPUInstPrinter.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInstBuilder.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/KlaussCPUABI.h"
#include "llvm/TargetParser/Triple.h"

#include "gtest/gtest.h"

#include <memory>

using namespace llvm;

// The calling convention's register lists (CC_KlaussCPU_ArgRegs,
// RetCC_KlaussCPU_ArgRegs), sorted by register number.
#define GET_CC_REGISTER_LISTS
#include "KlaussCPUGenCallingConv.inc"
#undef GET_CC_REGISTER_LISTS

// The calling convention itself (CC_KlaussCPU, RetCC_KlaussCPU), which
// assigns the registers in order.
#include "KlaussCPUGenCallingConv.inc"

namespace {

class KlaussCPUABITest : public testing::Test {
protected:
  std::unique_ptr<TargetMachine> TM;

  static void SetUpTestSuite() {
    LLVMInitializeKlaussCPUTargetInfo();
    LLVMInitializeKlaussCPUTarget();
    LLVMInitializeKlaussCPUTargetMC();
  }

  KlaussCPUABITest() {
    std::string Error;
    Triple TT("klausscpu-unknown-elf");
    const Target *TheTarget = TargetRegistry::lookupTarget(TT, Error);
    EXPECT_TRUE(TheTarget) << Error;
    if (TheTarget)
      TM.reset(TheTarget->createTargetMachine(TT, "generic", "",
                                              TargetOptions(), std::nullopt));
  }

  const MCRegisterInfo &MRI() const { return *TM->getMCRegisterInfo(); }

  // GPR hardware numbers of a register list.
  SmallVector<unsigned> numbers(ArrayRef<MCRegister> Regs) const {
    SmallVector<unsigned> N;
    for (MCRegister R : Regs)
      N.push_back(MRI().getEncodingValue(R));
    llvm::sort(N);
    return N;
  }

  // Word 0 of MI, as the code emitter encodes it.
  uint32_t encode(const MCInst &MI) const {
    MCContext Ctx(TM->getTargetTriple(), TM->getMCAsmInfo(), &MRI(),
                  TM->getMCSubtargetInfo());
    std::unique_ptr<MCCodeEmitter> CE(
        TM->getTarget().createMCCodeEmitter(*TM->getMCInstrInfo(), Ctx));
    SmallVector<char, 16> Bytes;
    SmallVector<MCFixup, 2> Fixups;
    CE->encodeInstruction(MI, Bytes, Fixups, *TM->getMCSubtargetInfo());
    EXPECT_GE(Bytes.size(), 4u);
    return support::endian::read32le(Bytes.data());
  }
};

TEST_F(KlaussCPUABITest, InstructionWords) {
  ASSERT_TRUE(TM);
  EXPECT_EQ(encode(MCInstBuilder(KlaussCPU::NOP_I)), KlaussCPUABI::NopWord);
  EXPECT_EQ(encode(MCInstBuilder(KlaussCPU::HALT_I)), KlaussCPUABI::HaltWord);
  EXPECT_EQ(encode(MCInstBuilder(KlaussCPU::TRAP_I)), KlaussCPUABI::TrapWord);
  EXPECT_EQ(encode(MCInstBuilder(KlaussCPU::JMPREL).addImm(0)),
            KlaussCPUABI::JmpRelWord);
  EXPECT_EQ(TM->getMCAsmInfo()->getMinInstAlignment(),
            KlaussCPUABI::MinInstBytes);
  // The word lld fills code gaps with must not decode as an instruction.
  EXPECT_EQ(KlaussCPUABI::InvalidWord >> 30, 0u); // LEN = 00
}

TEST_F(KlaussCPUABITest, Registers) {
  ASSERT_TRUE(TM);
  const MCRegisterClass &GPRs = MRI().getRegClass(KlaussCPU::GPRRegClassID);
  ASSERT_EQ(GPRs.getNumRegs(), KlaussCPUABI::NumGPRs);
  SmallVector<bool> Seen(KlaussCPUABI::NumGPRs);
  for (MCRegister R : GPRs) {
    unsigned N = MRI().getEncodingValue(R);
    ASSERT_LT(N, KlaussCPUABI::NumGPRs);
    Seen[N] = true;
    EXPECT_STREQ(KlaussCPUInstPrinter::getRegisterName(R),
                 KlaussCPUABI::GPRNames[N]);
    EXPECT_EQ(MRI().getDwarfRegNum(R, /*isEH=*/false), int(N));
  }
  EXPECT_TRUE(llvm::all_of(Seen, [](bool B) { return B; }));

  EXPECT_EQ(MRI().getDwarfRegNum(KlaussCPU::SP, false),
            int(KlaussCPUABI::DwarfSP));
  EXPECT_EQ(MRI().getDwarfRegNum(MRI().getRARegister(), false),
            int(KlaussCPUABI::DwarfRA));
  EXPECT_EQ(MRI().getEncodingValue(KlaussCPU::R15), KlaussCPUABI::FPReg);
}

TEST_F(KlaussCPUABITest, CallingConvention) {
  ASSERT_TRUE(TM);
  auto Sorted = [](ArrayRef<unsigned> A) {
    SmallVector<unsigned> V(A);
    llvm::sort(V);
    return V;
  };
  EXPECT_EQ(numbers(CC_KlaussCPU_ArgRegs), Sorted(KlaussCPUABI::ArgGPRs));
  EXPECT_EQ(numbers(RetCC_KlaussCPU_ArgRegs), Sorted(KlaussCPUABI::RetGPRs));

  KlaussCPURegisterInfo TRI;
  SmallVector<MCRegister> CSRs;
  for (const MCPhysReg *R = TRI.getCalleeSavedRegs(nullptr); *R; ++R)
    CSRs.push_back(*R);
  EXPECT_EQ(numbers(CSRs), Sorted(KlaussCPUABI::CalleeSavedGPRs));
}

// The register lists above are sorted, but their order matters too: lldb
// reads ArgGPRs[I] as argument I + 1 and RetGPRs[0]/[1] as the low and high
// halves of an i128.  Run the calling convention and check the order it
// assigns, and the stack slots after the registers.
TEST_F(KlaussCPUABITest, CallingConventionOrder) {
  ASSERT_TRUE(TM);
  LLVMContext Ctx;
  Module M("cc", Ctx);
  M.setDataLayout(TM->createDataLayout());
  Function *F = Function::Create(FunctionType::get(Type::getVoidTy(Ctx), false),
                                 GlobalValue::ExternalLinkage, "f", M);
  MachineModuleInfo MMI(TM.get());
  MachineFunction MF(*F, *TM, *TM->getSubtargetImpl(*F), MMI.getContext(), 0);

  auto RegNo = [&](const CCValAssign &VA) {
    return MRI().getEncodingValue(VA.getLocReg());
  };

  // Arguments: one i64 per argument register, in order, then stack slots.
  // A byte allocated first shows the slots' alignment.
  {
    SmallVector<CCValAssign> Locs;
    CCState State(CallingConv::C, /*IsVarArg=*/false, MF, Locs, Ctx);
    unsigned NumArgs = KlaussCPUABI::NumArgGPRs + 2;
    for (unsigned I = 0; I != NumArgs; ++I) {
      if (I == KlaussCPUABI::NumArgGPRs)
        State.AllocateStack(1, Align(1));
      // The last one is an i32, which takes a full slot like an i64.
      MVT VT = I + 1 == NumArgs ? MVT::i32 : MVT::i64;
      ASSERT_FALSE(CC_KlaussCPU(I, VT, VT, CCValAssign::Full,
                                ISD::ArgFlagsTy(), nullptr, State));
    }
    ASSERT_EQ(Locs.size(), NumArgs);
    for (unsigned I = 0; I != KlaussCPUABI::NumArgGPRs; ++I) {
      ASSERT_TRUE(Locs[I].isRegLoc());
      EXPECT_EQ(RegNo(Locs[I]), KlaussCPUABI::ArgGPRs[I]) << "argument " << I;
    }
    const CCValAssign &S0 = Locs[KlaussCPUABI::NumArgGPRs];
    const CCValAssign &S1 = Locs[KlaussCPUABI::NumArgGPRs + 1];
    ASSERT_TRUE(S0.isMemLoc());
    ASSERT_TRUE(S1.isMemLoc());
    EXPECT_EQ(S0.getLocMemOffset(), int64_t(KlaussCPUABI::StackAlign));
    EXPECT_EQ(S1.getLocMemOffset(),
              int64_t(KlaussCPUABI::StackAlign + KlaussCPUABI::SlotSize));
    EXPECT_EQ(S1.getLocVT(), MVT::i64);
    EXPECT_EQ(State.getStackSize(),
              uint64_t(KlaussCPUABI::StackAlign + 2 * KlaussCPUABI::SlotSize));
  }

  // Return values: an i128 is two i64s, low half first.
  {
    SmallVector<CCValAssign> Locs;
    CCState State(CallingConv::C, /*IsVarArg=*/false, MF, Locs, Ctx);
    for (unsigned I = 0; I != std::size(KlaussCPUABI::RetGPRs); ++I)
      ASSERT_FALSE(RetCC_KlaussCPU(I, MVT::i64, MVT::i64, CCValAssign::Full,
                                   ISD::ArgFlagsTy(), nullptr, State));
    for (unsigned I = 0; I != std::size(KlaussCPUABI::RetGPRs); ++I) {
      ASSERT_TRUE(Locs[I].isRegLoc());
      EXPECT_EQ(RegNo(Locs[I]), KlaussCPUABI::RetGPRs[I]) << "return " << I;
    }
    // No more return registers: a third i64 is returned in memory.
    EXPECT_TRUE(RetCC_KlaussCPU(2, MVT::i64, MVT::i64, CCValAssign::Full,
                                ISD::ArgFlagsTy(), nullptr, State));
  }
}

TEST_F(KlaussCPUABITest, Frame) {
  ASSERT_TRUE(TM);
  // The CIE's initial rules: CFA = SP + RetAddrSize, RA at CFA - RetAddrSize.
  ArrayRef<MCCFIInstruction> Init = TM->getMCAsmInfo()->getInitialFrameState();
  ASSERT_EQ(Init.size(), 2u);
  EXPECT_EQ(Init[0].getRegister(), KlaussCPUABI::DwarfSP);
  EXPECT_EQ(Init[0].getOffset(), int(KlaussCPUABI::RetAddrSize));
  EXPECT_EQ(Init[1].getRegister(), KlaussCPUABI::DwarfRA);
  EXPECT_EQ(Init[1].getOffset(), -int(KlaussCPUABI::RetAddrSize));
  EXPECT_EQ(KlaussCPUABI::IncomingArgOffset,
            KlaussCPUABI::OutgoingArgOffset + KlaussCPUABI::RetAddrSize);
}

TEST_F(KlaussCPUABITest, DataLayout) {
  ASSERT_TRUE(TM);
  // Pointers are 64 bits, the address space 32 (the upper half is ignored).
  EXPECT_EQ(TM->createDataLayout().getPointerSizeInBits(), 64u);
  EXPECT_EQ(KlaussCPUABI::MaxAddress, maxUIntN(KlaussCPUABI::AddressBits));
}

} // namespace
