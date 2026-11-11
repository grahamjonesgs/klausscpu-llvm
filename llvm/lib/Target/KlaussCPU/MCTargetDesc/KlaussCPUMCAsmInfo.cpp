//===-- KlaussCPUMCAsmInfo.cpp - KlaussCPU asm properties -----------------===//
//
// KlaussCPU LLVM backend — MCAsmInfo implementation.
//
//===----------------------------------------------------------------------===//

#include "KlaussCPUMCAsmInfo.h"
#include "KlaussCPUMCTargetDesc.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/TargetParser/KlaussCPUABI.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

void KlaussCPUMCAsmInfo::anchor() {}

KlaussCPUMCAsmInfo::KlaussCPUMCAsmInfo(const Triple & /*TheTriple*/,
                                        const MCTargetOptions & /*Options*/) {
  IsLittleEndian = true;             // register file is little-endian (DataLayout 'e')
  CommentString = "#";
  SupportsDebugInformation = true;
  ExceptionsType = ExceptionHandling::DwarfCFI;
  MinInstAlignment = KlaussCPUABI::MinInstBytes; // 1-3 words, word aligned
}

void KlaussCPUMCAsmInfo::printSpecifierExpr(raw_ostream &OS,
                                            const MCSpecifierExpr &Expr) const {
  StringRef Name = KlaussCPU::getSpecifierName(Expr.getSpecifier());
  if (Name.empty())
    llvm_unreachable("unknown KlaussCPU relocation specifier");
  OS << '%' << Name << '(';
  printExpr(OS, *Expr.getSubExpr());
  OS << ')';
}
