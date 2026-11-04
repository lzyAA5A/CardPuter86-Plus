/* ============================================================
 * TE.C -- small full-screen text editor for CardPuter86
 *
 * Replaces the old TE.COM (whose source was lost).  Two design
 * rules follow from how the CardPuter86 guest behaves:
 *
 *  1) NEVER draw through DOS/BIOS.  putchar() and INT 10h teletype
 *     run thousands of emulated instructions per character, which
 *     made the old editor redraw the whole screen on every key and
 *     feel laggy.  We write 0xB800:XXXX directly (40x25 text page,
 *     80 bytes per row, rows 0..15 visible on the CardPuter LCD).
 *
 *  2) DRAIN the whole BIOS keyboard buffer before drawing, then
 *     redraw once.  Slow redraws used to let the 15-slot BIOS
 *     buffer overflow, so fast typing silently lost characters.
 *
 * Layout: rows 0..14 = text, row 15 = status bar.
 * Long lines SOFT-WRAP visually (40 columns per row) and keep
 * their real length in the buffer, so nothing is ever "eaten".
 *
 * Keys (CardPuter):
 *   printable  insert        Backspace  delete left / join lines
 *   Fn+, Fn+.  left/right    Fn+;  Fn+/   up/down (visual rows)
 *   Enter      split line    Fn+Bs     delete under cursor
 *   Fn+`  Esc  quit          ^O or ^S  save      ^X  quit
 *   ^Y         delete line   Home/End  line start/end (if reported)
 *
 * Build (Digital Mars C/C++ 7.51/8.57, 16-bit DOS small model):
 *   dmc -msd -w te.c -o TE.EXE
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>

#define COLS        40
#define TEXT_ROWS   15
#define STATUS_ROW  15
#define MAXLINES    128
#define MAXCOLS     200
#define MAXVIS      (MAXLINES * 6)      /* 200/40 + 1 segments per line */

#define VRAM        ((unsigned char far *)0xB8000000UL)
#define ATTR_TEXT   0x07
#define ATTR_BAR    0x70

/* ------------------------------------------------------------------ */
/* data                                                                */
/* ------------------------------------------------------------------ */

static unsigned char buf[MAXLINES][MAXCOLS];
static unsigned short len[MAXLINES];
static int nlines = 1;
static int cy = 0, cx = 0;
static int top_vis = 0;
static int modified = 0;
static char fname[64] = "";

static unsigned char v_line[MAXVIS];
static unsigned short v_off[MAXVIS];
static int nvis = 0;

static unsigned char chunk[256];

/* ------------------------------------------------------------------ */
/* screen                                                              */
/* ------------------------------------------------------------------ */

static void vput(int row, int col, int ch, int attr)
{
    unsigned char far *v;
    unsigned off;
    if (row < 0 || row >= 16 || col < 0 || col >= COLS)
        return;
    v = VRAM;
    off = (unsigned)((row * 80 + col) << 1);
    v[off] = (unsigned char)ch;
    v[off + 1] = (unsigned char)attr;
}

/* blank all 4000 bytes of the text page: the emulator viewport would
   otherwise be panned away by leftover rows 16..24 */
static void clear_page(void)
{
    unsigned char far *v = VRAM;
    unsigned i;
    for (i = 0; i < 4000u; i += 2u) {
        v[i] = ' ';
        v[i + 1] = ATTR_TEXT;
    }
}

static void set_video_mode(int m)
{
    union REGS r;
    r.h.ah = 0x00;
    r.h.al = (unsigned char)m;
    int86(0x10, &r, &r);
}

static void hw_cursor(int on)
{
    union REGS r;
    r.h.ah = 0x01;
    r.x.cx = on ? 0x0607 : 0x2000;
    int86(0x10, &r, &r);
}

/* ------------------------------------------------------------------ */
/* visual line map (soft wrap)                                         */
/* ------------------------------------------------------------------ */

static void build_vis(void)
{
    int i, s, segs;
    nvis = 0;
    for (i = 0; i < nlines && nvis < MAXVIS; i++) {
        segs = (int)len[i] / COLS + 1;
        for (s = 0; s < segs && nvis < MAXVIS; s++) {
            v_line[nvis] = (unsigned char)i;
            v_off[nvis] = (unsigned short)(s * COLS);
            nvis++;
        }
    }
    if (nvis == 0) {                 /* cannot happen, but be safe */
        v_line[0] = 0;
        v_off[0] = 0;
        nvis = 1;
    }
}

/* index into the visual map of the cell the cursor sits on */
static int cur_vis(void)
{
    int k, want = cx / COLS;
    for (k = 0; k < nvis; k++) {
        if ((int)v_line[k] == cy && (int)(v_off[k] / COLS) == want)
            return k;
    }
    return 0;
}

static void draw_status_msg(const char *msg)
{
    int c, n;
    char s[COLS + 1];
    n = (int)strlen(msg);
    if (n > COLS) n = COLS;
    memcpy(s, msg, (unsigned)n);
    for (c = n; c < COLS; c++) s[c] = ' ';
    s[COLS] = 0;
    for (c = 0; c < COLS; c++) vput(STATUS_ROW, c, s[c], ATTR_BAR);
}

static void draw_status(void)
{
    char s[COLS + 2];
    char name[16];
    int n, i;
    const char *p = fname;
    /* strip the directory for the status bar */
    for (i = 0; fname[i]; i++) {
        if (fname[i] == '\\' || fname[i] == '/' || fname[i] == ':')
            p = fname + i + 1;
    }
    if (*p == 0) p = "(new)";
    strncpy(name, p, 11);
    name[11] = 0;
    n = sprintf(s, "%c%-11s %3d,%3d  ^O save  ^X exit",
                modified ? '*' : ' ', name, cy + 1, cx + 1);
    if (n > COLS) n = COLS;
    for (i = n; i < COLS; i++) s[i] = ' ';
    s[COLS] = 0;
    for (i = 0; i < COLS; i++) vput(STATUS_ROW, i, s[i], ATTR_BAR);
}

static void draw(void)
{
    int r, c, vi, L, off, n, cv, col;
    unsigned char ch;

    build_vis();
    cv = cur_vis();
    if (cv < top_vis)
        top_vis = cv;
    while (cv >= top_vis + TEXT_ROWS)
        top_vis++;

    for (r = 0; r < TEXT_ROWS; r++) {
        vi = top_vis + r;
        if (vi < nvis) {
            L = (int)v_line[vi];
            off = (int)v_off[vi];
            n = (int)len[L] - off;
            if (n < 0) n = 0;
            if (n > COLS) n = COLS;
            for (c = 0; c < COLS; c++)
                vput(r, c, (c < n) ? (int)buf[L][off + c] : ' ', ATTR_TEXT);
        } else {
            for (c = 0; c < COLS; c++)
                vput(r, c, ' ', ATTR_TEXT);
        }
    }

    /* attribute-block cursor (the hardware cursor is switched off) */
    if (cv >= top_vis && cv < top_vis + TEXT_ROWS) {
        r = cv - top_vis;
        L = (int)v_line[cv];
        off = (int)v_off[cv];
        col = cx - off;
        if (col < 0) col = 0;
        if (col >= COLS) col = COLS - 1;
        ch = ' ';
        if (off + col < (int)len[L])
            ch = buf[L][off + col];
        vput(r, col, (int)ch, ATTR_BAR);
    }
    draw_status();
}

/* ------------------------------------------------------------------ */
/* keyboard                                                            */
/* ------------------------------------------------------------------ */

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

static void key_flush(void)
{
    int guard = 0;
    while (key_hit() && guard < 256) {
        key_get();
        guard++;
    }
}

/* ------------------------------------------------------------------ */
/* DOS file I/O (raw INT 21h - no stdio buffers, no far heap)          */
/* ------------------------------------------------------------------ */

static int d_open(const char *name)
{
    union REGS r;
    r.h.ah = 0x3D;
    r.h.al = 0x00;                       /* read only */
    r.x.dx = (unsigned)name;
    int86(0x21, &r, &r);
    if (r.x.flags & 1) return -1;
    return (int)r.x.ax;
}

static int d_create(const char *name)
{
    union REGS r;
    r.h.ah = 0x3C;
    r.x.cx = 0;
    r.x.dx = (unsigned)name;
    int86(0x21, &r, &r);
    if (r.x.flags & 1) return -1;
    return (int)r.x.ax;
}

static int d_read(int h, void *p, unsigned n)
{
    union REGS r;
    r.h.ah = 0x3F;
    r.x.bx = (unsigned)h;
    r.x.cx = n;
    r.x.dx = (unsigned)p;
    int86(0x21, &r, &r);
    if (r.x.flags & 1) return -1;
    return (int)r.x.ax;
}

static int d_write(int h, const void *p, unsigned n)
{
    union REGS r;
    r.h.ah = 0x40;
    r.x.bx = (unsigned)h;
    r.x.cx = n;
    r.x.dx = (unsigned)p;
    int86(0x21, &r, &r);
    if (r.x.flags & 1) return -1;
    return (int)r.x.ax;
}

static void d_close(int h)
{
    union REGS r;
    r.h.ah = 0x3E;
    r.x.bx = (unsigned)h;
    int86(0x21, &r, &r);
}

static int load_file(void)
{
    int h, n, i, total = 0, truncated = 0;
    unsigned char c;

    h = d_open(fname);
    if (h < 0)
        return -1;                       /* new file */

    nlines = 1;
    len[0] = 0;
    for (;;) {
        n = d_read(h, chunk, sizeof(chunk));
        if (n <= 0)
            break;
        for (i = 0; i < n; i++) {
            c = chunk[i];
            if (c == 0x1A)
                goto loaded;             /* ^Z = DOS end of text */
            if (c == '\r')
                continue;
            if (c == '\n') {
                if (nlines < MAXLINES) {
                    len[nlines] = 0;
                    nlines++;
                } else {
                    truncated = 1;
                }
                continue;
            }
            if (len[nlines - 1] < MAXCOLS - 1) {
                buf[nlines - 1][len[nlines - 1]] = c;
                len[nlines - 1]++;
                total++;
            } else {
                truncated = 1;
            }
        }
    }
loaded:
    d_close(h);
    cy = 0;
    cx = 0;
    top_vis = 0;
    return truncated ? 2 : total;
}

static int save_file(void)
{
    static const char crlf[2] = { '\r', '\n' };
    int h, i;
    h = d_create(fname);
    if (h < 0)
        return -1;
    for (i = 0; i < nlines; i++) {
        if (len[i] && d_write(h, buf[i], len[i]) < 0) {
            d_close(h);
            return -1;
        }
        if (d_write(h, crlf, 2u) < 0) {
            d_close(h);
            return -1;
        }
    }
    d_close(h);
    modified = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* editing                                                             */
/* ------------------------------------------------------------------ */

static void ins_char(int ch)
{
    unsigned short l = len[cy];
    if (l >= MAXCOLS - 1) {
        draw_status_msg("Line full (200 chars) - press Enter");
        return;
    }
    if (cx < (int)l)
        memmove(&buf[cy][cx + 1], &buf[cy][cx], (unsigned)(l - cx));
    buf[cy][cx] = (unsigned char)ch;
    len[cy] = (unsigned short)(l + 1);
    cx++;
    modified = 1;
}

static void del_left(void)
{
    unsigned short l = len[cy];
    if (cx > 0) {
        if (cx < (int)l)
            memmove(&buf[cy][cx - 1], &buf[cy][cx], (unsigned)(l - cx));
        len[cy] = (unsigned short)(l - 1);
        cx--;
        modified = 1;
    } else if (cy > 0) {
        unsigned short pl = len[cy - 1];
        unsigned short i;
        for (i = 0; i < l && pl < MAXCOLS - 1; i++, pl++)
            buf[cy - 1][pl] = buf[cy][i];
        len[cy - 1] = pl;
        /* drop line cy */
        for (i = (unsigned short)cy; i < (unsigned short)(nlines - 1); i++) {
            memcpy(buf[i], buf[i + 1], MAXCOLS);
            len[i] = len[i + 1];
        }
        nlines--;
        cy--;
        cx = (int)pl;
        modified = 1;
    }
}

static void del_right(void)
{
    unsigned short l = len[cy];
    unsigned short i;
    if (cx < (int)l) {
        memmove(&buf[cy][cx], &buf[cy][cx + 1], (unsigned)(l - cx - 1));
        len[cy] = (unsigned short)(l - 1);
        modified = 1;
    } else if (cy < nlines - 1) {
        unsigned short nl = len[cy + 1];
        for (i = 0; i < nl && l < MAXCOLS - 1; i++, l++)
            buf[cy][l] = buf[cy + 1][i];
        len[cy] = l;
        for (i = (unsigned short)(cy + 1); i < (unsigned short)(nlines - 1); i++) {
            memcpy(buf[i], buf[i + 1], MAXCOLS);
            len[i] = len[i + 1];
        }
        nlines--;
        modified = 1;
    }
}

static void split_line(void)
{
    unsigned short l = len[cy];
    unsigned short i;
    if (nlines >= MAXLINES) {
        draw_status_msg("File full (128 lines)");
        return;
    }
    for (i = (unsigned short)nlines; i > (unsigned short)cy; i--) {
        memcpy(buf[i], buf[i - 1], MAXCOLS);
        len[i] = len[i - 1];
    }
    nlines++;
    len[cy] = (unsigned short)cx;
    len[cy + 1] = (unsigned short)(l - cx);
    if (l > (unsigned short)cx)
        memmove(buf[cy + 1], &buf[cy][cx], (unsigned)(l - cx));
    cy++;
    cx = 0;
    modified = 1;
}

static void delete_line(void)
{
    int i;
    if (nlines <= 1) {
        len[0] = 0;
        cx = 0;
    } else {
        for (i = cy; i < nlines - 1; i++) {
            memcpy(buf[i], buf[i + 1], MAXCOLS);
            len[i] = len[i + 1];
        }
        nlines--;
        if (cy >= nlines) {
            cy = nlines - 1;
            cx = (int)len[cy];
        } else if (cx > (int)len[cy]) {
            cx = (int)len[cy];
        }
    }
    modified = 1;
}

/* vertical move along VISUAL rows, keeping the column inside the row */
static void move_vert(int dir)
{
    int cv = cur_vis();
    int col = cx - (int)v_off[cv];
    int nv = cv + dir;
    int L, off, l;
    build_vis();
    cv = cur_vis();
    col = cx - (int)v_off[cv];
    nv = cv + dir;
    if (nv < 0 || nv >= nvis)
        return;
    L = (int)v_line[nv];
    off = (int)v_off[nv];
    l = (int)len[L];
    cy = L;
    cx = (off + col > l) ? l : (off + col);
}

static void move_home(void)
{
    int cv = cur_vis();
    cx = (int)v_off[cv];
}

static void move_end(void)
{
    int cv = cur_vis();
    int L = (int)v_line[cv];
    int off = (int)v_off[cv];
    int e = off + COLS;
    if ((int)len[L] < e)
        e = (int)len[L];
    if (e == off && (int)len[L] > off)
        e = (int)len[L];               /* empty tail segment */
    cy = L;
    cx = e;
    if (cx > (int)len[cy])
        cx = (int)len[cy];
}

/* ------------------------------------------------------------------ */
/* quit / save confirmation                                            */
/* ------------------------------------------------------------------ */

static int confirm_save(void)
{
    unsigned k;
    int a;
    draw_status_msg("Save changes?  Y = yes   N = no   Esc = cancel");
    for (;;) {
        if (!key_hit()) {
            /* idle spin: nothing else to do, BIOS buffer is the queue */
            continue;
        }
        k = key_get();
        a = (int)(k & 0xFF);            /* INT 16h returns AH = scan code */
        if (a == 0x1B)
            return -1;
        if (a == 'y' || a == 'Y' || a == 0x0F || a == 0x18) {
            if (save_file() == 0)
                return 0;
            draw_status_msg("SAVE FAILED - disk error");
            return -1;
        }
        if (a == 'n' || a == 'N')
            return 0;
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    unsigned k;
    int r;

    if (argc < 2) {
        printf("Usage: TE FILE.TXT\n");
        return 1;
    }
    strncpy(fname, argv[1], sizeof(fname) - 1);
    fname[sizeof(fname) - 1] = 0;

    set_video_mode(3);
    clear_page();
    hw_cursor(0);

    r = load_file();
    if (r == -1)
        draw_status_msg("New file");
    else if (r == 2)
        draw_status_msg("Loaded (truncated to 128 lines x 200 cols)");
    key_flush();
    draw();

    for (;;) {
        while (key_hit()) {
            int a, sc;
            k = key_get();
            a = (int)(k & 0xFF);            /* ASCII (0 = extended key) */
            sc = (int)((k >> 8) & 0xFF);    /* scan code in AH */

            if (a == 0x18) {                       /* ^X exit */
                if (modified) {
                    r = confirm_save();
                    if (r < 0) { draw(); continue; }
                }
                goto quit;
            }
            if (a == 0x1B) {                       /* Esc */
                if (modified) {
                    r = confirm_save();
                    if (r < 0) { draw(); continue; }
                }
                goto quit;
            }
            if (a == 0x0F || a == 0x13) {          /* ^O / ^S save */
                if (save_file() == 0)
                    draw_status_msg("Saved");
                else
                    draw_status_msg("SAVE FAILED - disk error");
                continue;
            }
            if (a == 0x19) { delete_line(); continue; }
            if (a == 0x0D) { split_line(); continue; }
            if (a == 0x08) { del_left(); continue; }
            if (a == 0x09) {                       /* Tab -> 4-col stops */
                int n = 4 - (cx & 3);
                while (n-- > 0) ins_char(' ');
                continue;
            }
            if (a == 0x00) {                       /* extended: scan in AH */
                /* sc already carries AH from the single key_get() above.
                 * Do NOT read another key here: a second INT 16h would both
                 * block and swallow the next keystroke (that is why arrows
                 * only seemed to work every other press). */
                if (sc == 0x48) move_vert(-1);
                else if (sc == 0x50) move_vert(1);
                else if (sc == 0x4B) { if (cx > 0) cx--; }
                else if (sc == 0x4D) { if (cx < (int)len[cy]) cx++; }
                else if (sc == 0x47) move_home();
                else if (sc == 0x4F) move_end();
                else if (sc == 0x53) del_right();
                else if (sc == 0x49) { int i; for (i = 0; i < TEXT_ROWS; i++) move_vert(-1); }
                else if (sc == 0x51) { int i; for (i = 0; i < TEXT_ROWS; i++) move_vert(1); }
                continue;
            }
            if (a >= 0x20 && a <= 0x7E) {
                ins_char(a);
                continue;
            }
            /* anything else: ignore */
        }
        draw();                                    /* one redraw per batch */
    }

quit:
    hw_cursor(1);
    set_video_mode(3);                             /* clears + restores */
    return 0;
}
