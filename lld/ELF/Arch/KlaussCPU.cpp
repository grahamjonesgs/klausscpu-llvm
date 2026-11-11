//===- KlaussCPU.cpp ------------------------------------------------------===//
//
// LLD ELF backend for KlaussCPU — a home-designed 64-bit CPU on FPGA.
//
// ELF: ELFCLASS32 / ELFDATA2LSB / EM_KLAUSSCPU (0x4B43)
//
// Relocation types:
//   R_KLAUSSCPU_ABS32  (1) — 32-bit absolute, LE — instruction word-1 slot
//                             (branch/call target) or 4-byte data reference.
//   R_KLAUSSCPU_ABS64  (2) — 64-bit absolute, LE — 8-byte pointer field in
//                             .rodata/.data (struct members, pointer arrays):
//                             S + A in 64 bits, the address S zero-extended.
//   R_KLAUSSCPU_PCREL32(3) — 32-bit signed PC-relative, LE — word-1 slot of
//                             JMPREL/JMPxxREL/CALLREL/LEAPC instructions.
//                             field_value = S + A − instr_addr.
//                             lld computes S + A − (instr_addr+4) (R_PC) and
//                             relocate() adds KlaussCPUABI::Word1Offset (4)
//                             for the hardware convention.
//   R_KLAUSSCPU_PC32   (4) — 32-bit signed PC-relative *data* — a cross-section
//                             symbol difference (A − B) in .rodata/.data, e.g.
//                             the -fPIC relative lookup tables emitted by
//                             RelLookupTableConverter (`.long str - table`).
//                             field_value = S + A − P exactly (NO +4: unlike the
//                             instruction form, the fixup slot IS the PC origin).
//   R_KLAUSSCPU_PC64   (5) — 64-bit PC-relative data, as PC32 for an 8-byte
//                             field (`.quad A − B`, e.g. InstrProf's
//                             __profd_* CounterPtr).
//   R_KLAUSSCPU_CALL18 (6) — ISA v3 short CALL (word 0 0x63000000), or the
//                             short jump of a tail call (0x61000000): simm18
//                             word displacement (S + A − P) / 4 in word0[17:0],
//                             P = the instruction (r_offset).  It reaches
//                             ±512 KiB; a callee further away is called
//                             through a thunk, `jmprel S + A` (Thunks.cpp),
//                             placed within reach.  The thunk returns
//                             nothing itself: the callee returns straight to
//                             the call's PC + 4.  In a dynamic link a call to a
//                             preemptible function goes to its PLT entry
//                             (R_PLT_PC), and a thunk to that entry.
//   R_KLAUSSCPU_TPREL32(7) — TLS local exec, `%tprel(sym)`: the 32-bit signed
//                             offset S + A − TP of a TLS symbol from the
//                             thread pointer, in the word-1 slot of an ADDI,
//                             an indexed load / store or a SETR.  TLS
//                             variant II, as musl lays it out by default
//                             (getTlsTpOffset): TP points at the thread
//                             structure with the static TLS block below it,
//                             so the offset is negative.  The compiler emits
//                             it for every TLS access in code that is not
//                             PIC, and for local exec in a PIE.
//
//   R_KLAUSSCPU_GOTPCREL32 (8), R_KLAUSSCPU_PLT32 (9),
//   R_KLAUSSCPU_TLS_GOTTPREL32 (10), R_KLAUSSCPU_TLS_GD32 (11) — the word-1
//                             immediate of a 2-word `leapc` / `callrel` /
//                             `jmprel`, relative to the instruction as
//                             PCREL32 is: G + A − I (the symbol's GOT entry),
//                             L + A − I (its PLT entry, or S), GT + A − I (its
//                             TP-offset GOT entry), GD + A − I (its
//                             general-dynamic GOT pair).
//
// Dynamic linking (ABI.md "Dynamic linking"): static executables, PIE and
// shared libraries for musl's dynamic linker.  The file is ELFCLASS32 with
// RELA relocations (12-byte Elf32_Rela), but every field the dynamic linker
// writes is 64 bits: GOT and .got.plt entries are 8 bytes (gotEntrySize, as
// x86-64's x32), with no reserved header entries, and the dynamic
// relocations COPY, GLOB_DAT, JUMP_SLOT, RELATIVE, ABS64, TPOFF64, DTPMOD64
// and DTPOFF64 (2, 12-18) write 8 bytes.  A GOT entry lld fills itself (a
// static link, a symbol that cannot be preempted) holds the same 64-bit
// value: an address zero-extended, a TP offset sign-extended, a module ID.
// There is no lazy binding: no PLT header, -z now by default for -shared and
// -pie (Driver.cpp), and each 16-byte PLT entry loads its .got.plt slot,
// which a JUMP_SLOT filled at load.  A short call (CALL18) to a preemptible
// function goes to its PLT entry, through a thunk if out of reach.  Shared
// libraries and PIE get 64 KiB pages; static links keep lld's defaults.
// Not supported (link errors): IFUNC, RELR, TLSDESC, local dynamic, 32-bit
// dynamic relocations.
//
// Code loaded at run time outside Linux is relocated by its loader, from an
// ET_REL object (Zephyr llext) or from the RELA sections kept by
// --emit-relocs.
//
// The gaps between code sections are filled with KlaussCPUABI::InvalidWord,
// an architecturally reserved word that traps as an invalid opcode.
//
//===----------------------------------------------------------------------===//

#include "InputFiles.h"
#include "InputSection.h"
#include "RelocScan.h"
#include "Symbols.h"
#include "SyntheticSections.h"
#include "Target.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/Endian.h"
#include "llvm/TargetParser/KlaussCPUABI.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::support::endian;
using namespace llvm::ELF;
using namespace lld;
using namespace lld::elf;

namespace {
class KlaussCPU final : public TargetInfo {
public:
  KlaussCPU(Ctx &);
  uint32_t calcEFlags() const override;
  RelExpr getRelExpr(RelType type, const Symbol &s,
                     const uint8_t *loc) const override;
  RelType getDynRel(RelType type) const override;
  void writePlt(uint8_t *buf, const Symbol &sym,
                uint64_t pltEntryAddr) const override;
  template <class ELFT, class RelTy>
  void scanSectionImpl(InputSectionBase &, Relocs<RelTy>);
  void scanSection(InputSectionBase &) override;
  void relocate(uint8_t *loc, const Relocation &rel,
                uint64_t val) const override;
  bool needsThunk(RelExpr expr, RelType type, const InputFile *file,
                  uint64_t branchAddr, const Symbol &s,
                  int64_t a) const override;
  uint32_t getThunkSectionSpacing() const override;
  bool inBranchRange(RelType type, uint64_t src,
                     uint64_t dst) const override;
};
} // namespace

KlaussCPU::KlaussCPU(Ctx &ctx) : TargetInfo(ctx) {
  // Fill between code sections: a reserved word (LEN = 00, class 0), which
  // the CPU traps on as an invalid opcode.  Not TRAP, the debugger's
  // breakpoint, nor HALT, which would look like a clean exit.
  write32le(trapInstr.data(), KlaussCPUABI::InvalidWord);
  needsThunks = true;

  // Dynamic linking (ABI.md).  No IRELATIVE, TLSDESC or local-dynamic
  // relocation: iRelativeRel, tlsDescRel stay 0.
  copyRel = R_KLAUSSCPU_COPY;
  gotRel = R_KLAUSSCPU_GLOB_DAT;
  pltRel = R_KLAUSSCPU_JUMP_SLOT;
  relativeRel = R_KLAUSSCPU_RELATIVE;
  symbolicRel = R_KLAUSSCPU_ABS64;
  tlsGotRel = R_KLAUSSCPU_TPOFF64;
  tlsModuleIndexRel = R_KLAUSSCPU_DTPMOD64;
  tlsOffsetRel = R_KLAUSSCPU_DTPOFF64;
  // Every GOT and .got.plt entry holds a 64-bit pointer (ELFCLASS32 sets
  // wordsize 4), and nothing reads reserved entries: musl binds every
  // JUMP_SLOT at load, so there is no PLT header either.
  gotEntrySize = 8;
  gotPltHeaderEntriesNum = 0;
  pltHeaderSize = 0;
  pltEntrySize = KlaussCPUABI::PltEntrySize;
  ipltEntrySize = KlaussCPUABI::PltEntrySize;

  // Shared libraries and PIE run under Linux with 64 KiB pages, the TLB's
  // only size, so that every PT_LOAD and the end of PT_GNU_RELRO are page
  // aligned.  Static links (bare metal, static Linux executables) keep the
  // defaults: their layout must not move.  The target is created after the
  // options are read, so -shared and -pie are known here.
  if (ctx.arg.shared || ctx.arg.pie) {
    defaultMaxPageSize = 0x10000;
    defaultCommonPageSize = 0x10000;
  }

  // ELF32 RELR describes 4-byte words; the fields here are 8 bytes.  REL
  // would keep the addends in those fields, which musl does not read.
  if (ctx.arg.relrPackDynRelocs)
    ErrAlways(ctx) << "-z pack-relative-relocs and --pack-dyn-relocs=relr "
                      "are not supported for KlaussCPU";
  if (ctx.arg.isPic && !ctx.arg.isRela)
    ErrAlways(ctx) << "-z rel is not supported for KlaussCPU shared libraries "
                      "and PIE";
}

// The low byte of e_flags is the calling convention the code follows
// (KlaussCPUABI::ABIVersion; 0 or 2 for code built before ABI version 3).
// Objects and shared libraries with code must agree: one compiled for another
// convention, such as one a build system left behind from an older toolchain,
// would otherwise call and be called wrongly.  Files without code (data,
// -b binary) do not count.
//
// This also rejects STT_GNU_IFUNC definitions: musl has no IFUNC, and there
// is no R_KLAUSSCPU_IRELATIVE.
uint32_t KlaussCPU::calcEFlags() const {
  const InputFile *first = nullptr;
  uint32_t target = KlaussCPUABI::ABIVersion;
  auto check = [&](const InputFile *f, bool hasCode, uint32_t eflags) {
    if (!hasCode)
      return;
    uint32_t abi = eflags & KlaussCPUABI::EFlagsABIMask;
    if (!first) {
      first = f;
      target = abi;
    } else if (abi != target) {
      Err(ctx) << f << ": KlaussCPU ABI version " << abi
               << " cannot be linked with ABI version " << target << " in "
               << first << "; rebuild it with the current toolchain";
    }
  };
  for (InputFile *f : ctx.objectFiles) {
    auto *obj = cast<ObjFile<ELF32LE>>(f);
    bool hasCode = llvm::any_of(obj->getSections(), [](InputSectionBase *s) {
      return s && s != &InputSection::discarded &&
             (s->flags & SHF_EXECINSTR) && s->getSize() != 0;
    });
    check(f, hasCode, obj->getObj().getHeader().e_flags);
    for (Symbol *sym : obj->getSymbols())
      if (sym->isGnuIFunc() && sym->isDefined() && sym->file == f)
        Err(ctx) << f << ": STT_GNU_IFUNC symbol " << sym
                 << " is not supported for KlaussCPU";
  }
  for (SharedFile *f : ctx.sharedFiles) {
    bool hasCode =
        llvm::any_of(f->getELFShdrs<ELF32LE>(), [](const ELF32LE::Shdr &s) {
          return (s.sh_flags & SHF_EXECINSTR) && s.sh_size != 0;
        });
    check(f, hasCode, f->getObj<ELF32LE>().getHeader().e_flags);
  }
  return target;
}

// The RelExpr of each type, for the sections the generic code relocates
// (non-SHF_ALLOC sections, .eh_frame); scanSectionImpl handles the rest.
RelExpr KlaussCPU::getRelExpr(RelType type, const Symbol &s,
                               const uint8_t *loc) const {
  switch (type) {
  case R_KLAUSSCPU_NONE:
    return R_NONE;
  case R_KLAUSSCPU_ABS32:
  case R_KLAUSSCPU_ABS64:
    return R_ABS;
  case R_KLAUSSCPU_PCREL32:
  case R_KLAUSSCPU_PC32:
  case R_KLAUSSCPU_PC64:
    return R_PC;
  case R_KLAUSSCPU_CALL18:
  case R_KLAUSSCPU_PLT32:
    return R_PLT_PC;
  case R_KLAUSSCPU_GOTPCREL32:
  case R_KLAUSSCPU_TLS_GOTTPREL32:
    return R_GOT_PC;
  case R_KLAUSSCPU_TLS_GD32:
    return R_TLSGD_PC;
  case R_KLAUSSCPU_TPREL32:
    return R_TPREL;
  default:
    Err(ctx) << getErrorLoc(ctx, loc) << "unrecognized relocation " << type;
    return R_NONE;
  }
}

// The only static relocation with a dynamic form is the 64-bit pointer.
// The 32-bit ones (ABS32, PCREL32, PC32) have none: against a preemptible
// symbol in a shared library they are errors, in a PIE a copy relocation or
// a canonical PLT entry.
RelType KlaussCPU::getDynRel(RelType type) const {
  if (type == R_KLAUSSCPU_ABS64)
    return R_KLAUSSCPU_ABS64;
  return R_KLAUSSCPU_NONE;
}

// PLT entry (ABI.md "PLT"), no header:
//   +0   leapc   r14, slot - entry     8B800E00 dddddddd
//   +8   ldidx64 r14, r14, 0           5B300EE0
//   +12  jmpr    r14                   6080000E
// R14 is caller-saved and neither an argument nor a return register; the
// entry changes neither SP nor R15, so the CIE's unwind rule holds in it.
void KlaussCPU::writePlt(uint8_t *buf, const Symbol &sym,
                         uint64_t pltEntryAddr) const {
  write32le(buf, KlaussCPUABI::PltLeaPcR14Word);
  write32le(buf + 4,
            static_cast<uint32_t>(sym.getGotPltVA(ctx) - pltEntryAddr));
  write32le(buf + 8, KlaussCPUABI::PltLdIdx64R14Word);
  write32le(buf + 12, KlaussCPUABI::PltJmpRR14Word);
}

template <class ELFT, class RelTy>
void KlaussCPU::scanSectionImpl(InputSectionBase &sec, Relocs<RelTy> rels) {
  RelocScan rs(ctx, &sec);
  sec.relocations.reserve(rels.size());

  for (auto it = rels.begin(); it != rels.end(); ++it) {
    RelType type = it->getType(false);
    uint32_t symIndex = it->getSymbol(false);
    Symbol &sym = sec.getFile<ELFT>()->getSymbol(symIndex);
    uint64_t offset = it->r_offset;
    if (type == R_KLAUSSCPU_NONE)
      continue;
    if (sym.isUndefined() && symIndex != 0 &&
        rs.maybeReportUndefined(cast<Undefined>(sym), offset))
      continue;
    int64_t addend = rs.getAddend<ELFT>(*it, type);
    const uint8_t *loc = sec.content().data() + offset;
    // A reference to an IFUNC another module defines (definitions are
    // rejected in calcEFlags).
    if (LLVM_UNLIKELY(sym.isGnuIFunc())) {
      Err(ctx) << getErrorLoc(ctx, loc) << "relocation " << type
               << " against STT_GNU_IFUNC symbol " << &sym
               << " is not supported for KlaussCPU";
      continue;
    }
    RelExpr expr;
    switch (type) {
    // Absolute: ABS64 becomes RELATIVE or a dynamic ABS64 in a writable
    // section of a shared library or PIE; ABS32 has no dynamic form.
    case R_KLAUSSCPU_ABS32:
    case R_KLAUSSCPU_ABS64:
      expr = R_ABS;
      break;

    // PC-relative, against the symbol itself.
    case R_KLAUSSCPU_PCREL32:
    case R_KLAUSSCPU_PC32:
    case R_KLAUSSCPU_PC64:
      rs.processR_PC(type, offset, addend, sym);
      continue;

    // Calls: the PLT entry of a preemptible function, else the function.
    case R_KLAUSSCPU_CALL18:
    case R_KLAUSSCPU_PLT32:
      rs.processR_PLT_PC(type, offset, addend, sym);
      continue;

    // The symbol's GOT entry: GLOB_DAT if preemptible, else RELATIVE (PIC)
    // or the address (static).
    case R_KLAUSSCPU_GOTPCREL32:
      if (sym.isTls()) {
        Err(ctx) << getErrorLoc(ctx, loc) << "relocation " << type
                 << " against TLS symbol " << &sym;
        continue;
      }
      expr = R_GOT_PC;
      break;

    // TLS.  Neither IE nor GD is relaxed (the code sequences keep their
    // loads and the call), so lld fills their GOT entries itself where it
    // can: a TP offset (IE), module 1 and the offset (GD).
    case R_KLAUSSCPU_TLS_GOTTPREL32:
    case R_KLAUSSCPU_TLS_GD32:
      if (!sym.isTls() && !sym.isUndefined()) {
        Err(ctx) << getErrorLoc(ctx, loc) << "relocation " << type
                 << " against non-TLS symbol " << &sym;
        continue;
      }
      if (type == R_KLAUSSCPU_TLS_GOTTPREL32)
        rs.handleTlsIe</*enableIeToLe=*/false>(R_GOT_PC, type, offset, addend,
                                               sym);
      else
        rs.handleTlsGd(R_TLSGD_PC, R_NONE, R_NONE, type, offset, addend, sym);
      continue;

    // TLS local exec: a TP offset, known only in the executable that holds
    // the TLS block, and only for its own TLS.
    case R_KLAUSSCPU_TPREL32:
      if (ctx.arg.shared) {
        Err(ctx) << getErrorLoc(ctx, loc) << "relocation " << type
                 << " against " << &sym << " cannot be used with -shared";
        continue;
      }
      if (!sym.isTls() && !sym.isUndefined()) {
        Err(ctx) << getErrorLoc(ctx, loc) << "relocation " << type
                 << " against non-TLS symbol " << &sym;
        continue;
      }
      if (sym.isPreemptible) {
        Err(ctx) << getErrorLoc(ctx, loc) << "relocation " << type
                 << " against " << &sym
                 << " defined in a shared library; recompile with -fPIE";
        continue;
      }
      expr = R_TPREL;
      break;

    default:
      Err(ctx) << getErrorLoc(ctx, loc) << "unrecognized relocation " << type;
      continue;
    }
    rs.process(expr, type, offset, sym, addend);
  }
}

void KlaussCPU::scanSection(InputSectionBase &sec) {
  elf::scanSection1<KlaussCPU, ELF32LE>(*this, sec);
}

void KlaussCPU::relocate(uint8_t *loc, const Relocation &rel,
                          uint64_t val) const {
  switch (rel.type) {
  case R_KLAUSSCPU_NONE:
    break;
  case R_KLAUSSCPU_ABS32:
    // Unsigned address, but also accept -1 (the .debug_* tombstone).
    checkIntUInt(ctx, loc, val, 32, rel);
    write32le(loc, static_cast<uint32_t>(val));
    break;
  case R_KLAUSSCPU_ABS64:
    // 8-byte field in .rodata/.data, or a GOT entry lld fills: the exact
    // 64-bit S + A, with the 32-bit address S zero-extended as SETRU, LEAPC
    // and the assembler's own FK_Data_8 build it.  lld hands an ELFCLASS32
    // target `val` sign-extended from 32 bits, which cannot tell an address
    // >= 2 GiB (positive) from a negative S + A (`(intptr_t)&x - 0x100000`
    // with x at a low address), so recompute it from the symbol: S + A in 64
    // bits, as the code computes it (KlaussCPUISelLowering keeps a negative
    // offset out of SETRU).
    if (rel.expr == R_ABS && rel.sym) {
      write64le(loc, rel.sym->getVA(ctx, rel.addend));
      break;
    }
    // An initial-exec GOT entry in an executable: the TP offset, negative,
    // sign-extended (`val` is).
    if (rel.expr == R_TPREL) {
      write64le(loc, val);
      break;
    }
    // A non-alloc section (relocateNonAlloc: expr R_NONE, no symbol), i.e.
    // .debug_*, where S + A is an address: zero-extend the low half, except
    // the all-ones tombstone of a reference to a discarded section, which
    // DWARF consumers expect.  (A live reference to exactly 0xFFFFFFFF also
    // reaches here as all-ones; relocateNoSym passes nothing to tell them
    // apart; nothing the compiler describes in DWARF lives there.)  Also
    // the module ID 1 of a general-dynamic GOT pair in an executable
    // (R_ADDEND).
    if (val == UINT64_MAX)
      write64le(loc, val);
    else
      write64le(loc, static_cast<uint32_t>(val));
    break;
  case R_KLAUSSCPU_PCREL32:
  case R_KLAUSSCPU_GOTPCREL32:
  case R_KLAUSSCPU_PLT32:
  case R_KLAUSSCPU_TLS_GOTTPREL32:
  case R_KLAUSSCPU_TLS_GD32:
    // val = X + A − (instr_addr+4) from lld's R_PC / R_GOT_PC / R_PLT_PC /
    // R_TLSGD_PC computation.  Hardware uses instr_addr as PC origin, so
    // field = val + 4.
    checkInt(ctx, loc, static_cast<int64_t>(val) + KlaussCPUABI::Word1Offset,
             32, rel);
    write32le(loc, static_cast<uint32_t>(val + KlaussCPUABI::Word1Offset));
    break;
  case R_KLAUSSCPU_PC32:
    // Data symbol difference (A − B): the fixup slot itself is the PC origin,
    // so the field is exactly S + A − P — no instruction "+4" correction.
    checkInt(ctx, loc, static_cast<int64_t>(val), 32, rel);
    write32le(loc, static_cast<uint32_t>(val));
    break;
  case R_KLAUSSCPU_PC64:
    // val is already sign-extended from 32 bits (ELFCLASS32), which is
    // exactly the 64-bit difference.
    write64le(loc, val);
    break;
  case R_KLAUSSCPU_TPREL32:
    // A negative offset from TP (variant II): signed 32 bits, which the
    // instruction sign-extends.
    checkInt(ctx, loc, static_cast<int64_t>(val), 32, rel);
    write32le(loc, static_cast<uint32_t>(val));
    break;
  case R_KLAUSSCPU_CALL18:
    // val = L + A − P with P the instruction itself.  A callee out of reach
    // was redirected to a thunk (needsThunk), so this only fails for a
    // misaligned target.
    checkAlignment(ctx, loc, val, 4, rel);
    checkInt(ctx, loc, static_cast<int64_t>(val), 20, rel);
    write32le(loc, (read32le(loc) & ~0x3FFFFu) |
                       (static_cast<uint32_t>(val >> 2) & 0x3FFFFu));
    break;
  case R_KLAUSSCPU_DTPOFF64:
  case R_KLAUSSCPU_TPOFF64:
  case R_KLAUSSCPU_DTPMOD64:
    // The offset of a TLS symbol in its module's TLS segment (the second
    // entry of a general-dynamic GOT pair lld fills itself), or an addend
    // written with --apply-dynamic-relocs: signed, sign-extended from 32
    // bits as `val` is.
    write64le(loc, val);
    break;
  case R_KLAUSSCPU_GLOB_DAT:
  case R_KLAUSSCPU_JUMP_SLOT:
  case R_KLAUSSCPU_RELATIVE:
    // An address (--apply-dynamic-relocs): zero-extended.
    write64le(loc, static_cast<uint32_t>(val));
    break;
  default:
    llvm_unreachable("unrecognized KlaussCPU relocation");
  }
}

// A short call whose callee is out of reach goes through a thunk.  An
// undefined weak callee resolves to address 0, which the thunk reaches as
// CALLREL would have.  A call to a preemptible function (R_PLT_PC) goes to
// its PLT entry, so that is the address the range is checked against and
// the thunk jumps to (KlaussCPUThunk).
bool KlaussCPU::needsThunk(RelExpr expr, RelType type, const InputFile *file,
                           uint64_t branchAddr, const Symbol &s,
                           int64_t a) const {
  if (type != R_KLAUSSCPU_CALL18)
    return false;
  uint64_t dst = expr == R_PLT_PC ? s.getPltVA(ctx) + a : s.getVA(ctx, a);
  return !inBranchRange(type, branchAddr, dst);
}

// ThunkSections every 448 KiB keep one within the ±512 KiB reach of every
// short call, with 64 KiB to spare for the thunks themselves.
uint32_t KlaussCPU::getThunkSectionSpacing() const {
  return 512 * 1024 - 0x10000;
}

bool KlaussCPU::inBranchRange(RelType type, uint64_t src, uint64_t dst) const {
  if (type != R_KLAUSSCPU_CALL18)
    return true;
  return isInt<20>(static_cast<int64_t>(dst - src));
}

void elf::setKlaussCPUTargetInfo(Ctx &ctx) {
  ctx.target.reset(new KlaussCPU(ctx));
}
