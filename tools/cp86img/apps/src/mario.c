/* ============================================================
 * MARIO.C -- a text-mode platformer for CardPuter86 DOS
 *
 * Screen budget: the emulator only shows 40x16 characters of the
 * 80x25 text screen (240x128 pixels, 6x8 font), so the world is
 * 40 columns wide and 16 rows tall:
 *
 *   row  0..12   sky / platforms / coins
 *   row  13      ground line   ('_')
 *   row  14      dirt          ('_')
 *   row  15      status bar
 *
 * Everything is drawn straight into B800:0000 with vput(), like the
 * other games, so a frame costs about 120 byte-writes.
 *
 * Build (Digital Mars C/C++, 16-bit DOS, small model):
 *     dmc -msd -w mario.c -o MARIO.EXE
 * Do NOT build a .COM: the tiny model needs ct.obj, which this
 * distribution does not ship (Error 2: File Not Found ct.obj).
 *
 * Controls:  A / D  (or Fn+comma / Fn+period)  walk
 *            W / space / UP                     jump
 *            Q  or  Esc                         quit
 * ============================================================ */

#include <stdio.h>
#include <string.h>
#include <dos.h>

#define SCR_COLS 40
#define SCR_ROWS 16
#define VRAM     ((unsigned char far *)0xB8000000UL)

#define WGROUND  13             /* solid ground row                  */
#define WORLD_W  120            /* level width in columns            */
#define MAX_EN   8              /* walking enemies                   */
#define MAX_LIVES 3

/* ---- screen ---------------------------------------------------- */

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

/* INT 16h gives AH = scan code, AL = ASCII.  Character tests must use AL. */
#define K_UP    0x4800u
#define K_DOWN  0x5000u
#define K_LEFT  0x4B00u
#define K_RIGHT 0x4D00u

static int is_alpha(unsigned char al, char lo)
{
    return al == (unsigned char)lo || al == (unsigned char)(lo - 32);
}

static int is_up(unsigned k)
{
    if ((unsigned char)(k & 0xFF) == 0) return (k >> 8) == 0x48;
    return is_alpha((unsigned char)(k & 0xFF), 'w');
}

static int is_lf(unsigned k)
{
    if ((unsigned char)(k & 0xFF) == 0) return (k >> 8) == 0x4B;
    return is_alpha((unsigned char)(k & 0xFF), 'a');
}

static int is_rt(unsigned k)
{
    if ((unsigned char)(k & 0xFF) == 0) return (k >> 8) == 0x4D;
    return is_alpha((unsigned char)(k & 0xFF), 'd');
}

static int is_jump(unsigned k)
{
    unsigned char al = (unsigned char)(k & 0xFF);
    if (al == ' ') return 1;
    if (al == 0x0D) return 1;
    if (al == 0) return (k >> 8) == 0x48;          /* UP */
    return is_alpha(al, 'w');
}

static int is_quit(unsigned k)
{
    unsigned char al = (unsigned char)(k & 0xFF);
    if (al == 0x1B) return 1;
    if (al == 0) return 0;
    return is_alpha(al, 'q');
}

/* ---- level data ------------------------------------------------ */

static unsigned char w_ground[WORLD_W];   /* 1 = solid ground column   */
static unsigned char w_brick[WORLD_W];    /* row of a block, 0 = none  */
static unsigned char w_coin[WORLD_W];     /* row of a coin, 0 = none   */

struct enemy {
    unsigned char col;
    unsigned char home;   /* patrol centre, +-ENEMY_RANGE columns */
    signed char dir;
    signed char alive;
};
#define ENEMY_RANGE 4

static struct enemy en[MAX_EN];

/* ---- mario ----------------------------------------------------- */

static int mx;                  /* world column                              */
static int my8;                 /* top row * 8 (1/8-row units)               */
static int vy8;                 /* vertical speed, 1/8 row per tick          */
static int on_ground;
static int cam;                 /* leftmost visible world column             */
static int dx;                  /* -1, 0, +1                                 */
static int dx_until;            /* tick until the sticky direction expires   */
static int score, coins, lives;
static int state;               /* 0 = playing, 1 = dead, 2 = cleared        */

static int solid(int c, int r)
{
    if (c < 0 || c >= WORLD_W) return 1;          /* level edges are walls */
    if (r <= 0) return 0;
    if (r == WGROUND) return w_ground[c] ? 1 : 0;
    if (r == WGROUND + 1) return w_ground[c] ? 1 : 0;
    if (r >= WGROUND + 2) return 1;               /* safe floor            */
    if (w_brick[c] && r == (int)w_brick[c]) return 1;
    return 0;
}

static void status_line(void)
{
    char buf[44];
    sprintf(buf, "MARIO %06d  $%02d  lives %d", score, coins, lives);
    vfill(15, 0, SCR_COLS, ' ', 0x70);
    vstr(15, 0, buf, 0x70);
    vstr(15, 31, " W=JUMP A/D  ", 0x70);
}

static void draw_world(void)
{
    int c, r, i;

    for (r = 0; r < WGROUND; r++)
        vfill(r, 0, SCR_COLS, ' ', 0x00);

    for (c = 0; c < SCR_COLS; c++) {
        int w = cam + c;
        if (w < 0 || w >= WORLD_W) continue;
        if (w_ground[w]) {
            vput(WGROUND, c, '_', 0x08);
            vput(WGROUND + 1, c, '_', 0x08);
        }
        if (w_brick[w]) vput((int)w_brick[w], c, '#', 0x06);
        if (w_coin[w]) vput((int)w_coin[w], c, 'o', 0x0E);
    }

    /* goal flag */
    {
        int sx = (WORLD_W - 2) - cam;
        if (sx >= 0 && sx < SCR_COLS) {
            vput(WGROUND - 2, sx, 'F', 0x0E);
            vput(WGROUND - 1, sx, '|', 0x07);
        }
    }

    for (i = 0; i < MAX_EN; i++) {
        int sx;
        if (!en[i].alive) continue;
        sx = (int)en[i].col - cam;
        if (sx < 0 || sx >= SCR_COLS) continue;
        vput(WGROUND - 1, sx, 'm', 0x0C);
    }

    {
        int sx = mx - cam;
        int sy = my8 >> 3;
        if (sx >= 0 && sx < SCR_COLS) vput(sy, sx, 'M', 0x0E);
    }

    status_line();
}

static void world_build(void)
{
    int i;

    memset(w_ground, 1, sizeof(w_ground));
    memset(w_brick, 0, sizeof(w_brick));
    memset(w_coin, 0, sizeof(w_coin));

    i = 15;
    while (i < WORLD_W - 14) {
        unsigned kind = rnd() % 10;
        if (kind < 3) {
            /* pit: 2..3 columns wide, nothing to stand on */
            int w = 2;
            int k;
            for (k = 0; k < w && i + k < WORLD_W - 14; k++) w_ground[i + k] = 0;
            i += w + 5;
        } else if (kind < 6) {
            /* brick row with coins above it */
            int w = 2 + (int)(rnd() % 3);
            int k;
            for (k = 0; k < w && i + k < WORLD_W - 14; k++) {
                w_brick[i + k] = 10;
                if (rnd() % 2) w_coin[i + k] = 9;
            }
            i += w + 3;
        } else if (kind < 8) {
            /* low step, sometimes with a coin on top */
            int w = 2 + (int)(rnd() % 2);
            int k;
            for (k = 0; k < w && i + k < WORLD_W - 14; k++) {
                w_brick[i + k] = 11;
                if (rnd() % 3 == 0) w_coin[i + k] = 10;
            }
            i += w + 4;
        } else {
            i += 4 + (int)(rnd() % 5);
        }
    }

    /* enemies: only on flat ground, well after the start */
    {
        int n = 0;
        int c = 20;
        while (n < MAX_EN && c < WORLD_W - 16) {
            if (w_ground[c] && w_ground[c - 1] && w_ground[c + 1] &&
                w_ground[c + 2] && !w_brick[c]) {
                en[n].col = (unsigned char)c;
                en[n].home = (unsigned char)c;
                en[n].dir = (rnd() % 2) ? 1 : -1;
                en[n].alive = 1;
                n++;
                c += 12 + (int)(rnd() % 8);
            } else {
                c++;
            }
        }
        for (; n < MAX_EN; n++) en[n].alive = 0;
    }
}

static void mario_reset(int full)
{
    mx = 1;
    my8 = (WGROUND - 1) << 3;
    vy8 = 0;
    on_ground = 1;
    dx = 0;
    dx_until = 0;
    cam = 0;
    state = 0;
    if (full) {
        score = 0;
        coins = 0;
        lives = MAX_LIVES;
    }
}

static void mario_die(void)
{
    lives--;
    if (lives <= 0) {
        state = 1;
    } else {
        mario_reset(0);
        /* keep the score, put the enemies back where they started */
    }
}

static void physics(int tick)
{
    int row;
    int here;

    /* horizontal walking: one column every two ticks while sticky */
    if (dx != 0 && tick < dx_until && (tick & 1) == 0) {
        int nx = mx + dx;
        row = my8 >> 3;
        if (nx >= 0 && nx < WORLD_W && !solid(nx, row)) mx = nx;
    }

    /* Vertical.  my8 counts 1/8 row DOWNWARD, so gravity is +1 per tick and
       a jump is a negative kick.
       The landing test has to SWEEP the rows crossed during this tick: the
       fall speed reaches 16 eighths = 2 rows per tick, and the old
       "look at row+1 only" test let Mario tunnel straight into the ground
       row and then stand inside it. */
    here = my8 >> 3;
    vy8 += 1;
    if (vy8 > 16) vy8 = 16;
    my8 += vy8;
    if (my8 < 0) { my8 = 0; vy8 = 0; }

    {
        int nrow = my8 >> 3;
        on_ground = 0;

        if (nrow > here) {                          /* moving down */
            int r;
            for (r = here + 1; r <= nrow; r++) {
                if (solid(mx, r)) {
                    my8 = (r - 1) << 3;
                    vy8 = 0;
                    on_ground = 1;
                    break;
                }
            }
        } else if (nrow < here) {                   /* moving up */
            int r;
            for (r = here - 1; r >= nrow; r--) {
                if (solid(mx, r)) {                 /* bonked a block */
                    my8 = (r + 1) << 3;
                    vy8 = 0;
                    break;
                }
            }
        } else if (solid(mx, nrow + 1)) {           /* still standing */
            my8 = nrow << 3;
            vy8 = 0;
            on_ground = 1;
        }
    }

    /* fell into a pit */
    if ((my8 >> 3) > WGROUND + 1) {
        mario_die();
        return;
    }

    /* coins */
    if (w_coin[mx] && (my8 >> 3) == (int)w_coin[mx]) {
        w_coin[mx] = 0;
        coins++;
        score += 100;
    }

    /* enemies */
    {
        int i;
        for (i = 0; i < MAX_EN; i++) {
            if (!en[i].alive) continue;
            if ((int)en[i].col != mx) continue;
            /* Stomp only when actually landing on the enemy's head
               (one row above it, moving down); jumping *over* it at the
               same row must not hurt, that was killing well-timed jumps. */
            if ((my8 >> 3) == WGROUND - 2 && vy8 > 0) {
                en[i].alive = 0;             /* stomped from above */
                score += 200;
                vy8 = -5;                    /* bounce */
            } else if ((my8 >> 3) >= WGROUND - 1) {
                mario_die();
                return;
            }
        }
    }

    /* reached the flag */
    if (mx >= WORLD_W - 3) {
        state = 2;
        score += 500;
    }

    /* camera */
    cam = mx - 12;
    if (cam < 0) cam = 0;
    if (cam > WORLD_W - SCR_COLS) cam = WORLD_W - SCR_COLS;
}

static void enemies_step(void)
{
    int i;
    for (i = 0; i < MAX_EN; i++) {
        int nx;
        if (!en[i].alive) continue;
        nx = (int)en[i].col + en[i].dir;
        if (nx < (int)en[i].home - ENEMY_RANGE || nx > (int)en[i].home + ENEMY_RANGE) {
            en[i].dir = (signed char)(-en[i].dir);
            continue;
        }
        if (nx < 1 || nx > WORLD_W - 3 || !solid(nx, WGROUND) || solid(nx, WGROUND - 1)) {
            en[i].dir = (signed char)(-en[i].dir);
            continue;
        }
        en[i].col = (unsigned char)nx;
    }
}

static void banner(const char *l1, const char *l2, const char *l3, int attr)
{
    vfill(6, 5, 30, ' ', attr);
    vstr(6, 5, l1, attr);
    vstr(7, 5, l2, attr);
    vstr(8, 5, l3, attr);
    vfill(9, 5, 30, ' ', attr);
}

int main(void)
{
    unsigned long last;
    int tick = 0;

    set_video_mode(3);
    cursor_off();
    clear_all();
    rng_state = bios_ticks() * 2654435761UL + 12345UL;
    world_build();
    mario_reset(1);
    key_flush();

    last = bios_ticks();
    draw_world();

    for (;;) {
        unsigned k;

        if (key_hit()) {
            k = key_get();
            if (is_quit(k)) break;
            if (state == 0) {
                if (is_jump(k)) {
                    if (on_ground) { vy8 = -7; on_ground = 0; }
                } else if (is_lf(k)) {
                    dx = -1;
                    dx_until = tick + 8;
                } else if (is_rt(k)) {
                    dx = 1;
                    dx_until = tick + 8;
                }
            } else if (state == 1) {
                unsigned char al = (unsigned char)(k & 0xFF);
                if (is_alpha(al, 'r')) {
                    world_build();
                    mario_reset(1);
                    last = bios_ticks();
                    draw_world();
                }
            } else {
                unsigned char al = (unsigned char)(k & 0xFF);
                if (is_alpha(al, 'r')) {
                    world_build();
                    mario_reset(1);
                    draw_world();
                }
            }
            continue;                   /* redraw right away */
        }

        /* one game step per BIOS tick (18.2 Hz) */
        if (bios_ticks() == last) continue;
        last = bios_ticks();
        tick++;

        if (state == 0) {
            physics(tick);
            if ((tick % 4) == 0) enemies_step();
            draw_world();
            if (state == 1) {
                char b2[32];
                sprintf(b2, "   GAME OVER  score %d   ", score);
                banner("         GAME OVER          ", b2,
                       "   R restart     Q quit     ", 0x4F);
            } else if (state == 2) {
                char b2[32];
                sprintf(b2, "   score %-6d coins %-3d", score, coins);
                banner("      LEVEL CLEARED         ", b2,
                       "   R new level   Q quit     ", 0x2F);
            }
        }
    }

    clear_all();
    set_video_mode(3);
    cursor_on();
    printf("MARIO - bye.\n");
    return 0;
}
