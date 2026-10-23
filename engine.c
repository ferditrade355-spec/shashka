/*
 * Shashka dvigateli (rus shashkasi, 8x8) - maksimal kuch versiyasi
 * Yasash:  gcc -O3 -march=native -o engine engine.c
 *
 * Kirish (stdin):
 *   BEST <millisekund>
 *   <64 belgi: o=mening shashkam, O=damkam, x/X=raqibniki, .=bo'sh>
 *   <n> <n ta "from to" juftligi>      (taqiqlangan yurishlar)
 *   <m> <m ta 64 belgili qator>        (oldin bo'lgan holatlar)
 * Chiqish: OK <baho> <chuqurlik> <tugunlar> <L> <L ta katak raqami>
 *
 * Qidiruv: ID + Aspiration + PVS + LMR + Futility/Razoring
 *          + IID + SEE + Countermove/History + katta TT + kuchli eval
 * Eslatma: Null-move olib tashlangan (rus shashkasi qoidalari + zugzwang)
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
#define MAXP 128
#define INF 1000000
#define WIN 30000
#define REP_PEN 35

/* -------------------- asosiy ma'lumotlar -------------------- */
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
static inline int zi(int p) { return p > 0 ? p : 2 - p; }
static inline int inb(int r, int c) { return r >= 0 && r < 8 && c >= 0 && c < 8; }

/* -------------------- Piece-Square Tables -------------------- */
/* Oddiy shashka (o'z nuqtai nazaridan, side=1 pastga) */
static const int PST_MAN[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
    12, 15, 18, 20, 20, 18, 15, 12,
    10, 14, 16, 18, 18, 16, 14, 10,
     6, 10, 14, 16, 16, 14, 10,  6,
     4,  8, 12, 14, 14, 12,  8,  4,
     2,  6, 10, 12, 12, 10,  6,  2,
     0,  4,  6,  8,  8,  6,  4,  0,
     0,  0,  2,  4,  4,  2,  0,  0
};
/* Shoh */
static const int PST_KING[64] = {
    10,  8, 12, 14, 14, 12,  8, 10,
     8, 14, 18, 20, 20, 18, 14,  8,
    12, 18, 24, 26, 26, 24, 18, 12,
    14, 20, 26, 30, 30, 26, 20, 14,
    14, 20, 26, 30, 30, 26, 20, 14,
    12, 18, 24, 26, 26, 24, 18, 12,
     8, 14, 18, 20, 20, 18, 14,  8,
    10,  8, 12, 14, 14, 12,  8, 10
};

/* -------------------- yurishlar -------------------- */
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

/* -------------------- make / unmake -------------------- */
static int8_t SAVEDP[MAXP + 4];

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

/* -------------------- SEE (Static Exchange Evaluation) -------------------- */
static int see_val(int p) {
    return (p == 1 || p == -1) ? 100 : 340;
}

static int see(int from, int to, int side) {
    /* oddiy 1-ply SEE: faqat birinchi urish qiymati */
    int victim = B[to];
    if (!victim) return 0;
    int gain = see_val(victim);
    int attacker = B[from];
    if (!attacker) return gain;
    /* agar raqib qayta urishi mumkin bo'lsa, taxminiy ayirish */
    int r = to >> 3, c = to & 7;
    int opp = -side;
    for (int d = 0; d < 4; d++) {
        int nr = r + DR[d], nc = c + DC[d];
        if (inb(nr, nc) && B[nr * 8 + nc] * opp > 0) {
            /* oddiy taxmin: eng arzon himoyachi */
            gain -= see_val(attacker) / 2;
            break;
        }
        if (B[nr * 8 + nc] == 0) {
            nr += DR[d]; nc += DC[d];
            if (inb(nr, nc) && (B[nr * 8 + nc] == 2 * opp || B[nr * 8 + nc] == -2 * (opp > 0 ? 1 : -1))) {
                gain -= see_val(attacker) / 2;
                break;
            }
        }
    }
    return gain;
}

/* -------------------- baholash -------------------- */
static int eval(void) {
    int s = 0, mat = 0, tot = 0;
    int my_men = 0, op_men = 0, my_k = 0, op_k = 0;
    int my_back = 0, op_back = 0;

    for (int sq = 0; sq < 64; sq++) {
        int p = B[sq];
        if (!p) continue;
        int r = sq >> 3, c = sq & 7;
        tot++;
        if (p == 1) {
            my_men++;
            int v = 100 + PST_MAN[sq];
            /* zanjir / himoya */
            if (r < 7) {
                if (c > 0 && B[(r + 1) * 8 + c - 1] > 0) v += 3;
                if (c < 7 && B[(r + 1) * 8 + c + 1] > 0) v += 3;
            }
            /* chetdan uzoq */
            if (c == 0 || c == 7) v -= 4;
            /* orqa qator himoyasi */
            if (r == 7) { v += 6; my_back++; }
            /* breakthrough yaqinligi */
            if (r <= 2) v += (3 - r) * 4;
            s += v; mat += 100;
        } else if (p == -1) {
            op_men++;
            int v = 100 + PST_MAN[63 - sq];
            if (r > 0) {
                if (c > 0 && B[(r - 1) * 8 + c - 1] < 0) v += 3;
                if (c < 7 && B[(r - 1) * 8 + c + 1] < 0) v += 3;
            }
            if (c == 0 || c == 7) v -= 4;
            if (r == 0) { v += 6; op_back++; }
            if (r >= 5) v += (r - 4) * 4;
            s -= v; mat -= 100;
        } else if (p == 2) {
            my_k++;
            int v = 340 + PST_KING[sq];
            /* markaz va diagonal */
            if (r + c == 7 || r == c) v += 8;
            s += v; mat += 340;
        } else {
            op_k++;
            int v = 340 + PST_KING[63 - sq];
            if (r + c == 7 || r == c) v += 8;
            s -= v; mat -= 340;
        }
    }

    /* material ustunligida almashtirish foydali */
    int phase = tot;
    if (phase > 24) phase = 24;
    s += mat * (24 - phase) / 80;

    /* oxirgi o'yin: shohlar ustunligi */
    if (my_men + op_men <= 4) {
        s += (my_k - op_k) * 25;
        /* opposition / tempo taxmini */
        if (my_k && op_k && my_men == 0 && op_men == 0)
            s += (my_k > op_k) ? 40 : (my_k < op_k) ? -40 : 0;
    }

    /* orqa qator himoyasi (dastlabki bosqich) */
    if (phase > 16) {
        s += my_back * 4 - op_back * 4;
    }

    return s;
}

/* -------------------- transpozitsiya jadvali -------------------- */
typedef struct {
    uint64_t key;
    int16_t score;
    int8_t depth;
    uint8_t flag;   /* 0=exact, 1=lower, 2=upper */
    uint8_t age;
    int8_t from, to, n;
} TTE;

#define TT_BITS 22
static TTE *TT;
static const uint64_t TT_MASK = ((uint64_t)1 << TT_BITS) - 1;
static uint8_t tt_age = 0;

static int hist[2][64][64];
static int cm_hist[64][64];          /* countermove */
static int16_t killer[MAXP + 4][2];
static uint64_t keys[MAXP + 4];
static int16_t prev_to[MAXP + 4];    /* oldingi yurish to */
static long long nodes, deadline;
static int stopf;
static int root_depth;

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

/* -------------------- negamax + barcha texnikalar -------------------- */
static int negamax(int side, int depth, int alpha, int beta, int ply) {
    nodes++;
    if ((nodes & 4095) == 0 && now_ms() > deadline) stopf = 1;
    if (stopf) return 0;

    Move ml[MAXM];
    int n = gen(side, ml);
    if (n == 0) return -WIN + ply;

    int capture = ml[0].nc > 0;

    /* terminal / qsearch */
    if (depth <= 0) {
        if (!capture) return side * eval();
        /* quiescence: faqat urishlar */
        depth = 0;
    }
    if (ply >= MAXP - 2) return side * eval();

    uint64_t key = H ^ SK[side > 0];
    keys[ply] = key;
    for (int i = ply - 2; i >= 0; i -= 2)
        if (keys[i] == key) return 0; /* takrorlanish */

    /* TT probe */
    int ttf = -1, ttt = -1, ttn = -1;
    int tt_score = 0, tt_hit = 0;
    TTE *e = &TT[key & TT_MASK];
    if (e->key == key) {
        ttf = e->from; ttt = e->to; ttn = e->n;
        tt_hit = 1;
        tt_score = score_from_tt(e->score, ply);
        if (e->depth >= depth) {
            if (e->flag == 0) return tt_score;
            if (e->flag == 1 && tt_score >= beta) return tt_score;
            if (e->flag == 2 && tt_score <= alpha) return tt_score;
        }
    }

    int static_eval = side * eval();

    /* ----- Razoring ----- */
    if (!capture && depth <= 3 && static_eval + 150 * depth < alpha && ply > 0)
        return static_eval;

    /* ----- Reverse Futility ----- */
    if (!capture && depth <= 6 && static_eval - 120 * depth >= beta && ply > 0)
        return static_eval;

    /* ----- Internal Iterative Deepening ----- */
    if (!tt_hit && depth >= 5 && !capture) {
        negamax(side, depth - 2, alpha, beta, ply);
        if (stopf) return 0;
        e = &TT[key & TT_MASK];
        if (e->key == key) {
            ttf = e->from; ttt = e->to; ttn = e->n;
            tt_hit = 1;
        }
    }

    /* move scoring */
    int hs = side > 0;
    int prev = (ply > 0) ? prev_to[ply - 1] : -1;
    for (int i = 0; i < n; i++) {
        Move *m = &ml[i];
        if (m->from == ttf && m->to == ttt && m->n == ttn) m->sc = 2000000;
        else if (m->nc) {
            int sv = 0;
            for (int k = 0; k < m->nc; k++) sv += see_val(m->capv[k]);
            m->sc = 1000000 + sv * 10 + m->nc * 50;
            /* SEE bonus */
            if (m->nc == 1) m->sc += see(m->from, m->cap[0], side);
        } else {
            int k = (m->from << 6) | m->to;
            m->sc = hist[hs][m->from][m->to];
            if (killer[ply][0] == k) m->sc += 90000;
            else if (killer[ply][1] == k) m->sc += 70000;
            if (prev >= 0 && cm_hist[prev][m->to]) m->sc += cm_hist[prev][m->to];
            if (m->king) m->sc += 15;
        }
    }

    int best = -INF, bi = -1, a0 = alpha;
    int moves_searched = 0;
    int quiet_searched = 0;

    for (int i = 0; i < n; i++) {
        /* selection sort */
        int bj = i;
        for (int j = i + 1; j < n; j++) if (ml[j].sc > ml[bj].sc) bj = j;
        if (bj != i) { Move t = ml[i]; ml[i] = ml[bj]; ml[bj] = t; }

        Move *m = &ml[i];
        int is_quiet = (m->nc == 0);

        /* Late Move Pruning (sodda) */
        if (!capture && depth <= 4 && is_quiet && quiet_searched >= 3 + depth * depth && best > -WIN + 100)
            continue;

        make_s(side, m, ply);
        prev_to[ply] = m->to;

        int sc;
        int new_depth = depth - 1;

        /* Late Move Reductions */
        int reduced = 0;
        if (depth >= 3 && moves_searched >= 3 && is_quiet && !capture) {
            int r = 1;
            if (moves_searched >= 6) r++;
            if (depth >= 6 && moves_searched >= 8) r++;
            if (m->sc < 0) r++;
            if (r > new_depth) r = new_depth;
            if (r > 0) {
                sc = -negamax(-side, new_depth - r, -alpha - 1, -alpha, ply + 1);
                reduced = 1;
            } else sc = alpha + 1; /* force full */
        } else sc = alpha + 1;

        if (!reduced || (sc > alpha && !stopf)) {
            if (moves_searched == 0)
                sc = -negamax(-side, new_depth, -beta, -alpha, ply + 1);
            else {
                sc = -negamax(-side, new_depth, -alpha - 1, -alpha, ply + 1);
                if (sc > alpha && sc < beta && !stopf)
                    sc = -negamax(-side, new_depth, -beta, -alpha, ply + 1);
            }
        }

        unmake_s(side, m, ply);
        if (stopf) return 0;

        moves_searched++;
        if (is_quiet) quiet_searched++;

        if (sc > best) {
            best = sc;
            bi = i;
        }
        if (sc > alpha) alpha = sc;
        if (alpha >= beta) {
            if (is_quiet) {
                int k = (m->from << 6) | m->to;
                if (killer[ply][0] != k) {
                    killer[ply][1] = killer[ply][0];
                    killer[ply][0] = k;
                }
                /* history gravity */
                int bonus = depth * depth;
                hist[hs][m->from][m->to] += bonus;
                if (hist[hs][m->from][m->to] > 200000)
                    hist[hs][m->from][m->to] /= 2;
                if (prev >= 0) {
                    cm_hist[prev][m->to] += bonus / 2;
                    if (cm_hist[prev][m->to] > 100000)
                        cm_hist[prev][m->to] /= 2;
                }
            }
            break;
        }
    }

    /* TT store */
    if (bi >= 0) {
        e = &TT[key & TT_MASK];
        int replace = (e->key != key) || (depth + (tt_age != e->age) * 2 >= e->depth);
        if (replace) {
            e->key = key;
            e->score = (int16_t)score_to_tt(best, ply);
            e->depth = (int8_t)depth;
            e->flag = best >= beta ? 1 : (best > a0 ? 0 : 2);
            e->age = tt_age;
            e->from = ml[bi].from;
            e->to = ml[bi].to;
            e->n = ml[bi].n;
        }
    }
    return best;
}

/* -------------------- perft -------------------- */
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

/* -------------------- asosiy -------------------- */
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

    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 64; j++) ZB[i][j] = rnd64();
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
    for (int i = 0; i < nex; i++)
        if (scanf("%d %d", &ex[i][0], &ex[i][1]) != 2) { nex = i; break; }

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
        for (int j = 0; j < nex; j++)
            if (all[i].from == ex[j][0] && all[i].to == ex[j][1]) bad = 1;
        if (!bad) rm[n++] = all[i];
    }
    if (n == 0) { printf("NONE\n"); return 0; }

    TT = (TTE *)calloc((size_t)1 << TT_BITS, sizeof(TTE));
    if (!TT) return 1;
    memset(hist, 0, sizeof hist);
    memset(cm_hist, 0, sizeof cm_hist);
    memset(killer, 0, sizeof killer);

    long long t0 = now_ms();
    long long limit = arg > 0 ? arg : 1000;
    deadline = t0 + limit;
    stopf = 0;
    nodes = 0;
    tt_age++;

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

    if (n == 1) {
        bestm = rm[0];
        bestsc = 0;
        bestdepth = 0;
    } else {
        int alpha = -INF, beta = INF;
        int window = 40;

        for (int depth = 1; depth < 64; depth++) {
            root_depth = depth;
            int bi = -1;
            int iter_best = -INF;
            stopf = 0;

            /* move ordering: oldingi eng yaxshi oldinda */
            for (int i = 1; i < n; i++) {
                Move t = rm[i]; int tp = pen[i]; int j = i - 1;
                while (j >= 0 && rm[j].sc < t.sc) {
                    rm[j + 1] = rm[j];
                    pen[j + 1] = pen[j];
                    j--;
                }
                rm[j + 1] = t;
                pen[j + 1] = tp;
            }

            /* aspiration */
            if (depth >= 5) {
                alpha = bestsc - window;
                beta = bestsc + window;
                if (alpha < -INF) alpha = -INF;
                if (beta > INF) beta = INF;
            } else {
                alpha = -INF;
                beta = INF;
            }

            keys[0] = H ^ SK[1];
            int research = 1;
            while (research && !stopf) {
                research = 0;
                iter_best = -INF;
                bi = -1;
                int a = alpha, b = beta;

                for (int i = 0; i < n && !stopf; i++) {
                    make_s(1, &rm[i], 0);
                    prev_to[0] = rm[i].to;
                    int sc;
                    int pp = pen[i];
                    if (i == 0) {
                        sc = -negamax(-1, depth - 1, -b - pp, -a - pp, 1) - pp;
                    } else {
                        sc = -negamax(-1, depth - 1, -(a + 1) - pp, -a - pp, 1) - pp;
                        if (sc > a && sc < b && !stopf)
                            sc = -negamax(-1, depth - 1, -b - pp, -a - pp, 1) - pp;
                    }
                    unmake_s(1, &rm[i], 0);
                    if (stopf) break;
                    rm[i].sc = sc;
                    if (sc > iter_best) {
                        iter_best = sc;
                        bi = i;
                        a = sc;
                    }
                }

                if (stopf || bi < 0) break;

                /* aspiration fail */
                if (iter_best <= alpha) {
                    alpha = -INF;
                    window *= 2;
                    research = 1;
                    continue;
                }
                if (iter_best >= beta) {
                    beta = INF;
                    window *= 2;
                    research = 1;
                    continue;
                }
            }

            if (stopf || bi < 0) break;

            /* eng yaxshini oldinga */
            Move bm = rm[bi];
            int bp = pen[bi];
            for (int k = bi; k > 0; k--) {
                rm[k] = rm[k - 1];
                pen[k] = pen[k - 1];
            }
            rm[0] = bm;
            pen[0] = bp;

            bestm = bm;
            bestsc = iter_best;
            bestdepth = depth;

            if (iter_best > WIN - 150 || iter_best < -WIN + 150) break;
            if ((now_ms() - t0) * 5 / 3 > limit) break; /* keyingi chuqurlikka vaqt yetmaydi */
            window = 40 + depth; /* keyingi iteratsiya uchun biroz kengaytirish */
        }
    }

    printf("OK %d %d %lld %d", bestsc, bestdepth, nodes, (int)bestm.n);
    for (int i = 0; i < bestm.n; i++) printf(" %d", (int)bestm.path[i]);
    printf("\n");

    free(TT);
    return 0;
}
