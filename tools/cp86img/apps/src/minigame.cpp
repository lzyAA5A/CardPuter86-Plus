/* ============================================================
 * MINIGAME.CPP -- one binary, seven games, for M5 Cardputer DOS
 *
 *   1  SNAKE          (C++ port of snake.c)
 *   2  MINESWEEPER    (12x8, 16 mines)
 *   3  GOMOKU         vs computer, 4 levels
 *   4  TETRIS         (10x14 well, 7 pieces, colours)
 *   5  PAC-MAN        (21x13 maze, 4 ghosts, power pellets)
 *   6  DINO RUN       (Chrome dino, jump cacti, speeds up)
 *   7  MAZE           (random 19x7-cell maze, find the exit)
 *
 * Build (Digital Mars C/C++ 8.57, 16-bit DOS, small model):
 *     dmc -msd -w minigame.cpp -o MINIGAME.EXE
 * then convert to a real .COM with tools\exe2com.py
 *     python tools\exe2com.py MINIGAME.EXE MINIGAME.COM
 *
 * Why the conversion: DMC's tiny model (-mt) needs ct.obj / sdt.lib
 * which this distribution does not ship, so a native .COM cannot be
 * linked.  exe2com merges the small-model segments into one 64 KB
 * block and patches every relocation by +0x10 (paragraph of 0x100),
 * which is exactly what a .COM load address implies.
 *
 * Display note: the emulator shows columns 0..39 and rows 0..15 of
 * the 80x25 text screen (240x128 px, 6x8 font), so every game lives
 * inside 40x16 and clear_all() blanks all 4000 VRAM bytes at start
 * so the emulator's auto-scroll never pans the viewport away.
 *
 * Keys on the Cardputer:
 *   WASD            plain letters
 *   Fn + ; , . /    arrow up/left/down/right
 *   Fn + `          Esc        Fn + backspace  Delete
 *   every game:  Q or Esc = back to the menu
 * ============================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>
#include "gomoku.h"

#define SCR_COLS 40
#define SCR_ROWS 16
#define VRAM     ((unsigned char far *)0xB8000000UL)

/* ============================================================
 * 1. tiny screen / keyboard / timer layer (shared by all games)
 * ============================================================ */

static void vput(int row, int col, int ch, int attr)
{
    unsigned char far *v;
    unsigned off;
    if (row < 0 || row >= SCR_ROWS || col < 0 || col >= SCR_COLS)
        return;
    v = VRAM;
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

/* blank the whole 80x25 page so the emulator viewport stays at row 0 */
static void clear_all(void)
{
    unsigned char far *v;
    unsigned i;
    v = VRAM;
    for (i = 0; i < 4000u; i += 2u) {
        v[i] = ' ';
        v[i + 1] = 0x07;
    }
}

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
    while (key_hit() && guard < 128) {
        key_get();
        guard++;
    }
}

static unsigned long bios_ticks(void)
{
    return *(unsigned long far *)0x0040006CUL;
}

static unsigned long rng_state = 12345UL;

static unsigned rnd(void)
{
    rng_state = rng_state * 1103515245UL + 12345UL;
    return (unsigned)((rng_state >> 16) & 0x7FFFu);
}

/* key classification helpers ------------------------------------- */

#define K_UP    0x4800u
#define K_DOWN  0x5000u
#define K_LEFT  0x4B00u
#define K_RIGHT 0x4D00u
#define K_ESC   0x001Bu
#define K_ENTER 0x000Du
#define K_SPACE 0x0020u

/* ASCII-only test on AL.  AH holds the *scan code* for ordinary keys
 * ('w' -> AX = 0x1161), so any "AH == 0" test on a letter key is dead
 * code - that is what made WASD/Q silent while "R restart" (which only
 * looked at AL) still worked.  Extended keys (arrows) come back as
 * AL == 0 + scancode in AH, which is why the AH tests below stay. */
static int is_alpha(unsigned char al, char lo)
{
    return al == (unsigned char)lo || al == (unsigned char)(lo - 'a' + 'A');
}

static int is_up(unsigned k)
{
    unsigned char al = (unsigned char)(k & 0xFF);
    unsigned char ah = (unsigned char)((k >> 8) & 0xFF);
    if (ah == 0x48) return 1;
    if (is_alpha(al, 'w')) return 1;
    return 0;
}

static int is_dn(unsigned k)
{
    unsigned char al = (unsigned char)(k & 0xFF);
    unsigned char ah = (unsigned char)((k >> 8) & 0xFF);
    if (ah == 0x50) return 1;
    if (is_alpha(al, 's')) return 1;
    return 0;
}

static int is_lf(unsigned k)
{
    unsigned char al = (unsigned char)(k & 0xFF);
    unsigned char ah = (unsigned char)((k >> 8) & 0xFF);
    if (ah == 0x4B) return 1;
    if (is_alpha(al, 'a')) return 1;
    return 0;
}

static int is_rt(unsigned k)
{
    unsigned char al = (unsigned char)(k & 0xFF);
    unsigned char ah = (unsigned char)((k >> 8) & 0xFF);
    if (ah == 0x4D) return 1;
    if (is_alpha(al, 'd')) return 1;
    return 0;
}

static int is_quit(unsigned k)
{
    unsigned char al = (unsigned char)(k & 0xFF);
    if (al == 0x1B) return 1;
    if (is_alpha(al, 'q')) return 1;
    return 0;
}

static int is_fire(unsigned k)
{
    unsigned char al = (unsigned char)(k & 0xFF);
    if (al == ' ') return 1;
    if (al == 0x0D) return 1;
    if (al == 'f' || al == 'F') return 1;   /* only used where noted */
    return 0;
}

/* draw a box from (r0,c0) to (r1,c1) inclusive */
static void vbox(int r0, int c0, int r1, int c1, int attr)
{
    int i;
    vput(r0, c0, '+', attr);
    vput(r0, c1, '+', attr);
    vput(r1, c0, '+', attr);
    vput(r1, c1, '+', attr);
    for (i = c0 + 1; i < c1; i++) {
        vput(r0, i, '-', attr);
        vput(r1, i, '-', attr);
    }
    for (i = r0 + 1; i < r1; i++) {
        vput(i, c0, '|', attr);
        vput(i, c1, '|', attr);
    }
}

/* ============================================================
 * 2. SNAKE
 * ============================================================ */

#define FX0 1
#define FX1 38
#define FY0 2
#define FY1 12
#define MAXLEN 600

static int sx[MAXLEN];
static int sy[MAXLEN];
static int slen;
static int sdx, sdy;
static int foodx, foody;
static unsigned long sscore;

static int body_at(int x, int y)
{
    int i;
    for (i = 0; i < slen; i++)
        if (sx[i] == x && sy[i] == y) return 1;
    return 0;
}

static void put_score(void)
{
    char buf[40];
    sprintf(buf, "SCORE %-8lu LEN %-3d", sscore, slen);
    vstr(0, 8, buf, 0x70);
}

static void snake_frame(void)
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
    vstr(FY1 + 2, 0, "WASD/Fn+;,. /  P pause  Q quit", 0x0F);
}

static void place_food(void)
{
    int guard = 0;
    do {
        foodx = FX0 + (int)(rnd() % (unsigned)(FX1 - FX0 + 1));
        foody = FY0 + (int)(rnd() % (unsigned)(FY1 - FY0 + 1));
        guard++;
    } while (body_at(foodx, foody) && guard < 4000);
    vput(foody, foodx, '$', 0x0E);
}

static void snake_reset(void)
{
    int i;
    slen = 4;
    sdx = 1;
    sdy = 0;
    sscore = 0;
    clear_all();
    snake_frame();
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

static int snake_step(void)
{
    int nx, ny, i, ate;

    nx = sx[0] + sdx;
    ny = sy[0] + sdy;

    if (nx < FX0 || nx > FX1 || ny < FY0 || ny > FY1) return 0;
    for (i = 0; i < slen - 1; i++)
        if (sx[i] == nx && sy[i] == ny) return 0;

    ate = (nx == foodx && ny == foody);

    if (!ate) {
        vput(sy[slen - 1], sx[slen - 1], ' ', 0x07);
        for (i = slen - 1; i > 0; i--) {
            sx[i] = sx[i - 1];
            sy[i] = sy[i - 1];
        }
    } else if (slen < MAXLEN) {
        for (i = slen; i > 0; i--) {
            sx[i] = sx[i - 1];
            sy[i] = sy[i - 1];
        }
        slen++;
        sscore += 10;
    }

    sx[0] = nx;
    sy[0] = ny;

    if (slen > 1) vput(sy[1], sx[1], '*', 0x02);
    vput(ny, nx, 'O', 0x0A);
    put_score();

    if (ate) place_food();
    return 1;
}

static void snake_over(void)
{
    char buf[48];
    vfill(7, 5, 30, ' ', 0x4F);
    vstr(7, 5, "         GAME OVER          ", 0x4F);
    sprintf(buf, "   score %-8lu len %-3d  ", sscore, slen);
    vstr(8, 5, buf, 0x4F);
    vstr(9, 5, "   R restart     Q quit     ", 0x4F);
}

static void game_snake(void)
{
    unsigned long last, now, delay;
    unsigned k;
    int running = 1;
    int alive = 1;

    snake_reset();
    last = bios_ticks();

    while (alive || running == 0) {
        if (key_hit()) {
            k = key_get();
            if (is_quit(k)) return;
            if (running) {
                if (is_up(k)) {
                    if (sdy != 1) { sdx = 0; sdy = -1; }
                } else if (is_dn(k)) {
                    if (sdy != -1) { sdx = 0; sdy = 1; }
                } else if (is_lf(k)) {
                    if (sdx != 1) { sdx = -1; sdy = 0; }
                } else if (is_rt(k)) {
                    if (sdx != -1) { sdx = 1; sdy = 0; }
                } else if ((unsigned char)(k & 0xFF) == 'p' ||
                           (unsigned char)(k & 0xFF) == 'P') {
                    vstr(FY1 + 2, 0, "       PAUSED - press any key       ", 0x8E);
                    key_get();
                    snake_frame();
                    put_score();
                    last = bios_ticks();
                }
            } else {
                unsigned char al = (unsigned char)(k & 0xFF);
                if (al == 'r' || al == 'R') {
                    snake_reset();
                    last = bios_ticks();
                    running = 1;
                }
            }
        }

        now = bios_ticks();
        if (running) {
            delay = 4UL;
            if (slen > 20) delay = 3UL;
            if (slen > 40) delay = 2UL;
            if (now - last >= delay) {
                last = now;
                if (!snake_step()) {
                    running = 0;
                    snake_over();
                }
            }
        } else {
            last = now;
            alive = 0;              /* stay in the loop until R or Q */
        }
        if (running) alive = 1;
    }
}

/* ============================================================
 * 3. MINESWEEPER   12 x 8, 16 mines, cell = 3 chars "[#]"
 * ============================================================ */

#define MW 12
#define MH 8
#define MCW 3
#define MCELLS (MW * MH)
#define MFX 2
#define MFY 2
#define MMINES 16

static int m_mine[MH][MW];
static int m_open[MH][MW];
static int m_flag[MH][MW];
static int m_nopen;
static int m_nflag;
static int m_placed;
static int mdead;
static int mwon;
static int mcx, mcy;
static int msafe_x, msafe_y;

static int mine_neighbors(int x, int y)
{
    int n = 0, dx, dy, px, py;
    for (dy = -1; dy <= 1; dy++)
        for (dx = -1; dx <= 1; dx++) {
            if (!dx && !dy) continue;
            px = x + dx;
            py = y + dy;
            if (px < 0 || px >= MW || py < 0 || py >= MH) continue;
            if (m_mine[py][px]) n++;
        }
    return n;
}

static void mine_place(int sx_, int sy_)
{
    int i, guard = 0, px, py, ok;
    for (py = 0; py < MH; py++)
        for (px = 0; px < MW; px++) m_mine[py][px] = 0;
    for (i = 0; i < MMINES; i++) {
        ok = 0;
        while (!ok && guard < 20000) {
            guard++;
            px = (int)(rnd() % (unsigned)MW);
            py = (int)(rnd() % (unsigned)MH);
            ok = 1;
            if (m_mine[py][px]) ok = 0;
            /* first click and its neighbours are always safe */
            if (px >= sx_ - 1 && px <= sx_ + 1 &&
                py >= sy_ - 1 && py <= sy_ + 1) ok = 0;
        }
        if (ok) m_mine[py][px] = 1;
    }
    m_placed = 1;
}

static void mine_draw_cell(int x, int y)
{
    int cs = MFX + x * MCW;
    int rs = MFY + y;
    int attr;
    char buf[8];
    int cur = (x == mcx && y == mcy);

    if (!m_open[y][x]) {
        if (m_flag[y][x]) {
            attr = cur ? 0xCE : 0x0C;
            buf[0] = '['; buf[1] = '*'; buf[2] = ']'; buf[3] = 0;
        } else {
            attr = cur ? 0x7F : 0x0F;
            buf[0] = '['; buf[1] = '#'; buf[2] = ']'; buf[3] = 0;
        }
    } else if (m_mine[y][x]) {
        attr = cur ? 0xCF : 0x0C;
        buf[0] = '['; buf[1] = '@'; buf[2] = ']'; buf[3] = 0;
    } else {
        int n = mine_neighbors(x, y);
        attr = cur ? 0x70 : 0x08;
        buf[0] = '[';
        buf[1] = (n == 0) ? ' ' : (char)('0' + n);
        buf[2] = ']'; buf[3] = 0;
    }
    vstr(rs, cs, buf, attr);
}

static void mine_draw_all(void)
{
    int x, y;
    for (y = 0; y < MH; y++)
        for (x = 0; x < MW; x++) mine_draw_cell(x, y);
}

static void mine_status(const char *msg)
{
    char buf[40];
    vfill(0, 0, SCR_COLS, ' ', 0x70);
    sprintf(buf, "MINE %02d  FLAG %02d  LEFT %02d", MMINES, m_nflag,
            MCELLS - MMINES - m_nopen);
    vstr(0, 0, buf, 0x70);
    vfill(11, 0, SCR_COLS, ' ', 0x0F);
    vstr(11, 0, msg, 0x0F);
}

/* iterative flood fill - recursion would eat ~3 KB of the 4 KB stack */
static int m_qx[MCELLS];
static int m_qy[MCELLS];

static void mine_flood(int x, int y)
{
    int head = 0, tail = 0, dx, dy, px, py, cx0, cy0;
    if (x < 0 || x >= MW || y < 0 || y >= MH) return;
    if (m_open[y][x] || m_flag[y][x]) return;
    m_qx[tail] = x; m_qy[tail] = y; tail++;
    while (head < tail) {
        cx0 = m_qx[head]; cy0 = m_qy[head]; head++;
        if (m_open[cy0][cx0] || m_flag[cy0][cx0]) continue;
        m_open[cy0][cx0] = 1;
        m_nopen++;
        mine_draw_cell(cx0, cy0);
        if (mine_neighbors(cx0, cy0) != 0) continue;
        for (dy = -1; dy <= 1; dy++)
            for (dx = -1; dx <= 1; dx++) {
                if (!dx && !dy) continue;
                px = cx0 + dx;
                py = cy0 + dy;
                if (px < 0 || px >= MW || py < 0 || py >= MH) continue;
                if (m_open[py][px] || m_flag[py][px]) continue;
                if (tail < MCELLS) { m_qx[tail] = px; m_qy[tail] = py; tail++; }
            }
    }
}

static void mine_reveal_all(void)
{
    int x, y;
    for (y = 0; y < MH; y++)
        for (x = 0; x < MW; x++)
            if (m_mine[y][x] && !m_flag[y][x]) {
                m_open[y][x] = 1;
                mine_draw_cell(x, y);
            }
}

static void mine_reset(void)
{
    int x, y;
    for (y = 0; y < MH; y++)
        for (x = 0; x < MW; x++) {
            m_mine[y][x] = 0;
            m_open[y][x] = 0;
            m_flag[y][x] = 0;
        }
    m_nopen = 0;
    m_nflag = 0;
    m_placed = 0;
    mdead = 0;
    mwon = 0;
    mcx = 0;
    mcy = 0;
    msafe_x = 0;
    msafe_y = 0;

    clear_all();
    vbox(1, 0, MFY + MH, SCR_COLS - 1, 0x08);
    vstr(12, 1, "SPACE open   F flag   Q quit", 0x0F);
    vstr(13, 1, "WASD or Fn+;,. /  move cursor", 0x07);
    mine_draw_all();
    mine_status("Pick a cell - mines placed after");
    key_flush();
}

static void game_mine(void)
{
    unsigned k;
    int over = 0;

    mine_reset();

    while (1) {
        if (!key_hit()) continue;
        k = key_get();
        if (is_quit(k)) return;

        if (over) {
            unsigned char al = (unsigned char)(k & 0xFF);
            if (al == 'r' || al == 'R') {
                mine_reset();
                over = 0;
            }
            continue;
        }

        if (is_up(k)) { if (mcy > 0) { mcy--; mine_draw_all(); } }
        else if (is_dn(k)) { if (mcy < MH - 1) { mcy++; mine_draw_all(); } }
        else if (is_lf(k)) { if (mcx > 0) { mcx--; mine_draw_all(); } }
        else if (is_rt(k)) { if (mcx < MW - 1) { mcx++; mine_draw_all(); } }
        else if ((unsigned char)(k & 0xFF) == 'f' ||
                 (unsigned char)(k & 0xFF) == 'F') {
            if (!m_open[mcy][mcx]) {
                if (m_flag[mcy][mcx]) { m_flag[mcy][mcx] = 0; m_nflag--; }
                else { m_flag[mcy][mcx] = 1; m_nflag++; }
                mine_draw_all();
                mine_status("Flag toggled.");
            }
        } else if ((unsigned char)(k & 0xFF) == ' ') {
            if (m_open[mcy][mcx] || m_flag[mcy][mcx]) continue;
            if (!m_placed) mine_place(mcx, mcy);
            if (m_mine[mcy][mcx]) {
                m_open[mcy][mcx] = 1;
                mdead = 1;
                mine_draw_all();
                mine_reveal_all();
                mine_status("BOOM!  R restart   Q quit");
                over = 1;
                continue;
            }
            mine_flood(mcx, mcy);
            mine_draw_all();
            if (MCELLS - MMINES - m_nopen <= 0) {
                mwon = 1;
                mine_status("CLEARED!   R restart   Q quit");
                vstr(13, 1, "*** YOU WIN ***   R replay  Q quit      ", 0x2F);
                over = 1;
            } else {
                mine_status("Keep going.");
            }
        }
    }
}

/* ============================================================
 * 4. GOMOKU  (11x11) with four difficulty levels
 *
 *    level 1  EASY    one ply, 35% random blunder, blocks 60%
 *    level 2  MEDIUM  one ply greedy, always wins / blocks
 *    level 3  HARD    two ply: my move minus opponent's best reply
 *    level 4  EXPERT  two ply + double-threat bonus, wider search
 * ============================================================ */


/* ---------- gomoku screen ---------- */

#define GX 8          /* left border column */
#define GY 1          /* top border row   */
#define GCW 2         /* chars per cell   */

static Gomoku gg;
static int g_cx, g_cy;
static int g_lvl;
static int g_over;
static int g_winwho;

static void gomoku_header(void)
{
    char buf[40];
    static const char *lname[4] = { "EASY", "MEDIUM", "HARD", "EXPERT" };
    vfill(0, 0, SCR_COLS, ' ', 0x70);
    sprintf(buf, "GOMOKU  LV%d %s", g_lvl, lname[g_lvl - 1]);
    vstr(0, 0, buf, 0x70);
    vstr(0, 26, "X=you O=pc", 0x70);
}

static void gomoku_cell(int x, int y)
{
    int col = GX + 1 + x * GCW;
    int row = GY + 1 + y;
    int cur = (x == g_cx && y == g_cy && !g_over);
    int v = gg.bd[y][x];
    int ch;
    int attr;

    if (v == P_HUM) ch = 'X';
    else if (v == P_AI) ch = 'O';
    else ch = '.';

    if (v == P_HUM) attr = cur ? 0x1F : 0x0B;
    else if (v == P_AI) attr = cur ? 0x2F : 0x0E;
    else attr = cur ? 0x7F : 0x08;

    vput(row, col, ' ', attr);
    vput(row, col + 1, ch, attr);
}

static void gomoku_board(void)
{
    int x, y;

    for (y = 0; y < BSZ; y++)
        for (x = 0; x < BSZ; x++) gomoku_cell(x, y);

    /* borders + coordinate ticks */
    vput(GY, GX, '+', 0x08);
    vput(GY, GX + 1 + BSZ * GCW, '+', 0x08);
    vput(GY + 1 + BSZ, GX, '+', 0x08);
    vput(GY + 1 + BSZ, GX + 1 + BSZ * GCW, '+', 0x08);
    for (x = 0; x < BSZ * GCW; x++) {
        vput(GY, GX + 1 + x, '-', 0x08);
        vput(GY + 1 + BSZ, GX + 1 + x, '-', 0x08);
    }
    for (y = 0; y < BSZ; y++) {
        vput(GY + 1 + y, GX, '|', 0x08);
        vput(GY + 1 + y, GX + 1 + BSZ * GCW, '|', 0x08);
    }
}

static void gomoku_msg(int row, const char *s)
{
    vfill(row, 0, SCR_COLS, ' ', 0x0F);
    vstr(row, 0, s, 0x0F);
}

static int game_gomoku(int lvl)
{
    unsigned k;
    int mx, my, turns;

    g_lvl = lvl;
    g_over = 0;
    g_winwho = 0;
    gg.reset(lvl);
    g_cx = BSZ / 2;
    g_cy = BSZ / 2;
    turns = 0;
    rng_state = bios_ticks() * 2654435761UL + (unsigned long)lvl * 7919UL + 17UL;
    gg.seed(rng_state);

    clear_all();
    gomoku_header();
    gomoku_board();
    gomoku_msg(14, "Your turn (X).");
    gomoku_msg(15, "WASD/Fn+;,. /  move   SPACE place   Q quit");
    key_flush();

    for (;;) {
        if (g_over) {
            if (!key_hit()) continue;
            k = key_get();
            if (is_quit(k)) return 0;
            if ((unsigned char)(k & 0xFF) == 'r' ||
                (unsigned char)(k & 0xFF) == 'R') {
                return 1;
            }
            continue;
        }

        if (turns & 1) {
            /* ---- computer ---- */
            gomoku_msg(14, "thinking ...");
            gg.think(&mx, &my);
            if (mx < 0 || mx >= BSZ || my < 0 || my >= BSZ) {
                mx = BSZ / 2;
                my = BSZ / 2;
            }
            gg.put(mx, my, P_AI);
            turns++;
            gomoku_cell(mx, my);
            if (gg.win_at(mx, my, P_AI)) {
                g_over = 1;
                g_winwho = 2;
                gomoku_msg(14, "*** computer wins. R replay  Q quit ***");
                continue;
            }
            gomoku_msg(14, "Your turn (X).");
            continue;
        }

        if (!key_hit()) continue;
        k = key_get();
        if (is_quit(k)) return 0;
        /* Move first, then repaint: gomoku_cell() decides "is this the cursor"
           from the *current* g_cx/g_cy, so erasing the old cell before the
           coordinate changed would repaint it highlighted and leave a grey
           trail across the board. */
        if (is_up(k)) { if (g_cy > 0) { g_cy--; gomoku_cell(g_cx, g_cy + 1); gomoku_cell(g_cx, g_cy); } }
        else if (is_dn(k)) { if (g_cy < BSZ - 1) { g_cy++; gomoku_cell(g_cx, g_cy - 1); gomoku_cell(g_cx, g_cy); } }
        else if (is_lf(k)) { if (g_cx > 0) { g_cx--; gomoku_cell(g_cx + 1, g_cy); gomoku_cell(g_cx, g_cy); } }
        else if (is_rt(k)) { if (g_cx < BSZ - 1) { g_cx++; gomoku_cell(g_cx - 1, g_cy); gomoku_cell(g_cx, g_cy); } }
        else if ((unsigned char)(k & 0xFF) == ' ') {
            if (gg.bd[g_cy][g_cx] != P_EMP) continue;
            gg.put(g_cx, g_cy, P_HUM);
            turns++;
            gomoku_cell(g_cx, g_cy);
            if (gg.win_at(g_cx, g_cy, P_HUM)) {
                g_over = 1;
                g_winwho = 1;
                gomoku_msg(14, "*** YOU WIN!  R replay  Q quit ***");
                continue;
            }
            if (turns >= BSZ * BSZ) {
                g_over = 1;
                g_winwho = 0;
                gomoku_msg(14, "*** draw.  R replay  Q quit ***");
                continue;
            }
            gomoku_msg(14, "thinking ...");
        }
    }
}

/* ============================================================
 * 5. TETRIS   10 x 14 well, each cell = 2 chars, colour per piece
 *
 * The well does not have room for a title above it (16 text rows), so
 * the frame owns rows 0..15 between columns 1 and 22 and the score
 * panel lives at columns 24..39.
 * ============================================================ */

#define TW      10
#define TH      14
#define TCOL    1
#define TROW    0

static unsigned char tboard[TH][TW];    /* 0 = empty, 1..7 = piece id + 1 */

/* spawn masks, 4x4 box, bit index = y * 4 + x (x = column) */
static const unsigned tet_mask[7] = {
    0x0F00u,   /* I */
    0x0066u,   /* O */
    0x0017u,   /* T */
    0x0036u,   /* S */
    0x0063u,   /* Z */
    0x0071u,   /* J */
    0x0074u    /* L */
};

/* background colour of each piece (text attribute, bg in the high nibble) */
static const unsigned char tet_attr[7] = {
    0x30, 0xE0, 0x50, 0x20, 0x40, 0x10, 0x60
};

static const int tet_points[5] = { 0, 40, 100, 300, 1200 };

static unsigned tet_rotate(unsigned m)
{
    unsigned r = 0;
    int y, x;
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            if (m & (1u << (y * 4 + x)))
                r |= 1u << (x * 4 + (3 - y));
    return r;
}

static unsigned tet_shape(int piece, int rot)
{
    unsigned m = tet_mask[piece];
    rot &= 3;
    while (rot-- > 0) m = tet_rotate(m);
    return m;
}

static int tet_fit(unsigned m, int px, int py)
{
    int y, x;
    for (y = 0; y < 4; y++) {
        for (x = 0; x < 4; x++) {
            int bx, by;
            if (!(m & (1u << (y * 4 + x)))) continue;
            bx = px + x;
            by = py + y;
            if (bx < 0 || bx >= TW) return 0;
            if (by >= TH) return 0;
            if (by >= 0 && tboard[by][bx]) return 0;
        }
    }
    return 1;
}

static void tet_cell(int y, int x, int attr)
{
    if (y < 0 || y >= TH || x < 0 || x >= TW) return;
    vfill(TROW + 1 + y, TCOL + 1 + x * 2, 2, ' ', attr);
}

static void tet_draw(unsigned m, int px, int py, int attr)
{
    int y, x;
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            if (m & (1u << (y * 4 + x)))
                tet_cell(py + y, px + x, attr);
}

static void tet_frame(void)
{
    int i;
    for (i = 0; i <= TW * 2 + 1; i++) {
        vput(TROW, TCOL + i, '=', 0x08);
        vput(TROW + TH + 1, TCOL + i, '=', 0x08);
    }
    for (i = 1; i <= TH; i++) {
        vput(TROW + i, TCOL, '|', 0x08);
        vput(TROW + i, TCOL + TW * 2 + 1, '|', 0x08);
    }
}

static void tet_panel(long score, int lines, int level, int next)
{
    char buf[40];
    int y, x;
    unsigned m;

    vstr(0, 24, "T E T R I S", 0x0E);
    sprintf(buf, "score %-8lu", (unsigned long)score);
    vstr(2, 24, buf, 0x0F);
    sprintf(buf, "lines %-5d", lines);
    vstr(3, 24, buf, 0x0F);
    sprintf(buf, "level %-4d", level);
    vstr(4, 24, buf, 0x0F);

    vstr(6, 24, "next:", 0x07);
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            vfill(8 + y, 26 + x * 2, 2, ' ', 0x00);
    m = tet_mask[next];
    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            if (m & (1u << (y * 4 + x)))
                vfill(8 + y, 26 + x * 2, 2, ' ', tet_attr[next]);

    vstr(12, 24, "A/D  move", 0x07);
    vstr(13, 24, "W  rotate", 0x07);
    vstr(14, 24, "S drop SPC", 0x07);
    vstr(15, 24, "P pause Q quit", 0x07);
}

/* move / rotate the falling piece; returns 1 when it actually moved */
static int tet_move(int piece, int rot, int *px, int *py, int nx, int ny)
{
    unsigned m = tet_shape(piece, rot);
    if (!tet_fit(m, nx, ny)) return 0;
    tet_draw(m, *px, *py, 0);
    *px = nx;
    *py = ny;
    tet_draw(m, *px, *py, tet_attr[piece]);
    return 1;
}

static int tet_spin(int piece, int *rot, int px, int py)
{
    unsigned old = tet_shape(piece, *rot);
    unsigned nw = tet_shape(piece, *rot + 1);
    if (!tet_fit(nw, px, py)) return 0;
    tet_draw(old, px, py, 0);
    *rot = (*rot + 1) & 3;
    tet_draw(nw, px, py, tet_attr[piece]);
    return 1;
}

/* stamp the piece into the board, clear full rows, repaint the well */
static int tet_lock(int piece, int rot, int px, int py,
                    long *score, int *lines, int *level)
{
    unsigned m = tet_shape(piece, rot);
    int y, x, i, cleared = 0;

    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++)
            if (m & (1u << (y * 4 + x))) {
                int by = py + y, bx = px + x;
                if (by >= 0 && by < TH && bx >= 0 && bx < TW)
                    tboard[by][bx] = (unsigned char)(piece + 1);
            }

    for (y = TH - 1; y >= 0; y--) {
        int full = 1;
        for (x = 0; x < TW; x++)
            if (!tboard[y][x]) { full = 0; break; }
        if (!full) continue;
        cleared++;
        for (i = y; i > 0; i--)
            for (x = 0; x < TW; x++)
                tboard[i][x] = tboard[i - 1][x];
        for (x = 0; x < TW; x++) tboard[0][x] = 0;
        y++;
    }

    if (cleared) {
        *score += (long)tet_points[cleared] * (long)(*level);
        *lines += cleared;
        *level = 1 + *lines / 10;
        if (*level > 9) *level = 9;
    }

    for (y = 0; y < TH; y++)
        for (x = 0; x < TW; x++)
            tet_cell(y, x, tboard[y][x] ? tet_attr[tboard[y][x] - 1] : 0);
    return cleared;
}

static void tet_over(long score, int lines)
{
    char buf[40];
    vfill(6, 4, 17, ' ', 0x4F);
    vstr(6, 4, "     GAME OVER    ", 0x4F);
    sprintf(buf, " score %-6lu    ", (unsigned long)score);
    vstr(7, 4, buf, 0x4F);
    sprintf(buf, " lines %-3d      ", lines);
    vstr(8, 4, buf, 0x4F);
    vstr(9, 4, " R retry  Q quit  ", 0x4F);
    vstr(10, 4, "                 ", 0x4F);
}

static void game_tetris(void)
{
    long score = 0;
    int lines = 0, level = 1;
    int piece, rot, px, py, next;
    unsigned long last, now, drop;
    unsigned k, m;
    int over = 0;
    char buf[40];

    memset(tboard, 0, sizeof(tboard));
    clear_all();
    tet_frame();
    next = (int)(rnd() % 7);
    piece = next;
    rot = 0;
    px = 3;
    py = 0;
    next = (int)(rnd() % 7);
    tet_panel(score, lines, level, next);
    m = tet_shape(piece, rot);
    tet_draw(m, px, py, tet_attr[piece]);
    last = bios_ticks();

    for (;;) {
        while (key_hit()) {
            k = key_get();
            if (is_quit(k)) return;
            if (over) {
                if (is_alpha((unsigned char)(k & 0xFF), 'r')) {
                    memset(tboard, 0, sizeof(tboard));
                    score = 0;
                    lines = 0;
                    level = 1;
                    over = 0;
                    clear_all();
                    tet_frame();
                    next = (int)(rnd() % 7);
                    piece = next;
                    rot = 0;
                    px = 3;
                    py = 0;
                    next = (int)(rnd() % 7);
                    tet_panel(score, lines, level, next);
                    tet_draw(tet_shape(piece, rot), px, py, tet_attr[piece]);
                    last = bios_ticks();
                }
                continue;
            }
            m = tet_shape(piece, rot);
            if (is_lf(k)) {
                tet_move(piece, rot, &px, &py, px - 1, py);
            } else if (is_rt(k)) {
                tet_move(piece, rot, &px, &py, px + 1, py);
            } else if (is_up(k)) {
                tet_spin(piece, &rot, px, py);
            } else if (is_dn(k)) {
                tet_move(piece, rot, &px, &py, px, py + 1);
                last = bios_ticks();
            } else if ((unsigned char)(k & 0xFF) == ' ') {
                while (tet_fit(m, px, py + 1)) {
                    tet_draw(m, px, py, 0);
                    py++;
                    tet_draw(m, px, py, tet_attr[piece]);
                }
                last = bios_ticks() - 4000UL;   /* force the lock below */
            } else if (is_alpha((unsigned char)(k & 0xFF), 'p')) {
                vfill(7, 4, 17, ' ', 0x8E);
                vstr(7, 4, "     PAUSED      ", 0x8E);
                vstr(8, 4, " press any key   ", 0x8E);
                key_get();
                for (m = 0; m < 17u; m++) {
                    vput(7, 4 + (int)m, ' ', 0x00);
                    vput(8, 4 + (int)m, ' ', 0x00);
                }
                last = bios_ticks();
                m = tet_shape(piece, rot);
                tet_draw(m, px, py, tet_attr[piece]);
            }
        }

        now = bios_ticks();
        if (over) {
            last = now;
            continue;
        }
        drop = (unsigned long)(level >= 9 ? 4 : 14 - level);
        if (now - last >= drop) {
            last = now;
            m = tet_shape(piece, rot);
            if (!tet_fit(m, px, py + 1)) {
                tet_lock(piece, rot, px, py, &score, &lines, &level);
                piece = next;
                rot = 0;
                px = 3;
                py = 0;
                next = (int)(rnd() % 7);
                tet_panel(score, lines, level, next);
                m = tet_shape(piece, rot);
                if (!tet_fit(m, px, py)) {
                    over = 1;
                    tet_over(score, lines);
                } else {
                    tet_draw(m, px, py, tet_attr[piece]);
                }
            } else {
                tet_draw(m, px, py, 0);
                py++;
                tet_draw(m, px, py, tet_attr[piece]);
            }
        }
    }
    (void)buf;
}

/* ============================================================
 * 6. PAC-MAN   21 x 13 maze, single-char cells, 4 ghosts
 *
 * The maze is generated as a 6 x 10 cell grid (cells on odd rows and
 * columns, walls on even ones) with extra walls knocked out so it has
 * loops instead of a perfect-maze tree.  Verified: every pellet is
 * reachable from (9,9), no dead pocket is isolated.
 * ============================================================ */

#define PM_H 13
#define PM_W 21

static const char *pm_template[PM_H] = {
    "#####################",
    "#.....#.......#.....#",
    "#.###.#.#.#.#.###.#.#",
    "#...#.....#.#.....#.#",
    "#.#.#####.#.#.#.###.#",
    "#...................#",
    "###.#.#.#.###.#####.#",
    "#.....#...#...#...#.#",
    "#.#.###.#.#.#.#.###.#",
    "#...................#",
    "#.#.#.#######.###.###",
    "#...#...............#",
    "#####################"
};

#define PM_START_X 9
#define PM_START_Y 9

static const int pm_home_x[4] = { 9, 5, 13, 9 };
static const int pm_home_y[4] = { 1, 5, 5, 11 };

static char pm_map[PM_H][PM_W];
static int pm_px, pm_py, pm_dx, pm_dy, pm_nx, pm_ny;
static int pm_gx[4], pm_gy[4], pm_gdx[4], pm_gdy[4];
static int pm_lives, pm_score, pm_pellets, pm_level, pm_ghost_eaten;
static unsigned long pm_power_end;
static unsigned long pm_freeze_end;   /* ghosts idle for ~2 s after a respawn */

static int pm_open(int y, int x)
{
    if (y < 0 || y >= PM_H || x < 0 || x >= PM_W) return 0;
    return pm_map[y][x] != '#';
}

static int pm_scared(void)
{
    return pm_power_end != 0UL && bios_ticks() < pm_power_end;
}

static void pm_paint(int y, int x)
{
    int i;
    if (y < 0 || y >= PM_H || x < 0 || x >= PM_W) return;
    if (pm_map[y][x] == '#') {
        vput(1 + y, 1 + x, '#', 0x01);
        return;
    }
    if (y == pm_py && x == pm_px) {
        vput(1 + y, 1 + x, 'C', 0x0E);
        return;
    }
    for (i = 0; i < 4; i++) {
        if (pm_gx[i] == x && pm_gy[i] == y) {
            vput(1 + y, 1 + x, 'M', pm_scared() ? 0x09 : 0x0C);
            return;
        }
    }
    if (pm_map[y][x] == '.') {
        vput(1 + y, 1 + x, '.', 0x0E);
        return;
    }
    if (pm_map[y][x] == 'o') {
        vput(1 + y, 1 + x, 'o', 0x0E);
        return;
    }
    vput(1 + y, 1 + x, ' ', 0x07);
}

static void pm_status(void)
{
    char buf[40];
    int i;
    vfill(0, 0, SCR_COLS, ' ', 0x07);
    sprintf(buf, "PAC %-6d", pm_score);
    vstr(0, 1, buf, 0x0F);
    for (i = 0; i < 3; i++)
        vput(0, 12 + i, (i < pm_lives) ? 'C' : ' ', 0x0E);
    sprintf(buf, "lvl %-2d", pm_level);
    vstr(0, 17, buf, 0x0F);
    vstr(2, 24, "PAC-MAN", 0x0E);
    sprintf(buf, "left %-3d", pm_pellets);
    vstr(4, 24, buf, 0x07);
    if (pm_scared()) vstr(6, 24, "POWER!", 0x0E);
    else vstr(6, 24, "      ", 0x07);
    vstr(8, 24, "eat all", 0x07);
    vstr(9, 24, "the dots", 0x07);
    vstr(11, 24, "o = power", 0x08);
    vstr(12, 24, "Q quit", 0x07);
}

static void pm_draw_all(void)
{
    int y, x;
    for (y = 0; y < PM_H; y++)
        for (x = 0; x < PM_W; x++)
            pm_paint(y, x);
    pm_status();
}

static void pm_build_map(void)
{
    int y, x;

    for (y = 0; y < PM_H; y++)
        for (x = 0; x < PM_W; x++)
            pm_map[y][x] = pm_template[y][x];

    pm_map[1][1] = 'o';
    pm_map[1][PM_W - 2] = 'o';
    pm_map[PM_H - 2][1] = 'o';
    pm_map[PM_H - 2][PM_W - 2] = 'o';

    pm_map[PM_START_Y][PM_START_X] = ' ';
    for (x = 0; x < 4; x++) pm_map[pm_home_y[x]][pm_home_x[x]] = ' ';

    pm_pellets = 0;
    for (y = 0; y < PM_H; y++)
        for (x = 0; x < PM_W; x++)
            if (pm_map[y][x] == '.' || pm_map[y][x] == 'o') pm_pellets++;
}

static void pm_place_actors(void)
{
    int i;
    pm_px = PM_START_X;
    pm_py = PM_START_Y;
    pm_dx = pm_dy = pm_nx = pm_ny = 0;
    for (i = 0; i < 4; i++) {
        pm_gx[i] = pm_home_x[i];
        pm_gy[i] = pm_home_y[i];
        pm_gdx[i] = 0;
        pm_gdy[i] = (i == 0) ? 1 : -1;
    }
    pm_power_end = 0UL;
    pm_freeze_end = bios_ticks() + 36UL;
    pm_ghost_eaten = 0;
}

static void pm_step_pac(void)
{
    int ox, oy;

    if ((pm_nx || pm_ny) && pm_open(pm_py + pm_ny, pm_px + pm_nx)) {
        pm_dx = pm_nx;
        pm_dy = pm_ny;
    }
    if (!pm_open(pm_py + pm_dy, pm_px + pm_dx)) {
        pm_dx = pm_dy = 0;
        return;
    }
    ox = pm_px;
    oy = pm_py;
    pm_px += pm_dx;
    pm_py += pm_dy;
    if (pm_map[pm_py][pm_px] == '.') {
        pm_map[pm_py][pm_px] = ' ';
        pm_score += 10;
        pm_pellets--;
    } else if (pm_map[pm_py][pm_px] == 'o') {
        pm_map[pm_py][pm_px] = ' ';
        pm_score += 50;
        pm_pellets--;
        pm_power_end = bios_ticks() + 220UL;
        pm_ghost_eaten = 0;
    }
    /* repaint only AFTER the move: pm_paint() draws whoever stands on the
     * cell it is given, so painting the old cell first would redraw Pac-Man
     * there and leave a trail of C's. */
    pm_paint(oy, ox);
    pm_paint(pm_py, pm_px);
    pm_status();
}

static void pm_step_ghost(int i, int scared)
{
    static const signed char DX[4] = { 0, 0, -1, 1 };
    static const signed char DY[4] = { -1, 1, 0, 0 };
    int j, pick = -1, best = 0, ncand = 0;
    int first = (int)(rnd() & 3);
    int cand[4];
    int wild = ((rnd() % 5) == 0);      /* one step in five is a wander */

    for (j = 0; j < 4; j++) {
        int d = (j + first) & 3;
        int nx = pm_gx[i] + DX[d], ny = pm_gy[i] + DY[d];
        int v;
        if (!pm_open(ny, nx)) continue;
        if (pm_gdx[i] == -DX[d] && pm_gdy[i] == -DY[d] &&
            (pm_gdx[i] || pm_gdy[i])) continue;
        cand[ncand++] = d;
        v = (nx > pm_px ? nx - pm_px : pm_px - nx) +
            (ny > pm_py ? ny - pm_py : pm_py - ny);
        if (scared) v = -v;
        if (pick < 0 || v < best) {
            pick = d;
            best = v;
        }
    }
    if (ncand > 0 && wild) pick = cand[(int)(rnd() % (unsigned)ncand)];
    if (pick < 0) {                 /* dead end: turn around */
        for (j = 0; j < 4; j++)
            if (pm_open(pm_gy[i] + DY[j], pm_gx[i] + DX[j])) { pick = j; break; }
    }
    if (pick < 0) return;

    {
        int ox = pm_gx[i], oy = pm_gy[i];
        pm_gdx[i] = DX[pick];
        pm_gdy[i] = DY[pick];
        pm_gx[i] = ox + DX[pick];
        pm_gy[i] = oy + DY[pick];
        pm_paint(oy, ox);               /* move first, then clear the old cell */
        pm_paint(pm_gy[i], pm_gx[i]);
    }
}

static void pm_repaint_ghosts(void)
{
    int i;
    for (i = 0; i < 4; i++) pm_paint(pm_gy[i], pm_gx[i]);
}

/* overlay 19 chars wide (columns 3..21) so nothing leaks into the panel */
static void pm_message(const char *top, const char *bot, int attr)
{
    vfill(6, 3, 19, ' ', attr);
    vstr(6, 3, top, attr);
    vfill(7, 3, 19, ' ', attr);
    vstr(7, 3, bot, attr);
}

/* returns 0 when the player has to leave the game */
static int pm_next_life(void)
{
    unsigned k;
    pm_lives--;
    if (pm_lives <= 0) return 0;
    pm_message("      OUCH!        ", "  press a key      ", 0x4F);
    while (!key_hit()) ;
    k = key_get();
    if (is_quit(k)) return -1;
    pm_place_actors();
    pm_draw_all();
    return 1;
}

static void game_pacman(void)
{
    unsigned long last_pac, last_ghost, now;
    unsigned long step_pac, step_ghost;
    unsigned k;
    int i, scared;
    int ret;

restart:
    pm_level = 1;
    pm_score = 0;
    pm_lives = 3;
    pm_build_map();
    pm_place_actors();
    clear_all();
    pm_draw_all();
    vstr(14, 1, "WASD/arrows move, P pause, Q quit", 0x08);
    vstr(15, 1, "eat every dot; o = power pellet", 0x08);

    last_pac = last_ghost = bios_ticks();

    for (;;) {
        while (key_hit()) {
            k = key_get();
            if (is_quit(k)) return;
            if (is_up(k)) { pm_nx = 0; pm_ny = -1; }
            else if (is_dn(k)) { pm_nx = 0; pm_ny = 1; }
            else if (is_lf(k)) { pm_nx = -1; pm_ny = 0; }
            else if (is_rt(k)) { pm_nx = 1; pm_ny = 0; }
            else if (is_alpha((unsigned char)(k & 0xFF), 'p')) {
                pm_message("      PAUSED       ", "  press a key      ", 0x8E);
                key_get();
                pm_draw_all();
                last_pac = last_ghost = bios_ticks();
            }
        }

        now = bios_ticks();
        step_pac = 2UL;
        step_ghost = (pm_level >= 3) ? 2UL : 3UL;

        if (pm_power_end != 0UL && now >= pm_power_end) {
            pm_power_end = 0UL;
            pm_repaint_ghosts();
            pm_status();
        }

        if (now - last_pac >= step_pac) {
            last_pac = now;
            pm_step_pac();
        }
        if (now - last_ghost >= step_ghost) {
            last_ghost = now;
            if (now >= pm_freeze_end) {          /* short grace after a respawn */
                scared = pm_scared();
                for (i = 0; i < 4; i++) pm_step_ghost(i, scared);
            }
        }

        /* collisions */
        for (i = 0; i < 4; i++) {
            if (pm_gx[i] != pm_px || pm_gy[i] != pm_py) continue;
            if (pm_scared()) {
                pm_ghost_eaten++;
                pm_score += 200 * pm_ghost_eaten;
                pm_gx[i] = pm_home_x[i];
                pm_gy[i] = pm_home_y[i];
                pm_gdx[i] = 0;
                pm_gdy[i] = 0;
                pm_paint(pm_gy[i], pm_gx[i]);
                pm_status();
            } else {
                ret = pm_next_life();
                if (ret < 0) return;            /* player pressed Q */
                if (ret == 0) goto game_over;   /* no lives left */
                last_pac = last_ghost = bios_ticks();
                break;
            }
        }

        if (pm_pellets <= 0) {
            pm_level++;
            pm_build_map();
            pm_place_actors();
            clear_all();
            pm_draw_all();
            vstr(14, 1, "WASD/arrows move, P pause, Q quit", 0x08);
            vstr(15, 1, "eat every dot; o = power pellet", 0x08);
            last_pac = last_ghost = bios_ticks();
        }
    }

    /* game over: keep the maze on screen, overlay the result */
game_over:
    pm_message("    GAME OVER     ", " R retry Q quit    ", 0x4F);
    for (;;) {
        while (!key_hit()) ;
        k = key_get();
        if (is_quit(k)) return;
        if (is_alpha((unsigned char)(k & 0xFF), 'r')) goto restart;
    }
}

/* ============================================================
 * 6. DINO RUN   (the Chrome offline dinosaur, 40x16 text mode)
 *
 *   ground line on row 13, dino stands on it and occupies 3 rows,
 *   cacti scroll in from the right, jump with SPACE / W / UP.
 *   Physics use 1/4 row units so the arc is smooth at 18.2 Hz.
 * ============================================================ */

#define DY_GROUND   13
#define DY_DINO_COL 5
#define DY_DINO_W   4
#define DY_DINO_H   3
#define DY_MAXOB    6

static int dy_ox[DY_MAXOB];        /* obstacle column                     */
static int dy_oh[DY_MAXOB];        /* obstacle height in rows (2 or 3)     */
static int dy_ow[DY_MAXOB];        /* obstacle width in columns (1 .. 3)   */
static int dy_on[DY_MAXOB];
static int dy_h;                   /* dino height above ground, 1/4 rows   */
static int dy_vy;
static int dy_air;
static int dy_score;
static int dy_best;
static int dy_tick;
static int dy_speed;               /* ticks between column scrolls         */
static int dy_spawn_gap;           /* columns left before the next spawn   */

/* two-frame "legs" sprite, 4 wide x 3 tall */
static const char *dy_sprite[2][DY_DINO_H] = {
    { " ## ", "##o#", "####" },
    { " ## ", "##o#", "## #" }
};

static void dino_status(void)
{
    char buf[44];
    vfill(15, 0, SCR_COLS, ' ', 0x70);
    sprintf(buf, "HI %04d  SCORE %05d", dy_best, dy_score);
    vstr(15, 0, buf, 0x70);
    vstr(15, 24, "  SPACE = JUMP  ", 0x70);
}

static void dino_draw(void)
{
    int r, c, i;
    int up = dy_h >> 2;
    int top = DY_GROUND - DY_DINO_H - up;
    int phase = (dy_tick >> 2) & 1;

    /* play field rows 0..14 (row 14 too: kills the menu leftovers) */
    for (r = 0; r < SCR_ROWS - 1; r++)
        vfill(r, 0, SCR_COLS, ' ', 0x00);
    /* ground */
    for (c = 0; c < SCR_COLS; c++)
        vput(DY_GROUND, c, '_', 0x08);

    for (i = 0; i < DY_MAXOB; i++) {
        int k;
        if (!dy_on[i]) continue;
        for (k = 0; k < dy_ow[i]; k++) {
            int x = dy_ox[i] + k;
            int h = (k == 0) ? dy_oh[i] : ((k == 1) ? 2 : 3);
            int q;
            if (x < 0 || x >= SCR_COLS) continue;
            vput(DY_GROUND - h, x, 'Y', 0x02);
            for (q = DY_GROUND - h + 1; q < DY_GROUND; q++)
                vput(q, x, '|', 0x02);
        }
    }

    r = phase;
    for (c = 0; c < DY_DINO_H; c++) {
        const char *s = dy_sprite[r][c];
        int k;
        for (k = 0; k < DY_DINO_W; k++) {
            char ch = s[k];
            if (ch == ' ') vput(top + c, DY_DINO_COL + k, ' ', 0x00);
            else vput(top + c, DY_DINO_COL + k, ch, ch == 'o' ? 0x0E : 0x0A);
        }
    }
    dino_status();
}

/* Called once per scrolled column.  dy_spawn_gap counts DOWN in columns,
   so obstacles enter from the right edge one after another instead of a
   whole crowd being created on the same tick (the old bug: the first
   obstacle arrived after 7 s and the rest were stacked on top of it). */
static void dino_spawn(void)
{
    int i;
    if (dy_spawn_gap > 0) {
        dy_spawn_gap--;
        return;
    }
    for (i = 0; i < DY_MAXOB; i++) {
        if (dy_on[i]) continue;
        dy_on[i] = 1;
        dy_ox[i] = SCR_COLS - 1;
        dy_oh[i] = (rnd() % 4 == 0) ? 3 : 2;
        {
            unsigned w = rnd() % 10;
            dy_ow[i] = (w < 6) ? 1 : ((w < 9) ? 2 : 3);
        }
        dy_spawn_gap = 10 + (int)(rnd() % 9);
        if (dy_score > 400) dy_spawn_gap -= 2;
        if (dy_spawn_gap < 7) dy_spawn_gap = 7;
        return;
    }
}

static int dino_hit(void)
{
    int up = dy_h >> 2;
    int top = DY_GROUND - DY_DINO_H - up;
    int bot = DY_GROUND - 1 - up;      /* the bottom moves up with the body */
    int i, k, l;
    for (i = 0; i < DY_MAXOB; i++) {
        int x;
        if (!dy_on[i]) continue;
        x = dy_ox[i];
        if (x + dy_ow[i] - 1 < DY_DINO_COL + 1) continue;
        if (x > DY_DINO_COL + DY_DINO_W - 2) continue;
        for (k = 0; k < dy_ow[i]; k++) {
            int oc = x + k;
            int oh = (k == 0) ? dy_oh[i] : 2;
            int otop = DY_GROUND - oh;
            if (oc < DY_DINO_COL + 1 || oc > DY_DINO_COL + DY_DINO_W - 2) continue;
            l = (otop > top) ? otop : top;                 /* overlap top    */
            {
                int hi = (bot < DY_GROUND - 1) ? bot : DY_GROUND - 1;
                if (l <= hi) return 1;
            }
        }
    }
    return 0;
}

static void dino_over(void)
{
    char buf[40];
    vfill(6, 5, 30, ' ', 0x4F);
    vstr(6, 5, "         GAME OVER          ", 0x4F);
    sprintf(buf, "   score %-6d best %-6d ", dy_score, dy_best);
    vstr(7, 5, buf, 0x4F);
    vstr(8, 5, "   R restart     Q quit     ", 0x4F);
}

static void dino_reset(void)
{
    int i;
    for (i = 0; i < DY_MAXOB; i++) { dy_on[i] = 0; dy_ox[i] = SCR_COLS + 12; }
    dy_h = 0;
    dy_vy = 0;
    dy_air = 0;
    dy_score = 0;
    dy_tick = 0;
    dy_speed = 4;
    dy_spawn_gap = 16;
    dino_draw();
}

static void game_dino(void)
{
    unsigned long last = bios_ticks(), now;
    unsigned long scroll_last = last;
    unsigned long phys_last = 0;
    unsigned k;
    int running = 1;
    int i;

    dino_reset();
    dino_spawn();
    last = bios_ticks();
    scroll_last = last;
    phys_last = last;

    while (1) {
        while (key_hit()) {
            k = key_get();
            if (is_quit(k)) return;
            if (!running) {
                unsigned char al = (unsigned char)(k & 0xFF);
                if (al == 'r' || al == 'R') {
                    dino_reset();
                    dino_spawn();
                    running = 1;
                    last = bios_ticks();
                    scroll_last = last;
                }
                continue;
            }
            if (is_fire(k) || is_up(k) || is_alpha((unsigned char)(k & 0xFF), 'w')) {
                if (dy_air == 0) { dy_vy = 7; dy_air = 1; }
            } else if (is_alpha((unsigned char)(k & 0xFF), 'q')) {
                return;
            }
        }
        if (!running) continue;

        now = bios_ticks();

        /* Jump physics: exactly one step per BIOS tick (18.2 Hz).
           NOT once per loop iteration -- the old code ran the whole
           parabola inside a few microseconds, so the dino never visibly
           left the ground even though the key was seen. */
        if (dy_air && now != phys_last) {
            phys_last = now;
            dy_h += dy_vy;
            dy_vy--;
            if (dy_h <= 0) { dy_h = 0; dy_vy = 0; dy_air = 0; }
        }

        if (now - scroll_last >= (unsigned long)dy_speed) {
            scroll_last = now;
            dy_tick++;
            if (dy_tick % 3 == 0) dy_score++;
            if (dy_score > 300) dy_speed = 3;
            if (dy_score > 700) dy_speed = 2;
            if (dy_score > 1200) dy_speed = 1;

            for (i = 0; i < DY_MAXOB; i++) {
                if (!dy_on[i]) continue;
                dy_ox[i]--;
                if (dy_ox[i] + dy_ow[i] < 0) {
                    dy_on[i] = 0;
                    dy_score += 10;
                }
            }
            dino_spawn();
            if (dino_hit()) {
                running = 0;
                if (dy_score > dy_best) dy_best = dy_score;
                dino_over();
                continue;
            }
            dino_draw();
        }
    }
}

/* ============================================================
 * 7. MAZE   (random 19x7-cell maze = 39x15 chars, find the exit)
 *
 *   recursive-backtracker carving, iterative (explicit stack),
 *   player walks one step per key press, visited cells are marked
 *   with a dim dot so you can see where you have been.
 * ============================================================ */

#define MZCW    19                      /* cells across        */
#define MZCH    7                       /* cells down          */
#define MZCOLS  (MZCW * 2 + 1)          /* 39                  */
#define MZROWS  (MZCH * 2 + 1)          /* 15                  */

static unsigned char mzw[MZROWS][MZCOLS];   /* 1 = wall            */
static unsigned char mzv[MZROWS][MZCOLS];   /* 1 = visited         */
static int mz_px, mz_py;
static int mz_moves;
static int mz_level;
static unsigned long mz_start_tick;
static unsigned long mz_best_ticks;

static void maze_status(void)
{
    char buf[44];
    unsigned long sec10 = (bios_ticks() - mz_start_tick) * 10UL / 182UL;
    vfill(15, 0, SCR_COLS, ' ', 0x70);
    sprintf(buf, "MAZE %-2d moves %-4d %2lu.%lus  Q=quit", mz_level, mz_moves,
            sec10 / 10UL, sec10 % 10UL);
    vstr(15, 0, buf, 0x70);
}

static void maze_put_player(void)
{
    vput(mz_py, mz_px, '@', 0x0E);
}

static void maze_build(void)
{
    static unsigned char sx[MZCW * MZCH];
    static unsigned char sy[MZCW * MZCH];
    int sp = 0;
    int i, j;
    int cx, cy;

    memset(mzw, 1, sizeof(mzw));
    memset(mzv, 0, sizeof(mzv));
    memset(sx, 0, sizeof(sx));
    memset(sy, 0, sizeof(sy));

    /* carve all cell centres, keep walls in between */
    for (j = 0; j < MZCH; j++)
        for (i = 0; i < MZCW; i++)
            mzw[j * 2 + 1][i * 2 + 1] = 0;

    sx[0] = 0; sy[0] = 0;
    sp = 1;
    mzv[0][0] = 1;
    while (sp > 0) {
        static const int dd[4][2] = { {0,-1}, {1,0}, {0,1}, {-1,0} };
        int open = 0;
        cx = sx[sp - 1];
        cy = sy[sp - 1];
        for (i = 0; i < 4; i++) {
            int nx = cx + dd[i][0];
            int ny = cy + dd[i][1];
            if (nx >= 0 && nx < MZCW && ny >= 0 && ny < MZCH && !mzv[ny][nx]) open++;
        }
        if (!open) { sp--; continue; }
        /* pick a random open neighbour */
        for (;;) {
            int d = (int)(rnd() % 4);
            int nx = cx + dd[d][0];
            int ny = cy + dd[d][1];
            if (nx < 0 || nx >= MZCW || ny < 0 || ny >= MZCH || mzv[ny][nx]) continue;
            /* knock down the wall between the two cells */
            mzw[cy * 2 + 1 + dd[d][1]][cx * 2 + 1 + dd[d][0]] = 0;
            mzv[ny][nx] = 1;
            sx[sp] = (unsigned char)nx;
            sy[sp] = (unsigned char)ny;
            sp++;
            break;
        }
    }
    /* the DFS above may leave cells unreachable if it backtracked early;
       carve a few extra links so the exit is always reachable */
    for (j = 0; j < MZCH; j++) {
        for (i = 0; i < MZCW; i++) {
            if (!mzv[j][i]) {
                mzw[j * 2 + 1][i * 2 + 1] = 1;      /* just wall it off */
            }
        }
    }
    /* guarantee a path from the start cell to the exit cell by carving
       an L-shaped corridor when the flood fill did not reach it */
    {
        static int reach[MZCH][MZCW];
        static unsigned char qx[MZCW * MZCH], qy[MZCW * MZCH];
        int head = 0, tail = 0;
        memset(reach, 0, sizeof(reach));
        reach[0][0] = 1;
        qx[tail] = 0; qy[tail] = 0; tail++;
        while (head < tail) {
            static const int dd2[4][2] = { {0,-1}, {1,0}, {0,1}, {-1,0} };
            int ax = qx[head], ay = qy[head];
            head++;
            for (i = 0; i < 4; i++) {
                int nx = ax + dd2[i][0];
                int ny = ay + dd2[i][1];
                if (nx < 0 || nx >= MZCW || ny < 0 || ny >= MZCH) continue;
                if (reach[ny][nx]) continue;
                if (mzw[ay * 2 + 1 + dd2[i][1]][ax * 2 + 1 + dd2[i][0]]) continue;
                reach[ny][nx] = 1;
                qx[tail] = (unsigned char)nx; qy[tail] = (unsigned char)ny; tail++;
            }
        }
        if (!reach[MZCH - 1][MZCW - 1]) {
            /* carve a horizontal + vertical corridor to the exit cell */
            for (i = 1; i <= MZCW - 1; i++)
                mzw[1][i * 2 + 1] = 0, mzw[1][i * 2] = 0;
            for (j = 1; j <= MZCH - 1; j++)
                mzw[j * 2 + 1][MZCW * 2 - 1] = 0, mzw[j * 2][MZCW * 2 - 1] = 0;
        }
    }

    mz_px = 1;
    mz_py = 1;
    mz_moves = 0;
}

static void maze_draw(void)
{
    int r, c;
    for (r = 0; r < MZROWS; r++) {
        for (c = 0; c < MZCOLS; c++) {
            if (mzw[r][c]) vput(r, c, '#', 0x08);
            else if (mzv[r][c]) vput(r, c, '.', 0x07);
            else vput(r, c, ' ', 0x00);
        }
        vfill(r, MZCOLS, SCR_COLS - MZCOLS, ' ', 0x00);
    }
    vput(MZROWS - 2, MZCOLS - 2, 'E', 0x0A);
    maze_put_player();
    maze_status();
}

static void game_maze(void)
{
    unsigned k;
    int done = 0;

    mz_level = 1;
    mz_best_ticks = 0;
    clear_all();
    maze_build();
    mz_start_tick = bios_ticks();
    maze_draw();

    while (1) {
        while (!key_hit()) ;
        k = key_get();
        if (is_quit(k)) return;
        if (is_alpha((unsigned char)(k & 0xFF), 'r')) {
            maze_build();
            maze_draw();
            mz_start_tick = bios_ticks();
            continue;
        }
        if (done) continue;

        {
            int nx = mz_px, ny = mz_py;
            if (is_up(k)) ny--;
            else if (is_dn(k)) ny++;
            else if (is_lf(k)) nx--;
            else if (is_rt(k)) nx++;
            else if (is_alpha((unsigned char)(k & 0xFF), 'w')) ny--;
            else if (is_alpha((unsigned char)(k & 0xFF), 's')) ny++;
            else if (is_alpha((unsigned char)(k & 0xFF), 'a')) nx--;
            else if (is_alpha((unsigned char)(k & 0xFF), 'd')) nx++;
            else continue;

            if (nx < 0 || nx >= MZCOLS || ny < 0 || ny >= MZROWS) continue;
            if (mzw[ny][nx]) continue;

            vput(mz_py, mz_px, '.', 0x07);
            mzv[mz_py][mz_px] = 1;
            mz_px = nx;
            mz_py = ny;
            mz_moves++;
            maze_put_player();
            maze_status();

            if (mz_px == MZCOLS - 2 && mz_py == MZROWS - 2) {
                unsigned long t = bios_ticks() - mz_start_tick;
                char buf[40];
                if (mz_best_ticks == 0 || t < mz_best_ticks) mz_best_ticks = t;
                vfill(6, 6, 28, ' ', 0x2F);
                vstr(6, 6, "        MAZE CLEARED         ", 0x2F);
                sprintf(buf, "  moves %-4d time %lu.%lus  ", mz_moves,
                        t * 10UL / 182UL / 10UL, (t * 10UL / 182UL) % 10UL);
                vstr(7, 6, buf, 0x2F);
                vstr(8, 6, "  any key = next maze  Q out ", 0x2F);
                vstr(9, 6, "                              ", 0x2F);
                done = 1;
                /* wait for a key, then start the next maze */
                while (!key_hit()) ;
                k = key_get();
                if (is_quit(k)) return;
                mz_level++;
                maze_build();
                maze_draw();
                mz_start_tick = bios_ticks();
                done = 0;
            }
        }
    }
}

/* ============================================================
 * 8. menus
 * ============================================================ */

static const char *MENU_ITEMS[7] = {
    "1  SNAKE",
    "2  MINESWEEPER",
    "3  GOMOKU  (vs computer)",
    "4  TETRIS",
    "5  PAC-MAN",
    "6  DINO RUN",
    "7  MAZE"
};

static void menu_draw(int sel)
{
    int i;
    int attr;

    clear_all();
    vbox(1, 2, 13, SCR_COLS - 3, 0x0B);
    vstr(2, 7, "M I N I G A M E", 0x0E);
    vstr(3, 8, "for Cardputer DOS", 0x07);

    for (i = 0; i < 7; i++) {
        attr = (i == sel) ? 0x70 : 0x0F;
        vfill(5 + i, 5, 28, ' ', attr);
        vstr(5 + i, 7, MENU_ITEMS[i], attr);
        if (i == sel) vstr(5 + i, 4, ">", 0x0E);
    }

    vstr(12, 4, "W/S or 1-7 choose, SPACE run, Q quit", 0x07);
    vstr(14, 2, "Cardputer keys: Fn+; , . /  = arrows", 0x08);
    vstr(15, 2, "Fn+` = Esc    Fn+backspace = Delete", 0x08);
}

/* run menu entry <sel>; 0..4 */
static int level_menu(void);

static void run_item(int sel)
{
    int lv;
    if (sel == 0) {
        game_snake();
    } else if (sel == 1) {
        game_mine();
    } else if (sel == 2) {
        lv = level_menu();
        while (lv && game_gomoku(lv)) ;
    } else if (sel == 3) {
        game_tetris();
    } else if (sel == 4) {
        game_pacman();
    } else if (sel == 5) {
        game_dino();
    } else {
        game_maze();
    }
}

static int level_menu(void)
{
    int sel = 1;
    unsigned k;
    static const char *names[4] = {
        "1  EASY    - misses blocks, blunders",
        "2  MEDIUM  - always wins/blocks, greedy",
        "3  HARD    - looks one move ahead",
        "4  EXPERT  - double threats, widest search"
    };

    clear_all();
    vbox(1, 1, 12, SCR_COLS - 2, 0x0B);
    vstr(2, 5, "GOMOKU - choose level", 0x0E);
    vstr(4, 3, "you are X, computer is O", 0x07);
    while (1) {
        int i;
        for (i = 0; i < 4; i++) {
            int attr = (i + 1 == sel) ? 0x70 : 0x0F;
            vfill(6 + i, 3, SCR_COLS - 6, ' ', attr);
            vstr(6 + i, 4, names[i], attr);
            if (i + 1 == sel) vstr(6 + i, 2, ">", 0x0E);
        }
        vstr(11, 3, "W/S pick, SPACE start, Q back", 0x07);

        while (!key_hit()) ;
        k = key_get();
        if (is_quit(k)) return 0;
        if (is_up(k)) { sel--; if (sel < 1) sel = 4; }
        else if (is_dn(k)) { sel++; if (sel > 4) sel = 1; }
        else if ((unsigned char)(k & 0xFF) >= '1' &&
                 (unsigned char)(k & 0xFF) <= '4')
            sel = (unsigned char)(k & 0xFF) - '0';
        else if ((unsigned char)(k & 0xFF) == ' ') return sel;
        else if ((unsigned char)(k & 0xFF) == 0x0D) return sel;
    }
}

#ifdef PAC_SELFTEST
/* Build check for the pellet / power-pellet path:
 *     dmc -msd -w -DPAC_SELFTEST minigame.cpp -o PACTEST.EXE
 * Puts the player next to the corner power pellet, steps him onto it and
 * prints the counters afterwards. */
int main(void)
{
    set_video_mode(3);
    printf("PAC selftest\n");
    pm_build_map();
    pm_place_actors();
    printf("start pellets=%d score=%d scared=%d\n",
           pm_pellets, pm_score, pm_scared() ? 1 : 0);
    pm_py = 1;
    pm_px = 2;
    pm_dx = -1;
    pm_dy = 0;
    pm_nx = -1;
    pm_ny = 0;
    pm_step_pac();
    printf("after o: pos=%d,%d pellets=%d score=%d scared=%d\n",
           pm_px, pm_py, pm_pellets, pm_score, pm_scared() ? 1 : 0);
    pm_nx = 0;
    pm_ny = 1;
    pm_step_pac();                     /* one ordinary dot */
    printf("after dot: pellets=%d score=%d\n", pm_pellets, pm_score);
    return 0;
}
#else
#ifdef TET_SELFTEST
/* Build check for the line-clear path only:
 *     dmc -msd -w -DTET_SELFTEST minigame.cpp -o TETTEST.EXE
 * Fills the bottom row except the four cells an I piece covers, drops that
 * piece in and prints what tet_lock() scored and how the rows moved. */
int main(void)
{
    long score = 0;
    int lines = 0, level = 1;
    int x;

    set_video_mode(3);
    printf("TET selftest\n");
    memset(tboard, 0, sizeof(tboard));
    for (x = 0; x < TW; x++) tboard[TH - 1][x] = 1;
    for (x = 0; x < 4; x++) tboard[TH - 1][x] = 0;
    tboard[TH - 2][5] = 2;
    tet_lock(0, 0, 0, TH - 3, &score, &lines, &level);
    printf("score=%ld lines=%d level=%d\n", score, lines, level);
    printf("last row x5=%d  prev row x5=%d\n",
           (int)tboard[TH - 1][5], (int)tboard[TH - 2][5]);
    return 0;
}
#else
int main(void)
{
    int sel = 0;
    unsigned k;
    unsigned char al;

    set_video_mode(3);
    cursor_off();
    rng_state = bios_ticks() * 2654435761UL + 12345UL;

    for (;;) {
        menu_draw(sel);
        key_flush();
        while (!key_hit()) ;
        k = key_get();
        al = (unsigned char)(k & 0xFF);

        if (is_quit(k)) break;
        if (is_up(k)) { sel--; if (sel < 0) sel = 6; }
        else if (is_dn(k)) { sel++; if (sel > 6) sel = 0; }
        else if (al >= '1' && al <= '7') {
            sel = al - '1';
            run_item(sel);
        } else if (al == ' ' || al == 0x0D) {
            run_item(sel);
        }
    }

    clear_all();
    set_video_mode(3);
    cursor_on();
    printf("MINIGAME - bye.\n");
    return 0;
}
#endif
#endif



