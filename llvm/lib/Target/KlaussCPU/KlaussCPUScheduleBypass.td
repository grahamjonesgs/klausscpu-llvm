//===-- KlaussCPUScheduleBypass.td - load-bypass consumers ---*- tablegen -*-===//
//
// The instructions that read a load's data one cycle early (klaussCPU ldbyp:
// ReadLoadByp in KlaussCPUSchedule.td) on their register operands: U_ALU
// (add/sub with and without carry, and/or/xor, inc/dec, the W and immediate
// forms), U_BOOL (the boolean compares, min/max) and the compares a fused
// branch is made of.  Included after KlaussCPUInstrInfo.td, since InstRW
// names the instruction records.  Each list keeps the format's writes.
//
//===----------------------------------------------------------------------===//

let SchedModel = KlaussCPUSchedModel in {
  def : InstRW<[WriteALU, WriteFlags, ReadLoadByp, ReadLoadByp],
               (instrs ADDR, ADDC, SUBR, SUBC, ADDW, SUBW,
                       CMPEQRW, CMPNERW, CMPGERW, CMPGTRW, CMPLERW, CMPLTRW,
                       CMPUGERW, CMPUGTRW, CMPULERW, CMPULTRW)>;
  def : InstRW<[WriteALU, WriteFlags, ReadLoadByp],
               (instrs ADDI, ADDIW, ADDV, MINUSV, INCR, DECR,
                       CMPEQV, CMPNEV, CMPGEV, CMPGTV, CMPLEV, CMPLTV,
                       CMPUGEV, CMPUGTV, CMPULEV, CMPULTV,
                       CMPEQVW, CMPNEVW, CMPGEVW, CMPGTVW, CMPLEVW, CMPLTVW,
                       CMPUGEVW, CMPUGTVW, CMPULEVW, CMPULTVW)>;
  def : InstRW<[WriteALU, ReadLoadByp, ReadLoadByp],
               (instrs ANDR, ORR, XORR, MINR, MAXR, MINUR, MAXUR,
                       CMPEQR, CMPNER, CMPGER, CMPGTR, CMPLER, CMPLTR,
                       CMPUGER, CMPUGTR, CMPULER, CMPULTR,
                       CMPRR_I, CMPRRW_I, FBR_RR)>;
  def : InstRW<[WriteALU, ReadLoadByp],
               (instrs ANDI, ANDV, ORI, ORV, XORI, XORV,
                       CMPRV_I, CMPRVU_I, CMPRVW_I, FBR_RI, FBR_RV)>;
}
