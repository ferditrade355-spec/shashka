/* Shashka AI - tez dvigatel (rus shashkasi, 8x8).
 * Protokol (stdin):
 *   1-qator: BEST <millisekund>   yoki   MOVES
 *   2-qator: 64 belgi: '.' bo'sh, 'o' mening shashkam, 'O' mening damkam, 'x' raqibniki, 'X' raqib damkasi
 *            (men doim pastdan yuqoriga yuraman)
 *   3-qator: <n> so'ng n ta "<boshlanish> <oxir>" (taqiqlangan yurishlar, katak = qator*8+ustun)
 *   4-qator: <m> so'ng m ta 64 belgili taxta (takrorlanishdan qochish uchun)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define MAXM 256
#define MATE 30000
#define TTBITS 22

typedef struct { int8_t path[16]; int8_t cap[16]; int8_t len, ncap, king; } Move;
typedef struct { int8_t from, to, oldp, newp, ncap; int8_t capsq[16]; int8_t capp[16]; uint64_t hash; } Undo;
typedef struct { uint64_t key; int16_t score; int8_t depth; uint8_t flag; int8_t from, to; } TTE;

static int8_t B[64];
static uint64_t ZOB[5][64], ZSIDE, HASH;
static const int DR[4] = {-1, -1, 1, 1};
static const int DC[4] = {-1, 1, -1, 1};
static TTE *TT;
static uint64_t nodes;
static clock_t t_end;
static int timeup;
static int HIST[64][64];
static int KILL[128][2];

static inline int inb(int r, int c) { return (unsigned)r < 8 && (unsigned)c < 8; }

static uint64_t rng_state = 88172645463325252ULL;
static uint64_t rnd64(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return rng_state;
}

/* ---------------- yurishlarni hosil qilish ---------------- */
typedef struct {
    Move *out; int n; int side;
    uint8_t taken[64];
    int8_t path[16]; int plen;
    int8_t caps[16]; int ncap;
} Ctx;

static int king_can_cap(int side, int r, int c, const uint8_t *taken) {
    for (int d = 0; d < 4; d++) {
        int nr = r + DR[d], nc = c + DC[d];
        while (inb(nr, nc) && B[nr * 8 + nc] == 0) { nr += DR[d]; nc += DC[d]; }
        if (inb(nr, nc) && B[nr * 8 + nc] * side < 0 && !taken[nr * 8 + nc]) {
            int lr = nr + DR[d], lc = nc + DC[d];
            if (inb(lr, lc) && B[lr * 8 + lc] == 0) return 1;
        }
    }
    return 0;
}

static void emit(Ctx *cx, int king) {
    if (cx->n >= MAXM) return;
    Move *m = &cx->out[cx->n++];
    m->len = cx->plen; m->ncap = cx->ncap; m->king = king;
    for (int i = 0; i < cx->plen; i++) m->path[i] = cx->path[i];
    for (int i = 0; i < cx->ncap; i++) m->cap[i] = cx->caps[i];
}

static void rec(Ctx *cx, int r, int c, int king) {
    int found = 0;
    int promo = cx->side == 1 ? 0 : 7;
    for (int d = 0; d < 4; d++) {
        if (!king) {
            int tr = r + DR[d], tc = c + DC[d], lr = r + 2 * DR[d], lc = c + 2 * DC[d];
            if (inb(lr, lc) && !cx->taken[tr * 8 + tc] && B[tr * 8 + tc] * cx->side < 0 && B[lr * 8 + lc] == 0) {
                found = 1;
                cx->taken[tr * 8 + tc] = 1;
                cx->caps[cx->ncap++] = tr * 8 + tc;
                cx->path[cx->plen++] = lr * 8 + lc;
                rec(cx, lr, lc, lr == promo);
                cx->plen--; cx->ncap--;
                cx->taken[tr * 8 + tc] = 0;
            }
        } else {
            int nr = r + DR[d], nc = c + DC[d];
            while (inb(nr, nc) && B[nr * 8 + nc] == 0) { nr += DR[d]; nc += DC[d]; }
            if (inb(nr, nc) && B[nr * 8 + nc] * cx->side < 0 && !cx->taken[nr * 8 + nc]) {
                int lands[8][2], nl = 0;
                int lr = nr + DR[d], lc = nc + DC[d];
                while (inb(lr, lc) && B[lr * 8 + lc] == 0 && nl < 8) {
                    lands[nl][0] = lr; lands[nl][1] = lc; nl++;
                    lr += DR[d]; lc += DC[d];
                }
                if (nl > 0) {
                    found = 1;
                    int cap = nr * 8 + nc;
                    cx->taken[cap] = 1;
                    cx->caps[cx->ncap++] = cap;
                    int cont[8], nco = 0;
                    for (int i = 0; i < nl; i++)
                        if (king_can_cap(cx->side, lands[i][0], lands[i][1], cx->taken)) cont[nco++] = i;
                    int cnt = nco ? nco : nl;
                    for (int k = 0; k < cnt; k++) {
                        int i = nco ? cont[k] : k;
                        cx->path[cx->plen++] = lands[i][0] * 8 + lands[i][1];
                        rec(cx, lands[i][0], lands[i][1], 1);
                        cx->plen--;
                    }
                    cx->ncap--;
                    cx->taken[cap] = 0;
                }
            }
        }
    }
    if (!found && cx->ncap > 0) emit(cx, king);
}

static int gen(int side, Move *out) {
    Ctx cx; memset(&cx, 0, sizeof(cx));
    cx.out = out; cx.side = side;
    for (int sq = 0; sq < 64; sq++) {
        int p = B[sq];
        if (p * side <= 0) continue;
        int r = sq >> 3, c = sq & 7;
        int8_t save = B[sq];
        B[sq] = 0;
        cx.plen = 1; cx.path[0] = sq; cx.ncap = 0;
        rec(&cx, r, c, (p == 2 || p == -2));
        B[sq] = save;
    }
    if (cx.n) return cx.n;
    int promo = side == 1 ? 0 : 7, fwd = side == 1 ? -1 : 1, n = 0;
    for (int sq = 0; sq < 64; sq++) {
        int p = B[sq];
        if (p * side <= 0) continue;
        int r = sq >> 3, c = sq & 7;
        if (p == 1 || p == -1) {
            for (int dc = -1; dc <= 1; dc += 2) {
                int nr = r + fwd, nc = c + dc;
                if (inb(nr, nc) && B[nr * 8 + nc] == 0 && n < MAXM) {
                    Move *m = &out[n++];
                    m->len = 2; m->ncap = 0; m->king = (nr == promo);
                    m->path[0] = sq; m->path[1] = nr * 8 + nc;
                }
            }
        } else {
            for (int d = 0; d < 4; d++) {
                int nr = r + DR[d], nc = c + DC[d];
                while (inb(nr, nc) && B[nr * 8 + nc] == 0 && n < MAXM) {
                    Move *m = &out[n++];
                    m->len = 2; m->ncap = 0; m->king = 1;
                    m->path[0] = sq; m->path[1] = nr * 8 + nc;
                    nr += DR[d]; nc += DC[d];
                }
            }
        }
    }
    return n;
}

/* ---------------- yurish / qaytarish ---------------- */
static void make(const Move *m, int side, Undo *u) {
    u->hash = HASH;
    int from = m->path[0], to = m->path[m->len - 1];
    u->from = from; u->to = to; u->oldp = B[from];
    u->newp = side * (m->king ? 2 : 1);
    HASH ^= ZOB[B[from] + 2][from];
    B[from] = 0;
    u->ncap = m->ncap;
    for (int i = 0; i < m->ncap; i++) {
        int sq = m->cap[i];
        u->capsq[i] = sq; u->capp[i] = B[sq];
        HASH ^= ZOB[B[sq] + 2][sq];
        B[sq] = 0;
    }
    B[to] = u->newp;
    HASH ^= ZOB[u->newp + 2][to];
    HASH ^= ZSIDE;
}

static void unmake(const Undo *u) {
    B[u->to] = 0;
    B[u->from] = u->oldp;
    for (int i = 0; i < u->ncap; i++) B[u->capsq[i]] = u->capp[i];
    HASH = u->hash;
}

/* ---------------- baholash ---------------- */
static const int ADV[8] = {0, 5, 11, 18, 26, 35, 55, 0};

static int eval(void) {
    int s = 0;
    for (int sq = 0; sq < 64; sq++) {
        int p = B[sq];
        if (!p) continue;
        int r = sq >> 3, c = sq & 7;
        int side = p > 0 ? 1 : -1, a = p > 0 ? p : -p;
        int central = (r >= 2 && r <= 5 && c >= 2 && c <= 5);
        int v;
        if (a == 1) {
            int adv = side == 1 ? 7 - r : r;
            int cc = side == 1 ? c : 7 - c;
            v = 100 + ADV[adv];
            if (central) v += 4;
            if (c == 0 || c == 7) v -= 3;
            if (adv == 0 && (cc == 2 || cc == 4)) v += 8;
        } else {
            v = 330;
            if (r + c == 7) v += 12; else if (central) v += 4;
        }
        s += side * v;
    }
    return s;
}

/* ---------------- qidiruv ---------------- */
static int search(int side, int depth, int alpha, int beta, int ply) {
    nodes++;
    if ((nodes & 2047) == 0 && clock() > t_end) timeup = 1;
    if (timeup) return 0;
    Move ms[MAXM];
    int n = gen(side, ms);
    if (n == 0) return -MATE + ply;
    if (depth <= 0 && ms[0].ncap == 0) return side * eval() + 2 * n;
    if (depth < -14 || ply > 120) return side * eval();

    uint64_t key = HASH;
    TTE *e = &TT[key & ((1ULL << TTBITS) - 1)];
    int ttf = -1, ttt = -1;
    int a0 = alpha;
    if (e->key == key) {
        ttf = e->from; ttt = e->to;
        if (e->depth >= depth && depth > 0) {
            int sc = e->score;
            if (e->flag == 0) return sc;
            if (e->flag == 1 && sc >= beta) return sc;
            if (e->flag == 2 && sc <= alpha) return sc;
        }
    }
    int sco[MAXM];
    for (int i = 0; i < n; i++) {
        int f = ms[i].path[0], t = ms[i].path[ms[i].len - 1];
        if (f == ttf && t == ttt) sco[i] = 1000000;
        else if (ms[i].ncap) sco[i] = 100000 + ms[i].ncap * 100;
        else {
            sco[i] = HIST[f][t];
            if (ply < 128 && (KILL[ply][0] == f * 64 + t || KILL[ply][1] == f * 64 + t)) sco[i] += 50000;
            if (ms[i].king) sco[i] += 500;
        }
    }
    int best = -MATE - 1, bf = -1, bt = -1;
    for (int i = 0; i < n; i++) {
        int bi = i;
        for (int j = i + 1; j < n; j++) if (sco[j] > sco[bi]) bi = j;
        if (bi != i) { Move tm = ms[i]; ms[i] = ms[bi]; ms[bi] = tm; int ts = sco[i]; sco[i] = sco[bi]; sco[bi] = ts; }
        Undo u; make(&ms[i], side, &u);
        int sc;
        if (i == 0) sc = -search(-side, depth - 1, -beta, -alpha, ply + 1);
        else {
            sc = -search(-side, depth - 1, -alpha - 1, -alpha, ply + 1);
            if (sc > alpha && sc < beta && !timeup) sc = -search(-side, depth - 1, -beta, -alpha, ply + 1);
        }
        unmake(&u);
        if (timeup) return 0;
        if (sc > best) { best = sc; bf = ms[i].path[0]; bt = ms[i].path[ms[i].len - 1]; }
        if (sc > alpha) alpha = sc;
        if (alpha >= beta) {
            if (!ms[i].ncap) {
                int f = ms[i].path[0], t = ms[i].path[ms[i].len - 1];
                HIST[f][t] += depth * depth;
                if (ply < 128 && KILL[ply][0] != f * 64 + t) { KILL[ply][1] = KILL[ply][0]; KILL[ply][0] = f * 64 + t; }
            }
            break;
        }
    }
    if (depth > 0) {
        e->key = key; e->score = (int16_t)best; e->depth = (int8_t)depth;
        e->flag = best >= beta ? 1 : (best > a0 ? 0 : 2);
        e->from = bf; e->to = bt;
    }
    return best;
}

/* ---------------- yordamchi ---------------- */
static int parse_board(const char *s) {
    if (strlen(s) < 64) return 0;
    HASH = 0;
    for (int i = 0; i < 64; i++) {
        int p = 0;
        switch (s[i]) { case 'o': p = 1; break; case 'O': p = 2; break; case 'x': p = -1; break; case 'X': p = -2; break; default: p = 0; }
        B[i] = (int8_t)p;
        if (p) HASH ^= ZOB[p + 2][i];
    }
    return 1;
}

static void print_move(const Move *m) {
    printf("%d", m->len);
    for (int i = 0; i < m->len; i++) printf(" %d", m->path[i]);
    printf(" %d", m->ncap);
    for (int i = 0; i < m->ncap; i++) printf(" %d", m->cap[i]);
    printf(" %d\n", m->king);
}

int main(void) {
    for (int p = 0; p < 5; p++) for (int s = 0; s < 64; s++) ZOB[p][s] = rnd64();
    ZSIDE = rnd64();
    char cmd[32]; char line[1024]; int ms_limit = 1000;
    if (!fgets(line, sizeof line, stdin)) return 1;
    if (sscanf(line, "%31s %d", cmd, &ms_limit) < 1) return 1;
    char bs[256];
    if (!fgets(line, sizeof line, stdin) || sscanf(line, "%255s", bs) != 1 || !parse_board(bs)) { printf("ERR board\n"); return 1; }
    uint64_t root_hash = HASH;
    Move root[MAXM];
    int nroot = gen(1, root);

    if (strcmp(cmd, "MOVES") == 0) {
        printf("%d\n", nroot);
        for (int i = 0; i < nroot; i++) print_move(&root[i]);
        return 0;
    }

    /* taqiqlangan yurishlar */
    int nex = 0, ex[64][2];
    if (fgets(line, sizeof line, stdin)) {
        char *p = line; int k = 0, used = 0;
        if (sscanf(p, "%d%n", &nex, &used) == 1) {
            p += used;
            if (nex > 64) nex = 64;
            for (k = 0; k < nex; k++) {
                if (sscanf(p, "%d %d%n", &ex[k][0], &ex[k][1], &used) != 2) { nex = k; break; }
                p += used;
            }
        } else nex = 0;
    }
    /* takrorlanishdan qochish */
    uint64_t seenh[256]; int nseen = 0;
    if (fgets(line, sizeof line, stdin)) {
        int m = 0, used = 0; char *p = line;
        if (sscanf(p, "%d%n", &m, &used) == 1) {
            p += used;
            for (int k = 0; k < m && nseen < 256; k++) {
                char b2[256];
                if (sscanf(p, "%255s%n", b2, &used) != 1) break;
                p += used;
                int8_t save[64]; memcpy(save, B, 64);
                if (parse_board(b2)) seenh[nseen++] = HASH ^ ZSIDE;
                memcpy(B, save, 64);
            }
        }
    }
    HASH = root_hash;

    /* taqiqlanganlarni olib tashlash */
    int n = 0;
    for (int i = 0; i < nroot; i++) {
        int f = root[i].path[0], t = root[i].path[root[i].len - 1], skip = 0;
        for (int k = 0; k < nex; k++) if (ex[k][0] == f && ex[k][1] == t) skip = 1;
        if (!skip) root[n++] = root[i];
    }
    if (n == 0) { printf("NONE\n"); return 0; }
    if (n == 1) { printf("OK 0 0 0 "); print_move(&root[0]); return 0; }

    TT = (TTE *)calloc((size_t)1 << TTBITS, sizeof(TTE));
    memset(HIST, 0, sizeof HIST); memset(KILL, 0xff, sizeof KILL);
    clock_t t0 = clock();
    t_end = t0 + (clock_t)((double)ms_limit * CLOCKS_PER_SEC / 1000.0);
    int best_i = 0, best_sc = 0, done_depth = 0;
    int order[MAXM], rsc[MAXM];
    for (int i = 0; i < n; i++) order[i] = i;

    for (int depth = 1; depth < 60; depth++) {
        int alpha = -MATE - 1, cur = -1, cursc = -MATE - 1;
        int newsc[MAXM];
        for (int oi = 0; oi < n; oi++) {
            int i = order[oi];
            Undo u; make(&root[i], 1, &u);
            int sc;
            if (oi == 0) sc = -search(-1, depth - 1, -MATE - 1, -alpha, 1);
            else {
                sc = -search(-1, depth - 1, -alpha - 1, -alpha, 1);
                if (sc > alpha && !timeup) sc = -search(-1, depth - 1, -MATE - 1, -alpha, 1);
            }
            int rep = 0;
            for (int k = 0; k < nseen; k++) if (seenh[k] == HASH) rep = 1;
            unmake(&u);
            if (timeup) break;
            if (rep) sc -= 25;
            newsc[oi] = sc;
            if (sc > cursc) { cursc = sc; cur = i; }
            if (sc > alpha) alpha = sc;
        }
        if (timeup) break;
        best_i = cur; best_sc = cursc; done_depth = depth;
        /* eng yaxshisini boshiga qo'yish */
        for (int oi = 0; oi < n; oi++) if (order[oi] == cur) { for (int k = oi; k > 0; k--) order[k] = order[k - 1]; order[0] = cur; break; }
        (void)rsc; (void)newsc;
        if (best_sc > MATE - 200 || best_sc < -MATE + 200) break;
        if (clock() > t_end) break;
    }
    printf("OK %d %d %llu ", best_sc, done_depth, (unsigned long long)nodes);
    print_move(&root[best_i]);
    return 0;
}
