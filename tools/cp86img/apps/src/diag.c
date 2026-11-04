/* DIAG.C -- stage marker probe for the CardPuter86 DOS environment.
 *
 * Both MINIGAME.COM and MINIGAME.EXE return to the prompt without drawing
 * anything, while ASM.EXE (same compiler, same model) runs fine.  This probe
 * prints a marker after every step MINIGAME does at start-up, so the first
 * missing marker names the broken call.
 *
 *     dmc -msd -w diag.c -o DIAG.EXE
 */

#include <stdio.h>
#include <dos.h>

static void vput(int row, int col, int ch, int attr)
{
    unsigned char far *v;
    unsigned off;
    if (row < 0 || row >= 16 || col < 0 || col >= 40) return;
    v = (unsigned char far *)0xB8000000UL;
    off = (unsigned)((row * 80 + col) << 1);
    v[off] = (unsigned char)ch;
    v[off + 1] = (unsigned char)attr;
}

static void mark(const char *s)
{
    printf("%s\n", s);
    fflush(stdout);
}

int main(void)
{
    union REGS r;
    unsigned far *head;
    unsigned far *tail;
    unsigned long ticks;
    unsigned i;
    int drawn;

    head = (unsigned far *)0x0040001AUL;   /* BIOS keyboard buffer head */
    tail = (unsigned far *)0x0040001CUL;

    mark("D1 start");

    /* ---- what DOS thinks we have ------------------------------------- */
    r.h.ah = 0x30;
    int86(0x21, &r, &r);
    printf("D2 dos %u.%u\n", (unsigned)r.h.al, (unsigned)r.h.ah);
    fflush(stdout);

    r.h.ah = 0x48;              /* allocate as much as possible: probe TPA */
    r.x.bx = 0xFFFF;
    int86(0x21, &r, &r);
    printf("D3 tpa %u para = %lu KB (err %u)\n",
           r.x.bx,
           (unsigned long)r.x.bx * 16UL / 1024UL,
           (unsigned)r.x.ax);
    fflush(stdout);

    /* ---- the sequence MINIGAME does ---------------------------------- */
    r.h.ah = 0x00;
    r.h.al = 0x03;
    int86(0x10, &r, &r);
    mark("D4 video mode 3 ok");

    {
        unsigned char far *v = (unsigned char far *)0xB8000000UL;
        unsigned j;
        for (j = 0; j < 4000u; j += 2u) {
            v[j] = ' ';
            v[j + 1] = 0x07;
        }
    }
    mark("D5 vram cleared");

    ticks = *(unsigned long far *)0x0040006CUL;
    printf("D6 ticks %lu\n", ticks);
    fflush(stdout);

    r.x.cx = 0x2000;            /* hide the hardware cursor */
    r.h.ah = 0x01;
    int86(0x10, &r, &r);
    mark("D7 cursor shape ok");

    /* ---- does polling the BIOS buffer work? -------------------------- */
    printf("D8 head %u tail %u\n", *head, *tail);
    fflush(stdout);

    r.h.ah = 0x01;              /* INT 16h AH=1: peek keystroke */
    int86(0x16, &r, &r);
    printf("D9 int16 peek ax %04X\n", r.x.ax);
    fflush(stdout);

    /* ---- draw something inside the 40x16 viewport -------------------- */
    vput(0, 0, '+', 0x0E);
    for (i = 1; i < 39; i++) vput(0, (int)i, '-', 0x0E);
    vput(0, 39, '+', 0x0E);
    vput(8, 0, '|', 0x0E);
    vput(8, 39, '|', 0x0E);
    vput(15, 0, '+', 0x0E);
    for (i = 1; i < 39; i++) vput(15, (int)i, '-', 0x0E);
    vput(15, 39, '+', 0x0E);
    drawn = 0;
    {
        static const char *msg = " DIAG: if you can read this, VRAM works ";
        int row = 8, col = 1;
        const char *p = msg;
        while (*p && col < 38) {
            vput(row, col, *p, 0x4F);
            col++;
            p++;
        }
    }
    drawn = 1;
    printf("D10 box drawn %d\n", drawn);
    fflush(stdout);

    /* ---- poll the keyboard buffer for up to ~5 seconds ---------------- */
    for (i = 0; i < 2000u; i++) {
        if (*head != *tail) break;
        {
            /* crude delay: read the BIOS tick counter a few times */
            unsigned long t = *(unsigned long far *)0x0040006CUL;
            unsigned long k;
            for (k = 0; k < 2000UL; k++) (void)t;
        }
    }
    if (*head != *tail) {
        r.h.ah = 0x00;
        int86(0x16, &r, &r);
        printf("D11 key seen ax %04X\n", r.x.ax);
    } else {
        mark("D11 no key in buffer (poll loop never fired)");
    }

    mark("D12 done - returning to DOS");
    return 0;
}
