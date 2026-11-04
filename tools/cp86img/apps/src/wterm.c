/* ============================================================
 * WTERM.C -- minimal COM1 terminal for the CardPuter86 DOS machine
 *
 * The emulator exposes COM1 (0x3F8) as a Hayes-style WiFi modem, so this
 * is the tool you need to talk to it from inside DOS:
 *
 *     WTERM
 *     AT                      -> OK
 *     AT+WIFI?                -> link state
 *     AT+WIFI=myssid,mypass   -> store credentials and connect
 *     AT+WIFISCAN             -> nearby access points
 *     ATDT example.com:80     -> raw TCP, type whatever the server wants
 *     AT+HTTPGET example.com/ -> the page body is printed here
 *     +++  (three plus)       -> back to command mode
 *
 * Keys:  Esc or F10 quits.  Everything else goes to the modem.
 * Ctrl-A is sent instead of Esc when you want an ESC byte on the wire.
 *
 * Build:  dmc -msd -w wterm.c -o WTERM.COM   (small model, .COM not required)
 * ============================================================ */
#include <stdio.h>
#include <dos.h>
#include <conio.h>

#define COM1        0x3F8
#define LSR_DR      0x01
#define LSR_THRE    0x20

static int com_data_ready(void)
{
    return (inp(COM1 + 5) & LSR_DR) != 0;
}

static void com_put(int c)
{
    int guard = 0;
    while (!(inp(COM1 + 5) & LSR_THRE) && guard < 20000) guard++;
    outp(COM1, (unsigned char)c);
}

static void com_puts(const char *s)
{
    while (*s) com_put(*s++);
}

static int key_hit(void)
{
    return *(unsigned far *)0x0040001AUL != *(unsigned far *)0x0040001CUL;
}

static unsigned key_get(void)
{
    union REGS r;
    r.h.ah = 0x00;
    int86(0x16, &r, &r);
    return r.x.ax;
}

int main(void)
{
    int last_cr = 0;

    printf("WTERM - COM1 terminal (Hayes WiFi modem)\n");
    printf("Esc/F10 quit. AT+WIFI? / AT+WIFISCAN / ATDT host:port\n");
    printf("AT+HTTPGET host/  +++ = command mode\n");
    printf("-------------------------------------------\n");

    for (;;) {
        if (com_data_ready()) {
            int c = inp(COM1);
            if (c == '\n' && last_cr) {         /* CRLF -> one newline */
                last_cr = 0;
                continue;
            }
            last_cr = (c == '\r');
            if (c == '\n') putchar('\n');
            else if (c == '\r') putchar('\n');
            else if (c >= 32 || c == 9) putchar(c);
            else if (c == 8) printf("\b \b");
        }

        if (key_hit()) {
            unsigned k = key_get();
            unsigned char al = (unsigned char)(k & 0xFF);
            unsigned char ah = (unsigned char)((k >> 8) & 0xFF);

            if (al == 0x1B) break;              /* Esc */
            if (al == 0 && ah == 0x44) break;   /* F10 */
            if (al == 1) { com_put(0x1B); continue; }   /* Ctrl-A -> ESC byte */
            if (al == 0x0D) { com_put('\r'); putchar('\n'); continue; }
            if (al == 8 || al == 127) { com_put(8); printf("\b \b"); continue; }
            if (al >= 32 && al < 127) { com_put(al); continue; }
        }
    }

    printf("\nwterm done.\n");
    return 0;
}
