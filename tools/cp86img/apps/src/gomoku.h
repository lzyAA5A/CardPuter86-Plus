/* gomoku.h -- 11x11 gomoku AI, four levels.  Plain C++ with no DOS or
 * Arduino dependency, so the very same code can be compiled for the
 * Cardputer (.COM) and for a 32-bit host test bed (test/gomoku_test.cpp)
 * where the four levels play each other.
 *
 * level 1 EASY    one ply, 35%% random blunder, only blocks 60%% of the time
 * level 2 MEDIUM  one ply greedy, always takes a win and always blocks
 * level 3 HARD    two ply: my value minus the opponent's best reply
 * level 4 EXPERT  two ply, wider candidate list, double-threat bonus
 *
 * The evaluation is the classic 5-window count: for a point p and a side c,
 * score5() sums a weight over the 20 five-in-a-row windows that contain p,
 * skipping windows blocked by the enemy.  weights { 0,1,12,150,2600,200000 }.
 */
#ifndef GOMOKU_AI_H
#define GOMOKU_AI_H

#define BSZ   11
#define MAXC  96
#define P_EMP 0
#define P_HUM 1
#define P_AI  2

static const int GDX[4] = { 1, 0, 1, 1 };
static const int GDY[4] = { 0, 1, 1, -1 };

struct Gomoku {
    int bd[BSZ][BSZ];
    int cx[MAXC];
    int cy[MAXC];
    int nc;
    int lvl;

    unsigned long rng;
    unsigned rnd(void);
    void seed(unsigned long s);
    void reset(int level);
    int  get(int x, int y) const;
    void put(int x, int y, int v);
    int  win_at(int x, int y, int who) const;
    long score5(int x, int y, int who) const;
    int  threat(int x, int y, int who) const;
    void gen(void);
    int  threat_count(int x, int y, int who) const;
    void think(int *ox, int *oy);
};

unsigned Gomoku::rnd(void)
{
    rng = rng * 1103515245UL + 12345UL;
    return (unsigned)((rng >> 16) & 0x7FFFu);
}

void Gomoku::seed(unsigned long s)
{
    rng = s | 1UL;
}

void Gomoku::reset(int level)
{
    int x, y;
    for (y = 0; y < BSZ; y++)
        for (x = 0; x < BSZ; x++) bd[y][x] = P_EMP;
    nc = 0;
    lvl = level;
}

int Gomoku::get(int x, int y) const
{
    if (x < 0 || x >= BSZ || y < 0 || y >= BSZ) return -1;
    return bd[y][x];
}

void Gomoku::put(int x, int y, int v)
{
    bd[y][x] = v;
}

/* 1 when the stone at (x,y) completes five or more of `who` */
int Gomoku::win_at(int x, int y, int who) const
{
    int d, l, r, px, py;
    for (d = 0; d < 4; d++) {
        l = 0;
        px = x - GDX[d]; py = y - GDY[d];
        while (get(px, py) == who) { l++; px -= GDX[d]; py -= GDY[d]; }
        r = 0;
        px = x + GDX[d]; py = y + GDY[d];
        while (get(px, py) == who) { r++; px += GDX[d]; py += GDY[d]; }
        if (1 + l + r >= 5) return 1;
    }
    return 0;
}

/* value of *playing* `who` at the empty point (x,y):
   sum over the 20 five-cell windows that contain the point. */
long Gomoku::score5(int x, int y, int who) const
{
    /* 32-bit sums: on the 16-bit DOS target `int` is 16 bits and 200000
       would wrap, so every accumulator here is a long. */
    static const long w[6] = { 0L, 1L, 12L, 150L, 2600L, 200000L };
    long total = 0;
    int d, s, k, cnt, blk, px, py, v;

    for (d = 0; d < 4; d++) {
        for (s = -4; s <= 0; s++) {
            cnt = 0;
            blk = 0;
            for (k = 0; k < 5; k++) {
                px = x + (s + k) * GDX[d];
                py = y + (s + k) * GDY[d];
                if (px < 0 || px >= BSZ || py < 0 || py >= BSZ) {
                    blk++;
                    continue;
                }
                if (px == x && py == y) {
                    cnt++;
                    continue;
                }
                v = bd[py][px];
                if (v == who) cnt++;
                else if (v != P_EMP) blk++;
            }
            if (blk == 0) total += w[cnt];
        }
    }
    return total;
}

/* 0 none, 1 open three, 2 four, 3 live four / five - playing (x,y) */
int Gomoku::threat(int x, int y, int who) const
{
    int d, l, r, px, py, cnt, open, best = 0;

    for (d = 0; d < 4; d++) {
        l = 0;
        px = x - GDX[d]; py = y - GDY[d];
        while (get(px, py) == who) { l++; px -= GDX[d]; py -= GDY[d]; }
        open = (get(px, py) == P_EMP) ? 1 : 0;
        r = 0;
        px = x + GDX[d]; py = y + GDY[d];
        while (get(px, py) == who) { r++; px += GDX[d]; py += GDY[d]; }
        open += (get(px, py) == P_EMP) ? 1 : 0;
        cnt = 1 + l + r;
        if (cnt >= 5) return 3;
        if (cnt == 4 && open == 2) { if (best < 3) best = 3; }
        else if (cnt == 4 && open == 1) { if (best < 2) best = 2; }
        else if (cnt == 3 && open == 2) { if (best < 1) best = 1; }
    }
    return best;
}

/* collect empty points within Chebyshev distance 2 of any stone */
void Gomoku::gen(void)
{
    int x, y, dx, dy, px, py, isNear, hasAny;

    hasAny = 0;
    nc = 0;
    for (y = 0; y < BSZ; y++)
        for (x = 0; x < BSZ; x++)
            if (bd[y][x] != P_EMP) hasAny = 1;

    if (!hasAny) {
        cx[0] = BSZ / 2;
        cy[0] = BSZ / 2;
        nc = 1;
        return;
    }

    for (y = 0; y < BSZ; y++) {
        for (x = 0; x < BSZ; x++) {
            if (bd[y][x] != P_EMP) continue;
            isNear = 0;
            for (dy = -2; dy <= 2 && !isNear; dy++)
                for (dx = -2; dx <= 2 && !isNear; dx++) {
                    px = x + dx;
                    py = y + dy;
                    if (px < 0 || px >= BSZ || py < 0 || py >= BSZ) continue;
                    if (bd[py][px] != P_EMP) isNear = 1;
                }
            if (isNear) {
                if (nc < MAXC) {
                    cx[nc] = x;
                    cy[nc] = y;
                    nc++;
                }
            }
        }
    }
    if (nc == 0) {
        cx[0] = BSZ / 2;
        cy[0] = BSZ / 2;
        nc = 1;
    }
}

/* best value the side to move can reach with one stone (used for the
   two-ply levels: how bad is the opponent's best answer) */
/* how many independent threats playing `who` at the empty point (x,y) makes:
   a live four counts 2 (two winning ends), any other four or a live three
   counts 1.  Two or more means a double threat - the opponent can only
   answer one of them, which is how gomoku games are actually won. */
int Gomoku::threat_count(int x, int y, int who) const
{
    int d, l, r, px, py, cnt, open, n = 0;

    for (d = 0; d < 4; d++) {
        l = 0;
        px = x - GDX[d]; py = y - GDY[d];
        while (get(px, py) == who) { l++; px -= GDX[d]; py -= GDY[d]; }
        open = (get(px, py) == P_EMP) ? 1 : 0;
        r = 0;
        px = x + GDX[d]; py = y + GDY[d];
        while (get(px, py) == who) { r++; px += GDX[d]; py += GDY[d]; }
        open += (get(px, py) == P_EMP) ? 1 : 0;
        cnt = 1 + l + r;
        if (cnt >= 5) return 9;
        if (cnt == 4 && open == 2) n += 2;
        else if (cnt == 4) n += 1;
        else if (cnt == 3 && open == 2) n += 1;
    }
    return n;
}

void Gomoku::think(int *ox, int *oy)
{
    int i, t, bi, bmove, bonus, topn, aw, dw;
    long v, best, bestsc, penalty;
    static long val[MAXC];
    int used[MAXC];

    /* level 1 hardly defends, so a beginner can win by simply building a
       line; level 2 and up weigh the opponent's threats almost as heavily
       as its own.  This is what makes the four levels feel different. */
    aw = (lvl <= 1) ? 20 : 10;
    dw = (lvl <= 1) ? 3 : 9;

    gen();

    /* 1. win right now */
    for (i = 0; i < nc; i++) {
        put(cx[i], cy[i], P_AI);
        v = win_at(cx[i], cy[i], P_AI);
        put(cx[i], cy[i], P_EMP);
        if (v) { *ox = cx[i]; *oy = cy[i]; return; }
    }

    /* 2. block an immediate loss */
    bi = -1;
    for (i = 0; i < nc; i++) {
        put(cx[i], cy[i], P_HUM);
        v = win_at(cx[i], cy[i], P_HUM);
        put(cx[i], cy[i], P_EMP);
        if (v && bi < 0) { bi = i; }
    }
    if (bi >= 0) {
        if (lvl >= 2) { *ox = cx[bi]; *oy = cy[bi]; return; }
        if (rnd() % 100 < 40) { *ox = cx[bi]; *oy = cy[bi]; return; }
    }

    for (i = 0; i < nc; i++) {
        val[i] = score5(cx[i], cy[i], P_AI) * aw +
                 score5(cx[i], cy[i], P_HUM) * dw;
        used[i] = 0;
    }

    if (lvl <= 2) {
        best = -1;
        bi = 0;
        for (i = 0; i < nc; i++)
            if (val[i] > best) { best = val[i]; bi = i; }
        if (lvl == 1 && rnd() % 100 < 30) {
            /* beginner blunder: pick a random candidate */
            i = (int)(rnd() % (unsigned)nc);
            *ox = cx[i];
            *oy = cy[i];
            return;
        }
        *ox = cx[bi];
        *oy = cy[bi];
        return;
    }

    /* level 3 / 4 : fork hunting.
       A pure "my value minus their best reply" term turned out to be worse
       than the greedy level in self play (it defends forever and loses the
       initiative), so instead the search asks the question that actually
       wins gomoku games: does this move create two threats at once? */
    topn = (lvl >= 4) ? 10 : 6;
    bestsc = -2000000L;
    bmove = 0;
    for (t = 0; t < topn; t++) {
        best = -1;
        bi = -1;
        for (i = 0; i < nc; i++)
            if (!used[i] && val[i] > best) { best = val[i]; bi = i; }
        if (bi < 0) break;
        used[bi] = 1;

        put(cx[bi], cy[bi], P_AI);
        bonus = threat_count(cx[bi], cy[bi], P_AI);
        if (bonus >= 9) bonus = 100000;          /* five: already handled */
        else if (bonus >= 2) bonus = 40000;      /* double threat: wins */
        else if (bonus == 1) bonus = 1200;

        penalty = 0;
        if (lvl >= 4) {
            int worst = 0, j, tk;
            for (j = 0; j < nc; j++) {
                tk = threat_count(cx[j], cy[j], P_HUM);
                if (tk > worst) worst = tk;
            }
            if (worst >= 2) penalty = 12000;     /* would hand them a fork */
            else if (worst == 1) penalty = 600;
        }
        put(cx[bi], cy[bi], P_EMP);

        v = val[bi] + bonus - penalty;
        if (v > bestsc) { bestsc = v; bmove = bi; }
    }
    *ox = cx[bmove];
    *oy = cy[bmove];
}

#endif /* GOMOKU_AI_H */
