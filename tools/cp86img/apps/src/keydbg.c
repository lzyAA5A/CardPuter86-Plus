/* KEYDBG.COM - print the raw AX returned by BIOS INT 16h for every key.
 * Used to verify which scan codes the emulated BIOS delivers (extended keys,
 * arrows, Home/End/PgUp/PgDn) before wiring them into TE.
 *
 * Build: dmc -msd -w keydbg.c -o KEYDBG.EXE     (then run KEYDBG.EXE)
 */
#include <stdio.h>
#include <dos.h>

static int key_hit(void)
{
    unsigned far *head = (unsigned far *)0x0040001AUL;
    unsigned far *tail = (unsigned far *)0x0040001CL;
    return (*head != *tail);
}

static unsigned key_get(void)
{
    union REGS r;
    r.h.ah = 0x00;
    int86(0x16, &r, &r);
    return (unsigned)r.x.ax;
}

int main(void)
{
    unsigned k;
    int row = 0, col = 0;
    unsigned far *vram = (unsigned far *)0xB8000000UL;

    /* clear the visible 40x25 area with spaces */
    {
        unsigned i;
        for (i = 0; i < 40 * 25; i++)
            vram[i] = 0x0720;
    }
    printf("KEYDBG: press keys, Esc quits.\n");

    for (;;) {
        if (!key_hit())
            continue;
        k = key_get();
        printf("AX=%04X  al=%02X ah=%02X\n", k, k & 0xFF, (k >> 8) & 0xFF);
        if ((k & 0xFF) == 0x1B)
            break;
        (void)row;
        (void)col;
    }
    return 0;
}
