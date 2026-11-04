/* ============================================================
 * SNAKE.EXE  --  Tan Chi She / Snake for M5 Cardputer DOS
 *
 * Build (Digital Mars C/C++ 8.57, 16-bit DOS, small model):
 *     dmc -msd snake.c -o SNAKE.EXE
 *
 * Runs on COMPAQ DOS 2.11 inside the CardPuter86 8086 emulator.
 *
 * Display note: the emulator shows columns 0..39 and rows 0..15 of
 * the 80x25 text screen, so the whole game lives in 40x16 and we
 * blank all 4000 VRAM bytes at start so the emulator's auto-scroll
 * never pans the viewport away.
 *
 * Keys:  W A S D   or   Fn+;  Fn+,  Fn+.  Fn+/   (arrow keys)
 *        P = pause,  Q / Esc = quit
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <dos.h>

#define SCR_COLS 40
#define SCR_ROWS 16

/* playfield interior */
#define FX0 1
#define FX1 38
#define FY0 2
#define FY1 12
#define MAXLEN 600

/* ---------------- direct video ---------------- */
static void vput(int row, int col, int ch, int attr)
{
    unsigned char far *v;
    unsigned off;
    if (row < 0 || row >= SCR_ROWS || col < 0 || col >= SCR_COLS)
        return;
    v = (unsigned char far *)0xB8000000UL;
    off = (unsigned)((row * 80 + col) << 1);
    v[off] = (unsigned char)ch;
    v[off + 1] = (unsigned char)attr;
}

static void vstr(int row, int col, const char *s, int attr)
{
    while (*s) {
        vput(row, col, (unsigned char)*s, attr);
        col++;
        s++;
    }
}

static void vfill(int row, int col, int n, int ch, int attr)
{
    int i;
    for (i = 0; i < n; i++)
        vput(row, col + i, ch, attr);
}

static void clear_all(void)
{
    unsigned char far *v;
    unsigned i;
    v = (unsigned char far *)0xB8000000UL;
    for (i = 0; i < 4000u; i += 2u) {
        v[i] = ' ';
        v[i + 1] = 0x07;
    }
}

/* ---------------- BIOS ---------------- */
static void set_video_mode(int m)
{
    union REGS r;
    r.h.ah = 0x00;
    r.h.al = (unsigned char)m;
    int86(0x10, &r, &r);
}

static void cursor_off(void)
{
    union REGS r;
    r.x.cx = 0x2000;          /* CH = 0x20 -> hardware cursor off */
    r.h.ah = 0x01;
    int86(0x10, &r, &r);
}

static void cursor_on(void)
{
    union REGS r;
    r.x.cx = 0x0607;
    r.h.ah = 0x01;
    int86(0x10, &r, &r);
}

static int key_hit(void)
{
    unsigned far *head;
    unsigned far *tail;
    head = (unsigned far *)0x0040001AUL;
    tail = (unsigned far *)0x0040001CUL;
    return *head != *tail;
}

static unsigned key_get(void)
{
    union REGS r;
    r.h.ah = 0x00;
    int86(0x16, &r, &r);
    return r.x.ax;            /* AH = scan code, AL = ASCII */
}

static void key_flush(void)
{
    int guard;
    guard = 0;
    while (key_hit() && guard < 128) {
        key_get();
        guard++;
    }
}

static unsigned long bios_ticks(void)
{
    return *(unsigned long far *)0x0040006CUL;   /* 18.2 Hz */
}

/* ---------------- game state ---------------- */
static int sx[MAXLEN];
static int sy[MAXLEN];
static int slen;
static int dx, dy;
static int foodx, foody;
static unsigned long score;
static unsigned long rng_state;

static unsigned rnd(void)
{
    rng_state = rng_state * 1103515245UL + 12345UL;
    return (unsigned)((rng_state >> 16) & 0x7FFFu);
}

static void put_score(void)
{
    char buf[40];
    sprintf(buf, "SCORE %-8lu LEN %-3d", score, slen);
    vstr(0, 8, buf, 0x70);
}

static void draw_frame(void)
{
    int i;
    vstr(0, 0, " SNAKE ", 0x70);
    vfill(0, 7, SCR_COLS - 7, ' ', 0x70);
    vfill(1, 0, SCR_COLS, '=', 0x08);
    for (i = FY0; i <= FY1; i++) {
        vput(i, 0, '|', 0x08);
        vput(i, SCR_COLS - 1, '|', 0x08);
    }
    vfill(FY1 + 1, 0, SCR_COLS, '=', 0x08);
    vstr(FY1 + 2, 0, "WASD or Fn+;,. /  turn   P pause  Q quit", 0x0F);
}

static int body_at(int x, int y)
{
    int i;
    for (i = 0; i < slen; i++)
        if (sx[i] == x && sy[i] == y)
            return 1;
    return 0;
}

static void place_food(void)
{
    int guard;
    guard = 0;
    do {
        foodx = FX0 + (int)(rnd() % (unsigned)(FX1 - FX0 + 1));
        foody = FY0 + (int)(rnd() % (unsigned)(FY1 - FY0 + 1));
        guard++;
    } while (body_at(foodx, foody) && guard < 4000);
    vput(foody, foodx, '$', 0x0E);
}

static void game_over_screen(void)
{
    char buf[48];
    vfill(7, 5, 30, ' ', 0x4F);
    vstr(7, 5, "         GAME OVER          ", 0x4F);
    sprintf(buf, "   score %-8lu len %-3d  ", score, slen);
    vstr(8, 5, buf, 0x4F);
    vstr(9, 5, "   R restart     Q quit     ", 0x4F);
}

/* returns 0 when the snake dies */
static int step(void)
{
    int nx, ny, i, ate;

    nx = sx[0] + dx;
    ny = sy[0] + dy;

    if (nx < FX0 || nx > FX1 || ny < FY0 || ny > FY1)
        return 0;

    for (i = 0; i < slen - 1; i++)
        if (sx[i] == nx && sy[i] == ny)
            return 0;

    ate = (nx == foodx && ny == foody);

    if (!ate) {
        vput(sy[slen - 1], sx[slen - 1], ' ', 0x07);
        for (i = slen - 1; i > 0; i--) {
            sx[i] = sx[i - 1];
            sy[i] = sy[i - 1];
        }
    } else {
        if (slen < MAXLEN) {
            for (i = slen; i > 0; i--) {
                sx[i] = sx[i - 1];
                sy[i] = sy[i - 1];
            }
            slen++;
            score += 10;
        }
    }

    sx[0] = nx;
    sy[0] = ny;

    if (slen > 1)
        vput(sy[1], sx[1], '*', 0x02);
    vput(ny, nx, 'O', 0x0A);
    put_score();

    if (ate)
        place_food();
    return 1;
}

static void reset_game(void)
{
    int i;
    slen = 4;
    dx = 1;
    dy = 0;
    score = 0;
    rng_state = bios_ticks() * 2654435761UL + 12345UL;

    clear_all();
    draw_frame();

    for (i = 0; i < slen; i++) {
        sx[i] = 12 - i;
        sy[i] = 7;
    }
    for (i = slen - 1; i >= 0; i--)
        vput(sy[i], sx[i], i == 0 ? 'O' : '*', i == 0 ? 0x0A : 0x02);
    put_score();
    place_food();
    key_flush();
}

/* returns 1 = play again, 0 = leave */
static int play_once(void)
{
    unsigned long last, now, delay;
    unsigned k;
    unsigned char al, ah;
    int running;

    reset_game();
    last = bios_ticks();
    running = 1;

    for (;;) {
        if (key_hit()) {
            k = key_get();
            al = (unsigned char)(k & 0xFF);
            ah = (unsigned char)((k >> 8) & 0xFF);

            if (running) {
                if (ah == 0x48) {
                    if (dy != 1) { dx = 0; dy = -1; }
                } else if (ah == 0x50) {
                    if (dy != -1) { dx = 0; dy = 1; }
                } else if (ah == 0x4B) {
                    if (dx != 1) { dx = -1; dy = 0; }
                } else if (ah == 0x4D) {
                    if (dx != -1) { dx = 1; dy = 0; }
                } else if (al == 'w' || al == 'W') {
                    if (dy != 1) { dx = 0; dy = -1; }
                } else if (al == 's' || al == 'S') {
                    if (dy != -1) { dx = 0; dy = 1; }
                } else if (al == 'a' || al == 'A') {
                    if (dx != 1) { dx = -1; dy = 0; }
                } else if (al == 'd' || al == 'D') {
                    if (dx != -1) { dx = 1; dy = 0; }
                } else if (al == 'q' || al == 'Q' || al == 27) {
                    return 0;
                } else if (al == 'p' || al == 'P') {
                    vstr(FY1 + 2, 0, "          PAUSED - press any key          ", 0x8E);
                    key_get();
                    draw_frame();
                    put_score();
                    last = bios_ticks();
                }
            } else {
                if (al == 'r' || al == 'R')
                    return 1;
                if (al == 'q' || al == 'Q' || al == 27)
                    return 0;
            }
        }

        now = bios_ticks();
        if (running) {
            delay = 4UL;
            if (slen > 20)
                delay = 3UL;
            if (slen > 40)
                delay = 2UL;
            if (now - last >= delay) {
                last = now;
                if (!step()) {
                    running = 0;
                    game_over_screen();
                }
            }
        } else {
            last = now;
        }
    }
}

int main(void)
{
    set_video_mode(3);
    cursor_off();

    while (play_once())
        ;

    clear_all();
    set_video_mode(3);
    cursor_on();
    printf("Thanks for playing SNAKE.\n");
    return 0;
}
