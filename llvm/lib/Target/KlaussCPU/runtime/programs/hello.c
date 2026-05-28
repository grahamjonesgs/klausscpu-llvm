/*
 * hello.c — KlaussCPU hello-world smoke test
 */

#include "../mmio.h"
#include <stdio.h>
#include <stdint.h>

int main(int argc, char **argv) {
    leds(0x0001);
    seg7(0x0001);

    puts("Hello, KlaussCPU! 1");
    puts("Hello, KlaussCPU! 2");

    leds(0x0003);
    seg7(0x0003);

    
    /* All six args are the same type (unsigned long), same width, same
       conversion specifier.  If varargs work correctly, output is:
           [test] 11 22 33 44 55 66
       If the 4th vararg is broken, position 4 will show something else. */
    printf("[test] %02lx %02lx %02lx %02lx %02lx %02lx\n",
           0x11ul, 0x22ul, 0x33ul, 0x44ul, 0x55ul, 0x66ul);

    /* Control: only 3 varargs.  If your calling convention passes 4 args in
       registers (fmt + 3 varargs all in regs), the buggy 4th-stack-arg case
       isn't hit.  This should print "11 22 33" regardless of the bug. */
    printf("[ctrl3] %02lx %02lx %02lx\n",
           0x11ul, 0x22ul, 0x33ul);

    /* Same as [test] but with uint8_t source, matching the pattern from
       eth_test.c (cast-promoted small types through varargs).  If the bug is
       in the uint8_t→unsigned long promotion specifically, only this one
       breaks.  Otherwise both [test] and [u8-6] will misbehave identically. */
    {
        uint8_t a = 0x11, b = 0x22, c = 0x33, d = 0x44, e = 0x55, f = 0x66;
        printf("[u8-6 ] %02lx %02lx %02lx %02lx %02lx %02lx\n",
               (unsigned long)a, (unsigned long)b, (unsigned long)c,
               (unsigned long)d, (unsigned long)e, (unsigned long)f);
    }

    /* Vary just the 4th vararg across iterations.  If position 4 is broken
       independently of value, all three lines show the same garbage at
       position 4.  If the bug is value-dependent, the garbage tracks i. */
    for (unsigned long i = 0xAA; i <= 0xCC; i += 0x11) {
        printf("[i=%02lx] %02lx %02lx %02lx %02lx %02lx %02lx\n",
               i, 0x11ul, 0x22ul, 0x33ul, i, 0x55ul, 0x66ul);
    }
    return 0;
}
