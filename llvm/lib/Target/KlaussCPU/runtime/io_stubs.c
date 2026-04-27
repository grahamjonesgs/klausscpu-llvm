// io_stubs.c — LED, 7-seg and delay helpers for KlaussCPU.

typedef unsigned long long uint64_t;

void leds(uint64_t val) {
    __builtin_klausscpu_leds(val);
}

void seg7(uint64_t val) {
    __builtin_klausscpu_seg7(val);
}

void seg7blank(void) {
    __builtin_klausscpu_seg7blank();
}

// Hardware spin-wait.  cycles is a count of CPU clock cycles.
void delay_hw(uint64_t cycles) {
    __builtin_klausscpu_delayv(cycles);
}
