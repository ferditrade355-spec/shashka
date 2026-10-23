/*
 * Shashka dvigateli (rus shashkasi, 8x8) - shashka.py bilan ishlaydi.
 * Yasash (Windows, MinGW):   gcc -O2 -o engine.exe engine.c
 * Yasash (Linux/Mac):        gcc -O2 -o engine engine.c
 *
 * Kirish (stdin):
 *   BEST <millisekund>
 *   <64 belgi: o=mening shashkam, O=damkam, x/X=raqibniki, .=bo'sh>
 *   <n> <n ta "from to" juftligi>      (taqiqlangan yurishlar)
 *   <m> <m ta 64 belgili qator>        (oldin bo'lgan holatlar - takrorlamaslik uchun)
 * Chiqish: OK <baho> <chuqurlik> <tugunlar> <L> <L ta katak raqami>
 *
 * Qidiruv: iterative deepening + PVS alpha-beta + transpozitsiya jadvali
 * + killer/history tartiblash + majburiy urishlarni oxirigacha qidirish.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
static long long now_ms(void) { return (long long)GetTickCount64(); }
#else
#include <time.h>
static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif

#define MAXM 160
#define MAXP 96
#define INF 1000000
#define WIN 30000
#define MOB 2
#define REP_PEN 25

typedef struct {
    int8_t from, to, n;
    int8_t path[16];
    int8_t nc;
    int8_t cap[14];
    int8_t capv[14];
    int8_t king;
    int sc;
} Move;

static int8_t B[64];
static uint8_t TK[64];
static uint64_t H;
static uint64_t ZB[5][64];
static uint64_t SK[2];

static const int DR[4] = {-1, -1, 1, 1};
static const int DC[4] = {-1, 1, -1, 1};

static uint64_t rng_s = 0x9E3779B97F4A7C15ULL;
static uint64_t rnd64(void) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 7;
    rng_s ^= rng_s << 17;
    return rng_s * 0x2545F4914F6CDD1DULL;
}
static inline int zi(int p) { return p > 0 ? p : 2 - p; } /* 1,2 -> 1,2 ; -1,-2 -> 3,4 */

static inline int inb(int r, int c) { return r >= 0 && r < 8 && c >= 0 && c < 8; }

/* ------------------------------ yurishlar ------------------------------ */
static int king_can_cap(int side, int r, int c) {
    for (int d = 0; d < 4; d++) {
        int nr = r + DR[d], nc = c + DC[d];
        while (inb(nr, nc) && B[nr * 8 + nc] == 0) { nr += DR[d]; nc += DC[d]; }
        if (inb(nr, nc) && B[nr * 8 + nc] * side < 0 && !TK[nr * 8 + nc]) {
            int lr = nr + DR[d], lc = nc + DC[d];
            if (inb(lr, lc) && B[lr * 8 + lc] == 0) return 1;
        }
    }
    return 0;
}

static void rec(int side, int r, int c, int king, Move *cur, Move *out, int *cnt) {
    int found = 0;
    int promo = side == 1 ? 0 : 7;
    for (int d = 0; d < 4; d++) {
        if (!king) {
            int tr = r + DR[d], tc = c + DC[d];
            int lr = r + 2 * DR[d], lc = c + 2 * DC[d];
            if (inb(lr, lc) && !TK[tr * 8 + tc] && B[tr * 8 + tc] * side < 0 && B[lr * 8 + lc] == 0) {
                found = 1;
                int t = tr * 8 + tc;
                TK[t] = 1;
                cur->cap[cur->nc] = t;
                cur->capv[cur->nc] = B[t];
                cur->nc++;
                cur->path[cur->n++] = lr * 8 + lc;
                rec(side, lr, lc, (lr == promo) ? 1 : 0, cur, out, cnt);
                cur->n--;
                cur->nc--;
                TK[t] = 0;
            }
        } else {
            int nr = r + DR[d], nc = c + DC[d];
            while (inb(nr, nc) && B[nr * 8 + nc] == 0) { nr += DR[d]; nc += DC[d]; }
            if (inb(nr, nc) && B[nr * 8 + nc] * side < 0 && !TK[nr * 8 + nc]) {
                int lands[8], nl = 0;
                int lr = nr + DR[d], lc = nc + DC[d];
                while (inb(lr, lc) && B[lr * 8 + lc] == 0) {
                    lands[nl++] = lr * 8 + lc;
                    lr += DR[d]; lc += DC[d];
                }
                if (nl) {
                    found = 1;
                    int t = nr * 8 + nc;
                    TK[t] = 1;
                    cur->cap[cur->nc] = t;
                    cur->capv[cur->nc] = B[t];
                    cur->nc++;
                    int cont[8], ncn = 0;
                    for (int i = 0; i < nl; i++)
                        if (king_can_cap(side, lands[i] >> 3, lands[i] & 7)) cont[ncn++] = lands[i];
                    int *lst = ncn ? cont : lands;
                    int ln = ncn ? ncn : nl;
                    for (int i = 0; i < ln; i++) {
                        cur->path[cur->n++] = lst[i];
                        rec(side, lst[i] >> 3, lst[i] & 7, 1, cur, out, cnt);
                        cur->n--;
                    }
                    cur->nc--;
                    TK[t] = 0;
                }
            }
        }
    }
    if (!found && cur->nc > 0) {
        if (*cnt < MAXM) {
            Move *m = &out[*cnt];
            *m = *cur;
            m->to = cur->path[cur->n - 1];
            m->king = (int8_t)king;
            (*cnt)++;
        }
    }
}

static int gen(int side, Move *out) {
    int cnt = 0;
    Move cur;
    for (int sq = 0; sq < 64; sq++) {
        int p = B[sq];
        if (p * side > 0) {
            memset(&cur, 0, sizeof cur);
            cur.from = sq;
            cur.n = 1;
            cur.path[0] = sq;
            B[sq] = 0;
            rec(side, sq >> 3, sq & 7, (p == 2 || p == -2), &cur, out, &cnt);
            B[sq] = p;
        }
    }
    if (cnt) return cnt;
    int fwd = side == 1 ? -1 : 1;
    int promo = side == 1 ? 0 : 7;
    for (int sq = 0; sq < 64; sq++) {
        int p = B[sq];
        if (p * side <= 0) continue;
        int r = sq >> 3, c = sq & 7;
        if (p == 1 || p == -1) {
            for (int dc = -1; dc <= 1; dc += 2) {
                int nr = r + fwd, nc = c + dc;
                if (inb(nr, nc) && B[nr * 8 + nc] == 0 && cnt < MAXM) {
                    Move *m = &out[cnt++];
                    memset(m, 0, sizeof *m);
                    m->from = sq; m->to = nr * 8 + nc; m->n = 2;
                    m->path[0] = sq; m->path[1] = m->to;
                    m->king = (nr == promo);
                }
            }
        } else {
            for (int d = 0; d < 4; d++) {
                int nr = r + DR[d], nc = c + DC[d];
                while (inb(nr, nc) && B[nr * 8 + nc] == 0) {
                    if (cnt < MAXM) {
                        Move *m = &out[cnt++];
                        memset(m, 0, sizeof *m);
                        m->from = sq; m->to = nr * 8 + nc; m->n = 2;
                        m->path[0] = sq; m->path[1] = m->to;
                        m->king = 1;
                    }
                    nr += DR[d]; nc += DC[d];
                }
            }
        }
    }
    return cnt;
}

static inline void make(int side, const Move *m) {
    int p = B[m->from];
    B[m->from] = 0;
    H ^= ZB[zi(p)][m->from];
    for (int i = 0; i < m->nc; i++) {
        H ^= ZB[zi(m->capv[i])][m->cap[i]];
        B[m->cap[i]] = 0;
    }
    int np = side * (m->king ? 2 : 1);
    B[m->to] = np;
    H ^= ZB[zi(np)][m->to];
}

/* unmake uchun asl shaklni bilish kerak: make da saqlab qo'yamiz */
static int8_t SAVEDP[MAXP + 4];

static inline void make_s(int side, const Move *m, int ply) {
    SAVEDP[ply] = B[m->from];
    make(side, m);
}
static inline void unmake_s(int side, const Move *m, int ply) {
    int np = B[m->to];
    H ^= ZB[zi(np)][m->to];
    B[m->to] = 0;
    for (int i = 0; i < m->nc; i++) {
        B[m->cap[i]] = m->capv[i];
        H ^= ZB[zi(m->capv[i])][m->cap[i]];
    }
    B[m->from] = SAVEDP[ply];
    H ^= ZB[zi(SAVEDP[ply])][m->from];
    (void)side;
}

/* ------------------------------ baholash ------------------------------ */
static int eval(void) {
    int s = 0, matd = 0, tot = 0;
    for (int sq = 0; sq < 64; sq++) {
        int p = B[sq];
        if (!p) continue;
        int r = sq >> 3, c = sq & 7, v, base;
        tot++;
        if (p == 1 || p == -1) {
            int adv, back, cc, br;
            if (p > 0) { adv = 7 - r; back = (r == 7); cc = c; br = r + 1; }
            else { adv = r; back = (r == 0); cc = 7 - c; br = r - 1; }
            base = 100;
            v = 100 + adv * 6;
            if (adv == 6) v += 18;
            if (r >= 2 && r <= 5 && c >= 2 && c <= 5) v += 4;
            if (back && (cc == 2 || cc == 4)) v += 8;
            if (c == 0 || c == 7) v -= 3;
            if (br >= 0 && br < 8) {
                if (c > 0 && B[br * 8 + c - 1] * p > 0) v += 2;
                if (c < 7 && B[br * 8 + c + 1] * p > 0) v += 2;
            }
        } else {
            base = 340;
            v = 340;
            if (r + c == 7) v += 12;
            else if (r >= 2 && r <= 5 && c >= 2 && c <= 5) v += 4;
        }
        if (p > 0) { s += v; matd += base; } else { s -= v; matd -= base; }
    }
    /* material ustunligida almashtirish foydali */
    s += matd * (24 - tot) / 100;
    return s;
}

/* --------------------------- transpozitsiya jadvali --------------------------- */
typedef struct {
    uint64_t key;
    int16_t score;
    int8_t depth;
    uint8_t flag;
    int8_t from, to, n, pad;
} TTE;

#define TT_BITS 21
static TTE *TT;
static const uint64_t TT_MASK = ((uint64_t)1 << TT_BITS) - 1;

static int hist[2][64][64];
static int16_t killer[MAXP + 4][2];
static uint64_t keys[MAXP + 4];
static long long nodes, deadline;
static int stopf;

static inline int score_to_tt(int s, int ply) {
    if (s > WIN - 200) return s + ply;
    if (s < -WIN + 200) return s - ply;
    return s;
}
static inline int score_from_tt(int s, int ply) {
    if (s > WIN - 200) return s - ply;
    if (s < -WIN + 200) return s + ply;
    return s;
}

static int negamax(int side, int depth, int alpha, int beta, int ply) {
    nodes++;
    if ((nodes & 4095) == 0 && now_ms() > deadline) stopf = 1;
    if (stopf) return 0;
    Move ml[MAXM];
    int n = gen(side, ml);
    if (n == 0) return -WIN + ply;
    int capture = ml[0].nc > 0;
    if (depth <= 0 && !capture) return side * eval() + MOB * n;
    if (ply >= MAXP - 2) return side * eval();

    uint64_t key = H ^ SK[side > 0];
    keys[ply] = key;
    for (int i = ply - 2; i >= 0; i -= 2)
        if (keys[i] == key) return 0; /* takrorlanish = durang */

    int ttf = -1, ttt = -1, ttn = -1;
    if (depth > 0) {
        TTE *e = &TT[key & TT_MASK];
        if (e->key == key) {
            ttf = e->from; ttt = e->to; ttn = e->n;
            if (e->depth >= depth) {
                int v = score_from_tt(e->score, ply);
                if (e->flag == 0) return v;
                if (e->flag == 1 && v >= beta) return v;
                if (e->flag == 2 && v <= alpha) return v;
            }
        }
    }

    int hs = side > 0;
    for (int i = 0; i < n; i++) {
        Move *m = &ml[i];
        if (m->from == ttf && m->to == ttt && m->n == ttn) m->sc = 1000000;
        else if (m->nc) m->sc = 100000 + m->nc * 100;
        else {
            int k = (m->from << 6) | m->to;
            m->sc = hist[hs][m->from][m->to];
            if (killer[ply][0] == k) m->sc += 50000;
            else if (killer[ply][1] == k) m->sc += 40000;
            if (m->king && ml[i].path[0] != m->to) m->sc += 5;
        }
    }

    int best = -INF, bi = -1, a0 = alpha;
    for (int i = 0; i < n; i++) {
        int bj = i;
        for (int j = i + 1; j < n; j++) if (ml[j].sc > ml[bj].sc) bj = j;
        if (bj != i) { Move t = ml[i]; ml[i] = ml[bj]; ml[bj] = t; }
        Move *m = &ml[i];
        make_s(side, m, ply);
        int sc;
        if (i == 0) sc = -negamax(-side, depth - 1, -beta, -alpha, ply + 1);
        else {
            sc = -negamax(-side, depth - 1, -alpha - 1, -alpha, ply + 1);
            if (sc > alpha && sc < beta && !stopf)
                sc = -negamax(-side, depth - 1, -beta, -alpha, ply + 1);
        }
        unmake_s(side, m, ply);
        if (stopf) return 0;
        if (sc > best) { best = sc; bi = i; }
        if (sc > alpha) alpha = sc;
        if (alpha >= beta) {
            if (!m->nc) {
                int k = (m->from << 6) | m->to;
                if (killer[ply][0] != k) { killer[ply][1] = killer[ply][0]; killer[ply][0] = k; }
                hist[hs][m->from][m->to] += depth * depth;
            }
            break;
        }
    }
    if (depth > 0 && bi >= 0) {
        TTE *e = &TT[key & TT_MASK];
        if (e->key != key || depth >= e->depth) {
            e->key = key;
            e->score = (int16_t)score_to_tt(best, ply);
            e->depth = (int8_t)depth;
            e->flag = best >= beta ? 1 : (best > a0 ? 0 : 2);
            e->from = ml[bi].from; e->to = ml[bi].to; e->n = ml[bi].n;
        }
    }
    return best;
}

/* ------------------------------ perft (sinov) ------------------------------ */
static long long perft(int side, int d, int ply) {
    if (d == 0) return 1;
    Move ml[MAXM];
    int n = gen(side, ml);
    if (d == 1) return n;
    long long t = 0;
    for (int i = 0; i < n; i++) {
        make_s(side, &ml[i], ply);
        t += perft(-side, d - 1, ply + 1);
        unmake_s(side, &ml[i], ply);
    }
    return t;
}

/* ------------------------------ asosiy ------------------------------ */
static uint64_t board_hash(void) {
    uint64_t h = 0;
    for (int i = 0; i < 64; i++) if (B[i]) h ^= ZB[zi(B[i])][i];
    return h;
}

int main(void) {
    char cmd[16], bs[256];
    int arg;
    if (scanf("%15s %d", cmd, &arg) != 2) return 1;
    if (scanf("%255s", bs) != 1 || strlen(bs) < 64) return 1;
    for (int i = 0; i < 5; i++) for (int j = 0; j < 64; j++) ZB[i][j] = rnd64();
    SK[0] = rnd64(); SK[1] = rnd64();
    for (int i = 0; i < 64; i++) {
        char ch = bs[i];
        B[i] = ch == 'o' ? 1 : ch == 'O' ? 2 : ch == 'x' ? -1 : ch == 'X' ? -2 : 0;
    }
    H = board_hash();

    if (strcmp(cmd, "PERFT") == 0) {
        printf("PERFT %lld\n", perft(1, arg, 0));
        return 0;
    }

    int nex = 0, ex[64][2];
    if (scanf("%d", &nex) != 1) nex = 0;
    if (nex > 64) nex = 64;
    for (int i = 0; i < nex; i++) if (scanf("%d %d", &ex[i][0], &ex[i][1]) != 2) { nex = i; break; }
    int ns = 0;
    static uint64_t seenh[512];
    if (scanf("%d", &ns) != 1) ns = 0;
    for (int i = 0; i < ns; i++) {
        char t[256];
        if (scanf("%255s", t) != 1) break;
        if (i < 512 && strlen(t) >= 64) {
            uint64_t h = 0;
            for (int k = 0; k < 64; k++) {
                int v = t[k] == 'o' ? 1 : t[k] == 'O' ? 2 : t[k] == 'x' ? -1 : t[k] == 'X' ? -2 : 0;
                if (v) h ^= ZB[zi(v)][k];
            }
            seenh[i] = h;
        }
    }
    if (ns > 512) ns = 512;

    Move all[MAXM], rm[MAXM];
    int na = gen(1, all), n = 0;
    for (int i = 0; i < na; i++) {
        int bad = 0;
        for (int j = 0; j < nex; j++) if (all[i].from == ex[j][0] && all[i].to == ex[j][1]) bad = 1;
        if (!bad) rm[n++] = all[i];
    }
    if (n == 0) { printf("NONE\n"); return 0; }

    TT = (TTE *)calloc((size_t)1 << TT_BITS, sizeof(TTE));
    if (!TT) return 1;

    long long t0 = now_ms();
    long long limit = arg > 0 ? arg : 1000;
    deadline = t0 + limit;
    int pen[MAXM];
    for (int i = 0; i < n; i++) {
        make_s(1, &rm[i], 0);
        pen[i] = 0;
        for (int j = 0; j < ns; j++) if (seenh[j] == H) { pen[i] = REP_PEN; break; }
        unmake_s(1, &rm[i], 0);
        rm[i].sc = 0;
    }

    Move bestm = rm[0];
    int bestsc = 0, bestdepth = 0;
    if (n > 1) {
        for (int depth = 1; depth < 60; depth++) {
            int alpha = -INF, beta = INF, bi = -1;
            int iter_best = -INF;
            /* eng yaxshisi oldinda, qolganlari avvalgi bahosi bo'yicha */
            for (int i = 1; i < n; i++) {
                Move t = rm[i]; int tp = pen[i]; int j = i - 1;
                while (j >= 1 && rm[j].sc < t.sc) { rm[j + 1] = rm[j]; pen[j + 1] = pen[j]; j--; }
                rm[j + 1] = t; pen[j + 1] = tp;
            }
            keys[0] = H ^ SK[1];
            for (int i = 0; i < n && !stopf; i++) {
                make_s(1, &rm[i], 0);
                int sc;
                int pp = pen[i];
                if (i == 0) {
                    sc = -negamax(-1, depth - 1, -beta - pp, -alpha - pp, 1) - pp;
                } else {
                    sc = -negamax(-1, depth - 1, -(alpha + 1) - pp, -alpha - pp, 1) - pp;
                    if (sc > alpha && !stopf)
                        sc = -negamax(-1, depth - 1, -beta - pp, -alpha - pp, 1) - pp;
                }
                unmake_s(1, &rm[i], 0);
                if (stopf) break;
                rm[i].sc = sc;
                if (sc > iter_best) { iter_best = sc; bi = i; alpha = sc; }
            }
            if (stopf || bi < 0) break;
            /* bi - eng yaxshi yurish: uni oldinga chiqaramiz */
            Move bm = rm[bi]; int bp = pen[bi];
            for (int k = bi; k > 0; k--) { rm[k] = rm[k - 1]; pen[k] = pen[k - 1]; }
            rm[0] = bm; pen[0] = bp;
            bestm = bm; bestsc = iter_best; bestdepth = depth;
            if (iter_best > WIN - 100 || iter_best < -WIN + 100) break;
            if ((now_ms() - t0) * 2 > limit) break; /* keyingi chuqurlikka vaqt yetmaydi */
        }
    }
    printf("OK %d %d %lld %d", bestsc, bestdepth, nodes, (int)bestm.n);
    for (int i = 0; i < bestm.n; i++) printf(" %d", (int)bestm.path[i]);
    printf("\n");
    return 0;
}
