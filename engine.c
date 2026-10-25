/*
 * Shashka dvigateli (rus shashkasi, 8x8) - shashka.py bilan ishlaydi.
 * Yasash (Windows, MinGW):   gcc -O3 -o engine.exe engine.c
 * Yasash (Linux/Mac):        gcc -O3 -o engine engine.c
 *
 * Tezroq (faqat shu kompyuterda ishlatiladigan fayl uchun): gcc -O3 -march=native -o engine engine.c
 *
 * Ixtiyoriy ko'p oqimli qidiruv (Lazy SMP, tajribaviy; 2-4 yadro):
 *   Linux/Mac: gcc -O3 -DUSE_THREADS -pthread -o engine engine.c
 *   Windows:   gcc -O3 -DUSE_THREADS -o engine.exe engine.c
 *   Oqimlar soni: DRTHREADS muhit o'zgaruvchisi (standart: yadrolar soni, eng ko'pi 4).
 *   Bayroqsiz yasalsa - oddiy bir oqimli versiya (tavsiya etiladi).
 *
 * Damka urish qoidasi (-DKING_STOP=n):
 *   2 (standart) = Gramora: damka bir chiziqda (diagonalda) keyingi shashkani ura olsa, urishi SHART;
 *                  faqat burilib (yo'nalishni o'zgartirib) urishda to'xtash mumkin.
 *   1 = damka zanjirning istalgan joyida to'xtay oladi;  0 = standart rus qoidasi (zanjir majburiy).
 *   Oddiy shashka har doim zanjirni oxirigacha uradi. shashka.py dagi KING_STOP_MODE shunga teng bo'lishi kerak.
 *   Endshpil bazasi har bir rejim uchun alohida (gen_tb.c bilan qayta hisoblangan va tekshirilgan).
 *
 * Server rejimi (shashka.py ishlatadi): engine --server
 *   NEWGAME | GO <ms> <taxta> <ro'yxatlar> (javob: OK ... / NONE) | PONDER <taxta> | STOP | QUIT
 *   Jarayon tirik turadi, transpozitsiya jadvali yurishlar orasida saqlanadi, PONDER raqib
 *   o'ylayotganda qidirishni davom ettiradi. Oddiy "BEST ..." rejimi avvalgidek ishlaydi.
 *   Ponderni o'chirish: -DNO_PONDER bilan yasang.
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


/* ---- ko'p oqimli qidiruv (Lazy SMP): -DUSE_THREADS bilan yoqiladi, Linux: -pthread kerak ---- */
#ifndef NO_PONDER
#define HAVE_THR 1
#ifdef _WIN32
typedef HANDLE thr_t;
typedef struct { void (*fn)(void *); void *arg; } ThrArg;
static DWORD WINAPI thr_tramp(LPVOID p) { ThrArg a = *(ThrArg *)p; free(p); a.fn(a.arg); return 0; }
static int thr_start(thr_t *t, void (*fn)(void *), void *arg) {
    ThrArg *a = (ThrArg *)malloc(sizeof *a);
    if (!a) return 0;
    a->fn = fn; a->arg = arg;
    *t = CreateThread(NULL, 0, thr_tramp, a, 0, NULL);
    return *t != NULL;
}
static void thr_join(thr_t t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
static int ncpu(void) __attribute__((unused));
static int ncpu(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_t thr_t;
typedef struct { void (*fn)(void *); void *arg; } ThrArg;
static void *thr_tramp(void *p) { ThrArg a = *(ThrArg *)p; free(p); a.fn(a.arg); return NULL; }
static int thr_start(thr_t *t, void (*fn)(void *), void *arg) {
    ThrArg *a = (ThrArg *)malloc(sizeof *a);
    if (!a) return 0;
    a->fn = fn; a->arg = arg;
    return pthread_create(t, NULL, thr_tramp, a) == 0;
}
static void thr_join(thr_t t) { pthread_join(t, NULL); }
static int ncpu(void) __attribute__((unused));
static int ncpu(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }
#endif
#endif
#ifdef USE_THREADS
#define TLS __thread
#else
#define TLS
#endif

#ifndef KING_STOP
#define KING_STOP 2 /* 0 = standart rus qoidasi; 1 = damka zanjirda istalgan joyda to'xtay oladi; 2 = Gramora: bir chiziqda keyingi shashkani urish majburiy, burilib urish ixtiyoriy */
#endif
#define MAXM 256
#define MAXP 96
#define INF 1000000
#define WIN 30000
#ifndef MOB
#define MOB 2
#endif
#define REP_PEN 25
#ifndef EXT_SINGLE
#define EXT_SINGLE 1
#endif
#ifndef USE_LMR
#define USE_LMR 1
#endif
#ifndef SOFT_PCT
#define SOFT_PCT 65
#endif
#ifndef LMR_I
#define LMR_I 3
#endif
#ifndef LMR_J
#define LMR_J 8
#endif
#ifndef USE_CM
#define USE_CM 0
#endif
#ifndef EVAL2
#define EVAL2 1
#endif
#ifndef CEN_W
#define CEN_W 3
#endif
#ifndef KMOB_W
#define KMOB_W 2
#endif
#ifndef NEWEVAL
#define NEWEVAL 1
#endif
#ifndef RUN_BASE
#define RUN_BASE 14
#endif
#ifndef RUN_STEP
#define RUN_STEP 10
#endif

typedef struct {
    uint64_t capm;
    int8_t from, to, nc, king;
    int sc;
} Move;
static TLS int8_t RP[MAXM][16];
static TLS int8_t RPn[MAXM];
static TLS int want_path = 0;
static TLS int8_t gp[20];
static TLS int gn;

static TLS int8_t B[64];
static TLS uint8_t TK[64];
static TLS uint64_t H;
static TLS uint64_t MP, MN, MK; /* o-shashkalar, x-shashkalar, damkalar bitmaskalari */
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
static int8_t NB[64][4];
/* bitmaskalar: qo'shnilar, oldingi qo'shnilar, orqadagi qo'shnilar, "erkin yo'l" konusi */
static uint64_t NBM[64];            /* 4 ta diagonal qo'shni */
static uint64_t FWDM[2][64];        /* [0=o yuqoriga, 1=x pastga] oldingi 2 qo'shni */
static uint64_t BEHL[2][64], BEHR[2][64];   /* orqadagi chap/o'ng qo'shni (borligi) */
static uint64_t CONE[2][64];        /* oldinga qaragan barcha kataklar konusi */
static void init_nb(void) {
    for (int sq = 0; sq < 64; sq++)
        for (int d = 0; d < 4; d++) {
            int r = (sq >> 3) + DR[d], c = (sq & 7) + DC[d];
            NB[sq][d] = inb(r, c) ? r * 8 + c : -1;
        }
    for (int sq = 0; sq < 64; sq++) {
        int r = sq >> 3, c = sq & 7;
        NBM[sq] = 0;
        for (int d = 0; d < 4; d++) if (NB[sq][d] >= 0) NBM[sq] |= 1ULL << NB[sq][d];
        for (int col = 0; col < 2; col++) {
            int fr = col == 0 ? r - 1 : r + 1, br = col == 0 ? r + 1 : r - 1;
            FWDM[col][sq] = BEHL[col][sq] = BEHR[col][sq] = CONE[col][sq] = 0;
            if (fr >= 0 && fr < 8) {
                if (c > 0) FWDM[col][sq] |= 1ULL << (fr * 8 + c - 1);
                if (c < 7) FWDM[col][sq] |= 1ULL << (fr * 8 + c + 1);
            }
            if (br >= 0 && br < 8) {
                if (c > 0) BEHL[col][sq] = 1ULL << (br * 8 + c - 1);
                if (c < 7) BEHR[col][sq] = 1ULL << (br * 8 + c + 1);
            }
            int step = col == 0 ? -1 : 1;
            for (int rr = r + step; rr >= 0 && rr < 8; rr += step) {
                int dist = rr > r ? rr - r : r - rr;
                for (int cc = c - dist; cc <= c + dist; cc++) if (cc >= 0 && cc < 8) CONE[col][sq] |= 1ULL << (rr * 8 + cc);
            }
        }
    }
}

static const uint64_t COL0 = 0x0101010101010101ULL, COL7 = 0x8080808080808080ULL;
static inline uint64_t shd(uint64_t x, int d) {   /* diagonal siljish (DR/DC tartibida), taxtadan chiqqanlar tashlanadi */
    switch (d) {
        case 0: return (x >> 9) & ~COL7;
        case 1: return (x >> 7) & ~COL0;
        case 2: return (x << 7) & ~COL7;
        default: return (x << 9) & ~COL0;
    }
}
static inline int pc64(uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
}
static int king_can_cap(int side, int sq) {
    for (int d = 0; d < 4; d++) {
        int s = NB[sq][d];
        while (s >= 0 && B[s] == 0) s = NB[s][d];
        if (s >= 0 && B[s] * side < 0 && !TK[s]) {
            int l = NB[s][d];
            if (l >= 0 && B[l] == 0) return 1;
        }
    }
    return 0;
}

static void emit(Move *out, int *cnt, uint64_t capm, int nc, int from, int king) {
    if (*cnt >= MAXM) return;
    Move *m = &out[*cnt];
    m->capm = capm; m->from = from; m->to = gp[gn - 1];
    m->nc = nc; m->king = (int8_t)king; m->sc = 0;
    if (want_path) { memcpy(RP[*cnt], gp, gn); RPn[*cnt] = gn; }
    (*cnt)++;
}

static void rec(int side, int sq, int king, uint64_t capm, int nc, int from, Move *out, int *cnt) {
    int found = 0;
    int promo = side == 1 ? 0 : 7;
    for (int d = 0; d < 4; d++) {
        int t = NB[sq][d];
        if (t < 0) continue;
        if (!king) {
            int l = NB[t][d];
            if (l >= 0 && !TK[t] && B[t] * side < 0 && B[l] == 0) {
                found = 1;
                TK[t] = 1;
                gp[gn++] = l;
                rec(side, l, ((l >> 3) == promo) ? 1 : 0, capm | (1ULL << t), nc + 1, from, out, cnt);
                gn--;
                TK[t] = 0;
            }
        } else {
            int s = t;
            while (s >= 0 && B[s] == 0) s = NB[s][d];
            if (s >= 0 && B[s] * side < 0 && !TK[s]) {
                int lands[8], nl = 0;
                int l = NB[s][d];
                while (l >= 0 && B[l] == 0) { lands[nl++] = l; l = NB[l][d]; }
                if (nl) {
                    found = 1;
                    TK[s] = 1;
#if KING_STOP
                    int same = 0;   /* shu chiziqning davomida yana urish mumkinmi (QAYRILMASDAN)? */
#if KING_STOP == 2
                    {
                        int t2 = NB[lands[nl - 1]][d];
                        if (t2 >= 0 && B[t2] * side < 0 && !TK[t2]) { int l2 = NB[t2][d]; if (l2 >= 0 && B[l2] == 0) same = 1; }
                    }
#endif
                    for (int i = 0; i < nl; i++) {
                        gp[gn++] = lands[i];
                        if (!same && king_can_cap(side, lands[i]))   /* burilib davom etish mumkin, lekin shu yerda to'xtash ham mumkin */
                            emit(out, cnt, capm | (1ULL << s), nc + 1, from, 1);
                        rec(side, lands[i], 1, capm | (1ULL << s), nc + 1, from, out, cnt);
                        gn--;
                    }
#else
                    int cont[8], ncn = 0;
                    for (int i = 0; i < nl; i++)
                        if (king_can_cap(side, lands[i])) cont[ncn++] = lands[i];
                    int *lst = ncn ? cont : lands;
                    int ln = ncn ? ncn : nl;
                    for (int i = 0; i < ln; i++) {
                        gp[gn++] = lst[i];
                        rec(side, lst[i], 1, capm | (1ULL << s), nc + 1, from, out, cnt);
                        gn--;
                    }
#endif
                    TK[s] = 0;
                }
            }
        }
    }
    if (!found && nc > 0) emit(out, cnt, capm, nc, from, king);
}

static inline void addq(Move *out, int *cnt, int from, int to, int king) {
    Move *m = &out[*cnt];
    m->capm = 0; m->from = from; m->to = to; m->nc = 0; m->king = king; m->sc = 0;
    if (want_path) { RP[*cnt][0] = from; RP[*cnt][1] = to; RPn[*cnt] = 2; }
    (*cnt)++;
}

static int gen(int side, Move *out) {
    int cnt = 0;
    uint64_t own = side > 0 ? MP : MN, enemy = side > 0 ? MN : MP;
    /* urishi mumkin bo'lgan oddiy shashkalar (bitmaska bilan) + barcha damkalar */
    uint64_t capmen = 0;
    {
        uint64_t men = own & ~MK, empty = ~(MP | MN);
        for (int d = 0; d < 4; d++) {
            uint64_t t = shd(men, d) & enemy;
            if (!t) continue;
            uint64_t l = shd(t, d) & empty;
            if (l) capmen |= shd(shd(l, 3 - d) & enemy, 3 - d) & men;
        }
    }
    for (uint64_t om = capmen | (own & MK); om; om &= om - 1) {
        int sq = __builtin_ctzll(om);
        int p = B[sq];
        {
            gn = 1; gp[0] = sq;
            B[sq] = 0;
            rec(side, sq, (p == 2 || p == -2), 0, 0, sq, out, &cnt);
            B[sq] = p;
        }
    }
    if (cnt) return cnt;
    int fd = side == 1 ? 0 : 2; /* DR indekslari: 0,1 = yuqoriga (-1), 2,3 = pastga */
    int promo = side == 1 ? 0 : 7;
    for (uint64_t om = own; om; om &= om - 1) {
        int sq = __builtin_ctzll(om);
        int p = B[sq];
        if (p == side) {
            for (int d = fd; d < fd + 2; d++) {
                int t = NB[sq][d];
                if (t >= 0 && B[t] == 0 && cnt < MAXM) addq(out, &cnt, sq, t, (t >> 3) == promo);
            }
        } else {
            for (int d = 0; d < 4; d++) {
                int t = NB[sq][d];
                while (t >= 0 && B[t] == 0) {
                    if (cnt < MAXM) addq(out, &cnt, sq, t, 1);
                    t = NB[t][d];
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
    uint64_t c = m->capm;
    while (c) {
        int t = __builtin_ctzll(c);
        c &= c - 1;
        H ^= ZB[zi(B[t])][t];
        B[t] = 0;
    }
    int np = side * (m->king ? 2 : 1);
    B[m->to] = np;
    H ^= ZB[zi(np)][m->to];
    uint64_t fb = 1ULL << m->from, tb = 1ULL << m->to;
    if (side > 0) { MP = (MP & ~fb) | tb; MN &= ~m->capm; }
    else { MN = (MN & ~fb) | tb; MP &= ~m->capm; }
    MK &= ~(fb | m->capm);
    if (m->king) MK |= tb;
}

static TLS int NPCS = 0;
static TLS int SNPC[MAXP + 4];
static TLS int8_t SB[MAXP + 4][64];
static TLS uint64_t SH[MAXP + 4];
static TLS uint64_t SMP[MAXP + 4], SMN[MAXP + 4], SMK[MAXP + 4];

static inline void make_s(int side, const Move *m, int ply) {
    memcpy(SB[ply], B, 64);
    SH[ply] = H;
    SNPC[ply] = NPCS;
    SMP[ply] = MP; SMN[ply] = MN; SMK[ply] = MK;
    for (uint64_t cm = m->capm; cm; cm &= cm - 1) NPCS--;
    make(side, m);
}
static inline void unmake_s(int side, const Move *m, int ply) {
    memcpy(B, SB[ply], 64);
    H = SH[ply];
    NPCS = SNPC[ply];
    MP = SMP[ply]; MN = SMN[ply]; MK = SMK[ply];
    (void)side; (void)m;
}


/* ---- damkalar endshpil bazasi (3v1, 2v1, 1v2, 1v1), kod ichiga joylangan ----
   Qiymatlar yuruvchi nuqtai nazaridan: 0 = durang, v>0 = v yarim yurishda g'alaba,
   v<0 = (-v-1) yarim yurishda mag'lubiyat. Doskaning 4 ta simmetriyasi bo'yicha
   faqat kanonik holatlar saqlanadi (base64, 4 bitdan). */
#if KING_STOP == 1
/* Gramora qoidasi (damka to'xtay oladi) bo'yicha qayta hisoblangan baza (gen_tb.c bilan tekshirilgan) */
static const char TBDATA_GR[] =
    "sBEREUsRERAksAG7ArubGhERkakRGSEZGpEjkqOYEaERiBEYESihEagig8ocERFBFBERLBTMocyyyQoREBEqEaERpBABGgJakhAR"
    "EZAZoRGRAhGgIaApCxERESsBEbEkEwsTCzuCGBgRgRiBGIEYEYEhgiiaEREaKRGRoZIRFBkqSZKoERGBGIERihKhgaEyKAsUEBEr"
    "G7EQtLABWwJLRLAREaAasQGhAhuwJbBKBhwRESsRERAkwwHDDDucmRkRkZmRGUEZGZEmkkW2GxERKxuxEbKxETsrO6u7ERGBG4ER"
    "gRIbgbsyuKqgEAGqEaoRphABGgJqIgoBEaESERpBChGgMaAkQRARAUkRGhEkEAEaA0oiiBgRgRiBGFEYEYExgiQMHBARSxEREMLA"
    "ATsCPEywERuRGxEBwQIbsCuwuw0dERFEARHQItAN3Q1ETZQRGoEXERFBFBqBKoKnTBwREUsREcHCwRzLLJvMzBwcQRsREcwSzLHM"
    "MsumEBARehEREaYQARoCesIMARGxEhARLDxBMMHAI1uwERFJARGxLBALGwu7kkUaEYEYERFaFKGBoYIosGAQEasRERCzoAFbAltk"
    "DQwRsRIRASE9HDDDwMMMwBEROREREC3AAcwMzKxAGxGBExERQRQbgbuCtwEKEBFKERERoxABGgNKsrEREbG5ERohGhuhI6KzKBmh"
    "EYMRGBEooRGoIoPKwRERMRQRESwUzKHMssmZGqkRmRqZEUmhEakilMrMEcHByhEbLBzMsczCyszMwRHMERzBPMEczCzDjCmBEZEY"
    "gRiBGBGBIYIouxERGysRQbGiERQaK0qSKBoRMRiBEYoSoYGhMiiaqRkamRGZoUkRGhkqlJIqmRGhqZEZQRkakSaSRbbBERErHEER"
    "ssEROyw7rCsbETEbgRGBEhuBuzK4u7kZEbkbmRFJsRG5K5QrmIIRkRiBGFEYEYExgiQEkaERcxEREUShEagieMoQHBExFBERLBzM"
    "scyyyczBzBG0ERHBLMEcyyyzzAzMHMEcERHMFMzBzELMWRAaEYMREaFFERoYKoiiALERMRMREUEUG4G7grcKEgERKhGhEaQQARoC"
    "SkMQERGQFKERQQIRoCGgNAw2ERErARHBJBAMGwxLhiiBEYEYgRhBGBGBIYI1mzYRGykRkbGSERsZK5m7qxERgRuBEYsSsYGxMri0"
    "QAEbKwGxsbQQBRsES7IKERGgFLARqwJRsEGwKpkZGRmZEZmRlREWGSRJIgqiEKASoRpRChGgIaA2ozABESoRoRGjEAEaBEo0AhMR"
    "kxIxETEyETBBoEMjOBgRKBGIEYgRERgkiAQKERChEhERMQoRoCGgNcxgAxErARHBwhAMGwxMxgAcEZESEBHcAsHAwcDMC6QRESgR"
    "EbFIERsYK4hrsBAQsRIQEVoLYbBBsCQjCgMRKhEREaQTARMEOoQpEhiRGIEYQRgRgSGCNbxhExsrEaHBohEcGiyavCsaETEbgRGL"
    "ErGBsTK4u7mRG7kRmbFJERsZK5SbqRKpoRmRGVoZYZFBkiQkKUQRKRFIEYQRERgkSLQAoRExEhERixSxgbGCuAsUARFKEREQtKAB"
    "WgJak5AzkZAZkQGRAhmQKZA5BhwREUQRERAkwAHLDJSMlBOBgRiBEYEUGIEogjhGGxERSBEREUShETgmOLu5MbGRGIERkRIbgbsy"
    "uKowAQGqERoRqhABGgKqEgsREJEUERFBOxEwIZAjghATAYgRGBEoEAEYAogiRxERcRZxEWEUEXEhcie0QAEBCxEbELawAWsCayMM"
    "GxCxExEBQTwbMCuws0KwEwEJERsQK7ABuwK7K0kZEYEZgRFBFBqBKoKoQgoBAUoRGhGkEAEaAlpCAjVQoRoRAWECGqAmoDomwAER"
    "SxERECPAATsMO8wEHMExHBEB0QIcwMxAzEuyERtzERERJLERtitzKyUQEJEUERFBAhFgIZAlBgALETsRERAjsAG7AruLIDmBMRiB"
    "EYESGIEogjgGsRERQxERESShETgmOLspG7MxGJERkRIbkbsyuIuwERuIERgRKLERuCqDKwcSETEWcRFhEhFxIXInkyAZEZMRGBEk"
    "oRGoIoi6JLGxMRcREUESG2G7MrekEBEAShEaEaQQARoCWjMLEwGREhERQQsRkEGQNEMwMQFIERgRJBABGARIM0cTEXEWcRFBFBFx"
    "QXI3kQsRAEkRGRG0EAEZA1kSoiABoxQxETE6ETAhoDMEBBMQSRERESMQARYEWUMHQhExFnERQRIRcUFyNyobExGqG6ERorEROiI6"
    "S6ERoZEXERFBFBqRKjKnHBwREUQREcFCwRzKLEtMqRGjkakRGUEZGpEqkqnMHDPBrBzBwcLBHMwsrMzMHMHBGxERzBTMwczCy5YS"
    "ERF5EZERlhERGSJ5sioxEasSERorGkGhsaIkWqEREUkREaFIERoZKnOilRoTmhkRGVoZoZGhkimgYhERqhqhEaKhEVoiWmQrPBGx"
    "whEbIRscscOywwuxERE5ERERQrERuStzu5AbE5GzERkhGRuRu5K5AikREXkRkRGSEREZI0kiJBoxkRMRESEUGpEjkqNMwhEROxER"
    "wcLBHMssS8wSzMFBGxERLBTMYcwyzCzEPBzMERHB0sEczCzMrEJCEZEXEREqFEGRoZInKyQbETkRERFCsRE5KzlLGxuxsbkRGkEU"
    "G6ErQrnd0THRbR2x0cLRHdstvM0czMExGxERzBPMwcwyy8zNHMzcwcERsrWxEbsZERpbFLGhsUIpDBwcETwcoRFCwRHKLJTMDcHM"
    "MRwRESwUzGHMQsyQIhERKRGREZIRERkiWcMqNhOrEhEaLBrBocGiyguxEREpERGxQhEbGStzu5AbMZsSERkrGbGRsZK5BioRoSoR"
    "oaGiERYaJEpCkkIRkRIRGSEZEZFBkkUrJBsRKRERsUIRGxkrecvAwRHMEhEaLBTBocFCyURSE1F5ERERRJERaSJ5YyQbEZEUEREh"
    "FBqRY5KzS7ERsXQREREksRG2K3O7RBsxixgREUEUG4GrgrhSJBERSBEREUQRERYiWDJAohGRExERQRQakSqSpysiGzEzERERRLER"
    "NiszuwTBETsYERFBFBthq0K4BCQUEWgRERFFEREWJFizsBEREbsRCyELERAjILMLGwEbsBERECsQAbECstvQERARFBABIQLREN0g"
    "3YgZGBqBGoEROBERoSKCysQRERzFERohG8ERzELJzMwRHMERERIhmRAaEZARoRGpEAERApKyCxERGxIQFCELQRCxICS7sAERsAER"
    "ESsQCxELspKIihEaGBEYgROhEaEiKKqwGxGgG7EQtBABYQJSZAscERHCEQQhCxEQwyDD3MABEdARERAsEAHBDMK8i4sREbsRGIET"
    "ERG7IriiCRoRkBGhEaUQAREDQnICG7ERGxEBQQsRECsguy3QERFBAREQ0hAN0Q1C3Q7d0BEdEAHhAtEQ3SDdzMIcHLERERE8ERzB"
    "LLK8JrARERYQEVELsRCxICQMAhwR0REREMQQAcEMQowjGagRqBEYgRMRESoiqDzBEcFRHKERshEcwSyUzCzMwREcESERkjiiGBoY"
    "ERiBE6ERoSIouyOLEbEbgRE4ERGxK4LLLMHMERsREcETwRHMIsuaIKERoBGhEaQQARECUsMLNhEcEhAbIQvBEMEgxNzAERDQARER"
    "LBAMEQzCvIsbGBsbERiBE7ERsSK4qwqxsaABsRG1EAYRBEIykjAUARIRFEEKERBBIEQMAhwRIQEREc0QDBEMQrw7soEbGxEYgROx"
    "EbEiuCmQE5GQEREQKRABkQKSYwIcEREUEQFBAhEQwyDDzMAxwMARERAsEAHBDMK8JxsRGxYREWESERG7IrdyAhERgBERESgQAREC"
    "ciIgsBEBEBEBQQIRECsgu9wCHMHBEREQJBABwQzCvCeyERsWERFhEhERuyK3cwITEYAREREkEAERBHJDGRoRGqcRGUETEREqQqdM"
    "wTERTByhEbIRHMEsRMwdzBEcHBEhEaKWoRGhFhEZUROhEaFCJAsZGxExG5ERMhERsStEyyLBHBwVEREhE8ERzCLECxkbESsRkREy"
    "ERsRK0S7JLERsRQREUETERG7IrUMHAERwRwREMLBATEELCywERExGzEBMRIbECuwtA0dERHRARHQItEN0Q0tLamRERGpkRFBGRoR"
    "JJKizBwRwcEcEcHCwRzBLCzMxBERERuxEZwSzBHMMsJLEBERsAEbsSsRCxELS6KZkREaGZEROhkxEaGSIsbQAR3RHREQwtEBMQ0t"
    "bQIcMQEbsQFBEhwQbLDEJrkZE5EbGRGZsRGxKykrDBsQERsRATEcGxAksLLt0AER0QER0OLRDdENLd0CHRERFBAB3RLdEN1A0izM"
    "ERGxERHBQ8EcwSwrvMIQEBEbEBE7HDEQsbAiJgwME8ERERDDwQHBBissKxsUEbuhEUEaGxEkorLN0RHR0R0R0cLRHdEtLc0kHBER"
    "G6ERnBLMEcwywqzMwcHBHBzBSsEcwSwkvLkSGhsboRE7GjERsaIiJiusE7EbGhGpwRHBLCrMAsERERwRETwUzBHMQsIsYBERswET"
    "wSMRDBEMS7yyFhsbG7EROxsxEbGysiwNFhDBARHB0xEDEQwrzLJiQRwbsRE8GzERwbLCskAREKMRExCzoQFRAkpjDBwBERsRASEc"
    "HBBjsMImwDERQxETECPBAcEMZGyyGxERGIERQRQaEWuCtCELERCQERkRtBEBEQJJIsJQAQMboQFBHBsQJbBUJgIcEMERERAjwQHB"
    "DCtsArIRERuBEUESGhFrQrQjCxMBkxETEbMRAREESSMqGxERuhERIRobESSiss3SMRHdHRHRwtEd0S0tzRIcEREbEREsFMwRzDLC"
    "LMwRE8wcEcHCwRzBLCy8ohIRsRoRESsaMRGxoiImKhwxoRsREaLBEcEsKszCwhERGxERLBTMEcyywi0dHRHdHRHRQtEd0S0kzbJi"
    "EcEbEREsGzERwbLCJiwbEZERERFDoRGxJikrCxsUEbARAUEbERAkILLd0BERHQ0RENIRDdENIt0EHQEREBAB0RLREN0g0lzLsREc"
    "HBEROREcwSwivLsQG7EQEBExGzEQsSAiJgu8MQEcERC0EQHBDCLcItAREREQATEd0RDdINJcKxwaHBwRETkRHMEsIsyyYEHBEBAR"
    "MRsxEMEgwiYCHBEBEREQJBEBwQxCzLLBERHMEREhE8ERzELCCBIREYAYGBEoEAEYAojCYBERESQQEWwCwbDBQMZGEhERcRcXEUcR"
    "ERciZ7JkERERKBERShuxgbGCZHu7ERFxFxexJxEbFytzmwkZMQGZkBGZApGQMZApiYQRE4EYGIFIERgYI4gyBBMRAUiBEUECEYAx"
    "gERjNBERcRcXEUcRERcjR0QCEhAReRERUQIRgCGQJyxgATGxAhHBJBAMGwxMzA3MERPEEBHcAsHAwUDMe7IbAXEXEbEnERsXK3db"
    "JVAwE0oQEWoCUaAxoCYDAAQRkRMREQMQARgDSWQgEhERc3ERcRIRcSFyJgthERExEhGhJBEbGCtItie7ERFzcRF7ErFxsTK3i7Ab"
    "EYEYGLEoERsYK4OaCIIxEYOBEYgSgYExgihkIBQRMRcXEUcRERcjR7QHsRsTcxERexKxcbFytQsgERGQABuxKxALGwu7kkkSERGY"
    "gRGKFKGBoYIkCwATMLECEbEmEAsbC0uSCTIREZOBEYoSoYGhgiQpIBMQkBkZESkQARkCmTICEwERSBERQQIRYDGANXMwIREwFxcR"
    "JxABFwN3MyYTERFjYRFhEhFhMTI2QQISEJAUGREEEAEZAlkzRDABEYgREWECEWAxgDhkIhQRMRYWESYRERYjY0MkEhERWBERQRQR"
    "YSGCJktiERGREhGhwhEbGStJthi7ERGEERGLErFhsTK4i7QbE4EYEbG4ERsYK4haRVIRE0kREWkUUZExkiYEJBQRgRMREUQRERYj"
    "WLQishsxeBERSxSxYbGCt4sQuxExGBGxSBEbFitUqkAyEREpERFqFKGRoZIlRCIUETEVERFGEREXI1ODAhIREXAREXECERAhICgs"
    "YBEREQIRESQQDBEMQswMzAERwBARwQLBEMEgzGuyGxERFxERJxEbEStimymQEROQEBGRApEQMSApgwITEQEXERGnEAERA0LELcAc"
    "McEQEdECwRDBIMRrIrsRERcREScRGxErYrsgIBERABARsQKxELEgJHMCExEBFxERJxABEQNysyixGxFRERFxE7ERsSK0TGAREQEE"
    "G8EpEQwRDES8RBYREYGBEToUMRGxgmQsAhYQEwsRwSMRAxEMK7wEYhERMYEROhIxEbFCZCskFhERGRGhRBETESspxiRgEREBEBEx"
    "EjEQwSDEKhQREaGqoRGioRFaInqTohGREZmREZESGpEoMjkmGxERQRQRESKhEbommpuZkZERmZkZkRkZkSmSOZYbERGRuZERkrER"
    "OSs5u7gRsRGIgRGBEhuBq4K4mRIZEZEZmRGZEREZIpkiKhEREUoRGiEaEaEhoiRyEhERkRgZEScRERgicyKIgRERiIEYgRgRgSGC"
    "KKpCGjGhqqoRqKERaiJ6IysbERM7GxshGxqxK7K0QrIREZEaGREpoRG5IpMrmpoRE6maGUEZGpEqkqmRKRkxkRmZEZURERkiSUIk"
    "FVEReRERQRQakSiSR0ayERGhFBERsqEROiY6yyQcwRGUERGBEhyBzDLJS7QRG4EYERFEsRG4KogrRRIREXkREVEUEYExkicGJBsR"
    "kRMREUOhEbkmeZsaGZkxmpoZkRQakSlCObbBERGxxqERosEROiw6vBgbsTGDgRGBEhuBqzK4u7yRG7ObkREimBEYMYmBGIEUEYEh"
    "QihqG6sxs7qqEUSxEboilLsEsbERgxERQRQbgatCuJkikRGRGZkRmBERGSJ5MyoTEREqERohGhGhQaI0QzIREZEUGREkEREYJEMz"
    "iBMYEYiBGEEYEYFBgjiiKqERoRqqEaURERojSiKSMhkTSZEZQRkRkUGSNQQkExGRFBERQxERGCRZRJYxgTGJgRhBFBGBQUI4SVIS"
    "YZEZGRFJkRGJIpkjJBUREUkREUEUGpEmklWCsjGxkRgYESixEbgigytIGhEaiIERgRkagSqCmHIkERFxFhgRRhERGCJ3MkRSERGp"
    "kRFhFBqRJpJWQiQbsTEXEREkoRG2JnO7CKEROoOBEYESGoEqMphCJBMRcRYYEUYRERgiZ0MqFREVqhoagRoaoSiiOqbCMRGhyqER"
    "osEROiw6vBQbERt0ERFBFBuRu5K3S7sRsqG7sREilRIREXkRGVEZEZEhkicLKhsRo7OhEaKxEboiqrtEshETORERIRQbkbOSs0wc"
    "HMGxrKERQpAyERF5ERkhGRGRQZI3RiQaEZoXERFEoRGpInmaKRoZGJEaGZETEREpIjlGshEREaWhEaIRETEmMssnHBEccREhESKX"
    "EhkRcREYcRMRESEiKFIpmxETqpERNBERsSKSyyjCERxxERFBExERzCLHUykTGREXgRE0ERERJIIjJ7IRsWEREWESERErIreiQqER"
    "EaoaEaahEUEiKmMrHBERsRsRIRscEdOywiayERERGhMRI6ERsSYja5IbGRGRmxFBGRsRu5KyIiqhEREaGhGjERERIioiolIaM6Gr"
    "EUEaGhElolImKxsRERoREUOhEbEmKmuywUERs1wRQRQcEcxCwiMqExoRGhoRoxERESQqI7JSERGRMRExFBoRJZJUJiocERHKERGi"
    "wRHBLCpsorJBEREaEUETERFrIrJHIhEREXcYEUcRERgidzIkExERkRQRQRsRgTGSNHMyERERdxcRJxERGCNzM0YTERFxdxFxFBFx"
    "MXI2ciQRMRGZGRFHEREZI5kyRDIREXGIEVEUEYFBcjRDJBMREXcREUURERgjd0QGMRERM3cRcRQRcTFCNkIkERERSRkRRRERGSNJ"
    "IicyERExZhFhFBFhITI3RCQTERFYERFEEREWI2gzJjIREREXEVETERExIjZDJBMREZETEUMREREjSbOwERARrBsLIQsRECPAswQb"
    "ERshGxEQLBABsQK729ARERFCAQEhAtEQ3dDUhBkRGkGngRGEERGhInTKyxERHJQcGyEbwRHMwsmmEAERoRehEaYQARECerILERAb"
    "LAEbIQtBELHAJFuwEREhFBARLBALEQu7koUaERqEERhRGKERoUIosGABEbGqsRCyEAFRAltkCxwQES0cCyEMERDD0MMMwBERIRMR"
    "ECIQAcEMzLyAGxERNBsYIRgREbtCtwIKARGhGaERohABEQNKIgsbsAGrEQEhCxEQK7C63dABEdMUEBDSEA3RDU3dAt3RMdIBASEC"
    "0RDdMN0swxwcwRERFCErCwsRsBoQEbIQCxELq8LAwBAxPBEBIQwREMzAyjSRERoBp4ERhBERoSJ0yhMcERxQHBshG8ERzMLJWRMa"
    "oQEYgRGFERoRKoSyMLERETAbGEEYERG7QrfcEMwcwRERFCGgIBEQoRKhEaIQARECWsMLNgEcLQEbIQzBEMHQzAzAEREhEhARIhAM"
    "EQzMvIAbERskERghGLERsUK4BgsRoLESsBGyEAYRBEtCokABESoRGiEKERBBoEUMDBwQwxIQEcIQDBEMzLwwsREbIBEYgRixEbFC"
    "uLRQE1ChGhEQthABYQKqYwscARFLEQEhDBEQw7DDTMARwSEcERAtEAHBDMO8NBsRG0MREUEUERG7MrVSChEQkRQREaQQARECWTKw"
    "sAEROxEBwQsRECuwuywCHMDDFBEQJBABwQxcvDSxERtDERFBFBERuzK1BAoUAZEWERGlEAERBFkjKRoREzQaGSEZEREjQqO8wiER"
    "TMOxEcIRHMEsrKySQhGhdBEZIRlBEaFCJyspGxFBs5ERkhERMSs0yzTCHBwUEUERspKyEbEkERkhGbERsUK3KyMbETMTERFEEREx"
    "KzR7CRsRG7EbC0ELERArILtN0BERHdTQEEIQDdENQr2WsBGxYQEbUQuxELEgJAwJHBERzcEQwhABwQxCzJDAEcEhARwhDMEQwSDE"
    "3AIcERwcERDUEAHBDMIsCxsBEcEbASEbERAkwLLd0BEQHd4QENIRDdENLt0CHRERIQEBIRLREN3Q0izLEREczBERshEcwSwkvLIQ"
    "AbHBAREhGzEQscAiJgwcMBHNERCyEQHBDC3c0tABEdMBASEd0RDd0NIsHBwRHMAREUkRHMEsJMzCYBHA0QERIRsxEMHQwiYMHAER"
    "HREQsxEBwQwrzLLCERHMHBEhG8ERzELCLQQdEdHREBBCEQ3RDSJNChIBEZEXEVEKERAhkCe8YBEQESsQEcIQDBEMTcwNzBERIQwR"
    "0QLBEMEwzGuzGxERcxERRxEbEStjW7VQAROhBBFhC1EQMaAmBAoUEBE5ERGkEAERA1nEIsAMMcMMEcEAwRDBwMxrELsREXAREUcR"
    "GxErU7uwMBEQsQIRYQuxELGwJUQCFAERaBERJhABEQNYszKyGxExFBFBFLERsUK13ALMERHBEBHdEAwRDELMwmAREBELEUEbMRDB"
    "sMKUUhFREXSaEZQREYEidGMqGxERQakaIRoREWOys1IpERERdIERlRERESJ0IpCyETFBoxlBGRERK0K3TCQcwREXERQhBCkUERF0"
    "gRGVERERJHRDRLIRsTEUEUEUERErMrUmKhsREbEaEaMREbEmJEtEQhEREXMRURQRETFCNgQbERuQFbEQK7EBsQLL29AREREkAQEt"
    "Et0Q3UDTSBkRGoEVgRFIoRGhIljKxBERESsRESwczBHMssPMzBEcwRzBwSzBHMEsw7wLGxEBSQEbuxKxELGwLHmkERGBFIGhRxEa"
    "ESpIwgIcEQEJEQzBEhwQzMDNKrQREYEQgRFIsRGxK4hLAhuwEVsRATESGxArsLUt0AER0RIQ0CLRDdENPd0O3dER5AEB7RLdEN1A"
    "3szCHBzBHBHBLMEcwSzMzADAEBEMEQFBEhwQzMDDCJIRGjEVgREooRGhIljKEBwRESMRESwSzBHMssO8whwcMRzBwSvBHMEsw8wM"
    "zMERzBEczBLMEcwyzHkgGhExFIGhJxEaESpIogKyEREDERiBFBsRu4K4zBDMHDEcEcEswRzBLMzMAxwRAQkBHMwSwRDBwM07pBER"
    "gRCBsUcRGxErSLsDohERAxEYexSxEbGCtEzAIcGALcEQLMEBwQzcvCcbERt3EBdxEhsRu3K3TAIcwME9ERAjwQHBDNy8B7IRG3MT"
    "F1ESGxG7crdEohGhkRUREUShEaEiWcokHBERKxERLBzMEcyyw9zBHMFBHBHBLcEcwSzDzEzMMRzMERHMHcwRzMLMWiQaEZEUEaFF"
    "ERoRKjmyQLIRETkREUEUGxG7krMsIszBwRcRwULBHMEsfMwMwRwcwxERzBTMEcxCzDskGxGREBGxRxEbEStJuySyEbGIExFBEhsR"
    "u4K4JrARsQEWERAkEQGxAlLbAh0RESEBASES0RDdINPd0B3QAR4QEC0RDdEN4s0szBEcwRERwRLBEcwizLsCGxEBFxARKxELEQtC"
    "wiLAEREAEQFBHBEQzCDN7QLd0REeEBAuEQ3RDeLNK8IcHMEREbESwRHMIszcAhwRARAQEcQRDBEMQswswBHB0BMBQRIREMwgzdwS"
    "zBEcHBERPREcwSzC3AQdERFAAQ1NEt0Q3UDSTMwREREbscFJwRzBLCvMBMIREcERGlwSzBHMQsIsLBwRERsRwUvBHMEsK9wk0BER"
    "AQEBQRLREN0g0szAHBEBQ8DBLBEMEQzcvCe7ERExFBZ7ErERsTK0eyK7ERFDYbEnERsRK0O7J7IbETEUEXsUsRGxMrPMAswREdAQ"
    "ESwRDBEMQnwkFREVkRUZcRQaESWSR0ayERERKhERsqERMSY6yyQcERyRGRZREhwRzDLJi7QRsRGIgRGYsRGxKogrRRIREZEUFmEU"
    "ERExkiQmJBsREQmREUShEbEmactFwhEckRkRMRIcEcySyWsQG7Exg2ERRrERsSqES0MyERGREBZBFBERQZJDQiIbERtnYxFGoRGx"
    "JGe7RLIRsZEXEUEaGxG7krdcIhwRHHERETQREcEsgmyyshERERoTMRQaEWuiskMiExERMZQRRRERESNDNAsbARuxFQFBGxEQK7C1"
    "3dAREBEtARDSEQ3RDT3dDt0RHSEeAOES0RDdMN7MwxzBEcQREUwRHMEsw7y1sAERsRQQURuxELGwIwwMHBARDBEQxBEBwQw83CLQ"
    "DR3THgDRENEQ3dDezBPMwRHAERFMERzBLMPMw8AREMEQENEcwRDBwMQ8AhwBHNwTECQRAcEM3MwywhzBQRQRIRTBEcxCxe0C3REd"
    "4QEQ3hEN0Q3i3dLQERARHQDRHdEQ3dDS3ALMARHBBBEtEQwRDDzMQ8IRwRF0EUEUERHMQsgoEhERkReREScRERkic7JiERERJBER"
    "KhKxMbGiY4gSGBGBGIEYiBERGCKIsmkRERspEZErGbGRsZK0i7sREYEYgbEoERsYK4iqKRkREZkRGZoSgZExMimZmRmRkRmRmZkR"
    "GRkjmTIkExERSREZQRIRkTEyRIM4GBGBGIEYiBERGCNIxCnMERFEERGcEsFhwTLFi7QbEYEXEbGYERsYK3iKGRIYEYkRiIEUEYEh"
    "Qii8YRHBsRIRxKIRHBUsSrwYuxERgxEYixKxgbEyqJu5m7GREZkRkqmRGRqaEZmaFJGRMUIpgxmEEZEWgRhIEREYI0S0CLEbEWMR"
    "EYsUsWGxQqarIhERkRSRoSkRGxkrk6KaEhkaqROZmhmhkaGSJKsaGrqhNqG6SREbGitEMicTExF4ERhxEhFhMTI3czQREXEWcRFG"
    "EREXI3czB0EREWMRF2ESEXExMjeUIhERkRcRGZQRERkiecIqFhPBKjGhLBrBocGixKuxGxFBFBOxShEbGStZu5m7IbEZkhkhVSoV"
    "EaoaE6qnERUaI6oykEIRETkRkSEZEZExkkRLJLsRkRcTsaIRGxkrecvJwRzBFqMaIQsqGxGrMhO6ohEbGitKMkRCERFIERFBFBFh"
    "MYI1lyKREREXERk4ERERInKyJBYRoSERQSEasRGxImR8whwRERcREiGaKZgRGhkRGTkRGREjkjKXMhkRURGRgRMRETEiRHwizBER"
    "FREROBEcESxCvJoikaFRE5GRE7ERsSIkcyITEREWEREmERERI1KzJBYREaERGpoSsRGxMmKbaZERGxmRsZMRExErKcu2YaHBsREa"
    "PBQxEcFCwiwqFhHBGhPBohETESwqvKVioREaERExEzERsSJiiyIbERF3gbEnERsYK4OiSKIREYEYGIoZoYGRgiiKEKQREYOBoSgR"
    "Ghgpg6JGQhoRkTQRahqhkaGSJmsishERcRERJxEbEStikikSGRGRGZmRGRGRIZIpozIRERFKERqiEREaIzozJxMREZEXGXESEYEx"
    "MjeDOIERERiIETKZMhkxkRmZQRkRkSGSNEMkExERWRERRBERFiRpNJgxgRGDgRQhciQTERFngRFGEREYInczlDIREXGRGSGUIhER"
    "EWQRFpQREREiVLIqFhGhQRKhIRqxEbGyZVUpFREadBEZlxEVESN0QpBCERFBE2FBGRERMUJFzCTMEREVERQhCykSERokExmWERsR"
    "K1RCREIREUEUEUEUERExMjUrKhYRobEREaQRExErJLZGQhsREUMRYRSxEbEyJZzCHBERSRHIKREcESyDvEe7ERGBFIF7FLERsYKk"
    "exC7ERFDEbhHERsRK0S6R7IbEZEUE3sUsRGxkrN8IswREYERETcRHBEsQqxAERARqxqhIAoRECWwNQkZERkgGREBKRABkQKSY8AR"
    "ERFCEREgAhEQzCDJiBkYGEGIERiIERGBIoRjuBEREXQagSEYERFjQrOZEAkRoBkRGZkQARECmiIJERARSxGRIQkRECGwJIIQEREg"
    "FxERJxABEQKCInZxERF0EXFxFxERIUImqkAKEbCqEQqmEAFhAlsjCxsQEUwasSALERArwLRCsBERIBwRASsQAbECsiuJiRERlBqB"
    "QRgRESpCqJEJCRGgGREZlRABEQJKYgoVYBGrERFgChEQJbBKtsABEbEUEQHCEAExDTwslBAQEVoREVEJERAxoCQGCwsRsRwRAbMQ"
    "AbECS4sjGYgRgBiBgRgRESgiSDaxEREBpREYghERMSYyKzYSFxFwEXFxFxERMSImUiOJEQGpERiEERGhJoKqCRIJAaoRkWEJERAh"
    "oDWTMBEQsRIRGZIQAREESzMEExEBQhERQQIREEEgNGM3cRFBFxEXdBERESRkI5kQCQGbEZFRCREQMbAkIgmTEKAUERmUEAERBFpD"
    "kDABEUoRETEJERBBoEVUIxMXARcRF3QREREkYqQKJQEGqiERoAoRECigOrJQERCxFBMBtBABYQJbJQsbEwuyMRGwAhEQKyC7YpMR"
    "oWERERMhcgkREJAXExGXEAERAnkitFABAQsxEWALERAmsFZyAhywsRsTAScQAbEMu6wGkhEaFhEhESKUMBEAeTERYQkRECGQNpRS"
    "EWFBlxEZlBERUSJ0ZCkbExFEGpEhGRERY7KzQigREUEVERaFERERI0QikKIRETQakSEZERFqQqQEKBMRQRURFoIREREkVLQ0ohGh"
    "FBFBEZIJGRkZkBmTkAkRECkgOUbAERERyRELQhABMQwyLIgQGBGAEYOBCBEQISAowgm7EQG8MQu0EAGxArI7iTCBEYARg0EIERBB"
    "IDiiQKEQAasRAaQRAUECK2MLHAER0RwRIBsRENPQwibAERExEhEBIxEBwQwibIIbGBGxGhFBGBERa0KyIgmREAEbERGTEQERAisi"
    "slALE7AaEUAbERAlsFImDRwQERwRAbMRAcEMK2yyskERARoRQRQREWsisiMJEwkBGxERkxEBEQQrI8JQERDDMREwGxEQJbBUJikb"
    "ERGsERGSERGxJiRrQsBBEQEcE0AUERDMIMKmIBEQAXkREacQARECaTIKEwERoRQRQQoREDHANGMwEREBchERJxABEQNiMzYTEREx"
    "FRFRExERMTI2kgkRMAGaERGZEAERA5oypDABEZAXEVEKERAhkDRDABMQEVkREQUQAREDSUQ2MhERMRURURMRETEiRkIJEQEBShER"
    "lRABEQNKIiYwERBwNxFxAhEQQXA2RCMTERFTERFEERERI0Q0JjAREQEXE3EJERAxIDZDDBMBEaMREaMRAREDSpMpFRkWQZmRkRkR"
    "EShCOZJSERERdBoZkhERMSI0JXYSGBFBF4FxFxERIUImoimVEROkGhmUERFhImQldjKBEUEXgUEXEREhQjZiIxoRGhYREyEiKRUZ"
    "EbEaEZQREVEiJCU2MhEREVMRURMRESFCNqZQEWARohoKphABUQKiYwscAREhxLEgDBEQwyDDQgkREBFSERaVEAERAkLCsLABEUG8"
    "sSALERArILQECRMBEaQRFpIQAREEUmPCwBEQEcIRIBsREMwgwkQKEwERIRURpRABEQNCUwoVARWwFRFAGhEQJbBFtsAREBErEQGy"
    "EQExDDzMBBwRHCAdEcASERDMIM1rsxGxEXQRETcREbErYyuXEAERoBURcRkREDGgJCYLGxABCxEBtBEBsQJrywXAARzRHREwEhEQ"
    "zNDNWyMbsRFwERE3ERGxK2JLlDAREKAQEUEZERBBoENCAhwBC2szAQQRAbEMa7w0shGxQRURQRQREbtCtMwCHBEc0BEDxBEBwQzS"
    "bNLAERAxHBEwGxEQ3LDClyKRERF0EZGXERERInQyKRMREUEUESYZERExQjNiKIEREXQRgYcREREjZCKXMhkRQRcRSRkREUFCNGMo"
    "EhgRdBOBhBERESNEQkYyERFRETERMpQygRERGxExGRERMUIyDBYBEcHCEQzBwAEcAiwmUBExETsRERACG7AhsFsNHRER0RIQAc3T"
    "ARMNLS1ZETERORqRERkakSGSWcwcEcEhHBwcIaQQARGhFREaoRABGgIqYgwREBwsAcHBDBHAYcAiJUAREbEUEBErEAEUBStSkhQR"
    "GkkRkaEZEZFRkiKyQAERsVsRW7FQARsCK3QMHRARLB3BEAwdwHHA0ibAERGxExEBIcABFAYrvJIbERE5G5ERGRuRYZKyIgoBEaET"
    "ERqhEAEaAyoiDBUwETsRERAMG7AhsFrt0AER0RIQAe3TARMNLV3CQBARSwERsQwRsFGwIiYMDBGxExEBwcABGwYsLBsVMRE7G6ER"
    "GhuhIaJZzdER0SEdHBwhJRsUsbEUERqrEREUJSrCssERETsboREaHEFhosKiIBEQoRIRGqEQARoCKtMMNwEdLAHB0QwRwNHAcixg"
    "ERGxEhARLBABFAwrtpIbERspEZGxGRGRsZJiJAsRULESFRu1EAEbBCsyojABESoRoREKEaBBoEIsDBYQsRIQEcwQARsMLMaywREb"
    "KxGhwRoRQcGiYrJAEzChExFBsUABGgKqYwwcARErEREQPBwwYbDCIgsREJEUERGxEAEZAkkiwlABETsRERAMG7AhsFIjCxMBkRMR"
    "EbEQARkEKSMqFRETOhujERoboSGiWs3SIREtHRwcISUqFBGrFDEaqxERGiUqwqLCERE6G6IRGhyhYaLCLCocEasSIRqsEREaLCpG"
    "CxURFmEbsRALERAhIFRFCxURGxMQE7ETAREFIsKywBERMRwxEDsREGEgwiwLHBEcEhATsRMBEQwiJgwWARHBHBEQHBwQIcBi7uAR"
    "EB7uEAHu4QERDi7eAh0REdEBEdASHRDR0NIszBERHMwREczBEREsLGzCEAHBwQERwRwREGHAIicMHTAR3BEBwdEBEQcs3eLQARHR"
    "ARHQHh0Q0dDSLRwdER3cERHN0RERLSzdwnAR0MEBEdEcERDRwHImDBwBERsRAcHBAREGK9zC0hERzR0S0RwdEdHC0i0NHRHR0RAB"
    "0REBEQ0iLQsSARGRExERCxGQIZAizGAREBErEBHMEwETDCzGAswREUEEEcECEUDBQGIrZBsREUgREboRERgrKEayQAEToUQRQQsR"
    "oDGgIiMLExARORERsRABGQMpxCLADDGxBBHBAhGwwcBiKxC2ERFDEREqEREUKyhWwiARELECEbEMEbBRsCIjAhMBETgRESEQARYD"
    "SLNCYhsRkRQToRwRkbGSYiwCzBERMRARIRMBEQwixsJgERARCxHBHBEQwbBiokIRMRE6FBqhQREaIqpzKxwRESG7sREiohIREaEU"
    "oREaEaEhoiQiKhUREzobGqGhERoiKjWiMhERoROhERoRoUGiMicrHBERsRsRsbEREScrLAsVARPBs7EQDBEQIcBUJQsUEBs8ARPB"
    "EwERBSzSssABEdHDMRA8ERBh0MIsCx0BHC0BE8ETAREMLdbS0BEQHd4QEB0RENHg0sJQETARKxEBwbEBEQIr1Q4dARHREhDQHh0Q"
    "0dDSLdAd0REkAQEt0QERDSTNQswRHLESEcEcHBHBssIlDBQQESsBEcsRAREFK2LCwAERsRMREBwcEGHAwi0C3dARLQEBLdEBEQ0t"
    "zQLBHBwxGRHBEhwRwbLCLAwWARELARHMEQERDCzGIsARMLEyERASHBBhsMIsJMwRHKsRE8zBEREsLNwi0B3RERIQEBIRENEg0qIi"
    "EREROhGhoRERGiIqwisXEbEhEbsRQqJCEUGhFBFKGhGhMaIiIyoTERE6EaGhEREaIypUoiIRsaEyEaoaEaFRoiKkQBFAEVsUoaQQ"
    "ARECSyOUEAERsRMREzkRECGwJCILFRAROxoxsBMBEQIrNZIwERCxFBETOREQMbAyCxsRGyAbsQGxsBG7ArLb0BEREUIBETAN0zHd"
    "IN0oGREaMRiBEYGhEagiiMrCERERQxERwRLMscyyyczMERwhHMERzMEczCzCnCgaERGDERihGKGBoYIo3MARESAdwQHBwBHMDMKs"
    "KxsREbMRGBEYG4G7grjt0B0dIR0QAeMwHdMN0s0szMERwxERwRzMwczCzCiSERoxGIERgaERqCKCyhIcERFDERHBEsxBzCLJzMIc"
    "HCEcsRHMwRzMLMLMLMzBEcwRHMEczMHMIsyJIhoRMRiBEYoRGhgqgqIqshERoxEYERgbQbsiuMwSzBwxGhERzMEcyyyivCsaERGz"
    "ERixGLGBsYK4qyIaETEagRGLERsUK4LLDBwSDMIRHBAMHMHMIMx7shGxMRdxEXGxEbcrd7snshEbcxEXERcbcbsitySiEaExFxER"
    "QaERqSJ5yiIcERFDERHBEsyxzLLE3MEcwSEcERHTMRzDLMLMLMwxHMMREcEczMHMwsxaIhoRMRQREYoRGhkqebIgshERMxERERIb"
    "kbuStywizMExFxERLMEcxix0zCzBHByjERHBHMzBzCLKCyIbETESERErERsZK3m7JLIRsYMREREZG2G7QrgrsBGxARsRAbEQEbEC"
    "stsCHRERQQEREALREd0g3d3QHdABHRAB0RAd0Q3SzSzMERzBERERHMERzCLMuwIbEQEbEBGxEBsRC7LCLcAREdARERAMERHMIMzt"
    "At3RER0QAeEQHdEN0s0swhwcwRERERzBEcwizNwCHBEBHRARwRAcEQzCzCzAEcHAEREQDBERzCDM3BLMERwcERHRERzBLMLMRBwR"
    "EbERGsEZzBHMQsJMIhwRERMxEZMxHMEsIswiwhERsRERwRLMEcxCwk0CHREREBAB0REd0Q0ivSi7ERExGTixGLGBsZK5iyK7ERGT"
    "gRKLERsYK5K7KLIbETEUMbEYsWGxQrnMAswREcAQEsEQHBEMwswoHBEcIRkYERgcgcwiyYuyEbERg4ERgbERuCqIuyixERszGBgR"
    "GBuBqyK4SyIbERtzERFBsRG5K3nLJ8IRwREXEREXERHMIscksBGwEbIRAcEQEbECstsCHQERIRQQEALREd0g1O3QHdER0gEB4RAd"
    "0Q3SzSzMERwhHBERHMERzCLMWwIbEBFCARHBEBsRC7LCIMABEcETERACERHMIMwtAt3QMdIBASEQHdEN0s0swRwcMRoRERzBEcwi"
    "ygwCHAERLAERIRAcEQzCzCTAEcDBHBEQDRERzCDMLCLMERxCEREhERzBLFLcLtAd0REdEBAO0RHdIN28whzBEcMRG7zBHMEsxMwq"
    "whwcMRwxMRrDEcwizNwizBEcwxER3MEcwSzEvCi7ERExGIGxGLGBsYKoixK7ERGDERiLERsYK4K6KbIbETEUE7EZsZGxkrUpkBGQ"
    "AZIREZAQEZECkmMCHAERIRQRAQIREcwgyczAEcEBwhERwBARwQzCvCYbERshFxERFxERuyK2ggIREAFyERFxEBERAoIiJLABESAc"
    "EQELERErILtMAhzAEcIREdAQEcEMwrwmshEbMRcRERcREbsitkMCEwEBShERQRAREQRCIyuwEbAgGxMBCxERKyC7SyIbERtSERFB"
    "ERGxK0IrAhUBE7ETEQEBG7EhsFst0BEQES0BEdDTERMNLd0C3REdIR0QAQMTMdEg2izCHMERkxERwcERGyxbXCJAARGxFBARCxFB"
    "UbAiJgIcEBE7EREQwBEUBivcItANHUEUEAENHdHRQNQsEszBEZMREcHBERQsUswiYBEQsRIQEQwRQcGwYiwCHAETNBEREMARGwaU"
    "zCLCHMExFDERHByxwbLEmRoZGpGlkRGRqRGhIlnKzBERHCwcEcHCzBHMwsLMzBEcwRzBEczCHMEszKyZmhEaSREZoZmhEaGSJCu5"
    "GRGRsJERkbkRsSspy9zMwRHMERHB3MwRzMLMqbGhG6G1oRGRtBGxIlTbHR0RHS0dEdHC3RHdwtK8wRwcwRyxEbzCHMEsw8zMzMwc"
    "zBHBIZsaq7GhFKERmxQbEStEwqLBGhEKHBoRSRwRzELCzBDMHMEcERHMxBzBLMS8kxsZGwkRGbGZsRGxkrI8GhzKoRChEZwUHBEs"
    "JLy3GxEbiBEYEbcbEauCuHsQG7GRGGERUbIRsSqDSyobERtaGxERpBsRK6K13dIxEd3SEREt3R3RLS3NHMwRHMQREcFMzBHMwszM"
    "zBzCzBwRHMKgwhERChwREaIcEcyiwiwtzMHBGhERLM0cwSys3N3RHd3dEdEhSyQbEZsXERFRtBGxK3lrKRoZGmEaERE0EREqIqVM"
    "whERHMIRESEbHMEsIswszBEc0REREqKXohmhQRERETehEaEiJCspmxERsBERQRMRsSsiyyzCHBzRERERPMERzCLNOykbGRsQERFB"
    "ExsRKyK7JbIRsYEREREkERG7IreszMERHMzBE6zMHMEsLNzV0cHR0R080UXdEd1C0nu0GxERSIERexQbEStIugexGxExFBaxJ7ER"
    "sTKkm7mRsRG5IRnCxsGhHJMcQyFMKhwRHMoxGjIpGhEaQaUREZQRESpCpbzCEREcJBwRIRscwSwsrJWiEaFBFBERlaERoUIkCykb"
    "ERE0GxFBGRGxKyTLTcIcHNERERSyk7IRsUEQERGXsRGxQrI7JBsRG0QREUEUEbErU8vazBEcwRLBwdrMEczCwqwQzMERTBEWrMIc"
    "wSwjvKq7GrFBEaEhrBrMGhwUERRSKRUZFUGVERGUERElQkWWshERESQaESEZETEmJCuGEhYRQRQREYURETFCJCIpmhERBBoRQRkR"
    "oSYkSoMyYRFBEBERhBERQUJCSyQaERoWETEholIRMREqGxERuhERIirVLB0R0SEdERxSokIRsaESERGrERFRoiIsKhwRETobERHK"
    "EREmKsyiwhGxoRAREawREcGiYrzEHMERzBEbwcscwRzEzCrBHBwxHDERo8MRzCHMdxIXEUEWERdhFxERInSSiIgRGIQRgRGIgREx"
    "QihzNxcRQRYRF2EXEREjRHQjEhcRYBFxEXYRESEiN4kjiIEBGBEYgRgYESSCMjdCFxFQEXERdhERQUJEmSiBoUEZERiBGBoRKkSS"
    "OSKBGlARgRGIoRGhImSIIIERARgRGIEIARECgpKZkBmRkAGREZmQEDEgKYMIgxEBGREYgQgBEQNCtJwgsbHAAbERu7AQsSAke2iB"
    "ERoUERExGBMRKyS2NWKBoQERERFDMRGxImJpIxoRERYREyFpIKoRERYREiGVIBEQEXIRGEEJARECclKlUAGhIRWgMaVQEDEgJAMJ"
    "FBARkBEYIQkBEQNAtLDAEbABErAxsrAQsSAkW7MbERFEERFBExsRK0O7NbIbETEUERE0sRGxIrR4IIEQAXIRgXEHARECgjJ4MAgR"
    "IBcRGHQQECFANJQgERARSxGREQkBEQIrQqRAAUGxExQToRMQMbAiIwkTEBE7ETEROQERAytTsiARoLESEBOxExBRsCJ7shsREWIR"
    "EREWGxErcrsnshsRMRYREWGxEbEitwsUARGxshELIQsLGwI7Q1ARYQFaERsQKrCwIbA6lBUZFpGVkRmRGRkZIpljCqEQAWoRqhGq"
    "EKAhoCqiEAERoRIRGiEKARoCOiIFEREBSREaESoQoCGgKVIYGBGBFIEYgRgRGCKIsgu0EAG7FbsVu1CwIbA7ssABEcHCEQshCwsc"
    "AjwsBBwRATsRGxAswLAhsMyiqRkRMRoZGSGhCgoRoBqhGqEKARoCqiILFVARWxERELCgsCGwSiIKARGhFBERAQoBGgOaYsDAEBE7"
    "EREQwMCwIbDDpFKRFqGlkRmRGRoZIpkklRIYEUkRiBGIEYExgiiqK6sRMRsaGiGrIKEQoBuhGqEKARoCqjMKFAERKhGhEaIQoEGg"
    "MzMwERGQFKERQQIBGgRKM4MUGBGIEYgRiBGBQYIzogqhEKAaoRqxCgEaA7oiojAKASoRqhGkEKAxoDUEChMQoRQRETEKARoEOkST"
    "QoERiRGIEYgRgUGCQ7RAEkCgFaFBoQsEGgKqI6UQAQFJERkRqRCQIZApQgsVELATsQGhCwobArslozARAEkRGRGpEJAhkDmiUhFR"
    "oaURGgEaGhoiqiSSEhERSRGREZARkTGSJwsqGxExGxoaIQQpFBGRFxEZARkRGSQ5ZAoVGxVgG7EQtBAQISA6QgmhEQEUERpBCgER"
    "AkIylEChEUARoRGkEBAxIDOyQLEQAbuxASEbCxECK2MMHQERwR0REMLRENHA0ibAERExGzEBIRIMEQwrbJIbGRGRGxkRkrERsZKy"
    "IgqhEAEaoREhGgERAioislALE7AbGxCysRAhsFImDRwQERwRATEdDBEMK2yywkERsRwaEaKxEcGiwiMKFAoBGqERURoBEQQqQwsS"
    "ARGQFRkRuRCQIZApUgoRMAFKoRGhCgEaA6oytTABEZATGRG5EJAhkDkyChEBAUqhEVEKARoDSkIqFBoUoUWqIapBoSGiOlIpkRER"
    "SZEZkxkRGSKZIqtSGjExqqERIpMykRGRGZkxmRGRMZIzs1ARUBFbGgsBCwERAqskoxABEaEUoRGgEBAxoCQ0ChQBEVoRGgEKARED"
    "OkQLFQEVsBKxELKhECGwQNbAERARKxEBIR0MEQw8zAQcERyQEsEQIsEQwcDAi7QRsREoERghFBsRKwgrpRABEaASoRGiERAxoCAm"
    "DBwQAQsRCyEcDBECO8wCwAEcwRIRECDBEMHAw4sgG7ERgxEYIRQbESsISyoSGhGhFaEaqhGhIaIqUimRERFJEZmRGREZI5kyqjIa"
    "ETERqhFSCxQLFLBFEUu0EBAhsCBCCqEQAVoRoUEKAREBSkILFAEVsbQRC7GwsCGwMkIKERARShGhEQoBGgIqQgIVARawFbEBobCx"
    "IaA6JsAREBErEREQMhwTDDvMDBwRHCAcQQHRwEHBIM2LshGxEYMRGBEbGxgruCslEAERoBShEaEQoSGQKkICHBABOxEbEAwcGwLL"
    "zCLAARzBFBEBAcDBwZDNiyIbsRGDERgRGhsUK6JLKhUaFaGioREhqhEhokBSKZERESkRGRGSEREjCbKishoRARsRGlIJEgkRsBQR"
    "GUEJECGwKVIJkRABWhGREZoAEQNKkqSRoRGUGpkRmaGRKpGpTBwRwUHGERwsHMzMHKzMzBzBEcIRHMEswbzMMcyapBmhQRmRGZoZ"
    "oRkambJLmxERtBuZEZmxkbuRuczCHBwxHBERzBDMzBzMnBMbuRGTG5kRObGRKzG5PdER0THWERotHd3aHb3NHMzBEcIRHMEswbzM"
    "MczMzMzMwRwcHME8wRoRwxyqETnBocwxycwQzBwxHBERzBPMyxzDvEsbGRu0EZmxmRGbsZG5i7IRsTEYgRGBEBu4Goi7CLERG4MR"
    "GBEIsYGrMbjMxBzCzBwcHNE90R3d3LHRESiSgaERqBEYgRMRoSKCyiIcETFREzERMhEczCLJzMIcwREcESEhiSKKERoYERiBE6ER"
    "KoKyKrIUEaEbgRE4ERG7IrjMIszBERoREcETwcEsoszGHBvBwRwbwcrBHMzBwouyGxERg4ERixCxGBuIukkbGRuRm5ERnBMcyTHJ"
    "GRNBKBkRGiGngRE0EREqAqdMwhERHFIcGiEcwcEsUJyFohGhIRiBETURGqECKAsoGxERNBsYQRMRsStwyy3CHByxERETwkzMERxB"
    "HMHBDMEczMHMm7SbERsZkSwhKhURE6GzERqxsaEhslnN0hERHdJBwSElKhQRG0oRobEbERQlK8KiwhERobMRGrHBQWGywszDHMER"
    "wxEcwRzMzBzMzMzMHNzBHBEdlFKRYRFZGpkRqaEZIpokhRIYEYEUgRiBGYExkiiqKqsREbOhsSGIGRgaQaUREYEYEaEiVJqHihEa"
    "RBEREYcRGqFCJDu4gbFBEBERcRixESskawkbGxtgGxEQtBEBKyC1uwm7EQsXEBGxG7EQC0LCuRwbwcEcERG5ERzMQsKzsBGwEVIb"
    "AUEbEbACUstMzBEcIRExESILFQETwbMRAbERASHAUiULFBAbPAERERsREAUswivMERwxHBERsREczCLMtFCxUAFbGhEQshEQAgsk"
    "pRAKEaATEREhGgExoCCEEhgRgRWBGIEYEYEiiFKVlhEZSRGZkZkRETmSKTopkaGhERkZIZQgoREBFBEaQQoREAJCUqZQG7FgAbER"
    "uhABMSAkK2mRERsZkRE7GRERKylLChIBEaEUERqhEAEqoCJECxQQFEtBsUELMbADK7IouxERMRiBEbsREbiCu6QgoRABWhGqEaIQ"
    "oAIqsiAREBG7E7ETtDABO7AmChIRGqAasQGhAhuwCquSKZGREZmRkRGiCqEQAaoRqhGqEAEaoCqiEAERoRIRGkEKEaABWiIJEREB"
    "mREaESkQARqgKYIYGBGBGIEYgRgRgSGIsguyEAG7E7sTtzABO7AmoQoKEaAaoRphChGgAWpyCxJwEasRERC3oAF7sCpiCgERoRcR"
    "EWEKEaABepIqEpkRqZGRESKYEhgRiRGIEYgRERiCKKsgoRCgG6EacQoRoAFqIqoQCgGqEaoRphABGqAmuiASYKAaoQGhCxqgBqoi"
    "qRABAZkRGRGpEAEZkCmnIhFxoRoaGiFiKRERkRcRGWEZEZEheSKZEBoRkBGhEakQAREgKbIgsRABO7ExURsTEANbIgwVARHBHBEQ"
    "wsEBMcBSIlARETEbMQExEhsQBUslkhUZEZEaGRGUoRFBklIiCqEQARqhEVEaERABSpILEgERkBkZEbkQARmQKaIKESABqqERoQoR"
    "oAGqoioSGhahqqERIpkSGRGRGZkRmRERGZIpYgoREBF6ERphChEQAXqiCxIBFLAWsRC6oQFBsCaqIqERERqhGiEqIBGgAasRGxAK"
    "G7EKqiICFQERsRQRAUGwEWuwVrKwEbEBshEbEAsbsQuyKygaERoxGIERgaERqIKYqiKhQRGmEaEhmSKRkRGpkaEhmRoZGpGpkRmR"
    "GRqRKpnKzBERHKwcwcHCwRzMzMrMzBEcwRzBEcwUzMHMzLybmxERuRuZEZmxEbmbuczNHBzBHBERzB3MwczMzMzMzBzMwcERi7sR"
    "sYEYgRGBGxuBq4jLzMwhzMzBwRGIkoGhEagRGIETEREqgsokHBHBURyhEbIRHMFMyczCHMERHBEhEZu5kbERuRkZISkVERORoxEZ"
    "kaERGZJZzMIRERzCwcERmSKRkRGZkZERABABEQEAEQABABAAQAAAABAREAAQAQEBAhAAIAAAABAREQEQEAEAAAAAAAAAABAQEAEA"
    "ARABEBABIAIAABARAQEAERAgEAABAAIAAAAREAEQAREAEAABAAIAABAAARAQEAIAERAQIAEBAQAQABAAAAQAEBEBAAEQASAQABAA"
    "AAIAABEQABEAAQAREBAgAAIAABABABAAEAAAAQACAAQAEBARIBABEAAAAQAAAAAAEBEBABEQECAAAQAAAAAAABERABAAEQABEQAg"
    "ACAAABABABEAEQAQARADAAIAEAARABEREAAAAQACAAAAEBARAAERAAIAAAAAAAAAAAERAAERACAAAAAAAAAAAAERABERAQABEAAg"
    "AAAAABARAAERAQAQABAAAAIAABARABEREAAAAQAAAAAgEAARABAAEQABEQAiAAAQEBEQABABAQIBEAAgAAAgAAERABEQAQABEAAg"
    "AAAAAAAQAAEBEQIAAhAQABEAAQAREBAgAAIAAhARABAAEQABEQAgAAAAAQARABERAQABEAAgAAAAAAEQAAEBAQAQABAAAAAAEBEB"
    "AAEQASAQABAAAAAAEBAQABEAAQAREBAgADACABAAAQFBAAAQEAESEBEAAAEAAQAAACAQAAEQARAAEAEBAQIAAAAQAAAQAQEBABAA"
    "IAAwAAAREAEQEQEBABAAAAAAAAABAQAQAQEBAhAAAAAAAAARAQEQAREBEBABAAIAAgAREAAQAREBABEAIQAgAgAQEAAQAQEBABAA"
    "IAAAAAIQAAEQEQEBAhAAAAAAACAQAQEQAREBEhABAAIAAwATAQAQAREBABEAQQAwAAIRAQEAERABEBABIAIAAAIBEQAAERAgEAAB"
    "AAIAAAEQAQEQEREAEAABAAIAAAAQAAAQEBAhACAQEQAQERAAEAEBAQIgACAQEQEAERAhEBABAAIAICAAAQEQEREgEAABAAIAACAA"
    "AQAQEBAhACAQEQASERAgEAEBAQIAACAQEQAQEREBEBABAAIAAAABAQEAEQABABEQICAAAAARERAAEAAhAAEQAAAAAAAQAAEQECAA"
    "AAAAEAEAAQERABAAEQACAgAAEBEAEAEQABABAQACACAAEBABAREQABAAAQACAAAAAQEAAQERABAAEQACACAAEQEAEREQIBABAQAC"
    "AAARAAEQIAAAABERARAAAQASABAAAAAAAAEREAABEQAQABEAAgIgABAQERAQAQAQABAAAAIAIBAQEAABEQAQABEAAgIgABABARAB"
    "AQEQEBAAAAAgIBARAQARESAQABEAAgIAABAQAQAQAQEQARAAIAIAABARAQAAEQACAQABAAAAABAREQABEQAQAQEBAgAAAgAQEAAQ"
    "EQACAQABAAAAIAAREQABEQASAQEBAgADAhEAEBAQAwACEBEBEBEBABEQECAAACAAEBEBABERIBAAEQACACAQEBABAAARAAERACIA"
    "AyAQEREBAAERAgERACAAACAQERABEBARIAERACAAAAAQEBABAAERIgASEBEBEAARABERECIAIgACEAEBAAARAAERACIAAAACERAB"
    "EBERAAERACAAAAABARADAAERMgAyAREBEAARABERECQAIwACEQEBEBARAAERACIAAAACEQEBAAERAgICARERABARAAERASAgMAAy"
    "ERERABARABERECMAAwAQARABAAEQABABAQIAAAAQAQEBAAAQAhAAAQAAAAAAAQEBEAARABAAEQAAAgAAAREBAAEQAhABAQAAAAIA"
    "ABAAAQEAAAAAEAEQIAEQIQABEAEAAAAAEAEQABEBAQAREAAAACAAEAEBAQABIRABEAAAAgAAAAERAQARAQABEAEAAAAAEAEBAAEA"
    "ABIAEAAAAAAAEAERABEQABAAEQACAAACABEAAQAAAAIQEAEQAQEgARABIAAAAAIQAQEQEQEAARABIAAAIAAQAQEQEBAgEQABAAIA"
    "AAIRAREAAREAAREBIAAAAAAQABEAAQACEBERABEQABIBAQECAAAAABEQEAEQIQAQABAQEAEQIQAgABEBEAMQIQMgAREBABAAEAAA"
    "EQAAEAEBAgADABABEQEAEQACEAEBAAAgABAAEQAQERAAEAERAgAiAAAAEQAAEQAAEAEBAgAwADABEAAQERAAEAERBAAjAAARAAAB"
    "AQAAIAAQABEAEBEAEBEQAAACAwATAREAEAEAEAEQAQARAAEAARACAAAAEAEBIQAAAAAAABAQEAABAQEAEQABACIgABAQEQAQARAA"
    "EAABAAIgABABECABAQEAEQABAAIAABABAQEAAQAQESAAAAAAABABEQAREAEQAAEAAgAAIgABEQAREAEQAAEAIgAAIgAREAAREQEQ"
    "AAEAAgAAAAAREAABAQAAEAABEBEQAgABAAABEBEQAgACERAAAAEAAQAQABAAAAAhADACAAEQAQARABEAEAAhACACAAAQAQABAAAw"
    "AEABEAAQAQAAAAEQAAEBEQAQABABABIAEAAQAAAAAwABEAEAEQAAAAERAAAAAAAAAAEAEAAAAAAAAAABAAABAAEAAAAAAAAAAAAA"
    "AAABAAAAAAABAAAAAAAQABAAAAEAEAAAAAAAAAABAAEAAAAAAAEAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAQAAAAAAAAAB"
    "AAEAEAAAAAAAEAAQAAABAAAAAAAQABAAAAAAAAAAAAABAAEAEAAAAAAAAAABAAEAEAAQAAAAAACZAJoAoAmgCQDKAAsAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACgAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAkAAAAAEAAAAAAAAAEAAAAAABAAAAAAAAABEAAAAAAAEAEAAAAAAAAAAQABAAAAAACQAJCQAAAAAAAAAAAAAAAAAAAQAQEA"
    "AAAAAAAAEAEAAQAAAAAAAAAQABAAAAEAAAAAABAAAAAAAAAAAAAAEAEBAQAAAAAAAAAAEAEAAQAQAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAEAAAAQ"
    "AAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAQAAAAAAAAEAAAAAAAAAEQAAAAAAAQAAEAAAAAABABAAAAAAAAAAAAAAAAAAAAAAmQkJAA"
    "AAAAABAAAQABAAAAAAAQAQEAAAAAAAAAAAEAAAAAAAAAAAAAARAAEAABAAAAAAAQAQEAAQAAAAAAAAAQAAEAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAR"
    "AAAAAQAAEAAAEIARAAAAAAAAAAAAAAAAEAABAAAAEAEJAQAAAAAAAAABEAAQAAAAEAABkAEAAAAAAAAAAAAAAAAAAAAAAAAAABAA"
    "ARCQAAAAAAAAAAAAAAABEAABAQkAAAAAAAAAmZmZmZmZmZmZAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAEAAAAAwAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAABAAAAEAAAABAAAAAAAAAAEAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAQAAAAAAAAAAAAEA"
    "EAAAAAAAAAAQAAABAAAAAAAAABAAAAAAAAAAAAAAEAEAEAAAAAAAAAAAAAEAEAAQAAAAAAAAAJAACgkACQAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAQAAAAAAAAAAABAAAAAAAAAJAAAAAAAAAAAAkACQAACgAAABAAEAAA"
    "AAAAAAAQAQAAAAAAAAAAABAAAAEAAAAAAAAAAQABEAAAAAAAAAAQARAAAAAAAAAAAAAQAQAQAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACAQ"
    "EAEAAAAAAAAAAAAAAAAAEAAAIAAQABAAAAAAAAAAAAABAAEAIAAAEAAQAAAAAAAAAAAAAAAAAAAAAAAAACAAABAAAQAAAAAAAAAA"
    "AAACAAABEBAAAAAAAAAAAAAAAAAACZCQkAAAAAAKAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACgAAAAAACQAAAAAAAAAAEAAAAAAAAAAAAAABAAAAAAAAAAAA"
    "EAAAAAAAAAAAAAABEAAAAAAAAAAAABABAAAAAAAAAAAAAAABAAEAAAAACgAAAACQAJCgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAKAAAAAAAAAAEAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "IACgARAAAAAAAAAAAAAAAAEAIACgAAAQAAAAAAAAAAAAAAAAAAAAAAAAACAAoAAAAQAAAAAAAAAAAAACAAoAEBAAAAAAAAAAAAAK"
    "AAACAJCQkAAAAAAACgAAAAAAAAAAAAAAAAAAAJAAAAAQAJAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACAAqgAAEA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAgAKoAAAAAAAAAAAAAAAIACgAAoAAQAAAAAAAAAACgAAIAACAAkJAAAAAAABAAEAEAAQAAABAAAA"
    "AAAQAQEBABAAAAAAAAAAAAAAEAEBAAEQAAAAAAAAAAAAABAAARAQEJEAAAAQAQAQABAAAAEAAAAAAAAAEAEQEAEAAAAAAAAAACAA"
    "ABAAAQERAgAAEQAAEAEBAQAQAAAAAAAgABEAAAEBEQIAAgEBEQAQECEA";
#define TBDATA TBDATA_GR
#elif KING_STOP == 2
/*TBDATA2_BOSHI*/
static const char TBDATA_GR2[] =
    "sBEREUsRERAksAG7ArubGhERkakRGSEZGpEjkqOYEaERiBEYESihEagig8ocERFBFBERLBTMocyyyQoREBEqEaERpBABGgJakhAR"
    "EZAZoRGRAhGgIaApCxERESsBEbEkEwsTCzuCGBgRgRiBGIEYEYEhgiiaEREaKRGRoZIRFBkqSZKoERGBGIERihKhgaEyKAsUEBEr"
    "G7EQtLABWwJLRLAREaAasQGhAhuwJbBKBhwRESsRERAkwwHDDDucmRkRkZmRGUEZGZEmkkW2GxERKxuxEbKxETsrO6u7ERGBG4ER"
    "gRIbgbsyuKqgEAGqEaoRphABGgJqIgoBEaESERpBChGgMaAkQRARAUkRGhEkEAEaA0oiiBgRgRiBGFEYEYExgiQMHBARSxEREMLA"
    "ATsCPEywERuRGxEBwQIbsCuwuw0dERFEARHQItAN3Q1ETZQRGoEXERFBFBqBKoKnTBwREUsREcHCwRzLLJvMzBwcQRsREcwSzLHM"
    "MsumEBARehEREaYQARoCesIMARGxEhARLDxBMMHAI1uwERFJARGxLBALGwu7kkUaEYEYERFaFKGBoYIosGAQEasRERCzoAFbAltk"
    "DQwRsRIRASE9HDDDwMMMwBEROREREC3AAcwMzKxAGxGBExERQRQbgbuCtwEKEBFKERERoxABGgNKsrEREbG5ERohGhuhI6KzKBmh"
    "EYMRGBEooRGoIoPKwRERMRQRESwUzKHMssmZGqkRmRqZEUmhEakilMrMEcHByhEbLBzMsczCyszMwRHMERzBPMEczCzDjCmBEZEY"
    "gRiBGBGBIYIouxERGysRQbGiERQaK0qSKBoRMRiBEYoSoYGhMiiaqRkamRGZoUkRGhkqlJIqmRGhqZEZQRkakSaSRbbBERErHEER"
    "ssEROyw7rCsbETEbgRGBEhuBuzK4u7kZEbkbmRFJsRG5K5QrmIIRkRiBGFEYEYExgiQEkaERcxEREUShEagieMoQHBExFBERLBzM"
    "scyyyczBzBG0ERHBLMEcyyyzzAzMHMEcERHMFMzBzELMWRAaEYMREaFFERoYKoiiALERMRMREUEUG4G7grcKEgERKhGhEaQQARoC"
    "SkMQERGQFKERQQIRoCGgNAw2ERErARHBJBAMGwxLhiiBEYEYgRhBGBGBIYI1mzYRGykRkbGSERsZK5m7qxERgRuBEYsSsYGxMri0"
    "QAEbKwGxsbQQBRsES7IKERGgFLARqwJRsEGwKpkZGRmZEZmRlREWGSRJIgqiEKASoRpRChGgIaA2ozABESoRoRGjEAEaBEo0AhMR"
    "kxIxETEyETBBoEMjOBgRKBGIEYgRERgkiAQKERChEhERMQoRoCGgNcxgAxErARHBwhAMGwxMxgAcEZESEBHcAsHAwcDMC6QRESgR"
    "EbFIERsYK4hrsBAQsRIQEVoLYbBBsCQjCgMRKhEREaQTARMEOoQpEhiRGIEYQRgRgSGCNbxhExsrEaHBohEcGiyavCsaETEbgRGL"
    "ErGBsTK4u7mRG7kRmbFJERsZK5SbqRKpoRmRGVoZYZFBkiQkKUQRKRFIEYQRERgkSLQAoRExEhERixSxgbGCuAsUARFKEREQtKAB"
    "WgJak5AzkZAZkQGRAhmQKZA5BhwREUQRERAkwAHLDJSMlBOBgRiBEYEUGIEogjhGGxERSBEREUShETgmOLu5MbGRGIERkRIbgbsy"
    "uKowAQGqERoRqhABGgKqEgsREJEUERFBOxEwIZAjghATAYgRGBEoEAEYAogiRxERcRZxEWEUEXEhcie0QAEBCxEbELawAWsCayMM"
    "GxCxExEBQTwbMCuws0KwEwEJERsQK7ABuwK7K0kZEYEZgRFBFBqBKoKoQgoBAUoRGhGkEAEaAlpCAjVQoRoRAWECGqAmoDomwAER"
    "SxERECPAATsMO8wEHMExHBEB0QIcwMxAzEuyERtzERERJLERtitzKyUQEJEUERFBAhFgIZAlBgALETsRERAjsAG7AruLIDmBMRiB"
    "EYESGIEogjgGsRERQxERESShETgmOLspG7MxGJERkRIbkbsyuIuwERuIERgRKLERuCqDKwcSETEWcRFhEhFxIXInkyAZEZMRGBEk"
    "oRGoIoi6JLGxMRcREUESG2G7MrekEBEAShEaEaQQARoCWjMLEwGREhERQQsRkEGQNEMwMQFIERgRJBABGARIM0cTEXEWcRFBFBFx"
    "QXI3kQsRAEkRGRG0EAEZA1kSoiABoxQxETE6ETAhoDMEBBMQSRERESMQARYEWUMHQhExFnERQRIRcUFyNyobExGqG6ERorEROiI6"
    "S6ERoZEXERFBFBqRKjKnHBwREUQREcFCwRzKLEtMqRGjkakRGUEZGpEqkqnMHDPBrBzBwcLBHMwsrMzMHMHBGxERzBTMwczCy5YS"
    "ERF5EZERlhERGSJ5sioxEasSERorGkGhsaIkWqEREUkREaFIERoZKnOilRoTmhkRGVoZoZGhkimgYhERqhqhEaKhEVoiWmQrPBGx"
    "whEbIRscscOywwuxERE5ERERQrERuStzu5AbE5GzERkhGRuRu5K5AikREXkRkRGSEREZI0kiJBoxkRMRESEUGpEjkqNMwhEROxER"
    "wcLBHMssS8wSzMFBGxERLBTMYcwyzCzEPBzMERHB0sEczCzMrEJCEZEXEREqFEGRoZInKyQbETkRERFCsRE5KzlLGxuxsbkRGkEU"
    "G6ErQrnd0THRbR2x0cLRHdstvM0czMExGxERzBPMwcwyy8zNHMzcwcERsrWxEbsZERpbFLGhsUIpDBwcETwcoRFCwRHKLJTMDcHM"
    "MRwRESwUzGHMQsyQIhERKRGREZIRERkiWcMqNhOrEhEaLBrBocGiyguxEREpERGxQhEbGStzu5AbMZsSERkrGbGRsZK5BioRoSoR"
    "oaGiERYaJEpCkkIRkRIRGSEZEZFBkkUrJBsRKRERsUIRGxkrecvAwRHMEhEaLBTBocFCyURSE1F5ERERRJERaSJ5YyQbEZEUEREh"
    "FBqRY5KzS7ERsXQREREksRG2K3O7RBsxixgREUEUG4GrgrhSJBERSBEREUQRERYiWDJAohGRExERQRQakSqSpysiGzEzERERRLER"
    "NiszuwTBETsYERFBFBthq0K4BCQUEWgRERFFEREWJFizsBEREbsRCyELERAjILMLGwEbsBERECsQAbECstvQERARFBABIQLREN0g"
    "3YgZGBqBGoEROBERoSKCysQRERzFERohG8ERzELJzMwRHMERERIhmRAaEZARoRGpEAERApKyCxERGxIQFCELQRCxICS7sAERsAER"
    "ESsQCxELspKIihEaGBEYgROhEaEiKKqwGxGgG7EQtBABYQJSZAscERHCEQQhCxEQwyDD3MABEdARERAsEAHBDMK8i4sREbsRGIET"
    "ERG7IriiCRoRkBGhEaUQAREDQnICG7ERGxEBQQsRECsguy3QERFBAREQ0hAN0Q1C3Q7d0BEdEAHhAtEQ3SDdzMIcHLERERE8ERzB"
    "LLK8JrARERYQEVELsRCxICQMAhwR0REREMQQAcEMQowjGagRqBEYgRMRESoiqDzBEcFRHKERshEcwSyUzCzMwREcESERkjiiGBoY"
    "ERiBE6ERoSIouyOLEbEbgRE4ERGxK4LLLMHMERsREcETwRHMIsuaIKERoBGhEaQQARECUsMLNhEcEhAbIQvBEMEgxNzAERDQARER"
    "LBAMEQzCvIsbGBsbERiBE7ERsSK4qwqxsaABsRG1EAYRBEIykjAUARIRFEEKERBBIEQMAhwRIQEREc0QDBEMQrw7soEbGxEYgROx"
    "EbEiuCmQE5GQEREQKRABkQKSYwIcEREUEQFBAhEQwyDDzMAxwMARERAsEAHBDMK8JxsRGxYREWESERG7IrdyAhERgBERESgQAREC"
    "ciIgsBEBEBEBQQIRECsgu9wCHMHBEREQJBABwQzCvCeyERsWERFhEhERuyK3cwITEYAREREkEAERBHJDGRoRGqcRGUETEREqQqdM"
    "wTERTByhEbIRHMEsRMwdzBEcHBEhEaKWoRGhFhEZUROhEaFCJAsZGxExG5ERMhERsStEyyLBHBwVEREhE8ERzCLECxkbESsRkREy"
    "ERsRK0S7JLERsRQREUETERG7IrUMHAERwRwREMLBATEELCywERExGzEBMRIbECuwtA0dERHRARHQItEN0Q0tLamRERGpkRFBGRoR"
    "JJKizBwRwcEcEcHCwRzBLCzMxBERERuxEZwSzBHMMsJLEBERsAEbsSsRCxELS6KZkREaGZEROhkxEaGSIsbQAR3RHREQwtEBMQ0t"
    "bQIcMQEbsQFBEhwQbLDEJrkZE5EbGRGZsRGxKykrDBsQERsRATEcGxAksLLt0AER0QER0OLRDdENLd0CHRERFBAB3RLdEN1A0izM"
    "ERGxERHBQ8EcwSwrvMIQEBEbEBE7HDEQsbAiJgwME8ERERDDwQHBBissKxsUEbuhEUEaGxEkorLN0RHR0R0R0cLRHdEtLc0kHBER"
    "G6ERnBLMEcwywqzMwcHBHBzBSsEcwSwkvLkSGhsboRE7GjERsaIiJiusE7EbGhGpwRHBLCrMAsERERwRETwUzBHMQsIsYBERswET"
    "wSMRDBEMS7yyFhsbG7EROxsxEbGysiwNFhDBARHB0xEDEQwrzLJiQRwbsRE8GzERwbLCskAREKMRExCzoQFRAkpjDBwBERsRASEc"
    "HBBjsMImwDERQxETECPBAcEMZGyyGxERGIERQRQaEWuCtCELERCQERkRtBEBEQJJIsJQAQMboQFBHBsQJbBUJgIcEMERERAjwQHB"
    "DCtsArIRERuBEUESGhFrQrQjCxMBkxETEbMRAREESSMqGxERuhERIRobESSiss3SMRHdHRHRwtEd0S0tzRIcEREbEREsFMwRzDLC"
    "LMwRE8wcEcHCwRzBLCy8ohIRsRoRESsaMRGxoiImKhwxoRsREaLBEcEsKszCwhERGxERLBTMEcyywi0dHRHdHRHRQtEd0S0kzbJi"
    "EcEbEREsGzERwbLCJiwbEZERERFDoRGxJikrCxsUEbARAUEbERAkILLd0BERHQ0RENIRDdENIt0EHQEREBAB0RLREN0g0lzLsREc"
    "HBEROREcwSwivLsQG7EQEBExGzEQsSAiJgu8MQEcERC0EQHBDCLcItAREREQATEd0RDdINJcKxwaHBwRETkRHMEsIsyyYEHBEBAR"
    "MRsxEMEgwiYCHBEBEREQJBEBwQxCzLLBERHMEREhE8ERzELCCBIREYAYGBEoEAEYAojCYBERESQQEWwCwbDBQMZGEhERcRcXEUcR"
    "ERciZ7JkERERKBERShuxgbGCZHu7ERFxFxexJxEbFytzmwkZMQGZkBGZApGQMZApiYQRE4EYGIFIERgYI4gyBBMRAUiBEUECEYAx"
    "gERjNBERcRcXEUcRERcjR0QCEhAReRERUQIRgCGQJyxgATGxAhHBJBAMGwxMzA3MERPEEBHcAsHAwUDMe7IbAXEXEbEnERsXK3db"
    "JVAwE0oQEWoCUaAxoCYDAAQRkRMREQMQARgDSWQgEhERc3ERcRIRcSFyJgthERExEhGhJBEbGCtItie7ERFzcRF7ErFxsTK3i7Ab"
    "EYEYGLEoERsYK4OaCIIxEYOBEYgSgYExgihkIBQRMRcXEUcRERcjR7QHsRsTcxERexKxcbFytQsgERGQABuxKxALGwu7kkkSERGY"
    "gRGKFKGBoYIkCwATMLECEbEmEAsbC0uSCTIREZOBEYoSoYGhgiQpIBMQkBkZESkQARkCmTICEwERSBERQQIRYDGANXMwIREwFxcR"
    "JxABFwN3MyYTERFjYRFhEhFhMTI2QQISEJAUGREEEAEZAlkzRDABEYgREWECEWAxgDhkIhQRMRYWESYRERYjY0MkEhERWBERQRQR"
    "YSGCJktiERGREhGhwhEbGStJthi7ERGEERGLErFhsTK4i7QbE4EYEbG4ERsYK4haRVIRE0kREWkUUZExkiYEJBQRgRMREUQRERYj"
    "WLQishsxeBERSxSxYbGCt4sQuxExGBGxSBEbFitUqkAyEREpERFqFKGRoZIlRCIUETEVERFGEREXI1ODAhIREXAREXECERAhICgs"
    "YBEREQIRESQQDBEMQswMzAERwBARwQLBEMEgzGuyGxERFxERJxEbEStimymQEROQEBGRApEQMSApgwITEQEXERGnEAERA0LELcAc"
    "McEQEdECwRDBIMRrIrsRERcREScRGxErYrsgIBERABARsQKxELEgJHMCExEBFxERJxABEQNysyixGxFRERFxE7ERsSK0TGAREQEE"
    "G8EpEQwRDES8RBYREYGBEToUMRGxgmQsAhYQEwsRwSMRAxEMK7wEYhERMYEROhIxEbFCZCskFhERGRGhRBETESspxiRgEREBEBEx"
    "EjEQwSDEKhQREaGqoRGioRFaInqTohGREZmREZESGpEoMjkmGxERQRQRESKhEbommpuZkZERmZkZkRkZkSmSOZYbERGRuZERkrER"
    "OSs5u7gRsRGIgRGBEhuBq4K4mRIZEZEZmRGZEREZIpkiKhEREUoRGiEaEaEhoiRyEhERkRgZEScRERgicyKIgRERiIEYgRgRgSGC"
    "KKpCGjGhqqoRqKERaiJ6IysbERM7GxshGxqxK7K0QrIREZEaGREpoRG5IpMrmpoRE6maGUEZGpEqkqmRKRkxkRmZEZURERkiSUIk"
    "FVEReRERQRQakSiSR0ayERGhFBERsqEROiY6yyQcwRGUERGBEhyBzDLJS7QRG4EYERFEsRG4KogrRRIREXkREVEUEYExkicGJBsR"
    "kRMREUOhEbkmeZsaGZkxmpoZkRQakSlCObbBERGxxqERosEROiw6vBgbsTGDgRGBEhuBqzK4u7yRG7ObkREimBEYMYmBGIEUEYEh"
    "QihqG6sxs7qqEUSxEboilLsEsbERgxERQRQbgatCuJkikRGRGZkRmBERGSJ5MyoTEREqERohGhGhQaI0QzIREZEUGREkEREYJEMz"
    "iBMYEYiBGEEYEYFBgjiiKqERoRqqEaURERojSiKSMhkTSZEZQRkRkUGSNQQkExGRFBERQxERGCRZRJYxgTGJgRhBFBGBQUI4SVIS"
    "YZEZGRFJkRGJIpkjJBUREUkREUEUGpEmklWCsjGxkRgYESixEbgigytIGhEaiIERgRkagSqCmHIkERFxFhgRRhERGCJ3MkRSERGp"
    "kRFhFBqRJpJWQiQbsTEXEREkoRG2JnO7CKEROoOBEYESGoEqMphCJBMRcRYYEUYRERgiZ0MqFREVqhoagRoaoSiiOqbCMRGhyqER"
    "osEROiw6vBQbERt0ERFBFBuRu5K3S7sRsqG7sREilRIREXkRGVEZEZEhkicLKhsRo7OhEaKxEboiqrtEshETORERIRQbkbOSs0wc"
    "HMGxrKERQpAyERF5ERkhGRGRQZI3RiQaEZoXERFEoRGpInmaKRoZGJEaGZETEREpIjlGshEREaWhEaIRETEmMssnHBEccREhESKX"
    "EhkRcREYcRMRESEiKFIpmxETqpERNBERsSKSyyjCERxxERFBExERzCLHUykTGREXgRE0ERERJIIjJ7IRsWEREWESERErIreiQqER"
    "EaoaEaahEUEiKmMrHBERsRsRIRscEdOywiayERERGhMRI6ERsSYja5IbGRGRmxFBGRsRu5KyIiqhEREaGhGjERERIioiolIaM6Gr"
    "EUEaGhElolImKxsRERoREUOhEbEmKmuywUERs1wRQRQcEcxCwiMqExoRGhoRoxERESQqI7JSERGRMRExFBoRJZJUJiocERHKERGi"
    "wRHBLCpsorJBEREaEUETERFrIrJHIhEREXcYEUcRERgidzIkExERkRQRQRsRgTGSNHMyERERdxcRJxERGCNzM0YTERFxdxFxFBFx"
    "MXI2ciQRMRGZGRFHEREZI5kyRDIREXGIEVEUEYFBcjRDJBMREXcREUURERgjd0QGMRERM3cRcRQRcTFCNkIkERERSRkRRRERGSNJ"
    "IicyERExZhFhFBFhITI3RCQTERFYERFEEREWI2gzJjIREREXEVETERExIjZDJBMREZETEUMREREjSbOwERARrBsLIQsRECPAswQb"
    "ERshGxEQLBABsQK729ARERFCAQEhAtEQ3dDUhBkRGkGngRGEERGhInTKyxERHJQcGyEbwRHMwsmmEAERoRehEaYQARECerILERAb"
    "LAEbIQtBELHAJFuwEREhFBARLBALEQu7koUaERqEERhRGKERoUIosGABEbGqsRCyEAFRAltkCxwQES0cCyEMERDD0MMMwBERIRMR"
    "ECIQAcEMzLyAGxERNBsYIRgREbtCtwIKARGhGaERohABEQNKIgsbsAGrEQEhCxEQK7C63dABEdMUEBDSEA3RDU3dAt3RMdIBASEC"
    "0RDdMN0swxwcwRERFCErCwsRsBoQEbIQCxELq8LAwBAxPBEBIQwREMzAyjSRERoBp4ERhBERoSJ0yhMcERxQHBshG8ERzMLJWRMa"
    "oQEYgRGFERoRKoSyMLERETAbGEEYERG7QrfcEMwcwRERFCGgIBEQoRKhEaIQARECWsMLNgEcLQEbIQzBEMHQzAzAEREhEhARIhAM"
    "EQzMvIAbERskERghGLERsUK4BgsRoLESsBGyEAYRBEtCokABESoRGiEKERBBoEUMDBwQwxIQEcIQDBEMzLwwsREbIBEYgRixEbFC"
    "uLRQE1ChGhEQthABYQKqYwscARFLEQEhDBEQw7DDTMARwSEcERAtEAHBDMO8NBsRG0MREUEUERG7MrVSChEQkRQREaQQARECWTKw"
    "sAEROxEBwQsRECuwuywCHMDDFBEQJBABwQxcvDSxERtDERFBFBERuzK1BAoUAZEWERGlEAERBFkjKRoREzQaGSEZEREjQqO8wiER"
    "TMOxEcIRHMEsrKySQhGhdBEZIRlBEaFCJyspGxFBs5ERkhERMSs0yzTCHBwUEUERspKyEbEkERkhGbERsUK3KyMbETMTERFEEREx"
    "KzR7CRsRG7EbC0ELERArILtN0BERHdTQEEIQDdENQr2WsBGxYQEbUQuxELEgJAwJHBERzcEQwhABwQxCzJDAEcEhARwhDMEQwSDE"
    "3AIcERwcERDUEAHBDMIsCxsBEcEbASEbERAkwLLd0BEQHd4QENIRDdENLt0CHRERIQEBIRLREN3Q0izLEREczBERshEcwSwkvLIQ"
    "AbHBAREhGzEQscAiJgwcMBHNERCyEQHBDC3c0tABEdMBASEd0RDd0NIsHBwRHMAREUkRHMEsJMzCYBHA0QERIRsxEMHQwiYMHAER"
    "HREQsxEBwQwrzLLCERHMHBEhG8ERzELCLQQdEdHREBBCEQ3RDSJNChIBEZEXEVEKERAhkCe8YBEQESsQEcIQDBEMTcwNzBERIQwR"
    "0QLBEMEwzGuzGxERcxERRxEbEStjW7VQAROhBBFhC1EQMaAmBAoUEBE5ERGkEAERA1nEIsAMMcMMEcEAwRDBwMxrELsREXAREUcR"
    "GxErU7uwMBEQsQIRYQuxELGwJUQCFAERaBERJhABEQNYszKyGxExFBFBFLERsUK13ALMERHBEBHdEAwRDELMwmAREBELEUEbMRDB"
    "sMKUUhFREXSaEZQREYEidGMqGxERQakaIRoREWOys1IpERERdIERlRERESJ0IpCyETFBoxlBGRERK0K3TCQcwREXERQhBCkUERF0"
    "gRGVERERJHRDRLIRsTEUEUEUERErMrUmKhsREbEaEaMREbEmJEtEQhEREXMRURQRETFCNgQbERuQFbEQK7EBsQLL29AREREkAQEt"
    "Et0Q3UDTSBkRGoEVgRFIoRGhIljKxBERESsRESwczBHMssPMzBEcwRzBwSzBHMEsw7wLGxEBSQEbuxKxELGwLHmkERGBFIGhRxEa"
    "ESpIwgIcEQEJEQzBEhwQzMDNKrQREYEQgRFIsRGxK4hLAhuwEVsRATESGxArsLUt0AER0RIQ0CLRDdENPd0O3dER5AEB7RLdEN1A"
    "3szCHBzBHBHBLMEcwSzMzADAEBEMEQFBEhwQzMDDCJIRGjEVgREooRGhIljKEBwRESMRESwSzBHMssO8whwcMRzBwSvBHMEsw8wM"
    "zMERzBEczBLMEcwyzHkgGhExFIGhJxEaESpIogKyEREDERiBFBsRu4K4zBDMHDEcEcEswRzBLMzMAxwRAQkBHMwSwRDBwM07pBER"
    "gRCBsUcRGxErSLsDohERAxEYexSxEbGCtEzAIcGALcEQLMEBwQzcvCcbERt3EBdxEhsRu3K3TAIcwME9ERAjwQHBDNy8B7IRG3MT"
    "F1ESGxG7crdEohGhkRUREUShEaEiWcokHBERKxERLBzMEcyyw9zBHMFBHBHBLcEcwSzDzEzMMRzMERHMHcwRzMLMWiQaEZEUEaFF"
    "ERoRKjmyQLIRETkREUEUGxG7krMsIszBwRcRwULBHMEsfMwMwRwcwxERzBTMEcxCzDskGxGREBGxRxEbEStJuySyEbGIExFBEhsR"
    "u4K4JrARsQEWERAkEQGxAlLbAh0RESEBASES0RDdINPd0B3QAR4QEC0RDdEN4s0szBEcwRERwRLBEcwizLsCGxEBFxARKxELEQtC"
    "wiLAEREAEQFBHBEQzCDN7QLd0REeEBAuEQ3RDeLNK8IcHMEREbESwRHMIszcAhwRARAQEcQRDBEMQswswBHB0BMBQRIREMwgzdwS"
    "zBEcHBERPREcwSzC3AQdERFAAQ1NEt0Q3UDSTMwREREbscFJwRzBLCvMBMIREcERGlwSzBHMQsIsLBwRERsRwUvBHMEsK9wk0BER"
    "AQEBQRLREN0g0szAHBEBQ8DBLBEMEQzcvCe7ERExFBZ7ErERsTK0eyK7ERFDYbEnERsRK0O7J7IbETEUEXsUsRGxMrPMAswREdAQ"
    "ESwRDBEMQnwkFREVkRUZcRQaESWSR0ayERERKhERsqERMSY6yyQcERyRGRZREhwRzDLJi7QRsRGIgRGYsRGxKogrRRIREZEUFmEU"
    "ERExkiQmJBsREQmREUShEbEmactFwhEckRkRMRIcEcySyWsQG7Exg2ERRrERsSqES0MyERGREBZBFBERQZJDQiIbERtnYxFGoRGx"
    "JGe7RLIRsZEXEUEaGxG7krdcIhwRHHERETQREcEsgmyyshERERoTMRQaEWuiskMiExERMZQRRRERESNDNAsbARuxFQFBGxEQK7C1"
    "3dAREBEtARDSEQ3RDT3dDt0RHSEeAOES0RDdMN7MwxzBEcQREUwRHMEsw7y1sAERsRQQURuxELGwIwwMHBARDBEQxBEBwQw83CLQ"
    "DR3THgDRENEQ3dDezBPMwRHAERFMERzBLMPMw8AREMEQENEcwRDBwMQ8AhwBHNwTECQRAcEM3MwywhzBQRQRIRTBEcxCxe0C3REd"
    "4QEQ3hEN0Q3i3dLQERARHQDRHdEQ3dDS3ALMARHBBBEtEQwRDDzMQ8IRwRF0EUEUERHMQsgoEhERkReREScRERkic7JiERERJBER"
    "KhKxMbGiY4gSGBGBGIEYiBERGCKIsmkRERspEZErGbGRsZK0i7sREYEYgbEoERsYK4iqKRkREZkRGZoSgZExMimZmRmRkRmRmZkR"
    "GRkjmTIkExERSREZQRIRkTEyRIM4GBGBGIEYiBERGCNIxCnMERFEERGcEsFhwTLFi7QbEYEXEbGYERsYK3iKGRIYEYkRiIEUEYEh"
    "Qii8YRHBsRIRxKIRHBUsSrwYuxERgxEYixKxgbEyqJu5m7GREZkRkqmRGRqaEZmaFJGRMUIpgxmEEZEWgRhIEREYI0S0CLEbEWMR"
    "EYsUsWGxQqarIhERkRSRoSkRGxkrk6KaEhkaqROZmhmhkaGSJKsaGrqhNqG6SREbGitEMicTExF4ERhxEhFhMTI3czQREXEWcRFG"
    "EREXI3czB0EREWMRF2ESEXExMjeUIhERkRcRGZQRERkiecIqFhPBKjGhLBrBocGixKuxGxFBFBOxShEbGStZu5m7IbEZkhkhVSoV"
    "EaoaE6qnERUaI6oykEIRETkRkSEZEZExkkRLJLsRkRcTsaIRGxkrecvJwRzBFqMaIQsqGxGrMhO6ohEbGitKMkRCERFIERFBFBFh"
    "MYI1lyKREREXERk4ERERInKyJBYRoSERQSEasRGxImR8whwRERcREiGaKZgRGhkRGTkRGREjkjKXMhkRURGRgRMRETEiRHwizBER"
    "FREROBEcESxCvJoikaFRE5GRE7ERsSIkcyITEREWEREmERERI1KzJBYREaERGpoSsRGxMmKbaZERGxmRsZMRExErKcu2YaHBsREa"
    "PBQxEcFCwiwqFhHBGhPBohETESwqvKVioREaERExEzERsSJiiyIbERF3gbEnERsYK4OiSKIREYEYGIoZoYGRgiiKEKQREYOBoSgR"
    "Ghgpg6JGQhoRkTQRahqhkaGSJmsishERcRERJxEbEStikikSGRGRGZmRGRGRIZIpozIRERFKERqiEREaIzozJxMREZEXGXESEYEx"
    "MjeDOIERERiIETKZMhkxkRmZQRkRkSGSNEMkExERWRERRBERFiRpNJgxgRGDgRQhciQTERFngRFGEREYInczlDIREXGRGSGUIhER"
    "EWQRFpQREREiVLIqFhGhQRKhIRqxEbGyZVUpFREadBEZlxEVESN0QpBCERFBE2FBGRERMUJFzCTMEREVERQhCykSERokExmWERsR"
    "K1RCREIREUEUEUEUERExMjUrKhYRobEREaQRExErJLZGQhsREUMRYRSxEbEyJZzCHBERSRHIKREcESyDvEe7ERGBFIF7FLERsYKk"
    "exC7ERFDEbhHERsRK0S6R7IbEZEUE3sUsRGxkrN8IswREYERETcRHBEsQqxAERARqxqhIAoRECWwNQkZERkgGREBKRABkQKSY8AR"
    "ERFCEREgAhEQzCDJiBkYGEGIERiIERGBIoRjuBEREXQagSEYERFjQrOZEAkRoBkRGZkQARECmiIJERARSxGRIQkRECGwJIIQEREg"
    "FxERJxABEQKCInZxERF0EXFxFxERIUImqkAKEbCqEQqmEAFhAlsjCxsQEUwasSALERArwLRCsBERIBwRASsQAbECsiuJiRERlBqB"
    "QRgRESpCqJEJCRGgGREZlRABEQJKYgoVYBGrERFgChEQJbBKtsABEbEUEQHCEAExDTwslBAQEVoREVEJERAxoCQGCwsRsRwRAbMQ"
    "AbECS4sjGYgRgBiBgRgRESgiSDaxEREBpREYghERMSYyKzYSFxFwEXFxFxERMSImUiOJEQGpERiEERGhJoKqCRIJAaoRkWEJERAh"
    "oDWTMBEQsRIRGZIQAREESzMEExEBQhERQQIREEEgNGM3cRFBFxEXdBERESRkI5kQCQGbEZFRCREQMbAkIgmTEKAUERmUEAERBFpD"
    "kDABEUoRETEJERBBoEVUIxMXARcRF3QREREkYqQKJQEGqiERoAoRECigOrJQERCxFBMBtBABYQJbJQsbEwuyMRGwAhEQKyC7YpMR"
    "oWERERMhcgkREJAXExGXEAERAnkitFABAQsxEWALERAmsFZyAhywsRsTAScQAbEMu6wGkhEaFhEhESKUMBEAeTERYQkRECGQNpRS"
    "EWFBlxEZlBERUSJ0ZCkbExFEGpEhGRERY7KzQigREUEVERaFERERI0QikKIRETQakSEZERFqQqQEKBMRQRURFoIREREkVLQ0ohGh"
    "FBFBEZIJGRkZkBmTkAkRECkgOUbAERERyRELQhABMQwyLIgQGBGAEYOBCBEQISAowgm7EQG8MQu0EAGxArI7iTCBEYARg0EIERBB"
    "IDiiQKEQAasRAaQRAUECK2MLHAER0RwRIBsRENPQwibAERExEhEBIxEBwQwibIIbGBGxGhFBGBERa0KyIgmREAEbERGTEQERAisi"
    "slALE7AaEUAbERAlsFImDRwQERwRAbMRAcEMK2yyskERARoRQRQREWsisiMJEwkBGxERkxEBEQQrI8JQERDDMREwGxEQJbBUJikb"
    "ERGsERGSERGxJiRrQsBBEQEcE0AUERDMIMKmIBEQAXkREacQARECaTIKEwERoRQRQQoREDHANGMwEREBchERJxABEQNiMzYTEREx"
    "FRFRExERMTI2kgkRMAGaERGZEAERA5oypDABEZAXEVEKERAhkDRDABMQEVkREQUQAREDSUQ2MhERMRURURMRETEiRkIJEQEBShER"
    "lRABEQNKIiYwERBwNxFxAhEQQXA2RCMTERFTERFEERERI0Q0JjAREQEXE3EJERAxIDZDDBMBEaMREaMRAREDSpMpFRkWQZmRkRkR"
    "EShCOZJSERERdBoZkhERMSI0JXYSGBFBF4FxFxERIUImoimVEROkGhmUERFhImQldjKBEUEXgUEXEREhQjZiIxoRGhYREyEiKRUZ"
    "EbEaEZQREVEiJCU2MhEREVMRURMRESFCNqZQEWARohoKphABUQKiYwscAREhxLEgDBEQwyDDQgkREBFSERaVEAERAkLCsLABEUG8"
    "sSALERArILQECRMBEaQRFpIQAREEUmPCwBEQEcIRIBsREMwgwkQKEwERIRURpRABEQNCUwoVARWwFRFAGhEQJbBFtsAREBErEQGy"
    "EQExDDzMBBwRHCAdEcASERDMIM1rsxGxEXQRETcREbErYyuXEAERoBURcRkREDGgJCYLGxABCxEBtBEBsQJrywXAARzRHREwEhEQ"
    "zNDNWyMbsRFwERE3ERGxK2JLlDAREKAQEUEZERBBoENCAhwBC2szAQQRAbEMa7w0shGxQRURQRQREbtCtMwCHBEc0BEDxBEBwQzS"
    "bNLAERAxHBEwGxEQ3LDClyKRERF0EZGXERERInQyKRMREUEUESYZERExQjNiKIEREXQRgYcREREjZCKXMhkRQRcRSRkREUFCNGMo"
    "EhgRdBOBhBERESNEQkYyERFRETERMpQygRERGxExGRERMUIyDBYBEcHCEQzBwAEcAiwmUBExETsRERACG7AhsFsNHRER0RIQAc3T"
    "ARMNLS1ZETERORqRERkakSGSWcwcEcEhHBwcIaQQARGhFREaoRABGgIqYgwREBwsAcHBDBHAYcAiJUAREbEUEBErEAEUBStSkhQR"
    "GkkRkaEZEZFRkiKyQAERsVsRW7FQARsCK3QMHRARLB3BEAwdwHHA0ibAERGxExEBIcABFAYrvJIbERE5G5ERGRuRYZKyIgoBEaET"
    "ERqhEAEaAyoiDBUwETsRERAMG7AhsFrt0AER0RIQAe3TARMNLV3CQBARSwERsQwRsFGwIiYMDBGxExEBwcABGwYsLBsVMRE7G6ER"
    "GhuhIaJZzdER0SEdHBwhJRsUsbEUERqrEREUJSrCssERETsboREaHEFhosKiIBEQoRIRGqEQARoCKtMMNwEdLAHB0QwRwNHAcixg"
    "ERGxEhARLBABFAwrtpIbERspEZGxGRGRsZJiJAsRULESFRu1EAEbBCsyojABESoRoREKEaBBoEIsDBYQsRIQEcwQARsMLMaywREb"
    "KxGhwRoRQcGiYrJAEzChExFBsUABGgKqYwwcARErEREQPBwwYbDCIgsREJEUERGxEAEZAkkiwlABETsRERAMG7AhsFIjCxMBkRMR"
    "EbEQARkEKSMqFRETOhujERoboSGiWs3SIREtHRwcISUqFBGrFDEaqxERGiUqwqLCERE6G6IRGhyhYaLCLCocEasSIRqsEREaLCpG"
    "CxURFmEbsRALERAhIFRFCxURGxMQE7ETAREFIsKywBERMRwxEDsREGEgwiwLHBEcEhATsRMBEQwiJgwWARHBHBEQHBwQIcBi7uAR"
    "EB7uEAHu4QERDi7eAh0REdEBEdASHRDR0NIszBERHMwREczBEREsLGzCEAHBwQERwRwREGHAIicMHTAR3BEBwdEBEQcs3eLQARHR"
    "ARHQHh0Q0dDSLRwdER3cERHN0RERLSzdwnAR0MEBEdEcERDRwHImDBwBERsRAcHBAREGK9zC0hERzR0S0RwdEdHC0i0NHRHR0RAB"
    "0REBEQ0iLQsSARGRExERCxGQIZAizGAREBErEBHMEwETDCzGAswREUEEEcECEUDBQGIrZBsREUgREboRERgrKEayQAEToUQRQQsR"
    "oDGgIiMLExARORERsRABGQMpxCLADDGxBBHBAhGwwcBiKxC2ERFDEREqEREUKyhWwiARELECEbEMEbBRsCIjAhMBETgRESEQARYD"
    "SLNCYhsRkRQToRwRkbGSYiwCzBERMRARIRMBEQwixsJgERARCxHBHBEQwbBiokIRMRE6FBqhQREaIqpzKxwRESG7sREiohIREaEU"
    "oREaEaEhoiQiKhUREzobGqGhERoiKjWiMhERoROhERoRoUGiMicrHBERsRsRsbEREScrLAsVARPBs7EQDBEQIcBUJQsUEBs8ARPB"
    "EwERBSzSssABEdHDMRA8ERBh0MIsCx0BHC0BE8ETAREMLdbS0BEQHd4QEB0RENHg0sJQETARKxEBwbEBEQIr1Q4dARHREhDQHh0Q"
    "0dDSLdAd0REkAQEt0QERDSTNQswRHLESEcEcHBHBssIlDBQQESsBEcsRAREFK2LCwAERsRMREBwcEGHAwi0C3dARLQEBLdEBEQ0t"
    "zQLBHBwxGRHBEhwRwbLCLAwWARELARHMEQERDCzGIsARMLEyERASHBBhsMIsJMwRHKsRE8zBEREsLNwi0B3RERIQEBIRENEg0qIi"
    "EREROhGhoRERGiIqwisXEbEhEbsRQqJCEUGhFBFKGhGhMaIiIyoTERE6EaGhEREaIypUoiIRsaEyEaoaEaFRoiKkQBFAEVsUoaQQ"
    "ARECSyOUEAERsRMREzkRECGwJCILFRAROxoxsBMBEQIrNZIwERCxFBETOREQMbAyCxsRGyAbsQGxsBG7ArLb0BEREUIBETAN0zHd"
    "IN0oGREaMRiBEYGhEagiiMrCERERQxERwRLMscyyyczMERwhHMERzMEczCzCnCgaERGDERihGKGBoYIo3MARESAdwQHBwBHMDMKs"
    "KxsREbMRGBEYG4G7grjt0B0dIR0QAeMwHdMN0s0szMERwxERwRzMwczCzCiSERoxGIERgaERqCKCyhIcERFDERHBEsxBzCLJzMIc"
    "HCEcsRHMwRzMLMLMLMzBEcwRHMEczMHMIsyJIhoRMRiBEYoRGhgqgqIqshERoxEYERgbQbsiuMwSzBwxGhERzMEcyyyivCsaERGz"
    "ERixGLGBsYK4qyIaETEagRGLERsUK4LLDBwSDMIRHBAMHMHMIMx7shGxMRdxEXGxEbcrd7snshEbcxEXERcbcbsitySiEaExFxER"
    "QaERqSJ5yiIcERFDERHBEsyxzLLE3MEcwSEcERHTMRzDLMLMLMwxHMMREcEczMHMwsxaIhoRMRQREYoRGhkqebIgshERMxERERIb"
    "kbuStywizMExFxERLMEcxix0zCzBHByjERHBHMzBzCLKCyIbETESERErERsZK3m7JLIRsYMREREZG2G7QrgrsBGxARsRAbEQEbEC"
    "stsCHRERQQEREALREd0g3d3QHdABHRAB0RAd0Q3SzSzMERzBERERHMERzCLMuwIbEQEbEBGxEBsRC7LCLcAREdARERAMERHMIMzt"
    "At3RER0QAeEQHdEN0s0swhwcwRERERzBEcwizNwCHBEBHRARwRAcEQzCzCzAEcHAEREQDBERzCDM3BLMERwcERHRERzBLMLMRBwR"
    "EbERGsEZzBHMQsJMIhwRERMxEZMxHMEsIswiwhERsRERwRLMEcxCwk0CHREREBAB0REd0Q0ivSi7ERExGTixGLGBsZK5iyK7ERGT"
    "gRKLERsYK5K7KLIbETEUMbEYsWGxQrnMAswREcAQEsEQHBEMwswoHBEcIRkYERgcgcwiyYuyEbERg4ERgbERuCqIuyixERszGBgR"
    "GBuBqyK4SyIbERtzERFBsRG5K3nLJ8IRwREXEREXERHMIscksBGwEbIRAcEQEbECstsCHQERIRQQEALREd0g1O3QHdER0gEB4RAd"
    "0Q3SzSzMERwhHBERHMERzCLMWwIbEBFCARHBEBsRC7LCIMABEcETERACERHMIMwtAt3QMdIBASEQHdEN0s0swRwcMRoRERzBEcwi"
    "ygwCHAERLAERIRAcEQzCzCTAEcDBHBEQDRERzCDMLCLMERxCEREhERzBLFLcLtAd0REdEBAO0RHdIN28whzBEcMRG7zBHMEsxMwq"
    "whwcMRwxMRrDEcwizNwizBEcwxER3MEcwSzEvCi7ERExGIGxGLGBsYKoixK7ERGDERiLERsYK4K6KbIbETEUE7EZsZGxkrUpkBGQ"
    "AZIREZAQEZECkmMCHAERIRQRAQIREcwgyczAEcEBwhERwBARwQzCvCYbERshFxERFxERuyK2ggIREAFyERFxEBERAoIiJLABESAc"
    "EQELERErILtMAhzAEcIREdAQEcEMwrwmshEbMRcRERcREbsitkMCEwEBShERQRAREQRCIyuwEbAgGxMBCxERKyC7SyIbERtSERFB"
    "ERGxK0IrAhUBE7ETEQEBG7EhsFst0BEQES0BEdDTERMNLd0C3REdIR0QAQMTMdEg2izCHMERkxERwcERGyxbXCJAARGxFBARCxFB"
    "UbAiJgIcEBE7EREQwBEUBivcItANHUEUEAENHdHRQNQsEszBEZMREcHBERQsUswiYBEQsRIQEQwRQcGwYiwCHAETNBEREMARGwaU"
    "zCLCHMExFDERHByxwbLEmRoZGpGlkRGRqRGhIlnKzBERHCwcEcHCzBHMwsLMzBEcwRzBEczCHMEszKyZmhEaSREZoZmhEaGSJCu5"
    "GRGRsJERkbkRsSspy9zMwRHMERHB3MwRzMLMqbGhG6G1oRGRtBGxIlTbHR0RHS0dEdHC3RHdwtK8wRwcwRyxEbzCHMEsw8zMzMwc"
    "zBHBIZsaq7GhFKERmxQbEStEwqLBGhEKHBoRSRwRzELCzBDMHMEcERHMxBzBLMS8kxsZGwkRGbGZsRGxkrI8GhzKoRChEZwUHBEs"
    "JLy3GxEbiBEYEbcbEauCuHsQG7GRGGERUbIRsSqDSyobERtaGxERpBsRK6K13dIxEd3SEREt3R3RLS3NHMwRHMQREcFMzBHMwszM"
    "zBzCzBwRHMKgwhERChwREaIcEcyiwiwtzMHBGhERLM0cwSys3N3RHd3dEdEhSyQbEZsXERFRtBGxK3lrKRoZGmEaERE0EREqIqVM"
    "whERHMIRESEbHMEsIswszBEc0REREqKXohmhQRERETehEaEiJCspmxERsBERQRMRsSsiyyzCHBzRERERPMERzCLNOykbGRsQERFB"
    "ExsRKyK7JbIRsYEREREkERG7IreszMERHMzBE6zMHMEsLNzV0cHR0R080UXdEd1C0nu0GxERSIERexQbEStIugexGxExFBaxJ7ER"
    "sTKkm7mRsRG5IRnCxsGhHJMcQyFMKhwRHMoxGjIpGhEaQaUREZQRESpCpbzCEREcJBwRIRscwSwsrJWiEaFBFBERlaERoUIkCykb"
    "ERE0GxFBGRGxKyTLTcIcHNERERSyk7IRsUEQERGXsRGxQrI7JBsRG0QREUEUEbErU8vazBEcwRLBwdrMEczCwqwQzMERTBEWrMIc"
    "wSwjvKq7GrFBEaEhrBrMGhwUERRSKRUZFUGVERGUERElQkWWshERESQaESEZETEmJCuGEhYRQRQREYURETFCJCIpmhERBBoRQRkR"
    "oSYkSoMyYRFBEBERhBERQUJCSyQaERoWETEholIRMREqGxERuhERIirVLB0R0SEdERxSokIRsaESERGrERFRoiIsKhwRETobERHK"
    "EREmKsyiwhGxoRAREawREcGiYrzEHMERzBEbwcscwRzEzCrBHBwxHDERo8MRzCHMdxIXEUEWERdhFxERInSSiIgRGIQRgRGIgREx"
    "QihzNxcRQRYRF2EXEREjRHQjEhcRYBFxEXYRESEiN4kjiIEBGBEYgRgYESSCMjdCFxFQEXERdhERQUJEmSiBoUEZERiBGBoRKkSS"
    "OSKBGlARgRGIoRGhImSIIIERARgRGIEIARECgpKZkBmRkAGREZmQEDEgKYMIgxEBGREYgQgBEQNCtJwgsbHAAbERu7AQsSAke2iB"
    "ERoUERExGBMRKyS2NWKBoQERERFDMRGxImJpIxoRERYREyFpIKoRERYREiGVIBEQEXIRGEEJARECclKlUAGhIRWgMaVQEDEgJAMJ"
    "FBARkBEYIQkBEQNAtLDAEbABErAxsrAQsSAkW7MbERFEERFBExsRK0O7NbIbETEUERE0sRGxIrR4IIEQAXIRgXEHARECgjJ4MAgR"
    "IBcRGHQQECFANJQgERARSxGREQkBEQIrQqRAAUGxExQToRMQMbAiIwkTEBE7ETEROQERAytTsiARoLESEBOxExBRsCJ7shsREWIR"
    "EREWGxErcrsnshsRMRYREWGxEbEitwsUARGxshELIQsLGwI7Q1ARYQFaERsQKrCwIbA6lBUZFpGVkRmRGRkZIpljCqEQAWoRqhGq"
    "EKAhoCqiEAERoRIRGiEKARoCOiIFEREBSREaESoQoCGgKVIYGBGBFIEYgRgRGCKIsgu0EAG7FbsVu1CwIbA7ssABEcHCEQshCwsc"
    "AjwsBBwRATsRGxAswLAhsMyiqRkRMRoZGSGhCgoRoBqhGqEKARoCqiILFVARWxERELCgsCGwSiIKARGhFBERAQoBGgOaYsDAEBE7"
    "EREQwMCwIbDDpFKRFqGlkRmRGRoZIpkklRIYEUkRiBGIEYExgiiqK6sRMRsaGiGrIKEQoBuhGqEKARoCqjMKFAERKhGhEaIQoEGg"
    "MzMwERGQFKERQQIBGgRKM4MUGBGIEYgRiBGBQYIzogqhEKAaoRqxCgEaA7oiojAKASoRqhGkEKAxoDUEChMQoRQRETEKARoEOkST"
    "QoERiRGIEYgRgUGCQ7RAEkCgFaFBoQsEGgKqI6UQAQFJERkRqRCQIZApQgsVELATsQGhCwobArslozARAEkRGRGpEJAhkDmiUhFR"
    "oaURGgEaGhoiqiSSEhERSRGREZARkTGSJwsqGxExGxoaIQQpFBGRFxEZARkRGSQ5ZAoVGxVgG7EQtBAQISA6QgmhEQEUERpBCgER"
    "AkIylEChEUARoRGkEBAxIDOyQLEQAbuxASEbCxECK2MMHQERwR0REMLRENHA0ibAERExGzEBIRIMEQwrbJIbGRGRGxkRkrERsZKy"
    "IgqhEAEaoREhGgERAioislALE7AbGxCysRAhsFImDRwQERwRATEdDBEMK2yywkERsRwaEaKxEcGiwiMKFAoBGqERURoBEQQqQwsS"
    "ARGQFRkRuRCQIZApUgoRMAFKoRGhCgEaA6oytTABEZATGRG5EJAhkDkyChEBAUqhEVEKARoDSkIqFBoUoUWqIapBoSGiOlIpkRER"
    "SZEZkxkRGSKZIqtSGjExqqERIpMykRGRGZkxmRGRMZIzs1ARUBFbGgsBCwERAqskoxABEaEUoRGgEBAxoCQ0ChQBEVoRGgEKARED"
    "OkQLFQEVsBKxELKhECGwQNbAERARKxEBIR0MEQw8zAQcERyQEsEQIsEQwcDAi7QRsREoERghFBsRKwgrpRABEaASoRGiERAxoCAm"
    "DBwQAQsRCyEcDBECO8wCwAEcwRIRECDBEMHAw4sgG7ERgxEYIRQbESsISyoSGhGhFaEaqhGhIaIqUimRERFJEZmRGREZI5kyqjIa"
    "ETERqhFSCxQLFLBFEUu0EBAhsCBCCqEQAVoRoUEKAREBSkILFAEVsbQRC7GwsCGwMkIKERARShGhEQoBGgIqQgIVARawFbEBobCx"
    "IaA6JsAREBErEREQMhwTDDvMDBwRHCAcQQHRwEHBIM2LshGxEYMRGBEbGxgruCslEAERoBShEaEQoSGQKkICHBABOxEbEAwcGwLL"
    "zCLAARzBFBEBAcDBwZDNiyIbsRGDERgRGhsUK6JLKhUaFaGioREhqhEhokBSKZERESkRGRGSEREjCbKishoRARsRGlIJEgkRsBQR"
    "GUEJECGwKVIJkRABWhGREZoAEQNKkqSRoRGUGpkRmaGRKpGpTBwRwUHGERwsHMzMHKzMzBzBEcIRHMEswbzMMcyapBmhQRmRGZoZ"
    "oRkambJLmxERtBuZEZmxkbuRuczCHBwxHBERzBDMzBzMnBMbuRGTG5kRObGRKzG5PdER0THWERotHd3aHb3NHMzBEcIRHMEswbzM"
    "MczMzMzMwRwcHME8wRoRwxyqETnBocwxycwQzBwxHBERzBPMyxzDvEsbGRu0EZmxmRGbsZG5i7IRsTEYgRGBEBu4Goi7CLERG4MR"
    "GBEIsYGrMbjMxBzCzBwcHNE90R3d3LHRESiSgaERqBEYgRMRoSKCyiIcETFREzERMhEczCLJzMIcwREcESEhiSKKERoYERiBE6ER"
    "KoKyKrIUEaEbgRE4ERG7IrjMIszBERoREcETwcEsoszGHBvBwRwbwcrBHMzBwouyGxERg4ERixCxGBuIukkbGRuRm5ERnBMcyTHJ"
    "GRNBKBkRGiGngRE0EREqAqdMwhERHFIcGiEcwcEsUJyFohGhIRiBETURGqECKAsoGxERNBsYQRMRsStwyy3CHByxERETwkzMERxB"
    "HMHBDMEczMHMm7SbERsZkSwhKhURE6GzERqxsaEhslnN0hERHdJBwSElKhQRG0oRobEbERQlK8KiwhERobMRGrHBQWGywszDHMER"
    "wxEcwRzMzBzMzMzMHNzBHBEdlFKRYRFZGpkRqaEZIpokhRIYEYEUgRiBGYExkiiqKqsREbOhsSGIGRgaQaUREYEYEaEiVJqHihEa"
    "RBEREYcRGqFCJDu4gbFBEBERcRixESskawkbGxtgGxEQtBEBKyC1uwm7EQsXEBGxG7EQC0LCuRwbwcEcERG5ERzMQsKzsBGwEVIb"
    "AUEbEbACUstMzBEcIRExESILFQETwbMRAbERASHAUiULFBAbPAERERsREAUswivMERwxHBERsREczCLMtFCxUAFbGhEQshEQAgsk"
    "pRAKEaATEREhGgExoCCEEhgRgRWBGIEYEYEiiFKVlhEZSRGZkZkRETmSKTopkaGhERkZIZQgoREBFBEaQQoREAJCUqZQG7FgAbER"
    "uhABMSAkK2mRERsZkRE7GRERKylLChIBEaEUERqhEAEqoCJECxQQFEtBsUELMbADK7IouxERMRiBEbsREbiCu6QgoRABWhGqEaIQ"
    "oAIqsiAREBG7E7ETtDABO7AmChIRGqAasQGhAhuwCquSKZGREZmRkRGiCqEQAaoRqhGqEAEaoCqiEAERoRIRGkEKEaABWiIJEREB"
    "mREaESkQARqgKYIYGBGBGIEYgRgRgSGIsguyEAG7E7sTtzABO7AmoQoKEaAaoRphChGgAWpyCxJwEasRERC3oAF7sCpiCgERoRcR"
    "EWEKEaABepIqEpkRqZGRESKYEhgRiRGIEYgRERiCKKsgoRCgG6EacQoRoAFqIqoQCgGqEaoRphABGqAmuiASYKAaoQGhCxqgBqoi"
    "qRABAZkRGRGpEAEZkCmnIhFxoRoaGiFiKRERkRcRGWEZEZEheSKZEBoRkBGhEakQAREgKbIgsRABO7ExURsTEANbIgwVARHBHBEQ"
    "wsEBMcBSIlARETEbMQExEhsQBUslkhUZEZEaGRGUoRFBklIiCqEQARqhEVEaERABSpILEgERkBkZEbkQARmQKaIKESABqqERoQoR"
    "oAGqoioSGhahqqERIpkSGRGRGZkRmRERGZIpYgoREBF6ERphChEQAXqiCxIBFLAWsRC6oQFBsCaqIqERERqhGiEqIBGgAasRGxAK"
    "G7EKqiICFQERsRQRAUGwEWuwVrKwEbEBshEbEAsbsQuyKygaERoxGIERgaERqIKYqiKhQRGmEaEhmSKRkRGpkaEhmRoZGpGpkRmR"
    "GRqRKpnKzBERHKwcwcHCwRzMzMrMzBEcwRzBEcwUzMHMzLybmxERuRuZEZmxEbmbuczNHBzBHBERzB3MwczMzMzMzBzMwcERi7sR"
    "sYEYgRGBGxuBq4jLzMwhzMzBwRGIkoGhEagRGIETEREqgsokHBHBURyhEbIRHMFMyczCHMERHBEhEZu5kbERuRkZISkVERORoxEZ"
    "kaERGZJZzMIRERzCwcERmSKRkRGZkZERABABEQEAEQABABAAQAAAABAREAAQAQEBAhAAIAAAABAREQEQEAEAAAAAAAAAABAQEAEA"
    "ARABEBABIAIAABARAQEAERAgEAABAAIAAAAREAEQAREAEAABAAIAABAAARAQEAIAERAQIAEBAQAQABAAAAQAEBEBAAEQASAQABAA"
    "AAIAABEQABEAAQAREBAgAAIAABABABAAEAAAAQACAAQAEBARIBABEAAAAQAAAAAAEBEBABEQECAAAQAAAAAAABERABAAEQABEQAg"
    "ACAAABABABEAEQAQARADAAIAEAARABEREAAAAQACAAAAEBARAAERAAIAAAAAAAAAAAERAAERACAAAAAAAAAAAAERABERAQABEAAg"
    "AAAAABARAAERAQAQABAAAAIAABARABEREAAAAQAAAAAgEAARABAAEQABEQAiAAAQEBEQABABAQIBEAAgAAAgAAERABEQAQABEAAg"
    "AAAAAAAQAAEBEQIAAhAQABEAAQAREBAgAAIAAhARABAAEQABEQAgAAAAAQARABERAQABEAAgAAAAAAEQAAEBAQAQABAAAAAAEBEB"
    "AAEQASAQABAAAAAAEBAQABEAAQAREBAgADACABAAAQFBAAAQEAESEBEAAAEAAQAAACAQAAEQARAAEAEBAQIAAAAQAAAQAQEBABAA"
    "IAAwAAAREAEQEQEBABAAAAAAAAABAQAQAQEBAhAAAAAAAAARAQEQAREBEBABAAIAAgAREAAQAREBABEAIQAgAgAQEAAQAQEBABAA"
    "IAAAAAIQAAEQEQEBAhAAAAAAACAQAQEQAREBEhABAAIAAwATAQAQAREBABEAQQAwAAIRAQEAERABEBABIAIAAAIBEQAAERAgEAAB"
    "AAIAAAEQAQEQEREAEAABAAIAAAAQAAAQEBAhACAQEQAQERAAEAEBAQIgACAQEQEAERAhEBABAAIAICAAAQEQEREgEAABAAIAACAA"
    "AQAQEBAhACAQEQASERAgEAEBAQIAACAQEQAQEREBEBABAAIAAAABAQEAEQABABEQICAAAAARERAAEAAhAAEQAAAAAAAQAAEQECAA"
    "AAAAEAEAAQERABAAEQACAgAAEBEAEAEQABABAQACACAAEBABAREQABAAAQACAAAAAQEAAQERABAAEQACACAAEQEAEREQIBABAQAC"
    "AAARAAEQIAAAABERARAAAQASABAAAAAAAAEREAABEQAQABEAAgIgABAQERAQAQAQABAAAAIAIBAQEAABEQAQABEAAgIgABABARAB"
    "AQEQEBAAAAAgIBARAQARESAQABEAAgIAABAQAQAQAQEQARAAIAIAABARAQAAEQACAQABAAAAABAREQABEQAQAQEBAgAAAgAQEAAQ"
    "EQACAQABAAAAIAAREQABEQASAQEBAgADAhEAEBAQAwACEBEBEBEBABEQECAAACAAEBEBABERIBAAEQACACAQEBABAAARAAERACIA"
    "AyAQEREBAAERAgERACAAACAQERABEBARIAERACAAAAAQEBABAAERIgASEBEBEAARABERECIAIgACEAEBAAARAAERACIAAAACERAB"
    "EBERAAERACAAAAABARADAAERMgAyAREBEAARABERECQAIwACEQEBEBARAAERACIAAAACEQEBAAERAgICARERABARAAERASAgMAAy"
    "ERERABARABERECMAAwAQARABAAEQABABAQIAAAAQAQEBAAAQAhAAAQAAAAAAAQEBEAARABAAEQAAAgAAAREBAAEQAhABAQAAAAIA"
    "ABAAAQEAAAAAEAEQIAEQIQABEAEAAAAAEAEQABEBAQAREAAAACAAEAEBAQABIRABEAAAAgAAAAERAQARAQABEAEAAAAAEAEBAAEA"
    "ABIAEAAAAAAAEAERABEQABAAEQACAAACABEAAQAAAAIQEAEQAQEgARABIAAAAAIQAQEQEQEAARABIAAAIAAQAQEQEBAgEQABAAIA"
    "AAIRAREAAREAAREBIAAAAAAQABEAAQACEBERABEQABIBAQECAAAAABEQEAEQIQAQABAQEAEQIQAgABEBEAMQIQMgAREBABAAEAAA"
    "EQAAEAEBAgADABABEQEAEQACEAEBAAAgABAAEQAQERAAEAERAgAiAAAAEQAAEQAAEAEBAgAwADABEAAQERAAEAERBAAjAAARAAAB"
    "AQAAIAAQABEAEBEAEBEQAAACAwATAREAEAEAEAEQAQARAAEAARACAAAAEAEBIQAAAAAAABAQEAABAQEAEQABACIgABAQEQAQARAA"
    "EAABAAIgABABECABAQEAEQABAAIAABABAQEAAQAQESAAAAAAABABEQAREAEQAAEAAgAAIgABEQAREAEQAAEAIgAAIgAREAAREQEQ"
    "AAEAAgAAAAAREAABAQAAEAABEBEQAgABAAABEBEQAgACERAAAAEAAQAQABAAAAAhADACAAEQAQARABEAEAAhACACAAAQAQABAAAw"
    "AEABEAAQAQAAAAEQAAEBEQAQABABABIAEAAQAAAAAwABEAEAEQAAAAERAAAAAAAAAAEAEAAAAAAAAAABAAABAAEAAAAAAAAAAAAA"
    "AAABAAAAAAABAAAAAAAQABAAAAEAEAAAAAAAAAABAAEAAAAAAAEAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAQAAAAAAAAAB"
    "AAEAEAAAAAAAEAAQAAABAAAAAAAQABAAAAAAAAAAAAABAAEAEAAAAAAAAAABAAEAEAAQAAAAAACZAJoAoAmgCQDKAAsAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACgAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAkAAAAAEAAAAAAAAAEAAAAAABAAAAAAAAABEAAAAAAAEAEAAAAAAAAAAQABAAAAAACQAJCQAAAAAAAAAAAAAAAAAAAQAQEA"
    "AAAAAAAAEAEAAQAAAAAAAAAQABAAAAEAAAAAABAAAAAAAAAAAAAAEAEBAQAAAAAAAAAAEAEAAQAQAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAEAAAAQ"
    "AAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAQAAAAAAAAEAAAAAAAAAEQAAAAAAAQAAEAAAAAABABAAAAAAAAAAAAAAAAAAAAAAmQkJAA"
    "AAAAABAAAQABAAAAAAAQAQEAAAAAAAAAAAEAAAAAAAAAAAAAARAAEAABAAAAAAAQAQEAAQAAAAAAAAAQAAEAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAR"
    "AAAAAQAAEAAAEIARAAAAAAAAAAAAAAAAEAABAAAAEAEJAQAAAAAAAAABEAAQAAAAEAABkAEAAAAAAAAAAAAAAAAAAAAAAAAAABAA"
    "ARCQAAAAAAAAAAAAAAABEAABAQkAAAAAAAAAmZmZmZmZmZmZAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAEAAAAAwAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAABAAAAEAAAABAAAAAAAAAAEAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAQAAAAAAAAAAAAEA"
    "EAAAAAAAAAAQAAABAAAAAAAAABAAAAAAAAAAAAAAEAEAEAAAAAAAAAAAAAEAEAAQAAAAAAAAAJAACgkACQAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAQAAAAAAAAAAABAAAAAAAAAJAAAAAAAAAAAAkACQAACgAAABAAEAAA"
    "AAAAAAAQAQAAAAAAAAAAABAAAAEAAAAAAAAAAQABEAAAAAAAAAAQARAAAAAAAAAAAAAQAQAQAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACAQ"
    "EAEAAAAAAAAAAAAAAAAAEAAAIAAQABAAAAAAAAAAAAABAAEAIAAAEAAQAAAAAAAAAAAAAAAAAAAAAAAAACAAABAAAQAAAAAAAAAA"
    "AAACAAABEBAAAAAAAAAAAAAAAAAACZCQkAAAAAAKAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACgAAAAAACQAAAAAAAAAAEAAAAAAAAAAAAAABAAAAAAAAAAAA"
    "EAAAAAAAAAAAAAABEAAAAAAAAAAAABABAAAAAAAAAAAAAAABAAEAAAAACgAAAACQAJCgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAKAAAAAAAAAAEAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "IACgARAAAAAAAAAAAAAAAAEAIACgAAAQAAAAAAAAAAAAAAAAAAAAAAAAACAAoAAAAQAAAAAAAAAAAAACAAoAEBAAAAAAAAAAAAAK"
    "AAACAJCQkAAAAAAACgAAAAAAAAAAAAAAAAAAAJAAAAAQAJAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACAAqgAAEA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAgAKoAAAAAAAAAAAAAAAIACgAAoAAQAAAAAAAAAACgAAIAACAAkJAAAAAAABAAEAEAAQAAABAAAA"
    "AAAQAQEBABAAAAAAAAAAAAAAEAEBAAEQAAAAAAAAAAAAABAAARAQEJEAAAAQAQAQABAAAAEAAAAAAAAAEAEQEAEAAAAAAAAAACAA"
    "ABAAAQERAgAAEQAAEAEBAQAQAAAAAAAgABEAAAEBEQIAAgEBEQAQECEA";
#define TBDATA TBDATA_GR2
/*TBDATA2_OXIRI*/
#else
/* standart rus qoidasi bazasi */
static const char TBDATA_STD[] =
    "oBEREUoRERAkoAGqAqqaGhERkakRGSEZGpEjkqOYEaERiBEYESihEagig7obERFBFBERKxS7obuyuQkREBEpEZERlBABGQJZkhAR"
    "EZAZkRGRAhGQIZApCxERESoBEbEkEwsTCzqCGBgRgRiBGIEYEYEhgiiaEREaKRGRoZIRFBkqSZKoERGBGIERihKhgaEyKAoUEBEq"
    "GqEQpKABWgJKRKAREaAaoQGhAhqgJaBKBhsRESsRERAkswGzCzubmRkRkZmRGUEZGZEmkkWiGxERKhuhEZKxETorOqu5ERGBGYER"
    "gRIbgbsyuKqgEAGqEaoRphABGgJqIgkBEZESERlBCRGQMZAkQRARAUkRGREkEAEZA0kiiBgRgRiBGFEYEYExgiQLGxARShERELKw"
    "ATsCOkugERqRGhEBsQIaoCqgqgwcERFEARHAIsAMzAxETJQRGoEWERFBFBqBKoKmSxsREUsREbHCsRu7K5vLzBwcQRsREZwSzJHM"
    "MsmWEBARaREREZYQARkCabILARGhEhARKztBMLGgI1uwERFJARGxLBALGgu6kkUaEYEYERFaFKGBoYIoMGAQEaoRERCjoAFaAlpk"
    "DAsRsRIRASE8GzCzoLMDsBERORERECywAbsLuytAGxGBExERQRQbgbuCtgEJEBFJERERkxABGQNJorEREaG5ERohGRuhI6KzKBmh"
    "EYMRGBEooRGoIoO6sRERMRQRESsUu6G7srmZGqkRmRqZEUmhEakilMrMEcHByhEbLBzMscyyypzMwRGcERzBPMEczCzDjCmBEZEY"
    "gRiBGBGBIYIoqxERGyoRQbGiERQaK0qSKBoRMRiBEYoSoYGhMiiaqRkamRGZoUkRGhkqlJIqmRGhqZEZQRkakSaSRaaxEREqG0ER"
    "krEROiw6qysbETEbgRGBEhuBuzK4srkZEbkbmRFJsRG5K5QrmIIRkRiBGFEYEYExgiQDkaERYxEREUShEagiaLoQGxExFBERKxy7"
    "sbuyuczBzBG0ERHBKcEcySyTzAPMHMEZERHMFMzBzELMORAaEXMREaFFERoYKoiiA7ERMRMREUEUG4G7grYKEgERKhGhEZMQARoC"
    "SkMQERGQFJERQQIRkCGQNAs2EREqARGxIxILEgsrhiiBEYEYgRhBGBGBIYI1mzIRGykRkbGSERsSKym7qxERgRuBEYsSsYGxMrik"
    "QAEaKgGhoaQQBRoESqIKERGgFKARqgJRoEGgKpkZGRmZEZmRlREWGSRJIgqiEKASoRpRChGgIaA1ozABESoRoRGTEAEaBEo0AhIR"
    "kxIxETEyETBBkEMjKBgRKBGIEYgRERgkiAQKERChEhERMQkRoCGgNLtgAxErARGxshILEgsrxgASEZESEBHMAsHAwcDMCyQRESgR"
    "EbFIERsYK4hroBAQoRIQEVoKUaBBoCQjCgMRKhEREZITARMEOoQpEhiRGIEYQRgRgSGCNbthExsrETGxohEbEisqvCkaETEZgRGL"
    "ErGBsTK4uymRG7kRmbFJERsZK5SbqRKpoRmRGVoZYZFBkiQjKUQRKRFIEYQRERgkSLQDoRExEhERixSxgbGCuCMUIRFCEREQtJAB"
    "SQJZk5IzkZAZkQGRAhmQKZA5AxIREUMRERAksAG3C3SLlBOBgRiBEYEUGIEogjhCGhERNREREUShETgmNrq4MbExGIERcRISITIi"
    "IqowAQFaERoROhIhEiIiEgoREIEUERExOhEwIWAjghATAYgRGBEoEAEYAogiRxERcRZxEWEUEXEhcie0QAEBChEaELawAWsCWyMM"
    "GhChExEBMTwaMCqgo0IgEwEJERsQK7ABuwK7K0kZEYEZgRFBFBqBKoKoQgoBAUoRGRGkEAEZAlpCIjVSIRkRAWECGZAlkDkmsAER"
    "NRERECOwAToLOrsEG7ExGxEBwQIbcLtAt0KyERtjERERJLERtStjKyUQEIEUERFBAhFQIYAlAwMKEToRERAjoAGqAqqKIDmBMRiB"
    "EYESGIEogjgGoRERMxERESShETgmNrooG7MxGIERcRISITIiIoKwERuIERgRKLERuCqDKwcSETEWcRFhEhFxIXInkyAZEZMRGBEk"
    "oRGoIoi6I7GxIRYREUESG1G7MrakEBEAShEaETQSIRIiIjMLEwGREhERMSsRIEFgMkMwMQFIERgRJBABGARIMkcSEXEWcRFBFBFx"
    "QXI3gQMRAEkRGRG0EAEYA1kSoiABoxQxETE6ETAhoDMEBBIQSRERESMQARUERzMHIhExFnERQRIRcUFyNyoaExE1GqERkiERIiIi"
    "SpERkTEXERFBFBlxKTKXGxsRETQREbFCsRu6K0pLqRGjkakRGUEZGpEqkqnCEjPBrBzBwcLBHMwsrCwjEiHBEhERzBTMwczCy4YS"
    "ERF4EYERhhERGCJ4oikxEZoSERkmGTEhoSIiWqEREUgREZFIERoYKnOilRoTmhkRGVoZoZGhkimQYhERmRmREZKREVkiWWQqOxFR"
    "shEaIRkSIbIisgqhEREzERERQqERpypzKpASE5GzERkhGRuRK5K5AigREXgRgRGCEREYI0giJBkxkRMRESEUGZEjkpNLshERNRER"
    "sbKxG7orSssSy7ExGxERLBTLYcsytyIkMhIsERHB0sEczCy8rEJCEYEXEREpFEGBoYInKiQaESkRERFCoRE5KjhKGhqhoakRGEEU"
    "GoEqQqnMwTHBbByxwbLBHMssu8wZzMExGRERzBPMwcwyy8zNHMzcwcERsqWxEasZERlbFLGRsUIpAhsbETsbgRFCsRG4K5TLA8HM"
    "MRcRESwUzGHMQsuAIhERKRGBEYIRERkiV7MqNhOqEhEaKxmxIbEisguxEREpERGxQhEbGStzK5ASMZsSERkrGbGRsZIpBikRkSkR"
    "kZGSERYZJEkygkIRkRIRGCEYEZFBckQrJBsRKRERsUIRGxkrd8vAIRHMEhEaLBTBkcFCyTRSE1FyERERRIERWCJ4YyQaEZETEREh"
    "FBIhYiKiSqERoWQREREkIREiIyIiQxIxghIREUEUG4ErgrhSJBERRxEREUQRERYiVzJAkhFRExERQRQZkSlylCoiGjEzERERRKER"
    "NSozKgSxETsXERFBFBthq0K3AyQUETURERFDEREWJEijoBEREaoRCiEKERAjIKMLGwEbsBERECsQAbECssvAERARFBABIQLBEMwg"
    "zIgZGBqBGoEROBERoSKCurQRERu1ERohGrERu0K5zMwRHMERERIhmRAZEZARkRGZEAERApKyChERGxIQFCEKQRCxICS7sAERsAER"
    "ESsQCxELspKIihEaGBEYgROhEaEiKKqgGhGgGqEQpBABYQJSZAobERGyEQQhChEQsyCz3MABEdARERAsEAHBDMK8iYsREbkRGIET"
    "ERG7IriSCRkRkBGREZUQAREDQnICGqERGhEBQQoRECogqizAERFBAREQwhAMwQxC3A3d0BEdEAHRAtEQ3SDdzMIcHLERERE8ERzB"
    "LLK8JrARERYQEVEKsRCxICQLAhsRwRERELQQAbELQosjGagRqBEYgRMRESoiqDuxEbFRG6ERohEbsSuUyyzMwREcESERkjiiGBoY"
    "ERiBE6ERoSIomyOLEZEbgRE4ERGxK4LLLMHMERsREcETwRHMIsuZIJERkBGREZQQARECUrMKNhEbEhATIQqxELEgwtzAERDQARER"
    "LBAMEQzCvIsbGBsbERiBE7ERsSK4qgqhoaABoRGlEAYRBEIykiAUARIRFEEJERBBIEQMAhwRIQEREcwQDBEMQrw7soEbGxEYgROx"
    "EbEiuCmSE5GQEREQKRABkQKSYwIbEREUEQFBAhEQsyCzzMAxwMARERApEAEhDCKyJxsRGxYREWESERG7IrdyAhERgBERESgQAREC"
    "ciIgsBEBEBEBQQIRECsgu8sCG7GxEREQJBABsQuyuyeyERsWERFhEhERuyK3cwISEYAREREkEAERBHJDExkRGZcRF0ETEREpQpdL"
    "sTERSxuhEaIRG7ErQssdyxEbGxEhEaKGoRGRFhEYUROhEaFCJAoTGhExGnERMhERoSpEyiKxHBsVEREhE8ERyyK0CxgbESsRgREy"
    "ERsRK0SrJKERoRQREUETERGqIqULGwERsRsRELKxATEEKyuwERExGjEBMRIbECugtAwcERHBARHAIsEMwQwsLKmRERGpkRFBGRoR"
    "JJKizBwRwcEcEcHCwRzBLCy8tBERERuxEZsSuxG7MrJKEBERoAEaoSoRChEKSqKZkREaGZEROhkxEaGSIsbAARzBHBEQwsEBMQws"
    "bAIbMSEaIQEhEhsQa7CzJrkZE5EbEhGSsRGxKykrCxsQERoRASEbGxAkoLLcwAERwQERwNLBDMEMLcwCHBERFBABzBLMEMxAwiu8"
    "ERGxERGxQ7EbsSsrq7IQEBEaEBE6GzEQoaAiJgwLE7ERERDCsQGxBisrKhsUEbqhEUEaGxEkorLMwRHBwRwRwcLBHMEsLLwkGxER"
    "G6ERmxK7EbsysqzMwcHBHBvBSsEcwSwkvJkSGhsaoRE7GjERsaIiIys7E7EbEhGisRGxLCq7ArERERsRETsUuxG7QrIrYBERswET"
    "sSMRCxELS7uSFhobGqEROxoxEbGisisMFhCxARGxwhEDEQsru5IyQRsaoRE7GjERsaLCskAREKMRExCzkSFRIkJjDBsBERsRASEc"
    "GxBjsLIjIDERQxETECOxAbEMJGuiGhERGIERQRQaEWqCpCEJERCQERkRpBEBEQJJIsJQASMaIQEhHBsQJaBTJgIbELERERAisQGx"
    "Cys7AqIRERqBEUESGhFqQqQjCRMBgxETEaMRAREESSMqGhERqhERIRkaESSiorzCMRG8HBHBssEcwSwivBIbEREbERErFLsRuzKy"
    "IiwRE8wcEcHCwRzBLCyskhIRoRkRESoZMRGhkiIjKhsxoRsREZKxEbErKrvCshERGxERKxS7EbuysiwcHBHMHBHBQsEcwSwkvJIy"
    "EbEaERErGjERsaKyIyoaEVERERFCoRGhJicqChsUEbARAUEaERAkILLMwBERHAwREMIRDMEMIswEHAEREBABwRLBEMwgwlu6oREb"
    "GxEROREbsSsiq6oQGqEQEBExGjEQoSAiJgo7MQEbERCiEQGxDCLLIsAREREQATEcwRDMIMJbKhsaGxsRETkRG7ErIruiYEGxEBAR"
    "MRoxELEgsiYCGxEBEREQJBEBsQxCu6KxERG7EREhE7ERu0KyCBIREXAYGBEoEAEYAoiyYBERESQQEVsCsbCxQMZGEhERcRcXEUcR"
    "ERciZ6IkERERKBERShqhgaGCZHu7ERFxFxexJxEbEisimykZMQGZkBGZApGQMZApiYQRE4EYGIFIERgYI4gyBBMRAUeBEUECEYAx"
    "gERjJBERcRcXEUcRERcjR0QCEhARaRERUQIRgCGQJitgATGxAhGxJBALGwtLzAzMERPEEBHMAsHAwUDMeyIbAXEXEbEnERsXK3db"
    "JVIyE0IQEWkCUaAxoCYDAAQRkRMREQMQARgDSGQgEhERc3ERcRIRcSFyJgphERExEhGhJBEaGCpItie7ERFzcRF7ErEhsSKyiyAb"
    "EYEYGLEoERsYK4OaCIIxEYOBEYgSgYExgihjIBQRMRcXEUcRERcjR7QDsRsTchERexKxcbFytQsgERGQABuxKxALGwu7kkkSERGY"
    "gRGKFKGBoYIkCwATMKECEbEmEAsaC0qSCTIREZOBEYoSoYGhgiQpIBMQkBUYESkSIRIiIjICEwERSBERMQIRYDFgNHMgIREwFxcR"
    "JxABFwN3MiYSERFjYRFhEhFhMTI2QQISEJAUGRE0EAEZAlkzRDABEYgREVECEVAxgDhjIhQRIRYWESYRERYjY0MkEhERWBERQRQR"
    "YSGCJkpiERFREhGhohEaEioithi7ERGDERGLErEhsSKyMiQSE4ESEbG4ERsYK4hSRVIRE0IREWkUUYExgiYDJBQRcRMREUQRERYj"
    "WLQishsxOBERSxSxYbGCtnsQshExGBGxRxEbFitUqkAyEREpERFpFKGRoXIlQyIUETEVERFCEREXI1ODAhIREXAREXECERAhICgr"
    "YBEREQIRESQQCxELQswMzAERwBARwQLBEMEgwmuyGxERFxERJxEbEStimymSEROQEBGRApEQMSApgwISEQEXERGXEAERA0LELMAc"
    "McEQEcECwRDBIMRrIrsRERcREScRGxErYrsgIBERABARsQKxELEgJHMCEhEBFxERJxABEQNysyixGxFRERFxE7ERsSK0S2AREQEE"
    "G7EpEQsRCySrQxYREWGBEToUMRGhUmQrAhYQEwsRsSMRAxELK6sDMhERMYEROhIxEaFCZCokExERFhGhRBETESoltiRgEREBEBEx"
    "EjEQsSC0KhQRESGqoRGSoRFaInqTohGREYmBEZESGZEoMjgmGhERQRQRESKhEakmmZqZkZERmZkZkRkZkSmSOZYSERGRuZERkrER"
    "OSs5OygRIREogRGBEhuBq4K4mRIZEZEYmBGZERESIiIiKREREUkRGSEZEZEhkiRiEhERcRgYESYRERgicyKIgRERiIEYgRgRgSGC"
    "KKpCGjGhqqoRqKERaiJ6IyoaERM6GxohGRqhKqKkQqIREZEaGREpoRG5IpMrmZIRE5maGUEZGpEqkqmRKRkxkRmZEZURERkiSUIj"
    "FVERYhERQRQakSWSRkaiERGhFBERoqEROiY6uiMbsRGEERGBEhuBOzK4MyQREoESERFEsRG4KogrRRIREWkREVEUEYExkiYGJBsR"
    "kRMREUOhEbkmaZsaGZkxmpoZkRQakSlCOaaxERGxtqERorEROiw6uxMbsTGDgRFxEhuBqzK4m7yRG5ObkREilxEYMYmBGHEUEYEh"
    "QihjG6sxs7qqEUSxEboilLsDsbERcxERMRQbgatCuJkikRGRGJgRmBEREiIiMykTEREqERkhGREhQaIyQyIREXEUGBEkEREYJEMz"
    "iBIYEYiBGEEYEYFBgiiSKaERoRqaEaURERojSiKSMhkTSZEZMRkRkUGSNQQkExGRFBERQxERGCRJRJYhgTGJgRhBFBGBQUI4SVIS"
    "YZEYGBFJkRGJIokjJBURETUREUEUGZEmYlSCojGxMRgYESchESIiIiJIEhESKIERgRkagSqCmGIkERFxFhgRRhERGCJ3MkRSERGp"
    "kRFhFBqRJpJWQiQaoTEWEREkkRGlI2M6A6EROnNxEXESGoEqMpdCJBMRcRIYEUYRERgiZ0MpFREVkhkZgRkZkSWSOaayMRFRs6ER"
    "kiERIisiqxQaERpzERFBFBpxqlKnQrsRspG7sREihRIREXgRGFEYEYEhgicDKRoRk6OREZKhEakimapDohETORERIRQakaOSozsb"
    "G7Gxi2ERQoAiERF5ERghGBGRQXI3QyQZEXkWERFEkRGYImiZKRoZGJEZGZETEREpIjlGohEREZWREZIRETEmMronHBEccREhESJ2"
    "EhgRcREYYRMRESEiKFIpmhETqpERNBERoSKSuiOyERtxERFBExERuyK3UycSGBEXgRE0ERERJIIjJ6IRsWEREWESERErIreSQqER"
    "EZoaEaahEUEiImMpGxERoRsRIRobEcOisiaiERERFhMRI6ERoSYjapISGRGRmxFBGRsRu5KyIiiBEREWGRFzERERIikiklIaM6Er"
    "ESEaGhElolImJxoRERYREUKhEaEmKWqisUERs1sRQRQbEctCsiMoExgRFhkRcxERESQpI3JSERFhMRExFBkRJZJUJiobERG6ERGS"
    "sRGxKyprgqJBEREZEUETERFqIqJHIhEREXcYETcRERIiIjIkExERYRMRMRkRgTGSNHMyERERdxYRJxEREiMiM0YSERFxchFxFBFx"
    "MXImUiMRMRGCEhFCEREZI4kyRDIREWGIEUEUEYFBcjRDJBMREWcREUURERgjZ0QDIRERM2cRcRQRcTFCNkIkERERRhkRRBERGSNJ"
    "IicyERExZhFhFBFhITI3RCQSERFYERFEEREWI2gzJSIREREXEVETERExIjYzJBMREYETEUMREREjNaOgERARqxoKIQoRECKgogQa"
    "ERohGhEQKxABoQKqysARERFCAQEhAsEQzMDEhBkRGiGmgRGEERGhImS6uhERG5QbGiEasRG7srmWEAERkRaREZYQARECabIKERAb"
    "KgEaIQoxELGgIluwEREhFBARLBALEQu6koUaERqCERhRGKERoUIooGABESGqoRCiEAFRAlpkChsQESsbCiELERCyoLILsBERIRMR"
    "ECIQAbELuyuAGxERNBsYIRgREbtCtgIJARGRGZERkhABEQNJIgoaoAGiEQEhChEQKqCqzMABEcMUEBDCEAzBDEzcAt3RMdIBASEC"
    "0RDdMN0swxwcsRERFCErCgsRIBkQEaIQCxELmrKwsBAxKxEBIQsRELuwujSRERohpoERgxERoSJkuhMbERtQGxohGrERu7K5WRMa"
    "oSEYgRGDERoRKnSyMLERETAbGDEYERG7QrbcEMwcsRERFCGgIBEQoRKhEZIQARECWrMKNgEbLAEaIQqxELHAwgzAEREhEhARIhAM"
    "EQzMvIASERskERghGLERsUK4BgoRoCESoBGiEAYRBEpCokABESoRGiEJERBBoEQMDBwQwxIQEcIQDBEMzLwwsREbIBEYMRixEbFC"
    "tzRSE1IhGREQthABUQKSYwobARE1EQEhCxEQsqCyS7ARsSEbERAsEAEhCyMiNBsRG0MREUEUERG7MrVSChEQIRQREaQQARECWDKw"
    "oAERMhEBsQsRECqgqisCG7CzFBEQJBABsQtbuzSxERtDERExFBERuzK1BAoUAZEVERGjEAERBEcjKRkREzIZGSEZEREjQpOrsiER"
    "S7OhEaIRG7ErkquCQhGRchEYIRhBEaFCJyopGhFBopERkhERMSo0yjSyHBsUEUERsoKyEbEkERghGLERsUK3KiMaETMTEREkEREx"
    "KjR6CRoRGqEaCkEKERAqIKpMwBERHMTAEEIQDMEMQryWsBGxYQEaUQqxELEgJAsJGxERvLEQshABsQtCy5AgEcEhARwhDMEQwSDE"
    "ywIbERsbERDEEAGxC7IrChsBEbEbASEaERAksLLMwBEQHM0QEMIRDMEMLcwCHBERIQEBIRLBEMzAwiu6EREbshERohEbsSskq6IQ"
    "AaGxAREhGjEQobAiJgobMBG8ERCiEQGxDCzLwsABEdMBASEcwRDMwMIrGhsRG7IREUMRG7ErJLuiYBGwwQERIRoxELHAsiYKGwER"
    "GxEQohEBsQsru6KyERErGxEhGrERu0KyLAQcEcHBEBBCEQzBDCJMChIBESEWEVEKERAhkCarYBEQESsQEaIQCxELK8wMzBERIQwR"
    "wQLBEMEwwmsjGxERcxERRxEbEStjW7VSIRMhBBFhC1EQMSAmBAoUEBE5ERGkEAERA1jEIsAMMcMMEcEAwRDBwMxrELsREXAREUMR"
    "GxErU7uwMBEQIQIRYQuxELGgJUQCFAERWBERIhABEQNYszKyGxExFBFBErERsUK1zALCERHBEBHMEAwRDEK8omAREBELEUEaMRCx"
    "sLKUUhFREWKaEZQREVEiYmMpGhERQZkZIRkREWKiolIpERERYoERlRERESJkIpCiETEhoxlBGRERKkKmSyQbsREXERQhBCkUERFk"
    "gRGTERERJGRDRKIRoSEUEUEUEREqMqUmKBoRESEZEXIREaEmJEpEQhEREWMRURQRETFCNgQbERuQFaEQK7EBsQLKy8AREREkAQEs"
    "EswQzEDDSBkRGoESgRFIoRGhIli6tBERESsRESscuxG7srPMzBEckRyRwSnBHMEskqwKGhEBSQEaqhKhEKGgK2mkERGBEoGhRhEa"
    "ESpIMgIcEQEJEQzBEhwQzMDMKrQREYESgRFIsRGxK4hLAhqgEVoRATESGhAqoKUswAERwRIQwCLBDMEMPNwN3dER1AEB3RLdEN1A"
    "3czCHBzBEhHBLMEcwSzMvACwEBELEQEhEhsQu7CzCJIRGjEVgREooRGhIli6EBsRESMRESsSuxG7srO8whwcMRyRwSnBHMEskswJ"
    "zMERLBEczBLMEcwyzGkgGhExFIGhJhEaESpIMgKyEREDERiBFBsRu4K4PBDMHCEcEcEswRzBLMy8AxIRAQkBG7sSsRCxsLw7pBER"
    "gRKBsUYRGxErSLsDMhERAxEYaxSxEbGCtEzAIcFwLJEQJ8EBwQwnvCcbERsnEBdxEhsRu3K3SwIbsLE8ERAjsQGxC8s7B7IRG3IT"
    "F1ESGxG7crdEkhGRkRUREUSREZEiWbkkGxERJRERKxu7Ebsis8yxHLExGxHBLLEcsSxyK0MiMRIsERHMHcwRzMLCWiQaEYEUEZFF"
    "ERoRKjgyQKIRETkRESEUGhGqgqMsIsuxsRYRwUKxHLEsa8sIwRwcIxERzBTMEcxCzDskExGREBGxRhEbESsnOySiEaGIExEhEhoR"
    "qoKoJrARsQESERAkEQGxAlLLAhwRESEBASESwRDMIMPd0B3QAR0QEC0RDdEN0s0rzBEcwRERwRLBEcwizKoCGhEBEhARKhEKEQpC"
    "wiLAEREgEQFBHBEQzCDM3QLd0REdEBAtEQ3RDdLNK8IcHMEREbESwRHMIszLAhsRARIQEbQRCxELQssswBHBIBMBQRIREMwgzMwS"
    "yxEbGxERPBEcsSyyywQcERFAAQxMEswQzEDCS7wREREbsbFJsRuxKyu7BLIREbERGlsSuxG7QrIrLBsRERsRsUuxG7ErK8skwBER"
    "AQEBQRLBEMwgwruwGxEBQ7CxKxELEQsruya7ERExEhVrErERsTK0ayKzERFCUbEmERsRK0O7JjIbETEUESsUsRGxMrO7ArsRESAQ"
    "ESsRCxELQmskFREVkRUYYRQaESUiR0aiERERKhERoqERMSY6uiQbERtxGBVREhsROyK4gyQRIREogRGYsRGxKigrRRIREZEUFVEU"
    "ERExkiQmJBsREQmREUShEbEmabtFshEbkRkRMRIbEbuSuTsQG7ExI2ERRrERsSqES0MyERGREBVBFBERQZJDQiIaERpnUxFGkRGh"
    "JGeqRKIRoZESEUEWGhGqkqdbIhsRGyERETQREbErgmtyohERERYTMRQaEWqSokMiExERMYQRRBERESJDNAoaARohFQFBGhEQKqCl"
    "zMAREBEsARDCEQzBDDzcDd0RHSEdANES0RDdMN3MwxzBESQREUwRHMEsw6yloAERIRQQURqhEKGgIwsMGxARCxEQwhEBsQs72yLQ"
    "DR3THQDRENEQ3dDdzBPMwRHAERFDERzBLMO8s7AREMEQEMEbsRCxwLI7AhsBG8sTECIRAbELy8syshyxQRQRIRLBEctCtd0C3REd"
    "IQEQ3REN0Q3SzcLAERAREgDBHMEQzMDCywK7ARHBBBEiEQsRCzy7Q7IRsRF0EUEUERG7QrcoEhERgRaBEScRERgiY6JiERERJBER"
    "KhKhMaFiY4gSGBGBGIEYiBERGCKIsmkRERspEZErGbGRsZK0i7sREYESgbEoERsYK4iqKBkREZkRGYkSgYExMimZmRmRkRmRmZkR"
    "GRkjmTIkExERSBEYQRIRgTEyJII4GBGBGIEYiBERGCNItCO7ERFEERGLErFhsTIlO7QbEYESEbGYERsYK2iKGRIXEXkRh4EUEYEh"
    "QiejYRGxoRIRtHIRGxUrSDwTNBERcxEXixKxgbEyp5u5m7GREZkRkqiRGRqaEZmaFJGRMUIpgxl0EZEWcRhIEREYI0Q0A0ETEWMR"
    "ETsUsWGxQqaqIhERkRSRoSkRGxkrk6KaEhkaqROZmhmhkaGSJIMaE7qhNqG6SREbGitEMicSExFzERdxEhEhMSIicjQREXEScRFG"
    "EREXI2czA0EREWMRF2ESEWExMiZ0IhERcRYRGIQRERgiaDIpFhOhKTGRKxmxIbEisnNBExFBFBOxSREbGCtYu5myIbEZkhkhVSkV"
    "ESkZE5mXERUZI5kycEIRETcRgSEYEYExgkQzJDQRcRYTsZIRGxgraMvDwRzBFpMZIQMpExGaMhOpkhEaGSpJMkRCERFHERFBFBFR"
    "MWI0hyKBEREWERg4ERERImKiJBYRkSERQSEXoRGhImR8shwRERcREiGaKZgRGRkRGTkRGREjkjKHIhgRURGBgRMRETEiRDsiuxER"
    "FREROBEbEStCq5oikaFRE5GRE6ERoSIkcyISEREVEREmERERI1KiJBYREZERGZoSoRGhMmKbaZERGxmRsZMRExErKTuWYaGxoREa"
    "OxQxEbFCwiMpFhGxGROxkhETESspq5VikREZERExEzERoSJiiiIbERF3gbEnERsSKyKiSKIREYESGIoZoYGRgigzEDMREXNxoScR"
    "GhgpczJGMhMRcTQRaRmhgaGCJmoishERcRERJxEbEStigikSGRGRGJiBGREhISIiczIRERFJERiCEREZIzYzJhMREXEWGGESEYEx"
    "MiaDKIERERiIETKYMhkxkRmZQRkRgSGSNCMkExERWRERRBERFSRZNJcxcRFzgRQhYiQTERFncRFGEREYImczdDIREWGBGCGUIhER"
    "EVIRFZQREREiVKInFhGRQRKBIRihEaGCYlUpFREaYhEZlxEVESNiMpBCERFBE1FBGRERMUJFeyS7EREVERQhCikSERoiExmWERoR"
    "KlQyREIREUEUEUEUERExMjQqKRYRkSEREZQRExEqJKZGQhoREUIRYRShEaEyJYuyGxERSBG4KBEbESuCska7ERGBEoFrFLERsYKi"
    "MxA0EREjEbdGERsRK0Q6RkITEXESE2sUsRGxgrN7IrsRESERETcRGxErQptAERARmhmRIAkRECWwNQkZERkgGREBKRABkQKSY7AR"
    "ERFCEREgAhEQuyC5iBkYGEGIERiIERGBIoRjpxEREWQZgSEYEREjIqOZEAkRoBkRGZkQARECKiIHERARShGBIQgRECGgJIIQEREg"
    "FxERJxABEQKCInZxERFyEXFxFxERIUImqkAKEbCqEQqmEAFhAlsjChoQEUsZoSAKERAqsKRCsBERIBwRASsQAbECsiuJiRERlBqB"
    "QRgRESpCqJEJCRGgGREZlRABEQJKYgMVYBGaERFgCREQJbBJprABEbEUEQGiEAExDDIrhBAQEVIREVEIERAxoCQGCgoRsRsRAaMQ"
    "AaECS4ojGYgRgBiBgRgRESgiSDahEREBlREYghERMSYyKjYSFxFyEXFxFxERMSImUiOJEQGpERiEERGhJoKqCRIJAZoRkWEJERAh"
    "oDJzMBEQsRIRGIIQAREEKzMEEhEBQhERQQIREEEgNGMncRFBFxEXdBERESRkI5kQCQGSEZFRCREQMbAkIgmTEKAUERmTEAERBFpD"
    "gDABEUoRETEIERBBoERUIxIXARcRF3QREREjYqQKJQEGoiERoAoRECggOqJQERChFBMBpBABYQJaJQsbEwuyMRGQAhEQIiAiYpMR"
    "oWERERMhcgkRECAXExGXEAERAnkipFABAQsxEWAKERAmsFZyAhugoRoTAScQAaELqqsGkhEaFhEhESKUMBEAKTERYQkRECGQNoRS"
    "EWFBhxEYhBERUSJ0ZCcaExE0GYEhGBERYiKiQicRESEVERZ1ERERI0QigJIRETQZgSEYERFpIpQEJxMRQRURFnIREREjUqQ0khGR"
    "FBFBEZIJGRkZkBmTkAkRECkgOTYgERERtxEHQhABMQsyK4gQGBGAEYOBCBEQISAokgmSEQG5MQmUEAGxArI7iCCBEYARg0EIERBB"
    "ICiiQKEQAZsRAaQRAUECK2MKGwERwRsRIBoREMPAsiawERExEhEBIxEBsQwia4IaGBEhGRFBGBERakKiIgmREAEaERGTEQERAioi"
    "olAKE7AZESAaERAlsFImChsQERsRAaIRAbEMK2uSokERIRkRQRQREToioiMJEwkBGhERkxEBEQQqI6JQERCjMREwGhEQJaBUJiga"
    "ERGSERGCERGhIyJqQiBBEQEbE0AUERDLILKmIBEQAXIRETcQARECKTIHEwERoRQRMQgREDGgNGMgEREBchERJxABEQMiMzYSEREx"
    "FRFRExERMTI2UiMRMiGSERGSEAERA5IylDABEZAXEVEJERAhkDRDABMQEVkREQUQAREDSUQ2IhERMRURURMRETEiRkIJEQEBQhER"
    "lRABEQNKIiYwERBwNxFxAhEQQXA2RCMTERFTERFEERERI0Q0JiAREQESE3EIERAxICZDCRMBEWMREZMRAREDOZMpFRkWIZmRkRkR"
    "ESciOXJSERERZBkYghERMSIyJXYSGBEhF4FhFxERIUImkimVEROUGRmUERFhImQldjKBEUESgUEXEREhQjZiIxoRGhYREyEiKBUY"
    "ESEZEXQREVEiJCU1MhEREVMRURIRESFCNpZQEWARkhkJlhABUQKSYwobAREhtKEgChEQsiCyQggREBFSERWFEAERAkKyoKABEUGr"
    "oSAKERAqIKQECBIBEZQRFYIQAREEUmOisBEQEbIRIBoREMsgskQJEgERIRURlRABEQNCUwoVARWwFRFAGhEQJbBFprAREBErEQGi"
    "EQExCzvLBBwRHCASEcASERDMIMxrsxGxESQRETcREbErYyuWEAERIBURYRkREDGgJCYKGxABCxEBpBEBsQJruwWwARvBHBEwEhEQ"
    "u8C8WyMbsREgERE3ERGxI2JLlDAREKAQEUEZERBBoENCAhsBC2ozAQQRAbELaqs0ohGhQRURQRQRETpCpHwCEhESIBEDdBEBwQzC"
    "bKKwERAxEhEwGhEQy7CylyKRERFiEYGXERERIiQyJxMREUEUESUWERExIjNiKIEREXIRgYIREREjYiJ3MhgRQRYRSBgREUFCNGMn"
    "EhgRYhOBhBERESNEQkYyERFRETERMnQygREREhExGBERMUIyCxYBEbGyEQuhsAEbAiomUBExEToRERACGqAhoFoMHBERwRIQAbzD"
    "ARMMLCxZETERORqRERkakSGSWcwcEcEhHBwcIZQQARGRFREZkRABGQIpYgoREBsqAaGhChGgYaAiJUAREaEUEBErEAEUBSpSkhQR"
    "GkkRkaEZEZFRkiKiQAERoVsRWqFQARoCKnQLHBARKxyxEAocsHGgwiawERGhEhEBIbABFAYqK5IbEREpG5ERGRuRYZKyIgkBEZET"
    "ERmREAEZAykiCxUgESoRERAKGqAhoFrcwAERwRIQAdzDARMMLFyiMBARKgERoQoRoFGgIiYLCxGhEhEBobABGgYrKxoVMRE6G6ER"
    "GhuhIZJZzMERwSEcHBwhJRoUsaEUERqqEREUJSrCorERESoboREaG0FhkrKiIBEQoRIRGZEQARoCKcMLNwEcKwGxwQsRsMGwcitg"
    "ERGhEhARKxABFAsqtpISERspEZGxGRGRsZJiJAoRUKESFRqlEAEaBCoyojABESoRkREJEaBBkEIrDBYQoRIQEcsQARoLKraiwREb"
    "KxGhsRoRQbGiYjJCEzIhExFBsUABGQIpYwwbARElEREQMhswYaCyIgoREHEUERGhEAEYAkgiwlABESoRERAMGqAhoFIjChMBcRIR"
    "EbEQARgEJyMlFRESKhIjERIaoSGSU7zCIREsHBsaISUpExGWEjESKhERGSUpslKyEREqEiIREhuhYZKyKyobEasSIRIqEREaKypG"
    "ChURFmEaoRAKERAhIFRFChURGxMQE6ETAREFIrKisBERMRsxEDsREGEgsisKHBEbEhATsRMBEQsiJgsWARGxGxEQGxsQIbBi3dAR"
    "EB3dEAHd0QERDS3NAhwREcEBEcASHBDBwMIszBERHMwREczBEREsLGyyEAGxsQERsRsREGGwIicMHDARzBEBwcEBEQcszNLAARHB"
    "ARHAHRwQwcDCLBwcERzMERHMwRERLCzMwnARwMEBEcEcERDBwHImDBsBERoRAcGxAREGKsuywhERvBwSwRIcEcGywiwMHBHBwRAB"
    "wREBEQwiLAoSARGRExERChGQIZAiy2AREBEqEBErEwETCyq2ArwREUEEEbECEUCxQGIqJBoREUgRESkRERgqKEayQiETIUQRQQsR"
    "oDGgIiMKExARKRERoRABGQMotCLACzGhAhGxAhGgsaBiKhCmERFDEREpEREUKihWwiAREKECEbEMEaBRoCIjAhMBETcRESEQARYD"
    "SKNCYhoRkRITkRIRkaGSYisCvBERMRARIRMBEQsitsJgERARChGxHBEQsaBiokIRMREyFBqhQREaIilzKhsRESGroREikhIREZEU"
    "kREZEZEhkiQiKhUREyobGqGRERoiKTWiMhERkRKRERkRkUGSMicpGxERoRsRobEREScqKwsVAROxo7EQChEQIbBUJQoUEBs7AROh"
    "EwERBSvCsrABEcGyMRA6ERBhwLIrCxwBGysBE7ETARELK8bCwBEQHM0QEB0REMHQwrJQETARKhEBsaEBEQIqxQ0cARHBEhDAEhwQ"
    "wcDCLMAcwREkAQEswQERDCS8QrsRG6ESEbESGxGxorIlCxQQESoBEbsRAREFKmLCsAERoRIREBwbEGGgsiwCzMARLAEBLMEBEQws"
    "vAKxGxsxGRGxEhsRsaKyKwwWAREqARHLEQERCyq2IrARMKEyERASGxBhoLIrJLsRG5oREyuxERErKssiwBzBERIQEBIREMEgwpIi"
    "ERERNxGRYRERGCIosioXEbEhEZoRQqJCEUEhFBFKGhGhMZIiIykTEREnEZFhEREYIyhUoiIRsaEyEZoaEaFRkiKUQBFAEVoUkZQQ"
    "ARECKyODEAERoRMREzYRECGgJCIKFRAROxoxoBMBEQIrNYIwERCxFBETNhEQMaAyCxsRGyAbsQGxsBG7ArLLwBEREUIBETAMwzHM"
    "IMwoGREaMRiBEYGhEagiiLqyERERQxERsRK7sbuyuczMERwhHJERnMEcySySnCgaERGDERihGKGBoYIowsARESAcwQHBwBHMDMKs"
    "KRsREZMRGBEYG4G7grjd0B0dIR0QAdMwHdMN0s0szMERwhERwRnMkcySySiSERohGIERgaERqCKCuhIbERFCERGxErtBuyK5zMIc"
    "HCEckRGcwRzJLJLMKczBEZwRHMEczMHMIsyJIhoRIRiBEYoRGhgqgjIqshERohEYERgbQbsiuDwSzBwhGhERnMEcySySvCsaERGz"
    "ERixGLGBsYK4myITESEZgRGLERsUK4LLDBwSDMIRGRAJHJHMIMl7shGxIRchESGxEbcrdzsnshEbchESERIbcbsitySSEZExFxER"
    "IZERmSJ5uSIbEREzERGxEruhu6K0zLEcsSEbEREjMRyzLHIrIyIxEiMREcEczMHMwstaIhoRMRQRESkRGhgqdzIgohERMxERERIa"
    "kaqCpywiy7EhFhERLLEctixkyynBHBySERHBHMzBzCLKCyITETESERErERsZK3c7JKIRoXIRERESGmGqQqcrsBGxARsRAbEQEbEC"
    "sssCHBERQQEREALBEcwgzN3QHdABHRAB0RAd0Q3SzSzMERzBERERHMERzCLMuwIbEQEbEBGxEBsRC7LCLcAREdARERAMERHMIMzd"
    "At3RER0QAdEQHdEN0s0swhwcwRERERzBEcwizNwCHBEBHRARwRAcEQzCzCzAEcHAEREQAhERzCDM3BLLERsbERHRERyxLLK7RBsR"
    "EbERGrEZuxG7QrJLIhsRERMxEZMxG7ErIrsishERsRERsRK7EbtCskwCHBEREBABwREcwQwivCi7EREhGDIhEiGBITI3iyKzERGC"
    "IRIiERIYInKzKDIbESEUMSESIWEhMjjMAswREcAQEiEQEhECkrwoHBEbIRgYERgcgSwiyIMiESERI4ERgbERuCqIuyOxERsjGBgR"
    "FxuBqyK4SiIaERpzEREhoRGpKnm6J8IRwREXEREXERG8IsckoBGgEaIRASEQEaECosoCHAERIRQQEALBEcwgxN3QHdER0gEBIRAd"
    "0Q3SzSzMERwhHBERHMERzCLLWwIbEBFCAREhEBsRC7KyILABEbETERACERG7ILstAt3QMdIBASEQHdEN0s0swRwcIRoRERPBEcwi"
    "ygwCHAERLAERIRAcEQzCvCSwEbCxGxEQAhERuyC7LCLLERtCEREhERyxLFLbLdAd0REdEBAN0RHdIN28whzBEcMRGZzBHMEslMwq"
    "whwcIRMxMRnDEcwiycwiyxEbMxERLLEcsSx0uyi7ERExEoGxGLGBsYKoMxI0ERFyEReLERsYK3I6J0ITETEUE7ESsYGxcrUpkBGQ"
    "AZIREZAQEZECkmMCGwERIRQRAQIREbsguczAEcEBwhERwBARwQLCvCYbERshFxERFxERuyK2ggIREAFyERFxEBERAoIiJLABESAc"
    "EQELERErILsrAhuwEbIREcAQEbECsrsmshEbIRcRERcRETsitkMCEwEBSRERQRAREQJCIyuwEbAgGxMBAhERKyC7KiIaERpSEREh"
    "ERGhI0IqAhUBE6ETEQEBGqEhoFoswBEQESwBEcDDERMMLMwCzBEcIRwQAQMTMcEgyiuyG7ERkxERsbERGitaWyJAARGhFBARCxFB"
    "UaAiJgIbEBEqEREQsBEUBirLIsAMHEEUEAEMHMHBQMQrEruxEZIREbGxERQrUrsiYBEQoRIQEQsRQbGgYisCGwETNBERELARGgaU"
    "uyKyG7ExFDERGxuhsaK0mRoZGpGlkRGRqRGhIlnKzBERHCwcEcHCzBHMwsKbvBEcsRmxEbvCG8Ery6yZmhEaSREZoZmhEaGSJCK5"
    "GRGRsJERkbkRsSspu8O8wRGbERGxy7wRvLLMl6GRGpGlkRFxpBGhIlQ6HBMRHCwcEcHCzBHMssIzMRMTsRSxEbvCG8Erw8zMzMwc"
    "zBHBIXoZmqGRFJERehQaESpEMpIxGhEKGxkRRxsRO0KyMxAzE7EUERE7xBvBK8S8kxIZGwkRGbGZsRGxkrIzGRO5kRCREXsUGxEr"
    "JCO3GxEbiBEYEbcbEauCuDMQEzFxFGERUbIRsSMjSykaERpZGhERlBoRKpKlszIxEbzCEREsyxzBLCI8GTMRE5QREbFLuxG7srvM"
    "zBzCLBwRHDKQMhERCRsREZIbETuSsiMsMzGxGRERK7wbsSury8PBHMzMEcEhQyQTEXMWERFRpBGhI2hqJxoYGiEZERE0EREqIqVL"
    "shERG7IRESEaG7ErIrsrvBEc0REREpKXkhmRIRERETeREZEiJCMnixERshERQRMRsSsiuyPCGxzBERERO7ERvCLMOikTGRoSERFB"
    "ExoRKiI6JbIRsSEREREkERG7IrKsy7ERHMqxE6zLHMEsKzy1MbHBoRw7wUXMEcxCwmokGhERSIERahQaESpHOgMxExExFBahJqER"
    "oTIym7mRsRGyIRkyxrGRG5MbQyFDKRsRG7kxGTIoGhEaIZUREYQRESpCpauyEREbIhsRIRobsSsrm5WSEZEhFBERlZERkUIkAygb"
    "EREyGxEhGBGxKyS7SsIbHMERERSikzIRoSEQERGXoRGhQqIzJBsRG0QREUEUEbErU7vJvBEcsRKxscq8EbyywjMQMzEROxEWq8Ib"
    "wSsjvJm7GbEhEZEhexmzGRsUERRSKBUYFUGVERGEERElQkV2ohERESIZESEYETEjIiqGEhYRIRQREYURETFCJCIoihERAhkRQRgR"
    "oSQkSoMyYREhEBERhBERQUJCRCQaERoWETEhklIRMREpGhERqRERIinFLBwRwSEcERxSokIRoZESERGqERFRoiIjKRsRESobERG6"
    "EREmKbuiMhGxkRIREasREbGiYpvEG8ERmxEbscsbwRvEPCMxExMxFDERo7MRvCHMdxIXEUEWERdhFxERInKSiIgRGIQRgRGIgREx"
    "QihzNxcRQRYRF2EXEREjRHIjEhcRYBFxEXYRESEiN4kjiIEBGBEYgRgYESSCMjdCFxFQEXERdhERQUJEmSiBoUEZERiBGBoRKkSS"
    "OSKBGlARgRGIoRGhIkR4IIERARgRGIEIARECgpKZkBmRkAGREZmQEDEgKYMHgxEBGBEYgQgBEQNCtJwgsbHAAbERu7AQsSAkamiB"
    "ERkSERExFxMRKiSmNWKBkSERERFDMRGhIjJpIxoRERYREyFpIKQRERYREiGVIBEQEWIRGEEJARECYlKlUAGRIRWgMaVQEDEgJAMJ"
    "FBARkBEYIQkBEQNAtKCwEbABEqAxorAQsSAkW7MbEREkERFBExsRK0O7NbIbETESERE0sRGxIjR4IIEQAXIRgXEHARECIjJ4MAgR"
    "IBcRGHQQECFANJQgERARShGREQkBEQIpQqRAAUGxExQToRMQMbAiIwkTEBE6ETEROQERAylToiARoLESEBOhExBRsCJ7shsREWIR"
    "EREWGxErcrsnshsRIRYREWGxEbEiNwoUARGhohEKIQoKGgI6Q1ARYQFaERoQKqCgIaA6lBUZFpGVkRmRGRkZIpljCqEQAWoRqhGq"
    "EKAhoCqSEAERkRIRGSEJARkCOSIFEREBSREZESIQkCGQKVIYGBGBFIEYIRgRGCKIsgu0EAG7FbsVu1CwIbA7srABEbGyEQshCwsb"
    "AjsrBBsRAToRGxAssLAhsLyiqRkRMRoZGSGhCgoRoBqhGqEKARoCqiIDFVARWhEREKCgoCGgSiIJARGRFBERIQkBGQOZYrCwEBEr"
    "EREQsLCwIbCzpFKRFqGlkRmRGRoZIpkklRIYEUkRiBGCEYExgiiqKqsRMRsaGiGrIKEQoBuhGqEKARoCWjMKFAERKhGhEZIQoEGg"
    "MzMgERGQFJERQQIBGQRJM4MSGBGIEYgRiBGBQYIzUgqhEKAaoRohCgEaA7oiojAKASoRqhGkEKAxoDUEChMQoRIRETEJARoEOkST"
    "QoERiRGIEYgRgTGCQ7RAEkCgFaFBoQsEGgIqI6UQAQFJERkRohCQIZApQgsVELATsQGhCwobArslozARACkRGRGpEJAhkDmSUhFR"
    "kZURGQEZGRkimSSCEhERSBGBEYIRgTGCJwoqGhEhGhoZIQQoFBGREhEYARgRGSMnZAoVGhVgGqEQpBAQISA6QgmREQEUERkhCQER"
    "AkIylCCREUARkRGUEBAxIDOiQKEQAaqhASEaChECKmMMHAERwRwREMLBEMHAwiawERExGjEBIRILEQwra5IbGRGRGxkRkrERsZKy"
    "IgmhEAEZkREhGgERAioislALE7AbGxCysRAhsFImDBsQERsRASEcCxEMK2uiskERsRsaEaKxETGisiMJFAoBGZERURoBEQQqQwsS"
    "ARGQFRgRMhCQIZAoUiMRMiFCIREhCgEaA6oypTABEZATGRGpEJAhkDkyChEBAUqRESEKARkDSkIqFBoUoUWqIapBoSGiMlIpkRER"
    "SZEZIxkRGSKZIqpSGjExqqERIpMykRGREpkxmRGRMZIzo1ARUBFSGgoBCgERAqokkxABEZEUkRGSEBAxkCQ0ChQBEUoRGgEJARED"
    "KkQKFQEVoBKhEKKhECGgQMawERARKxEBIRwLEQs7ywQcERyQEsEQIsEQwcDAi7QRsREoERghFBsRKwgrpRABEZASoRGiERAxoCAm"
    "CxsQAQsRCyEbCxECO7sCsAEbsRIRECCxELGws4sgG7ERgxEYIRQbESMISyoSGhGRFZEaohGRIZIpUimRERFCEZIhGREYI5kymTIZ"
    "ETERmRFSCxQKFLBFEUu0EBAhsCJCCpEQAVoRkSEKAREBSkIKFAEVoaQRCqGgoCGgMkIJERARSRGREQkBGQIpQgIVARagFaEBoaCh"
    "IaA6JrAREBErEREQMhsTCzrLDBwRHCAcQQHRwEEhIM2LshGxEYMRGBEbGxgruCslEAERkBSRESEQkSGQKUICGxABOxEbEAwbGwLK"
    "uyKwARuxFBEBAbCxIZC8iyIbsRGCERgRGhsUI6JLKRUZFZGSkREhmREhkkBSJ5ERESgRGBGSEREjCLKSohoRARoRGlIJEgkRsBQR"
    "GSEJECEgKVIJkRABWhGREZIAEQNKkqSRoRGUGpkRmaGRKpGpTBwRwUHGERwsHMzLHKzMyRzBEZIRHMEswbzMMcyapBmhQRmRGZoZ"
    "oRkamSJLmxERtBuZEZmxkbuRuTzCHBwxGBERzBDMzBzMnBMbuRGTG5kRObGRKzG5MzERwTHGERosHMzKHLw8EzMxETIRHMEswbzM"
    "MczMzMzMwRwcHDE5MRoRkxyqETnBoTwxyTMQMxMhExERPBPMyxzDvEsSGRu0EZmxmRGbsZG5grIRsTEXgRGBEBu4Gng7AzEREzIR"
    "FxEIsYE7IbfMxBzCzBwcHNEzwR3cy7HBESiSgaERqBEYgRIRoSKCuiIbETFREzERMhEbuyK5zMIcwREcESEhiSKKERoYERiBEqER"
    "KoIyKbIUEZEbgREoERG7Irg8IszBERoREcETwcEsoszGHBvBsRwbwcrBHMzBwnsiGxERc4ERexCxFxuIukkbGRuRm5ERgxMcyTHJ"
    "GRNBKBkRGiGmgRE0EREqAqZLshERG1IbGiEasbErUJuFohGhIRiBETURGqECKAMoGxERNBsYQRMRsStgyyTCHByxERETwkfMERxB"
    "EsHBDMEczMHMm7SbERsYkSwhKhURE6GzERqhsaEhklnMwhERHMJBwSElKhQRG0oRoaEaERQlKjKishERobIRGqGxQWGSspzDHMER"
    "kxEcwRzMzBzMzMzMHMzBHBEdlFKRYRFZGpkRqaEZIpokhRIYEYEUgRghGYExkiiqKqsREbOhoSGIGRgaQaUREYEYEaEiIpqGihEa"
    "RBEREYYRGqEiIju4gbFBEBERYRixESskawkbGhtgGxEQpBEBKyC1qgmqEQoXEBGhGqEQCkKyqRsasbEbERGpERu7QrKjoBGgEVIa"
    "AUEaEaACUspLzBEcIRExESIKFQETsaMRAaERASGwUiUKFBAbKwERERoREAUrwivMERwxHBERsREczCLMpFChUAFaGhEQohEQAgIk"
    "pBAKEZATEREhGgExoCCEEhgRgRWBGIEYEYEiKFKVlhEZSRGZkZkRETmSKTopkaGhERkZIZQgkREBFBEZQQkREAIiUqZQGqFgAaER"
    "qhABMSAkK2mRERsZkRErGRERKyk7ChIBEaEUERqhEAEqkCJEChQQFEpBoUEKMaADKrIouxERMRiBEZsREbiCuaQgoRABWhGqEaIQ"
    "oAIqoiAREBGqE6ETpDABOqAmChIRGqAaoQGhAhqgCqqSKZGREZmRkRGiCqEQAaoRqhGqEAEaoCqSEAERkRIRGUEJEZABWSIJEREB"
    "mREZESkQARmQKYIYGBGBGIEYgRgRgSGIsguyEAG7E7sTtzABO7AmoQoKEaAaoRphChGgAWpyAhJwEaoRERCnoAF6oCpiCQERkRYR"
    "EWEJEZABaZIqEpkRqZGRESKYEhgRiRGIEYgRERiCKKsgoRCgEqEacQoRoAFaIqoQCgGqEaoRphABGqAmuiASYKAaoQGhCxqgBqoi"
    "qRABASkRGRGpEAEZkCmXIhFxkRkZGSFiKBERgRcRGGEYEYEheCKZEBkRkBGREZkQAREgKaIgoRABOqExURoTEANaIgsVARGxGxEQ"
    "srEBMbBSIlARETEaMQExEhsQBUolkhUZEZEaGRGUoRFBklIiCZEQARmREVEZERABSZILEgERkBgYESkQARmQKFIjESIhoiERIQoR"
    "oAGqoioSGhahqqERIpkSGBGREikRKRERGZIoYgkREBFpERlhCREQAWmSChIBFKAWoRApoQFBoCaqIqERERmhGiEqIBGgAaoRGhAK"
    "GqEKqiICFQERoRQRAUGgEWqgVrKwEbEBshEbEAsbsQuyKygaERoxGIERgaERqIKYmSKRQRGWEZEhmSKRkRGpkaEhmRoZGpGpkRmR"
    "GRqRKpnKzBERHKwcwcHCwRzMzMqczBEcwRnBEcwUzMHMzCybmxERuRuZEZmxEbmbuSzNHBzBGRERzB3MwczMzMzMzBzMwcERgrsR"
    "sYEYgRGBGxuBq4jLzMwhzMzBwRGIkoGhEagRGIETEREqgrokGxGxURuhEaIRG7FLuczCHMERHBEhEZu5kbERuRkZISkVERORoxEZ"
    "kaERGZJZzMIRERzCwcERmSKRkRGZkZERABABEQEAEQABABAAQAAAABAREAAQAQEBAhAAIAAAABAREQEQEAEAAAAAAAAAABAQEAEA"
    "ARABEBABIAIAABARAQEAERAgEAABAAIAAAAREAEQAREAEAABAAIAABAAARAQEAIAERAQIAEBAQAQABAAAAQAEBEBAAEQASAQABAA"
    "AAIAABEQABEAAQAREBAgAAIAABABABAAEAAAAQACAAQAEBARIBABEAAAAQAAAAAAEBEBABEQECAAAQAAAAAAABERABAAEQABEQAg"
    "ACAAABABABEAEQAQARADAAIAEAARABEREAAAAQACAAAAEBARAAERAAIAAAAAAAAAAAERAAERACAAAAAAAAAAAAERABERAQABEAAg"
    "AAAAABARAAERAQAQABAAAAIAABARABEREAAAAQAAAAAgEAARABAAEQABEQAiAAAQEBEQABABAQIBEAAgAAAgAAERABEQAQABEAAg"
    "AAAAAAAQAAEBEQIAAhAQABEAAQAREBAgAAIAAhARABAAEQABEQAgAAAAAQARABERAQABEAAgAAAAAAEQAAEBAQAQABAAAAAAEBEB"
    "AAEQASAQABAAAAAAEBAQABEAAQAREBAgADACABAAAQFBAAAQEAESEBEAAAEAAQAAACAQAAEQARAAEAEBAQIAAAAQAAAQAQEBABAA"
    "IAAwAAAREAEQEQEBABAAAAAAAAABAQAQAQEBAhAAAAAAAAARAQEQAREBEBABAAIAAgAREAAQAREBABEAIQAgAgAQEAAQAQEBABAA"
    "IAAAAAIQAAEQEQEBAhAAAAAAACAQAQEQAREBEhABAAIAAwATAQAQAREBABEAQQAwAAIRAQEAERABEBABIAIAAAIBEQAAERAgEAAB"
    "AAIAAAEQAQEQEREAEAABAAIAAAAQAAAQEBAhACAQEQAQERAAEAEBAQIgACAQEQEAERAhEBABAAIAICAAAQEQEREgEAABAAIAACAA"
    "AQAQEBAhACAQEQASERAgEAEBAQIAACAQEQAQEREBEBABAAIAAAABAQEAEQABABEQICAAAAARERAAEAAhAAEQAAAAAAAQAAEQECAA"
    "AAAAEAEAAQERABAAEQACAgAAEBEAEAEQABABAQACACAAEBABAREQABAAAQACAAAAAQEAAQERABAAEQACACAAEQEAEREQIBABAQAC"
    "AAARAAEQIAAAABERARAAAQASABAAAAAAAAEREAABEQAQABEAAgIgABAQERAQAQAQABAAAAIAIBAQEAABEQAQABEAAgIgABABARAB"
    "AQEQEBAAAAAgIBARAQARESAQABEAAgIAABAQAQAQAQEQARAAIAIAABARAQAAEQACAQABAAAAABAREQABEQAQAQEBAgAAAgAQEAAQ"
    "EQACAQABAAAAIAAREQABEQASAQEBAgADAhEAEBAQAwACEBEBEBEBABEQECAAACAAEBEBABERIBAAEQACACAQEBABAAARAAERACIA"
    "AyAQEREBAAERAgERACAAACAQERABEBARIAERACAAAAAQEBABAAERIgASEBEBEAARABERECIAIgACEAEBAAARAAERACIAAAACERAB"
    "EBERAAERACAAAAABARADAAERMgAyAREBEAARABERECQAIwACEQEBEBARAAERACIAAAACEQEBAAERAgICARERABARAAERASAgMAAy"
    "ERERABARABERECMAAwAQARABAAEQABABAQIAAAAQAQEBAAAQAhAAAQAAAAAAAQEBEAARABAAEQAAAgAAAREBAAEQAhABAQAAAAIA"
    "ABAAAQEAAAAAEAEQIAEQIQABEAEAAAAAEAEQABEBAQAREAAAACAAEAEBAQABIRABEAAAAgAAAAERAQARAQABEAEAAAAAEAEBAAEA"
    "ABIAEAAAAAAAEAERABEQABAAEQACAAACABEAAQAAAAIQEAEQAQEgARABIAAAAAIQAQEQEQEAARABIAAAIAAQAQEQEBAgEQABAAIA"
    "AAIRAREAAREAAREBIAAAAAAQABEAAQACEBERABEQABIBAQECAAAAABEQEAEQIQAQABAQEAEQIQAgABEBEAMQIQMgAREBABAAEAAA"
    "EQAAEAEBAgADABABEQEAEQACEAEBAAAgABAAEQAQERAAEAERAgAiAAAAEQAAEQAAEAEBAgAwADABEAAQERAAEAERBAAjAAARAAAB"
    "AQAAIAAQABEAEBEAEBEQAAACAwATAREAEAEAEAEQAQARAAEAARACAAAAEAEBIQAAAAAAABAQEAABAQEAEQABACIgABAQEQAQARAA"
    "EAABAAIgABABECABAQEAEQABAAIAABABAQEAAQAQESAAAAAAABABEQAREAEQAAEAAgAAIgABEQAREAEQAAEAIgAAIgAREAAREQEQ"
    "AAEAAgAAAAAREAABAQAAEAABEBEQAgABAAABEBEQAgACERAAAAEAAQAQABAAAAAhADACAAEQAQARABEAEAAhACACAAAQAQABAAAw"
    "AEABEAAQAQAAAAEQAAEBEQAQABABABIAEAAQAAAAAwABEAEAEQAAAAERAAAAAAAAAAEAEAAAAAAAAAABAAABAAEAAAAAAAAAAAAA"
    "AAABAAAAAAABAAAAAAAQABAAAAEAEAAAAAAAAAABAAEAAAAAAAEAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAQAAAAAAAAAB"
    "AAEAEAAAAAAAEAAQAAABAAAAAAAQABAAAAAAAAAAAAABAAEAEAAAAAAAAAABAAEAEAAQAAAAAACZAJoAoAmgCQDKAAsAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACgAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAkAAAAAEAAAAAAAAAEAAAAAABAAAAAAAAABEAAAAAAAEAEAAAAAAAAAAQABAAAAAACQAJCQAAAAAAAAAAAAAAAAAAAQAQEA"
    "AAAAAAAAEAEAAQAAAAAAAAAQABAAAAEAAAAAABAAAAAAAAAAAAAAEAEBAQAAAAAAAAAAEAEAAQAQAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAEAAAAQ"
    "AAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAQAAAAAAAAEAAAAAAAAAEQAAAAAAAQAAEAAAAAABABAAAAAAAAAAAAAAAAAAAAAAmQkJAA"
    "AAAAABAAAQABAAAAAAAQAQEAAAAAAAAAAAEAAAAAAAAAAAAAARAAEAABAAAAAAAQAQEAAQAAAAAAAAAQAAEAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAR"
    "AAAAAQAAEAAAEIARAAAAAAAAAAAAAAAAEAABAAAAEAEJAQAAAAAAAAABEAAQAAAAEAABkAEAAAAAAAAAAAAAAAAAAAAAAAAAABAA"
    "ARCQAAAAAAAAAAAAAAABEAABAQkAAAAAAAAAmZmZmZmZmZmZAAAAAAAAAAAAAAAAAAAAAAAAAAABAAAAAAEAAAAAwAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAABAAAAEAAAABAAAAAAAAAAEAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAQAAAAAAAAAAAAEA"
    "EAAAAAAAAAAQAAABAAAAAAAAABAAAAAAAAAAAAAAEAEAEAAAAAAAAAAAAAEAEAAQAAAAAAAAAJAACgkACQAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAQAAAAAAAAAAABAAAAAAAAAJAAAAAAAAAAAAkACQAACgAAABAAEAAA"
    "AAAAAAAQAQAAAAAAAAAAABAAAAEAAAAAAAAAAQABEAAAAAAAAAAQARAAAAAAAAAAAAAQAQAQAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACAQ"
    "EAEAAAAAAAAAAAAAAAAAEAAAIAAQABAAAAAAAAAAAAABAAEAIAAAEAAQAAAAAAAAAAAAAAAAAAAAAAAAACAAABAAAQAAAAAAAAAA"
    "AAACAAABEBAAAAAAAAAAAAAAAAAACZCQkAAAAAAKAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACgAAAAAACQAAAAAAAAAAEAAAAAAAAAAAAAABAAAAAAAAAAAA"
    "EAAAAAAAAAAAAAABEAAAAAAAAAAAABABAAAAAAAAAAAAAAABAAEAAAAACgAAAACQAJCgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAKAAAAAAAAAAEAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "IACgARAAAAAAAAAAAAAAAAEAIACgAAAQAAAAAAAAAAAAAAAAAAAAAAAAACAAoAAAAQAAAAAAAAAAAAACAAoAEBAAAAAAAAAAAAAK"
    "AAACAJCQkAAAAAAACgAAAAAAAAAAAAAAAAAAAJAAAAAQAJAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACAAqgAAEA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAgAKoAAAAAAAAAAAAAAAIACgAAoAAQAAAAAAAAAACgAAIAACAAkJAAAAAAABAAEAEAAQAAABAAAA"
    "AAAQAQEBABAAAAAAAAAAAAAAEAEBAAEQAAAAAAAAAAAAABAAARAQEJEAAAAQAQAQABAAAAEAAAAAAAAAEAEQEAEAAAAAAAAAACAA"
    "ABAAAQERAgAAEQAAEAEBAQAQAAAAAAAgABEAAAEBEQIAAgEBEQAQECE=";
#define TBDATA TBDATA_STD
#endif
#define TB_N31 36198
#define TB_N21 3786
#define TB_N12 3786
#define TB_N11 262

static int DKI[64];
static int TRD[4][32];
static int BN[33][4];
static int8_t S1[32], S2[496 * 2], S3[4960 * 3];
static int8_t *SETP[4];

typedef struct { int a, b, n; long long ksz; int32_t *cidx; size_t base; } TbC;
static TbC TBC[4] = {{3, 1, TB_N31, 0, NULL, 0}, {2, 1, TB_N21, 0, NULL, 0},
                     {1, 2, TB_N12, 0, NULL, 0}, {1, 1, TB_N11, 0, NULL, 0}};
static uint8_t *tb_nib = NULL;
static int tb_state = 0; /* 0: tayyor emas, 1: tayyor, -1: xato */

static void tb_tables_init(void) {
    int k = 0, dsq[32];
    for (int sq = 0; sq < 64; sq++) {
        if (((sq >> 3) + (sq & 7)) & 1) { DKI[sq] = k; dsq[k] = sq; k++; }
        else DKI[sq] = -1;
    }
    for (int n = 0; n < 33; n++) {
        BN[n][0] = 1;
        for (int j = 1; j < 4; j++) BN[n][j] = (n == 0) ? 0 : BN[n - 1][j - 1] + BN[n - 1][j];
    }
    for (int i = 0; i < 32; i++) {
        int r = dsq[i] >> 3, c = dsq[i] & 7;
        int tr[4] = {r * 8 + c, c * 8 + r, (7 - r) * 8 + (7 - c), (7 - c) * 8 + (7 - r)};
        for (int t = 0; t < 4; t++) TRD[t][i] = DKI[tr[t]];
    }
    SETP[1] = S1; SETP[2] = S2; SETP[3] = S3;
    for (int i = 0; i < 32; i++) S1[i] = (int8_t)i;
    for (int b = 1; b < 32; b++) for (int a = 0; a < b; a++) {
        int r = BN[a][1] + BN[b][2];
        S2[r * 2] = (int8_t)a; S2[r * 2 + 1] = (int8_t)b;
    }
    for (int c = 2; c < 32; c++) for (int b = 1; b < c; b++) for (int a = 0; a < b; a++) {
        int r = BN[a][1] + BN[b][2] + BN[c][3];
        S3[r * 3] = (int8_t)a; S3[r * 3 + 1] = (int8_t)b; S3[r * 3 + 2] = (int8_t)c;
    }
}

static inline int tb_keyc(const TbC *C, const int *own, const int *opp, int t) {
    int o[3], p[3], x, i, j;
    for (i = 0; i < C->a; i++) o[i] = TRD[t][own[i]];
    for (i = 0; i < C->b; i++) p[i] = TRD[t][opp[i]];
    for (i = 1; i < C->a; i++) { x = o[i]; for (j = i - 1; j >= 0 && o[j] > x; j--) o[j + 1] = o[j]; o[j + 1] = x; }
    for (i = 1; i < C->b; i++) { x = p[i]; for (j = i - 1; j >= 0 && p[j] > x; j--) p[j + 1] = p[j]; p[j + 1] = x; }
    int ro = 0, rp = 0;
    for (i = 0; i < C->a; i++) ro += BN[o[i]][i + 1];
    for (i = 0; i < C->b; i++) rp += BN[p[i]][i + 1];
    return ro * BN[32][C->b] + rp;
}

static int tb_init(void) {
    if (tb_state) return tb_state > 0;
    tb_state = -1;
    static const char *AL = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int8_t dec[256];
    memset(dec, -1, sizeof dec);
    for (int i = 0; i < 64; i++) dec[(uint8_t)AL[i]] = (int8_t)i;
    size_t L = strlen(TBDATA);
    uint8_t *raw = (uint8_t *)malloc(L / 4 * 3 + 8);
    size_t rn = 0, total = 0;
    uint32_t acc = 0; int bits = 0;
    if (!raw) return 0;
    for (size_t i = 0; i < L; i++) {
        int v = dec[(uint8_t)TBDATA[i]];
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; raw[rn++] = (uint8_t)(acc >> bits); acc &= (1u << bits) - 1; }
    }
    for (int c = 0; c < 4; c++) total += (size_t)TBC[c].n;
    tb_nib = (uint8_t *)malloc(total + 4);
    if (!tb_nib || rn * 2 < total) { free(raw); return 0; }
    for (size_t i = 0; i < total; i++) tb_nib[i] = (raw[i >> 1] >> ((i & 1) * 4)) & 15;
    free(raw);
    size_t base = 0;
    for (int c = 0; c < 4; c++) {
        TbC *C = &TBC[c];
        C->ksz = (long long)BN[32][C->a] * BN[32][C->b];
        C->base = base;
        C->cidx = (int32_t *)malloc(sizeof(int32_t) * (size_t)C->ksz);
        if (!C->cidx) return 0;
        long long nb = BN[32][C->b];
        int cnt = 0;
        for (long long key = 0; key < C->ksz; key++) {
            int ro = (int)(key / nb), rp = (int)(key % nb);
            int own[3], opp[3], bad = 0;
            for (int i = 0; i < C->a; i++) own[i] = SETP[C->a][ro * C->a + i];
            for (int i = 0; i < C->b; i++) opp[i] = SETP[C->b][rp * C->b + i];
            for (int i = 0; i < C->a; i++) for (int j = 0; j < C->b; j++) if (own[i] == opp[j]) bad = 1;
            C->cidx[key] = -1;
            if (bad) continue;
            int mn = (int)key;
            for (int t = 1; t < 4; t++) { int x = tb_keyc(C, own, opp, t); if (x < mn) mn = x; }
            if (mn != (int)key) continue;
            if (cnt >= C->n) return 0;
            C->cidx[key] = cnt++;
        }
        if (cnt != C->n) return 0;
        base += (size_t)C->n;
    }
    tb_state = 1;
    return 1;
}

/* 1 = baza holatni biladi; val: 0 durang, >0 g'alaba (yarim yurish), <0 mag'lubiyat (-val-1 yarim yurish) */
static volatile int tb_done = 0, tb_lock = 0;
static void tb_ensure(void) {
    while (__sync_lock_test_and_set(&tb_lock, 1)) { }
    if (!tb_done) { tb_init(); __sync_synchronize(); tb_done = 1; }
    __sync_lock_release(&tb_lock);
}
static int kt_probe(int side, int *val) {
    int own[3], opp[3], na = 0, nb = 0;
    for (int sq = 0; sq < 64; sq++) {
        int p = B[sq];
        if (!p) continue;
        if (p == 1 || p == -1) return 0;
        if (p * side > 0) { if (na == 3) return 0; own[na++] = DKI[sq]; }
        else { if (nb == 3) return 0; opp[nb++] = DKI[sq]; }
    }
    int c;
    if (na == 3 && nb == 1) c = 0;
    else if (na == 2 && nb == 1) c = 1;
    else if (na == 1 && nb == 2) c = 2;
    else if (na == 1 && nb == 1) c = 3;
    else return 0;
    if (!tb_done) tb_ensure();
    if (tb_state < 0) return 0;
    const TbC *C = &TBC[c];
    int best = tb_keyc(C, own, opp, 0);
    for (int t = 1; t < 4; t++) { int x = tb_keyc(C, own, opp, t); if (x < best) best = x; }
    int n = tb_nib[C->base + (size_t)C->cidx[best]];
    if (n == 0) *val = 0;
    else if (n < 8 || c == 0) *val = 2 * n - 1;
    else *val = -(2 * (n - 8)) - 1;
    return 1;
}
/* yuruvchida 1 damka, raqibda 3 damka (faqat damkalar)? */
static int kt_lone(int side) {
    int na = 0, nb = 0;
    for (int sq = 0; sq < 64; sq++) {
        int p = B[sq];
        if (!p) continue;
        if (p == 1 || p == -1) return 0;
        if (p * side > 0) na++; else nb++;
    }
    return na == 1 && nb == 3;
}

/* ------------------------------ baholash ------------------------------ */
/* sozlanadigan baholash parametrlari (DRP muhit o'zgaruvchisi bilan almashtiriladi) */
enum { P_KING, P_ADV, P_ADV6, P_CENSQ, P_BACK, P_EDGE, P_SUP, P_RUNB, P_RUNS, P_CENW,
       P_KMAIN, P_KCEN, P_KMAINE, P_KMOBW, P_TRADE, P_MOB,
       P_BACKALL, P_TRI, P_BLOCK, P_KDOM, P_ADVQ, P_N };
#ifndef NEWTERMS
#define NEWTERMS 1
#endif
static int P[P_N] = {307, 3, 22, 3, 14, 4, 1, 23, 8, 9, 17, 7, 4, 1, 75, 1,
                     3, 1, 7, 25, 1};

#ifndef USE_PST
#define USE_PST 1
#endif
/* o'rganilgan katak jadvallari (mover nuqtai nazaridan, 32 qora katak): oddiy shashka va damka */
static int PSTM[32] = {0, 0, 0, 0, 15, 13, 6, 18, -1, 10, 5, 16, 10, -9, -21, -10, -12, -1, -4, 15, -5, -10, 16, -7, -9, 13, 13, 11, -17, 3, 23, 26};
static int PSTK[32] = {-16, -22, -13, -1, 3, -9, -1, 9, 0, 0, 6, 2, -13, -6, -4, -2, -5, 2, -8, 1, 1, 3, -5, 2, 4, -4, -6, -3, 8, -1, -4, -5};

/* oldindan hisoblangan statik qiymatlar (katak va rangga bog'liq, taxtaga bog'liq bo'lmagan qismlar) */
static int SMAN[2][64];       /* [rang(0=o,1=x)][katak] */
static int SKING[2][2][64];   /* [rang][NPCS<=10][katak] */
static void init_eval_tables(void) {
    for (int col = 0; col < 2; col++) for (int sq = 0; sq < 64; sq++) {
        if (!(((sq >> 3) + (sq & 7)) & 1)) continue;
        int r = sq >> 3, c = sq & 7;
        int adv, back, cc;
        if (col == 0) { adv = 7 - r; back = (r == 7); cc = c; }
        else { adv = r; back = (r == 0); cc = 7 - c; }
        int v = 100 + adv * P[P_ADV];
#if USE_PST
        v += PSTM[DKI[col == 0 ? sq : 63 - sq]];
#endif
        if (adv == 6) v += P[P_ADV6];
        if (r >= 2 && r <= 5 && c >= 2 && c <= 5) v += P[P_CENSQ];
#if EVAL2
        if ((r == 3 && (c == 2 || c == 4)) || (r == 4 && (c == 3 || c == 5))) v += P[P_CENW];
#endif
        if (back && (cc == 2 || cc == 4)) v += P[P_BACK];
#if NEWTERMS
        if (back && !(cc == 2 || cc == 4)) v += P[P_BACKALL];
        v += P[P_ADVQ] * adv * adv / 4;
#endif
        if (c == 0 || c == 7) v -= P[P_EDGE];
        SMAN[col][sq] = v;
        for (int e = 0; e < 2; e++) {
            int k = P[P_KING];
#if USE_PST
            k += PSTK[DKI[col == 0 ? sq : 63 - sq]];
#endif
            if (r + c == 7) k += P[P_KMAIN];
            else if (r >= 2 && r <= 5 && c >= 2 && c <= 5) k += P[P_KCEN];
#if EVAL2
            if (r + c == 7 && e) k += P[P_KMAINE];
            { int dr = (2 * r - 7) < 0 ? 7 - 2 * r : 2 * r - 7, dc = (2 * c - 7) < 0 ? 7 - 2 * c : 2 * c - 7;
              k += P[P_KMOBW] * (7 - (dr + dc) / 2) / 3; }
#endif
            SKING[col][e][sq] = k;
        }
    }
}

/* bir rang uchun bit-parallel hisob: himoya, "uchburchak", qotib qolgan shashkalar */
typedef struct { int sup, tri, blk; } Dyn;
static inline Dyn dyn_side(int col, uint64_t own, uint64_t men, uint64_t empty) {
    Dyn r;
    int df0 = col ? 2 : 0, df1 = col ? 3 : 1;          /* oldinga yo'nalishlar */
    int db0 = col ? 0 : 2, db1 = col ? 1 : 3;          /* orqaga yo'nalishlar */
    uint64_t sl = shd(own, 3 - db0) & men, sr = shd(own, 3 - db1) & men;
    r.sup = pc64(sl) + pc64(sr);
    r.tri = pc64(sl & sr);
    uint64_t notlast = men & (col ? ~(0xFFULL << 56) : ~0xFFULL);   /* oxirgi qatordagi (mumkin bo'lmagan) shashkalar hisobga olinmaydi */
    r.blk = pc64(notlast & ~shd(empty, 3 - df0) & ~shd(empty, 3 - df1));
    return r;
}

static int eval(void) {
    int s = 0, matd = 0;
    const uint64_t occ = MP | MN, empty = ~occ;
    const uint64_t menO = MP & ~MK, menX = MN & ~MK, kO = MP & MK, kX = MN & MK;
    const int ko = pc64(kO), kx = pc64(kX);
    const int tot = pc64(occ);
    const int e10 = NPCS <= 10;
    /* statik qismlar (jadvaldan) */
    for (uint64_t om = menO; om; om &= om - 1) s += SMAN[0][__builtin_ctzll(om)];
    for (uint64_t om = menX; om; om &= om - 1) s -= SMAN[1][__builtin_ctzll(om)];
    for (uint64_t om = kO; om; om &= om - 1) s += SKING[0][e10][__builtin_ctzll(om)];
    for (uint64_t om = kX; om; om &= om - 1) s -= SKING[1][e10][__builtin_ctzll(om)];
    matd = 100 * (pc64(menO) - pc64(menX)) + P[P_KING] * (ko - kx);
    Dyn dO = dyn_side(0, MP, menO, empty), dX = dyn_side(1, MN, menX, empty);
    s += (dO.sup - dX.sup) * P[P_SUP];
#if NEWTERMS
    s += (dO.tri - dX.tri) * P[P_TRI];
    s -= (dO.blk - dX.blk) * P[P_BLOCK];
#endif
    /* erkin o'tuvchi shashka: raqib damkasi yo'q va yo'lida to'siq yo'q (adv >= 3) */
    if (kx == 0) {
        for (uint64_t om = menO & 0x000000FFFFFFFFFFULL; om; om &= om - 1) {   /* qatorlar 0..4 */
            int sq = __builtin_ctzll(om);
            if (!(CONE[0][sq] & MN)) s += P[P_RUNB] + (4 - (sq >> 3)) * P[P_RUNS];   /* adv-3 = (7-r)-3 = 4-r */
        }
    }
    if (ko == 0) {
        for (uint64_t om = menX & 0xFFFFFFFFFF000000ULL; om; om &= om - 1) {   /* qatorlar 3..7 */
            int sq = __builtin_ctzll(om);
            if (!(CONE[1][sq] & MP)) s -= P[P_RUNB] + ((sq >> 3) - 3) * P[P_RUNS];  /* adv-3 = r-3 */
        }
    }
#if NEWTERMS
    if (ko > 0 && kx == 0) s += P[P_KDOM];     /* faqat menda damka bor */
    else if (kx > 0 && ko == 0) s -= P[P_KDOM];
#endif
    /* material ustunligida almashtirish foydali */
    s += matd * (24 - tot) * P[P_TRADE] / 10000;
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
typedef struct { uint64_t k, d; } TTS; /* lockless: k = key ^ d */

#ifndef TT_BITS
#define TT_BITS 22
#endif
static TTS *TT;
static uint64_t TT_MASK = ((uint64_t)1 << 16) - 1; /* main() da vaqtga qarab o'rnatiladi */

static TLS int hist[2][64][64];
static TLS int16_t killer[MAXP + 4][2];
static TLS int16_t cmv[2][64][64]; /* qarshi-yurish: raqib (from,to) ga javob */
static TLS int16_t lastk[MAXP + 4]; /* shu pliyga olib kelgan yurish kaliti */
static TLS uint64_t keys[MAXP + 4];
static TLS long long nodes;
static long long deadline;
static volatile int stopf;

static inline int tt_load(uint64_t key, TTE *o) {
    TTS *e = &TT[key & TT_MASK];
    uint64_t d = e->d, k = e->k;
    if ((k ^ d) != key) return 0;
    o->key = key;
    o->score = (int16_t)(uint16_t)(d & 0xFFFF);
    o->depth = (int8_t)((d >> 16) & 0xFF);
    o->flag = (uint8_t)((d >> 24) & 0xFF);
    o->from = (int8_t)((d >> 32) & 0xFF);
    o->to = (int8_t)((d >> 40) & 0xFF);
    o->n = (int8_t)((d >> 48) & 0xFF);
    o->pad = (int8_t)((d >> 56) & 0xFF);
    return 1;
}
static volatile int g_gen = 0, g_gen_bumped = 0, g_server = 0; /* server rejimida TT eskirishi */
static inline void tt_store(uint64_t key, int score, int depth, int flag, int from, int to, int n) {
    uint64_t d = (uint64_t)(uint16_t)(int16_t)score | ((uint64_t)(uint8_t)depth << 16) | ((uint64_t)(uint8_t)flag << 24)
               | ((uint64_t)(uint8_t)from << 32) | ((uint64_t)(uint8_t)to << 40) | ((uint64_t)(uint8_t)n << 48)
               | ((uint64_t)(uint8_t)g_gen << 56);
    TTS *e = &TT[key & TT_MASK];
    e->d = d; e->k = key ^ d;
}
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


/* barg tugun uchun tezkor tekshiruv: 1 = urish bor (to'liq gen kerak), 0 = yo'q, *q = oddiy yurishlar soni */
static int leaf_scan(int side, int *q) {
    int cnt = 0;
    uint64_t own = side > 0 ? MP : MN, enemy = side > 0 ? MN : MP;
    uint64_t empty = ~(MP | MN);
    uint64_t men = own & ~MK;
    for (int d = 0; d < 4; d++) {
        uint64_t t = shd(men, d) & enemy;
        if (t && (shd(t, d) & empty)) return 1;
    }
    int fd = side == 1 ? 0 : 2;
    cnt = pc64(shd(men, fd) & empty) + pc64(shd(men, fd + 1) & empty);
    for (uint64_t om = own & MK; om; om &= om - 1) {
        int sq = __builtin_ctzll(om);
        for (int d = 0; d < 4; d++) {
            int t = NB[sq][d];
            while (t >= 0 && B[t] == 0) { cnt++; t = NB[t][d]; }
            if (t >= 0 && B[t] * side < 0) { int l = NB[t][d]; if (l >= 0 && B[l] == 0) return 1; }
        }
    }
    *q = cnt;
    return 0;
}

static int negamax(int side, int depth, int alpha, int beta, int ply) {
    nodes++;
    if ((nodes & 2047) == 0 && now_ms() > deadline) stopf = 1;
    if (stopf) return 0;
    if (NPCS <= 4) {
        int tv;
        if (kt_probe(side, &tv)) {
            if (tv == 0) return 0;
            int d = ply + (tv > 0 ? tv : -tv - 1);
            if (d > 98) d = 98;
            return tv > 0 ? WIN - 100 - d : -(WIN - 100 - d);
        }
    }
    if (depth <= 0 && ply < MAXP - 2) {
        int q;
        if (!leaf_scan(side, &q) && !(q > 0 && NPCS == 4 && kt_lone(side))) {
            if (q == 0) return -WIN + ply;
            return side * eval() + P[P_MOB] * q;
        }
    }
    Move ml[MAXM];
    int n = gen(side, ml);
    if (n == 0) return -WIN + ply;
    int capture = ml[0].nc > 0;
    if (depth <= 0 && !capture) {
        if (NPCS == 4 && kt_lone(side)) depth = 1; /* yolg'iz damka: bola holatlar bazadan o'qiladi */
        else return side * eval() + P[P_MOB] * n;
    }
    if (ply >= MAXP - 2) return side * eval();

    uint64_t key = H ^ SK[side > 0];
    keys[ply] = key;
    for (int i = ply - 2; i >= 0; i -= 2)
        if (keys[i] == key) return 0; /* takrorlanish = durang */

    int ttf = -1, ttt = -1, ttn = -1;
    if (depth > 0) {
        TTE tmp; TTE *e = &tmp;
        if (tt_load(key, &tmp)) {
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
        if (m->from == ttf && m->to == ttt && m->nc == ttn) m->sc = 1000000;
        else if (m->nc) m->sc = 100000 + m->nc * 100;
        else {
            int k = (m->from << 6) | m->to;
            m->sc = hist[hs][m->from][m->to];
#if USE_CM
            if (ply > 0 && lastk[ply] >= 0 && cmv[hs][lastk[ply] >> 6][lastk[ply] & 63] == k) m->sc += 45000;
#endif
            if (killer[ply][0] == k) m->sc += 50000;
            else if (killer[ply][1] == k) m->sc += 40000;
        }
    }

    int ext = (n == 1) ? EXT_SINGLE : 0;
    int best = -INF, bi = -1, a0 = alpha;
    for (int i = 0; i < n; i++) {
        int bj = i;
        for (int j = i + 1; j < n; j++) if (ml[j].sc > ml[bj].sc) bj = j;
        if (bj != i) { Move t = ml[i]; ml[i] = ml[bj]; ml[bj] = t; }
        Move *m = &ml[i];
        int promo_mv = m->king && B[m->from] == side;
        make_s(side, m, ply);
        lastk[ply + 1] = (int16_t)((m->from << 6) | m->to);
        int sc;
        int nd = depth - 1 + ext;
        if (nd > 0) __builtin_prefetch(&TT[(H ^ SK[-side > 0]) & TT_MASK]);   /* bola tugun jadvalini oldindan keshga yuklash */
        if (i == 0) sc = -negamax(-side, nd, -beta, -alpha, ply + 1);
        else {
            int r = 0;
            if (USE_LMR && !capture && i >= LMR_I && depth >= 3 && !promo_mv) {
                r = 1;
                if (i >= LMR_J && depth >= 6) r = 2;
            }
            sc = -negamax(-side, nd - r, -alpha - 1, -alpha, ply + 1);
            if (r && sc > alpha && !stopf)
                sc = -negamax(-side, nd, -alpha - 1, -alpha, ply + 1);
            if (sc > alpha && sc < beta && !stopf)
                sc = -negamax(-side, nd, -beta, -alpha, ply + 1);
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
#if USE_CM
                if (ply > 0 && lastk[ply] >= 0) cmv[hs][lastk[ply] >> 6][lastk[ply] & 63] = (int16_t)k;
#endif
            }
            break;
        }
    }
    if (depth > 0 && bi >= 0) {
        TTE old;
        if (!tt_load(key, &old) || depth >= old.depth || (uint8_t)old.pad != (uint8_t)g_gen)
            tt_store(key, score_to_tt(best, ply), depth, best >= beta ? 1 : (best > a0 ? 0 : 2),
                     ml[bi].from, ml[bi].to, ml[bi].nc);
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


#define MAXTH 4
typedef struct { Move m; int pen; int pn; int8_t path[16]; } Root;
typedef struct {
    int tid, n;
    int8_t board0[64];
    Root *rm;
    long long t0, limit;
    Root bestr;
    int bestsc, bestdepth;
    long long nodes;
} TCtx;

static uint64_t board_hash(void);

/* ildiz qidiruvi: iterative deepening. tid==0 - asosiy oqim (natijani shu beradi), qolganlari TT ni to'ldirib yordam beradi */
static void run_root(void *arg) {
    TCtx *c = (TCtx *)arg;
    memcpy(B, c->board0, 64);
    H = board_hash();
    MP = MN = MK = 0; NPCS = 0;
    for (int i = 0; i < 64; i++) {
        if (B[i]) NPCS++;
        if (B[i] > 0) MP |= 1ULL << i; else if (B[i] < 0) MN |= 1ULL << i;
        if (B[i] == 2 || B[i] == -2) MK |= 1ULL << i;
    }
    nodes = 0;
    Root *rm = c->rm;
    int n = c->n;
    Root bestr = rm[0];
    int bestsc = 0, bestdepth = 0;
    int maxd = 60;
    { const char *e = getenv("DRDEPTH"); if (e) maxd = atoi(e) + 1; }
    for (int depth = 1 + (c->tid & 1); depth < maxd; depth++) {
        int alpha = -INF, beta = INF, bi = -1;
        int iter_best = -INF;
        for (int i = 2; i < n; i++) {
            Root t = rm[i]; int j = i - 1;
            while (j >= 1 && rm[j].m.sc < t.m.sc) { rm[j + 1] = rm[j]; j--; }
            rm[j + 1] = t;
        }
        keys[0] = H ^ SK[1];
        lastk[0] = -1;
        for (int i = 0; i < n && !stopf; i++) {
            make_s(1, &rm[i].m, 0);
            lastk[1] = (int16_t)((rm[i].m.from << 6) | rm[i].m.to);
            int sc, pp = rm[i].pen;
            int nd = depth - 1;
            if (i == 0) {
                sc = -negamax(-1, nd, -beta - pp, -alpha - pp, 1) - pp;
            } else {
                sc = -negamax(-1, nd, -(alpha + 1) - pp, -alpha - pp, 1) - pp;
                if (sc > alpha && !stopf)
                    sc = -negamax(-1, nd, -beta - pp, -alpha - pp, 1) - pp;
            }
            unmake_s(1, &rm[i].m, 0);
            if (stopf) break;
            rm[i].m.sc = sc;
            if (sc > iter_best) { iter_best = sc; bi = i; alpha = sc; }
        }
        if (bi < 0) break; /* hatto birinchi yurish tugamadi */
        Root bm = rm[bi];
        for (int k = bi; k > 0; k--) rm[k] = rm[k - 1];
        rm[0] = bm;
        bestr = bm; bestsc = iter_best;
        if (!stopf) bestdepth = depth;
        if (stopf) break;
        if (iter_best > WIN - 200 || iter_best < -WIN + 200) break;
        if (c->tid == 0 && (now_ms() - c->t0) * 100 > c->limit * SOFT_PCT) break;
    }
    c->bestr = bestr; c->bestsc = bestsc; c->bestdepth = bestdepth;
    c->nodes = nodes;
}

/* ---------------------------- so'rov va qidiruv ---------------------------- */
typedef struct { int nex; int ex[64][2]; int ns; uint64_t seenh[512]; } Req;
static Req g_req;

static void load_board(const char *bs) {
    for (int i = 0; i < 64; i++) {
        char ch = bs[i];
        B[i] = ch == 'o' ? 1 : ch == 'O' ? 2 : ch == 'x' ? -1 : ch == 'X' ? -2 : 0;
    }
    H = board_hash();
    MP = MN = MK = 0;
    NPCS = 0;
    for (int i = 0; i < 64; i++) {
        if (B[i]) NPCS++;
        if (B[i] > 0) MP |= 1ULL << i; else if (B[i] < 0) MN |= 1ULL << i;
        if (B[i] == 2 || B[i] == -2) MK |= 1ULL << i;
    }
}

static void read_req(Req *q) {
    q->nex = 0; q->ns = 0;
    if (scanf("%d", &q->nex) != 1) q->nex = 0;
    if (q->nex > 64) q->nex = 64;
    for (int i = 0; i < q->nex; i++) if (scanf("%d %d", &q->ex[i][0], &q->ex[i][1]) != 2) { q->nex = i; break; }
    if (scanf("%d", &q->ns) != 1) q->ns = 0;
    int ns = q->ns;
    for (int i = 0; i < ns; i++) {
        char t[256];
        if (scanf("%255s", t) != 1) { q->ns = i; break; }
        if (i < 512 && strlen(t) >= 64) {
            uint64_t h = 0;
            for (int k = 0; k < 64; k++) {
                int v = t[k] == 'o' ? 1 : t[k] == 'O' ? 2 : t[k] == 'x' ? -1 : t[k] == 'X' ? -2 : 0;
                if (v) h ^= ZB[zi(v)][k];
            }
            q->seenh[i] = h;
        } else if (i < 512) q->seenh[i] = 0;
    }
    if (q->ns > 512) q->ns = 512;
}

/* hozirgi taxta (B) uchun eng yaxshi yurishni topadi va "OK ..." / "NONE" chiqaradi */
static int do_search(long long limit, const Req *q) {
    static Root rm[MAXM];
    Move all[MAXM];
    int nex = q->nex, ns = q->ns;
    want_path = 1;
    int na = gen(1, all), n = 0;
    for (int i = 0; i < na; i++) {
        int bad = 0;
        for (int j = 0; j < nex; j++) if (all[i].from == q->ex[j][0] && all[i].to == q->ex[j][1]) bad = 1;
        if (!bad) {
            rm[n].m = all[i]; rm[n].pn = RPn[i];
            memcpy(rm[n].path, RP[i], 16);
            n++;
        }
    }
    want_path = 0;
    if (n == 0) { printf("NONE\n"); fflush(stdout); return 0; }

    if (!g_gen_bumped) g_gen++;
    g_gen_bumped = 0;
    stopf = 0;
    memset(hist, 0, sizeof hist);
    memset(killer, 0, sizeof killer);

    long long t0 = now_ms();
    if (limit <= 0) limit = 1000;
    deadline = t0 + limit;
    for (int i = 0; i < n; i++) {
        make_s(1, &rm[i].m, 0);
        rm[i].pen = 0;
        for (int j = 0; j < ns; j++) if (q->seenh[j] == H) { rm[i].pen = REP_PEN; break; }
        unmake_s(1, &rm[i].m, 0);
        rm[i].m.sc = 0;
    }

    TCtx ctx[MAXTH];
    int nth = 1;
#ifdef USE_THREADS
    { const char *e = getenv("DRTHREADS");
      int want = e ? atoi(e) : ncpu();
      if (want > 4) want = 4;
      if (want < 1) want = 1;
      nth = (n > 1) ? want : 1; }
#endif
    if (!g_server) {   /* TT hajmi: kutiladigan tugunlar soniga mos (qisqa qidiruvda katta jadval sahifa-xatolari tufayli vaqt yeydi) */
        double expn = (double)limit * 2500.0 * nth;
        int bits = 16;
        while (bits < TT_BITS && (double)((uint64_t)1 << (bits + 1)) <= expn) bits++;
        TT_MASK = ((uint64_t)1 << bits) - 1;
        TT = (TTS *)calloc((size_t)1 << bits, sizeof(TTS));
        if (!TT) return 1;
    }
    static Root rms[MAXTH][MAXM];
    for (int t = 0; t < nth; t++) {
        ctx[t].tid = t; ctx[t].n = n; ctx[t].t0 = t0; ctx[t].limit = limit;
        memcpy(ctx[t].board0, B, 64);
        if (t == 0) ctx[t].rm = rm;
        else { memcpy(rms[t], rm, sizeof(Root) * (size_t)n); ctx[t].rm = rms[t]; }
        ctx[t].bestr = rm[0]; ctx[t].bestsc = 0; ctx[t].bestdepth = 0; ctx[t].nodes = 0;
    }
#ifdef USE_THREADS
    thr_t th[MAXTH];
    int started[MAXTH] = {0};
    for (int t = 1; t < nth; t++) started[t] = thr_start(&th[t], run_root, &ctx[t]);
#endif
    if (n > 1) run_root(&ctx[0]);
    stopf = 1;
#ifdef USE_THREADS
    for (int t = 1; t < nth; t++) if (started[t]) thr_join(th[t]);
#endif
    long long total_nodes = 0;
    for (int t = 0; t < nth; t++) total_nodes += ctx[t].nodes;
    Root bestr = ctx[0].bestr;
    int bestsc = ctx[0].bestsc, bestdepth = ctx[0].bestdepth;
    nodes = total_nodes;
    printf("OK %d %d %lld %d", bestsc, bestdepth, nodes, bestr.pn);
    for (int i = 0; i < bestr.pn; i++) printf(" %d", (int)bestr.path[i]);
    printf("\n");
    fflush(stdout);
    return 0;
}

/* ------------------------- ponder: raqib o'ylayotganda o'ylash ------------------------- */
#ifdef HAVE_THR
static thr_t pth;
static int ponder_active = 0;
static Root prm[MAXM];
static TCtx pctx;

static void ponder_worker(void *arg) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    run_root((TCtx *)arg);
}
static void ponder_stop(void) {
    if (ponder_active) { stopf = 1; thr_join(pth); ponder_active = 0; }
}
/* bs: o'zimizning yurishimizdan KEYINGI taxta (o = bizniki), navbat raqibda. Raqib nuqtai nazaridan qidiramiz. */
static void ponder_start(const char *bs) {
    char f[65];
    for (int i = 0; i < 64; i++) {
        char ch = bs[63 - i];
        f[i] = ch == 'o' ? 'x' : ch == 'x' ? 'o' : ch == 'O' ? 'X' : ch == 'X' ? 'O' : '.';
    }
    f[64] = 0;
    load_board(f);
    Move all[MAXM];
    want_path = 1;
    int na = gen(1, all), n = 0;
    for (int i = 0; i < na; i++) {
        prm[n].m = all[i]; prm[n].pn = RPn[i];
        memcpy(prm[n].path, RP[i], 16);
        prm[n].pen = 0; prm[n].m.sc = 0;
        n++;
    }
    want_path = 0;
    if (n == 0) return;
    if (!g_gen_bumped) { g_gen++; g_gen_bumped = 1; }
    stopf = 0;
    memset(hist, 0, sizeof hist);
    memset(killer, 0, sizeof killer);
    long long t0 = now_ms();
    deadline = t0 + 1000000000LL;
    pctx.tid = 0; pctx.n = n; pctx.t0 = t0; pctx.limit = 1000000000LL;
    memcpy(pctx.board0, B, 64);
    pctx.rm = prm; pctx.bestr = prm[0]; pctx.bestsc = 0; pctx.bestdepth = 0; pctx.nodes = 0;
    ponder_active = thr_start(&pth, ponder_worker, &pctx);
}
#else
static void ponder_stop(void) { }
static void ponder_start(const char *bs) { (void)bs; }
#endif

#ifndef SERVER_TT_BITS
#define SERVER_TT_BITS 22
#endif

/* server rejimi: jarayon tirik, TT yurishlar orasida saqlanadi.
   Buyruqlar: NEWGAME | GO <ms> <taxta> <ro'yxatlar> | PONDER <taxta> | STOP | QUIT */
static int server_loop(void) {
    g_server = 1;
    TT_MASK = ((uint64_t)1 << SERVER_TT_BITS) - 1;
    TT = (TTS *)calloc((size_t)1 << SERVER_TT_BITS, sizeof(TTS));
    if (!TT) return 1;
    char cmd[16];
    while (scanf("%15s", cmd) == 1) {
        ponder_stop();
        if (strcmp(cmd, "QUIT") == 0) break;
        if (strcmp(cmd, "NEWGAME") == 0) {
            memset(TT, 0, sizeof(TTS) * ((size_t)1 << SERVER_TT_BITS));
            g_gen = 0; g_gen_bumped = 0;
        } else if (strcmp(cmd, "GO") == 0) {
            long long ms = 0;
            char bs[256];
            if (scanf("%lld", &ms) != 1 || scanf("%255s", bs) != 1 || strlen(bs) < 64) {
                printf("ERR\n"); fflush(stdout);
                continue;
            }
            read_req(&g_req);
            load_board(bs);
            do_search(ms, &g_req);
        } else if (strcmp(cmd, "PONDER") == 0) {
            char bs[256];
            if (scanf("%255s", bs) == 1 && strlen(bs) >= 64) ponder_start(bs);
        }
    }
    ponder_stop();
    return 0;
}

static void init_all(void) {
    init_nb();
    { const char *e = getenv("DRP");
      if (e) { int i = 0; char *q; while (i < P_N) { long v = strtol(e, &q, 10); if (q == e) break; P[i++] = (int)v; e = q; while (*e == ',' || *e == ' ') e++; } } }
    for (int i = 0; i < 5; i++) for (int j = 0; j < 64; j++) ZB[i][j] = rnd64();
    SK[0] = rnd64(); SK[1] = rnd64();
    tb_tables_init();
    { const char *e = getenv("DPST");
      if (e) { int i = 0; char *q; while (i < 64) { long v = strtol(e, &q, 10); if (q == e) break; if (i < 32) PSTM[i] = (int)v; else PSTK[i - 32] = (int)v; i++; e = q; while (*e == ',' || *e == ' ') e++; } } }
    init_eval_tables();
}

/* paket baholash: har qatorda "taxta navbat(0|1)"; chiqish: baho urish_bormi shashkalar_soni */
static int evals_mode(void) {
    char bs[256]; int sd;
    while (scanf("%255s %d", bs, &sd) == 2) {
        load_board(bs);
        int side = sd == 0 ? 1 : -1;
        Move ml[MAXM];
        int n = gen(side, ml);
        int cap = n > 0 && ml[0].nc > 0;
        printf("%d %d %d %d\n", eval(), cap, NPCS, n);
    }
    return 0;
}

int main(int argc, char **argv) {
    init_all();
    if (argc > 1 && strcmp(argv[1], "--server") == 0) return server_loop();
    if (argc > 1 && strcmp(argv[1], "--evals") == 0) return evals_mode();
    char cmd[16], bs[256];
    int arg;
    if (scanf("%15s %d", cmd, &arg) != 2) return 1;
    if (scanf("%255s", bs) != 1 || strlen(bs) < 64) return 1;
    load_board(bs);
    if (strcmp(cmd, "MOVES") == 0) {   /* sinov uchun: hozirgi taxtadagi barcha yurishlar yo'llari */
        Move all[MAXM];
        want_path = 1;
        int na = gen(1, all);
        printf("%d\n", na);
        for (int i = 0; i < na; i++) {
            for (int k = 0; k < RPn[i]; k++) printf("%d ", (int)RP[i][k]);
            printf("\n");
        }
        return 0;
    }
    if (strcmp(cmd, "PERFT") == 0) {
        printf("PERFT %lld\n", perft(1, arg, 0));
        return 0;
    }
    read_req(&g_req);
    return do_search(arg > 0 ? arg : 1000, &g_req);
}
