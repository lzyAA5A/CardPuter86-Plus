/* ============================================================
 * G2048.EXE  --  2048 for M5 Cardputer DOS
 *
 * Build (Digital Mars C 8.57, 16-bit DOS, small model):
 *     dmc -msd game2048.c -o G2048.EXE
 *
 * Runs on COMPAQ DOS 2.11 inside the CardPuter86 8086 emulator.
 * The board is 29x9 characters and lives inside the 40x16 viewport
 * the emulator shows.
 *
 * Keys:  W A S D or Fn+;  Fn+,  Fn+.  Fn+/   (arrow keys)
 *        R = restart,  Q / Esc = quit
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <dos.h>

#define SCR_COLS 40
#define SCR_ROWS 16

#define GX 5            /* screen column of the board */
#define GY 2            /* screen row of the board    */
#define CELL 6          /* characters inside one tile */
#define STEP (CELL + 1)

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
    r.x.cx = 0x2000;
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
    return r.x.ax;
}

static unsigned long bios_ticks(void)
{
    return *(unsigned long far *)0x0040006CUL;
}

/* ---------------- board ---------------- */
static int g[4][4];
static unsigned long score;
static unsigned long best;
static unsigned long rng_state;
static int won_shown;

static unsigned rnd(void)
{
    rng_state = rng_state * 1103515245UL + 12345UL;
    return (unsigned)((rng_state >> 16) & 0x7FFFu);
}

static int tile_attr(int v)
{
    switch (v) {
    case 0:    return 0x08;   /* empty    */
    case 2:    return 0x0F;
    case 4:    return 0x0E;
    case 8:    return 0x0C;
    case 16:   return 0x0D;
    case 32:   return 0x0B;
    case 64:   return 0x0A;
    case 128:  return 0x09;
    case 256:  return 0x0D;
    case 512:  return 0x0C;
    case 1024: return 0x0E;
    default:   return 0x0F;
    }
}

static int tile_bg(int v)
{
    if (v == 0)     return 0x08;
    if (v >= 1024)  return 0x40;
    if (v >= 128)   return 0x20;
    if (v >= 16)    return 0x10;
    return 0x00;
}

static void draw_tile(int r, int c)
{
    char buf[8];
    int v, row, col, i, len, attr;

    v = g[r][c];
    row = GY + 1 + r * 2;
    col = GX + 1 + c * STEP;

    if (v == 0)
        sprintf(buf, "%6s", "");
    else
        sprintf(buf, "%6d", v);
    len = 0;
    while (buf[len])
        len++;

    attr = (tile_bg(v) << 4) | tile_attr(v);
    if (v == 0)
        attr = 0x08;

    for (i = 0; i < CELL; i++) {
        if (v >= 1000 && (i == 0 || i == 1))
            vput(row, col + i, ' ', attr);
        else
            vput(row, col + i, buf[i], attr);
    }
}

static void draw_grid(void)
{
    int r, c, row, col, i;

    for (r = 0; r < 4; r++) {
        row = GY + 1 + r * 2;
        for (c = 0; c < 4; c++)
            draw_tile(r, c);
    }

    /* separators */
    for (r = 0; r < 5; r++) {
        row = GY + r * 2;
        col = GX;
        vput(row, col, '+', 0x08);
        for (c = 0; c < 4; c++) {
            for (i = 0; i < CELL; i++)
                vput(row, col + 1 + i, '-', 0x08);
            col += STEP;
            vput(row, col, '+', 0x08);
        }
    }
    /* side walls */
    for (r = 0; r < 4; r++) {
        row = GY + 1 + r * 2;
        vput(row, GX, '|', 0x08);
        vput(row, GX + 4 * STEP, '|', 0x08);
    }
}

static void draw_status(void)
{
    char buf[40];
    sprintf(buf, "2048  SCORE %-7lu BEST %-7lu", score, best);
    vfill(0, 0, SCR_COLS, ' ', 0x70);
    vstr(0, 0, buf, 0x70);
}

static void draw_help(void)
{
    vfill(12, 0, SCR_COLS, ' ', 0x07);
    vfill(13, 0, SCR_COLS, ' ', 0x07);
    vfill(14, 0, SCR_COLS, ' ', 0x07);
    vstr(12, 0, "WASD / Fn+;,. / = move tiles", 0x07);
    vstr(13, 0, "R = new game     Q = quit", 0x07);
    vstr(14, 0, "get 2048 to win", 0x07);
}

static void draw_all(void)
{
    draw_grid();
    draw_status();
    draw_help();
}

static int free_cells(void)
{
    int r, c, n;
    n = 0;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++)
            if (g[r][c] == 0)
                n++;
    return n;
}

static void spawn(void)
{
    int r, c, pick, n;
    if (free_cells() == 0)
        return;
    n = (int)(rnd() % (unsigned)(free_cells()));
    pick = -1;
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            if (g[r][c] == 0) {
                pick++;
                if (pick == n) {
                    g[r][c] = (rnd() % 10 == 0) ? 4 : 2;
                    return;
                }
            }
        }
    }
}

/* slide one line of 4 values toward index 0; returns 1 if changed */
static int slide_line(int *line)
{
    int i, j, moved;
    int out[4];

    moved = 0;
    for (i = 0; i < 4; i++)
        out[i] = 0;

    j = 0;
    for (i = 0; i < 4; i++) {
        if (line[i] != 0)
            out[j++] = line[i];
    }
    for (i = 0; i < 3; i++) {
        if (out[i] != 0 && out[i] == out[i + 1]) {
            out[i] = out[i] * 2;
            score += (unsigned long)out[i];
            if (out[i] == 2048 && !won_shown) {
                won_shown = 1;
                vstr(15, 0, "  ***  2048!  you win  ***   ", 0x8E);
            }
            for (j = i + 1; j < 3; j++)
                out[j] = out[j + 1];
            out[3] = 0;
            moved = 1;
        }
    }
    for (i = 0; i < 4; i++) {
        if (line[i] != out[i])
            moved = 1;
        line[i] = out[i];
    }
    return moved;
}

/* dir: 0 = left, 1 = right, 2 = up, 3 = down ; returns 1 if moved */
static int move_board(int dir)
{
    int line[4];
    int r, c, i, moved;

    moved = 0;
    for (i = 0; i < 4; i++) {
        if (dir == 0 || dir == 1) {
            for (c = 0; c < 4; c++)
                line[c] = g[i][(dir == 0) ? c : (3 - c)];
            if (slide_line(line)) {
                moved = 1;
                for (c = 0; c < 4; c++)
                    g[i][(dir == 0) ? c : (3 - c)] = line[c];
            }
        } else {
            for (r = 0; r < 4; r++)
                line[r] = g[(dir == 2) ? r : (3 - r)][i];
            if (slide_line(line)) {
                moved = 1;
                for (r = 0; r < 4; r++)
                    g[(dir == 2) ? r : (3 - r)][i] = line[r];
            }
        }
    }
    return moved;
}

static int can_move(void)
{
    int r, c;
    if (free_cells() > 0)
        return 1;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++) {
            if (c < 3 && g[r][c] == g[r][c + 1])
                return 1;
            if (r < 3 && g[r][c] == g[r + 1][c])
                return 1;
        }
    return 0;
}

static void new_game(void)
{
    int r, c;
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++)
            g[r][c] = 0;
    score = 0;
    won_shown = 0;
    rng_state = bios_ticks() * 2654435761UL + 12345UL;
    clear_all();
    spawn();
    spawn();
    draw_all();
}

int main(void)
{
    unsigned k;
    unsigned char al, ah;
    int dir, moved;

    set_video_mode(3);
    cursor_off();
    best = 0;
    new_game();

    for (;;) {
        if (!key_hit())
            continue;

        k = key_get();
        al = (unsigned char)(k & 0xFF);
        ah = (unsigned char)((k >> 8) & 0xFF);

        if (al == 'q' || al == 'Q' || al == 27)
            break;
        if (al == 'r' || al == 'R') {
            new_game();
            continue;
        }

        dir = -1;
        if (ah == 0x4B || al == 'a' || al == 'A')
            dir = 0;
        else if (ah == 0x4D || al == 'd' || al == 'D')
            dir = 1;
        else if (ah == 0x48 || al == 'w' || al == 'W')
            dir = 2;
        else if (ah == 0x50 || al == 's' || al == 'S')
            dir = 3;
        if (dir < 0)
            continue;

        moved = move_board(dir);
        if (moved) {
            spawn();
            draw_all();
            if (score > best)
                best = score;
        }
        if (!can_move()) {
            vfill(15, 0, SCR_COLS, ' ', 0x4F);
            vstr(15, 0, "  NO MORE MOVES - R restart  ", 0x4F);
        }
    }

    clear_all();
    set_video_mode(3);
    cursor_on();
    printf("2048 closed.\n");
    return 0;
}
