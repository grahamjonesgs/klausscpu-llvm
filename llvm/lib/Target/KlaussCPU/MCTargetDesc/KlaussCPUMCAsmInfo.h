//===-- KlaussCPUMCAsmInfo.h - KlaussCPU asm properties --------*- C++ -*-===//
//
// KlaussCPU LLVM backend — MCAsmInfo class declaration.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUMCASMINFO_H
#define LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUMCASMINFO_H

#include "llvm/MC/MCAsmInfoELF.h"

namespace llvm {
class Triple;

class KlaussCPUMCAsmInfo : public MCAsmInfoELF {
  void anchor() override;

public:
  explicit KlaussCPUMCAsmInfo(const Triple &TheTriple,
                               const MCTargetOptions &Options);

  // `%tprel(expr)`, `%gotpcrel(sym)`, `%plt(sym)`, `%gottprel(sym)`,
  // `%tlsgd(sym)` (KlaussCPU::Specifier).
  void printSpecifierExpr(raw_ostream &OS,
                          const MCSpecifierExpr &Expr) const override;
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_KLAUSSCPU_MCTARGETDESC_KLAUSSCPUMCASMINFO_H
