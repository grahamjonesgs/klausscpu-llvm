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

}
