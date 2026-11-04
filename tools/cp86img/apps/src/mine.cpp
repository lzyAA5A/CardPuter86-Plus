/* ============================================================
 * MINE.EXE  --  Sao Lei / Minesweeper for M5 Cardputer DOS
 *
 * Build (Digital Mars C++ 8.57, 16-bit DOS, small model):
 *     dmc -msd mine.cpp -o MINE.EXE
 *
 * Written in C++ on purpose: the whole board is a class with
 * private state and member functions (this is the C++ demo of
 * the DOS game pack).
 *
 * Runs on COMPAQ DOS 2.11 inside the CardPuter86 8086 emulator.
 * Everything is drawn inside columns 0..39 / rows 0..15 because
 * that is the viewport the emulator shows.
 *
 * Keys:  W A S D or Fn+; Fn+, Fn+. Fn+/  = move
 *        SPACE / ENTER = open cell
 *        F = toggle flag
 *        R = new game      Q / Esc = quit
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <dos.h>

#define GW 12          /* field width  in cells */
#define GH 8           /* field height in cells */
#define MINES 16       /* mines on the board    */

#define FX 2           /* screen column of the first cell */
#define FY 2           /* screen row    of the first cell */
#define CW 3           /* characters per cell */

#define SCR_COLS 40
#define SCR_ROWS 16
#define ROW_MSG (FY + GH + 1)   /* = 11: message line */

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

/* ============================================================
 * The board: a real class, so this file is genuinely C++.
 * ============================================================ */
class Board {
private:
    unsigned char is_mine[GH][GW];
    unsigned char is_open[GH][GW];
    unsigned char is_flag[GH][GW];
    int flags_left;
    int opened;
    int placed;
    unsigned long seed;

    unsigned long next_random();

public:
    void reset();
    void place_mines(int safe_x, int safe_y);
    int  adjacent(int x, int y) const;
    int  open_cell(int x, int y);
    void toggle_flag(int x, int y);
    int  is_won() const;
    void reveal_all();

    int  mine_here(int x, int y) const { return is_mine[y][x]; }
    int  open_here(int x, int y) const { return is_open[y][x]; }
    int  flag_here(int x, int y) const { return is_flag[y][x]; }
    int  flags() const { return flags_left; }
    int  done() const { return opened; }
};

unsigned long Board::next_random()
{
    seed = seed * 1103515245UL + 12345UL;
    return (seed >> 16) & 0x7FFFu;
}

void Board::reset()
{
    int x, y;
    for (y = 0; y < GH; y++) {
        for (x = 0; x < GW; x++) {
            is_mine[y][x] = 0;
            is_open[y][x] = 0;
            is_flag[y][x] = 0;
        }
    }
    flags_left = MINES;
    opened = 0;
    placed = 0;
    seed = bios_ticks() * 2654435761UL + 7919UL;
}

/* Mines are laid only after the first click, so the first click is
 * never a mine (and never a zero-click on a mine). */
void Board::place_mines(int safe_x, int safe_y)
{
    int i, x, y;
    if (placed)
        return;
    placed = 1;
    i = 0;
    while (i < MINES) {
        x = (int)(next_random() % GW);
        y = (int)(next_random() % GH);
        if (x >= safe_x - 1 && x <= safe_x + 1 &&
            y >= safe_y - 1 && y <= safe_y + 1)
            continue;
        if (is_mine[y][x])
            continue;
        is_mine[y][x] = 1;
        i++;
    }
}

int Board::adjacent(int x, int y) const
{
    int dx, dy, n, xx, yy;
    n = 0;
    for (dy = -1; dy <= 1; dy++) {
        for (dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0)
                continue;
            xx = x + dx;
            yy = y + dy;
            if (xx < 0 || xx >= GW || yy < 0 || yy >= GH)
                continue;
            if (is_mine[yy][xx])
                n++;
        }
    }
    return n;
}

/* returns 1 if a mine was hit */
int Board::open_cell(int x, int y)
{
    unsigned char stack_x[GH * GW];
    unsigned char stack_y[GH * GW];
    int sp, cx, cy, dx, dy, xx, yy;

    if (x < 0 || x >= GW || y < 0 || y >= GH)
        return 0;
    if (is_flag[y][x] || is_open[y][x])
        return 0;

    if (!placed)
        place_mines(x, y);

    if (is_mine[y][x]) {
        is_open[y][x] = 1;
        opened++;
        return 1;
    }

    sp = 0;
    stack_x[sp] = (unsigned char)x;
    stack_y[sp] = (unsigned char)y;
    sp++;

    while (sp > 0) {
        sp--;
        cx = stack_x[sp];
        cy = stack_y[sp];
        if (cx < 0 || cx >= GW || cy < 0 || cy >= GH)
            continue;
        if (is_open[cy][cx] || is_flag[cy][cx] || is_mine[cy][cx])
            continue;
        is_open[cy][cx] = 1;
        opened++;
        if (adjacent(cx, cy) != 0)
            continue;
        for (dy = -1; dy <= 1; dy++) {
            for (dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0)
                    continue;
                xx = cx + dx;
                yy = cy + dy;
                if (xx < 0 || xx >= GW || yy < 0 || yy >= GH)
                    continue;
                if (is_open[yy][xx] || is_flag[yy][xx])
                    continue;
                if (sp < GH * GW) {
                    stack_x[sp] = (unsigned char)xx;
                    stack_y[sp] = (unsigned char)yy;
                    sp++;
                }
            }
        }
    }
    return 0;
}

void Board::toggle_flag(int x, int y)
{
    if (x < 0 || x >= GW || y < 0 || y >= GH)
        return;
    if (is_open[y][x])
        return;
    if (is_flag[y][x]) {
        is_flag[y][x] = 0;
        flags_left++;
    } else {
        if (flags_left <= 0)
            return;
        is_flag[y][x] = 1;
        flags_left--;
    }
}

int Board::is_won() const
{
    return opened + MINES == GW * GH;
}

void Board::reveal_all()
{
    int x, y;
    for (y = 0; y < GH; y++)
        for (x = 0; x < GW; x++)
            if (is_mine[y][x])
                is_open[y][x] = 1;
}

/* ============================================================
 * drawing
 * ============================================================ */
static Board board;
static int cur_x, cur_y;
static int game_over, game_won;
static unsigned long start_tick;
static char message[40];

static int digit_attr(int n)
{
    static const int a[9] = { 0x07, 0x0B, 0x0A, 0x0C, 0x09, 0x0D, 0x0E, 0x0F, 0x07 };
    if (n < 0 || n > 8)
        return 0x07;
    return a[n];
}

static void draw_cell(int x, int y)
{
    int row, col, attr, n;
    char c;

    row = FY + y;
    col = FX + x * CW;

    attr = 0x07;
    c = '.';
    if (board.open_here(x, y)) {
        if (board.mine_here(x, y)) {
            c = '*';
            attr = 0x4C;
        } else {
            n = board.adjacent(x, y);
            if (n == 0) {
                c = ' ';
                attr = 0x08;
            } else {
                c = (char)('0' + n);
                attr = digit_attr(n);
            }
        }
    } else if (board.flag_here(x, y)) {
        c = 'F';
        attr = 0x0C;
    }

    if (!game_over && x == cur_x && y == cur_y)
        attr = (attr & 0x0F) | 0x70;   /* reverse video = cursor */

    vput(row, col, ' ', attr);
    vput(row, col + 1, c, attr);
    vput(row, col + 2, ' ', attr);
}

static void draw_all(void)
{
    int x, y;

    vfill(0, 0, SCR_COLS, ' ', 0x70);
    vstr(0, 0, " MINE ", 0x70);

    for (y = 0; y < GH; y++) {
        vput(FY + y, FX - 1, '|', 0x08);
        vput(FY + y, FX + GW * CW, '|', 0x08);
    }
    vfill(FY - 1, FX - 1, GW * CW + 2, '=', 0x08);
    vfill(FY + GH, FX - 1, GW * CW + 2, '=', 0x08);

    for (y = 0; y < GH; y++)
        for (x = 0; x < GW; x++)
            draw_cell(x, y);

    vfill(ROW_MSG, 0, SCR_COLS, ' ', 0x07);
    vfill(ROW_MSG + 1, 0, SCR_COLS, ' ', 0x07);
    vstr(ROW_MSG + 1, 0, "WASD/Fn-arrows move SPACE open F flag", 0x07);
    vfill(ROW_MSG + 2, 0, SCR_COLS, ' ', 0x07);
    vstr(ROW_MSG + 2, 0, "R new game          Q quit", 0x07);
}

static void draw_status(void)
{
    char buf[40];
    unsigned long sec;
    sec = (bios_ticks() - start_tick) / 18UL;
    sprintf(buf, "MINES %-3d TIME %-4lu", board.flags(), sec);
    vstr(0, 8, buf, 0x70);
}

static void set_message(const char *s)
{
    int i;
    i = 0;
    while (s[i] && i < 39) {
        message[i] = s[i];
        i++;
    }
    message[i] = 0;
    vfill(ROW_MSG, 0, SCR_COLS, ' ', 0x0F);
    vstr(ROW_MSG, 1, message, 0x0F);
}

/* ---------------- one game ---------------- */
static void new_game(void)
{
    clear_all();
    board.reset();
    cur_x = GW / 2;
    cur_y = GH / 2;
    game_over = 0;
    game_won = 0;
    start_tick = bios_ticks();
    draw_all();
    draw_status();
    set_message("Use WASD / Fn-arrows to move.");
}

static void open_here(void)
{
    int hit;
    if (game_over)
        return;
    hit = board.open_cell(cur_x, cur_y);
    if (hit) {
        game_over = 1;
        board.reveal_all();
        draw_all();
        set_message("BOOM!  R = new game, Q = quit");
        return;
    }
    draw_all();
    draw_status();
    if (board.is_won()) {
        game_over = 1;
        game_won = 1;
        set_message("YOU WIN!  R = new game, Q = quit");
    } else {
        set_message("Keep going...");
    }
}

/* returns 1 = new game, 0 = quit */
static int run_game(void)
{
    unsigned k;
    unsigned char al, ah;
    unsigned long now_sec, last_sec;

    new_game();
    last_sec = 0;
    for (;;) {
        /* refresh the clock line once per second, not once per poll */
        now_sec = (bios_ticks() - start_tick) / 18UL;
        if (now_sec != last_sec) {
            last_sec = now_sec;
            draw_status();
        }

        if (!key_hit())
            continue;

        k = key_get();
        al = (unsigned char)(k & 0xFF);
        ah = (unsigned char)((k >> 8) & 0xFF);

        if (al == 'q' || al == 'Q' || al == 27)
            return 0;
        if (al == 'r' || al == 'R')
            return 1;
        if (game_over)
            continue;

        if (ah == 0x48 || al == 'w' || al == 'W') {
            if (cur_y > 0) { int ox = cur_x, oy = cur_y; cur_y--; draw_cell(ox, oy); draw_cell(cur_x, cur_y); }
        } else if (ah == 0x50 || al == 's' || al == 'S') {
            if (cur_y < GH - 1) { int ox = cur_x, oy = cur_y; cur_y++; draw_cell(ox, oy); draw_cell(cur_x, cur_y); }
        } else if (ah == 0x4B || al == 'a' || al == 'A') {
            if (cur_x > 0) { int ox = cur_x, oy = cur_y; cur_x--; draw_cell(ox, oy); draw_cell(cur_x, cur_y); }
        } else if (ah == 0x4D || al == 'd' || al == 'D') {
            if (cur_x < GW - 1) { int ox = cur_x, oy = cur_y; cur_x++; draw_cell(ox, oy); draw_cell(cur_x, cur_y); }
        } else if (al == ' ') {
            open_here();
        } else if (al == 'f' || al == 'F') {
            board.toggle_flag(cur_x, cur_y);
            draw_cell(cur_x, cur_y);
            draw_status();
        }
    }
}

int main(void)
{
    set_video_mode(3);
    cursor_off();

    while (run_game())
        ;

    clear_all();
    set_video_mode(3);
    cursor_on();
    printf("Minesweeper closed.\n");
    return 0;
}

