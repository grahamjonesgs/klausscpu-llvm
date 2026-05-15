# KlaussCPU FreeRTOS Port

## Quick start

```bash
# 1. Get the FreeRTOS kernel (once only, ~2 MB shallow clone)
bash get-freertos.sh

# 2. Build picolibc with -fPIC (if not already done)
cd ../ && bash build-picolibc.sh && cd freertos

# 3. Build the demo
make demo.elf demo.bin
```

## Port files

| File | Purpose |
|------|---------|
| `portable/KlaussCPU/portmacro.h` | FreeRTOS type defs, critical-section macros, yield |
| `portable/KlaussCPU/port.c` | `pxPortInitialiseStack`, `xPortStartScheduler`, critical-section impl |
| `portable/KlaussCPU/port.S` | `vPortTickISR`, `vPortStartFirstTask`, `vPortYield` |
| `FreeRTOSConfig.h` | Kernel configuration (tick rate, heap, priorities) |

## Hardware mapping

| FreeRTOS concept | KlaussCPU hardware |
|---|---|
| `portDISABLE_INTERRUPTS()` | `REG_INT_MASK = 0` |
| `portENABLE_INTERRUPTS()` | `REG_INT_MASK = 1` |
| Tick interrupt | Timer source 0; period = `CPU_CLK / TICK_RATE_HZ` |
| `portYIELD()` | `vPortYield()` — CALL-slot patching, no extra hardware |

## Stack frame layout

Hardware saves one 64-bit word on interrupt entry:
```
bits[63:43] = 0
bits[42:39] = INT_MASK (4-bit, restored by IRET)
bits[38:32] = flags (7-bit)
bits[31: 0] = PC
```

Software (`vPortTickISR`) saves R0–R15 (16 × 8 bytes = 128 bytes).

Per-task frame in memory (low → high address, SP saved at lowest):

```
[SP+0]   R15  ← pxTopOfStack saved here
[SP+8]   R14
  ...
[SP+120] R0   ← pvParameters on first run
[SP+128] hw_frame {INT_MASK, flags, PC}
```

`pxPortInitialiseStack` builds this layout, returning the R15 slot address
as the new `pxTopOfStack`.

## Key design decisions

**No new hardware required.** Every FreeRTOS primitive maps to existing
KlaussCPU mechanisms:
- Critical sections = MMIO mask write (single cycle)
- Tick = timer interrupt source 0 (already present)
- Context switch = PUSH/POP R0–R15 + IRET (same as the custom RTOS)
- Yield = CALL-slot patching trick from `context_switch.S`

**`portYIELD()` from task context** patches the CALL return-address slot
into a valid IRET frame (sets INT_MASK[0]=1 in the upper 32 bits at
`[SP+132]`), allowing the IRET at the end of `vPortYield` to re-enable the
timer and return to the caller transparently.

**Kernel stack** (`g_freertos_kernel_sp`): a 256-word (2 KB) static buffer
in port.c.  The timer ISR switches to this before calling
`xTaskIncrementTick` / `vTaskSwitchContext`, keeping scheduler code off
the interrupted task's stack.

## Porting to FreeRTOS from the custom RTOS

The two RTOSes share the same assembly patterns.  Differences:

| Custom RTOS | FreeRTOS port |
|---|---|
| `g_current_tcb->sp` (uint32_t at offset 0) | `pxCurrentTCB->pxTopOfStack` (StackType_t* at offset 0) |
| `tick_count_update` + `pick_next_task` | `xTaskIncrementTick` + `vTaskSwitchContext` |
| `stidx32` to save SP | `stidx64` to save SP (field is 64-bit pointer) |
| Round-robin only | Priority-based with time-slicing |
