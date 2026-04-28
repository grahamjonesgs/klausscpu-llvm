//===-- IntrinsicsKlaussCPU.td - KlaussCPU intrinsics ------*- tablegen -*-===//
//
// LLVM intrinsics for KlaussCPU target-specific I/O instructions.
// These map 1:1 to hardware instructions and are lowered in
// KlaussCPUISelDAGToDAG.cpp to their corresponding machine instructions.
//
//===----------------------------------------------------------------------===//

let TargetPrefix = "klausscpu" in {
  // TXR: transmit 64-bit register value as 16 hex digits over UART.
  def int_klausscpu_txr
      : Intrinsic<[], [llvm_i64_ty], [IntrHasSideEffects]>;

  // TXMEMR: transmit 64-bit value at mem[ptr] as hex.
  def int_klausscpu_txmemr
      : Intrinsic<[], [llvm_ptr_ty], [IntrHasSideEffects]>;

  // TXCHARMEMR: transmit byte at mem[ptr] as ASCII character.
  def int_klausscpu_txcharmemr
      : Intrinsic<[], [llvm_ptr_ty], [IntrHasSideEffects]>;

  // TXSTRMEMR: transmit null-terminated string at mem[ptr].
  def int_klausscpu_txstrmemr
      : Intrinsic<[], [llvm_ptr_ty], [IntrHasSideEffects]>;

  // RXRB: blocking receive — waits for UART byte, returns it as i64.
  def int_klausscpu_rxrb
      : Intrinsic<[llvm_i64_ty], [], [IntrHasSideEffects]>;

  // RXRNB: non-blocking receive — returns UART byte or 0; zero_flag=1 if empty.
  def int_klausscpu_rxrnb
      : Intrinsic<[llvm_i64_ty], [], [IntrHasSideEffects]>;

  // NEWLINE: transmit CR+LF over UART.
  def int_klausscpu_newline
      : Intrinsic<[], [], [IntrHasSideEffects]>;

  // LEDR: drive 16 on-board LEDs from bits [15:0] of the argument.
  def int_klausscpu_leds
      : Intrinsic<[], [llvm_i64_ty], [IntrHasSideEffects]>;

  // SEG7_1R: set 7-seg display 1 from argument.
  def int_klausscpu_seg7_1
      : Intrinsic<[], [llvm_i64_ty], [IntrHasSideEffects]>;

  // SEG7_2R: set 7-seg display 2 from argument.
  def int_klausscpu_seg7_2
      : Intrinsic<[], [llvm_i64_ty], [IntrHasSideEffects]>;

  // SEG7R: set both 7-seg displays from argument.
  def int_klausscpu_seg7
      : Intrinsic<[], [llvm_i64_ty], [IntrHasSideEffects]>;

  // SEG7BLANK: blank both 7-seg displays.
  def int_klausscpu_seg7blank
      : Intrinsic<[], [], [IntrHasSideEffects]>;

  // DELAYV/DELAYR: hardware spin-wait for N clock cycles.
  def int_klausscpu_delayv
      : Intrinsic<[], [llvm_i64_ty], [IntrHasSideEffects]>;

  // MEMGET32: read 32-bit value at address in register, zero-extend to i64.
  // Returns the raw physical 32-bit word; in big-endian physical memory the
  // lowest-address byte is at bits[31:24] of the result.  Use shift=24..0 to
  // extract bytes in correct order.
  def int_klausscpu_memget32
      : Intrinsic<[llvm_i64_ty], [llvm_ptr_ty], [IntrReadMem, IntrArgMemOnly]>;
}
