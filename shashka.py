# -*- coding: utf-8 -*-
"""
Shashka AI - Gramora (Telegram Desktop) uchun avtomatik shashka o'ynovchi.
Windows uchun. Rus shashkasi (8x8). Siz doim taxtaning pastida o'ynaysiz.
Kerakli kutubxonalar: mss, numpy   (1_ornatish.bat o'rnatadi)
"""
import os
import sys
import time
import json
import queue
import ctypes
import threading
import traceback

try:
    import tkinter as tk
except Exception:  # sinov uchun
    tk = None
try:
    import numpy as np
except Exception:
    np = None

# Ekran masshtabi (DPI) to'g'ri ishlashi uchun
if os.name == "nt":
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception:
            pass

if getattr(sys, "frozen", False):  # .exe bo'lib ishlayotganda
    HERE = os.path.dirname(sys.executable)
else:
    HERE = os.path.dirname(os.path.abspath(__file__))
LOG_PATH = os.path.join(HERE, "shashka_xato.txt")
ACT_LOG = os.path.join(HERE, "shashka_log.txt")


def log(msg):
    """Nima bo'layotganini faylga yozib boradi (muammo bo'lsa menga yuboring)."""
    try:
        if os.path.exists(ACT_LOG) and os.path.getsize(ACT_LOG) > 300000:
            os.remove(ACT_LOG)
        with open(ACT_LOG, "a", encoding="utf-8") as f:
            f.write("%s  %s\n" % (time.strftime("%H:%M:%S"), msg))
    except Exception:
        pass


# ======================================================================
#  1) O'YIN MANTIQI (rus shashkasi)
#     Taxta: 8x8 ro'yxat. +1 = mening shashkam, +2 = mening damkam,
#     -1 / -2 = raqibniki, 0 = bo'sh. Men yuqoriga (qator kamayadi) yuraman.
# ======================================================================
DIRS = ((-1, -1), (-1, 1), (1, -1), (1, 1))

# Damka urish qoidasi (dvigateldagi KING_STOP bilan bir xil bo'lishi kerak):
#   0 = standart rus qoidasi (zanjir majburiy, oxirigacha uriladi)
#   1 = damka zanjirning istalgan joyida to'xtay oladi
#   2 = Gramora: damka bir chiziqda (diagonalda) keyingi shashkani ura olsa, urishi SHART;
#       faqat burilib (yo'nalishni o'zgartirib) urishda to'xtash mumkin
# Oddiy shashka har doim zanjirni oxirigacha uradi.
KING_STOP_MODE = 2


def inb(r, c):
    return 0 <= r < 8 and 0 <= c < 8


def _king_can_capture(b, side, r, c, taken):
    for dr, dc in DIRS:
        nr, nc = r + dr, c + dc
        while inb(nr, nc) and b[nr][nc] == 0:
            nr += dr
            nc += dc
        if inb(nr, nc) and b[nr][nc] * side < 0 and (nr, nc) not in taken:
            lr, lc = nr + dr, nc + dc
            if inb(lr, lc) and b[lr][lc] == 0:
                return True
    return False


def _find_caps(b, side, r0, c0, out):
    piece = b[r0][c0]
    king0 = abs(piece) == 2
    promo = 0 if side == 1 else 7
    b[r0][c0] = 0  # yuruvchi shashka joyidan "ko'tarildi"

    def rec(r, c, king, path, taken):
        found = False
        for dr, dc in DIRS:
            if not king:
                tr, tc = r + dr, c + dc
                lr, lc = r + 2 * dr, c + 2 * dc
                if (inb(lr, lc) and (tr, tc) not in taken
                        and b[tr][tc] * side < 0 and b[lr][lc] == 0):
                    found = True
                    rec(lr, lc, king or lr == promo,
                        path + [(lr, lc)], taken + [(tr, tc)])
            else:
                nr, nc = r + dr, c + dc
                while inb(nr, nc) and b[nr][nc] == 0:
                    nr += dr
                    nc += dc
                if inb(nr, nc) and b[nr][nc] * side < 0 and (nr, nc) not in taken:
                    lands = []
                    lr, lc = nr + dr, nc + dc
                    while inb(lr, lc) and b[lr][lc] == 0:
                        lands.append((lr, lc))
                        lr += dr
                        lc += dc
                    if lands:
                        found = True
                        nt = taken + [(nr, nc)]
                        if KING_STOP_MODE:
                            same = False      # shu chiziq davomida (burilmasdan) yana urish mumkinmi?
                            if KING_STOP_MODE == 2:
                                tr2, tc2 = lands[-1][0] + dr, lands[-1][1] + dc
                                if inb(tr2, tc2) and b[tr2][tc2] * side < 0 and (tr2, tc2) not in nt:
                                    lr2, lc2 = tr2 + dr, tc2 + dc
                                    same = inb(lr2, lc2) and b[lr2][lc2] == 0
                            for l in lands:
                                if not same and _king_can_capture(b, side, l[0], l[1], nt):
                                    out.append((path + [l], nt, True))     # burilib davom etish mumkin, lekin shu yerda to'xtash ham mumkin
                                rec(l[0], l[1], True, path + [l], nt)
                        else:
                            cont = [l for l in lands if _king_can_capture(b, side, l[0], l[1], nt)]
                            for l in (cont if cont else lands):
                                rec(l[0], l[1], True, path + [l], nt)
        if not found and taken:
            out.append((path, taken, king))

    rec(r0, c0, king0, [(r0, c0)], [])
    b[r0][c0] = piece


def gen_moves(b, side):
    """Yurishlar: (yo'l, olingan_shashkalar, damka_bo'ladimi)"""
    caps = []
    for r in range(8):
        for c in range(8):
            if b[r][c] * side > 0:
                _find_caps(b, side, r, c, caps)
    if caps:
        return caps
    quiet = []
    fwd = -1 if side == 1 else 1
    promo = 0 if side == 1 else 7
    for r in range(8):
        for c in range(8):
            p = b[r][c]
            if p * side <= 0:
                continue
            if abs(p) == 1:
                for dc in (-1, 1):
                    nr, nc = r + fwd, c + dc
                    if inb(nr, nc) and b[nr][nc] == 0:
                        quiet.append(([(r, c), (nr, nc)], [], nr == promo))
            else:
                for dr, dc in DIRS:
                    nr, nc = r + dr, c + dc
                    while inb(nr, nc) and b[nr][nc] == 0:
                        quiet.append(([(r, c), (nr, nc)], [], True))
                        nr += dr
                        nc += dc
    return quiet


def apply_move(b, m, side):
    path, taken, king = m
    nb = [row[:] for row in b]
    r0, c0 = path[0]
    r1, c1 = path[-1]
    nb[r0][c0] = 0
    for (r, c) in taken:
        nb[r][c] = 0
    nb[r1][c1] = side * (2 if king else 1)
    return nb


def evaluate(b):
    s = 0
    matd = 0
    tot = 0
    for r in range(8):
        for c in range(8):
            p = b[r][c]
            if p == 0:
                continue
            if abs(p) == 1:
                if p > 0:
                    adv, back, cc = 7 - r, r == 7, c
                else:
                    adv, back, cc = r, r == 0, 7 - c
                v = 100 + adv * 6
                if adv == 6:
                    v += 18          # keyingi yurishda damka bo'ladi
                if 2 <= c <= 5 and 2 <= r <= 5:
                    v += 4           # markaz
                if back and cc in (2, 4):
                    v += 8           # orqa qator qo'riqchilari
                if c in (0, 7):
                    v -= 3           # chetdagi shashka kam harakatchan
                br = r + 1 if p > 0 else r - 1
                if 0 <= br < 8:
                    for cx in (c - 1, c + 1):
                        if 0 <= cx < 8 and b[br][cx] * p > 0:
                            v += 2   # orqasida himoyachi bor
            else:
                v = 340
                if r + c == 7:
                    v += 12          # bosh yo'l (a1-h8) - damka uchun eng kuchli
                elif 2 <= c <= 5 and 2 <= r <= 5:
                    v += 4
            s += v if p > 0 else -v
            matd += (100 if abs(p) == 1 else 340) * (1 if p > 0 else -1)
            tot += 1
    return s + matd * (24 - tot) // 100   # ustun bo'lsangiz almashtirish foydali


class Timeout(Exception):
    pass


_nodes = [0]
TT = {}


HIST = {}
MOB = 2   # harakatchanlik og'irligi


def _order(ms, ttm):
    def k(m):
        if ttm is not None and m[0] == ttm[0]:
            return -10 ** 6
        if m[1]:
            return -10 ** 4 - len(m[1]) * 10
        return -HIST.get((m[0][0], m[0][-1]), 0) - (5 if m[2] else 0)
    return sorted(ms, key=k)


def _negamax(b, side, depth, alpha, beta, dl):
    _nodes[0] += 1
    if _nodes[0] % 256 == 0 and time.time() > dl:
        raise Timeout()
    ms = gen_moves(b, side)
    if not ms:
        return -10000 - depth
    if depth <= 0 and not ms[0][1]:
        return side * evaluate(b) + MOB * len(ms)
    key = (tuple(x for row in b for x in row), side)
    ttm = None
    a0 = alpha
    ent = TT.get(key)
    if ent:
        d, flag, val, ttm = ent
        if d >= depth > 0:
            if flag == 0:
                return val
            if flag == 1 and val >= beta:
                return val
            if flag == 2 and val <= alpha:
                return val
    if len(ms) > 1:
        ms = _order(ms, ttm)
    best = -10 ** 9
    bm = None
    for m in ms:
        sc = -_negamax(apply_move(b, m, side), -side, depth - 1, -beta, -alpha, dl)
        if sc > best:
            best = sc
            bm = m
        if sc > alpha:
            alpha = sc
        if alpha >= beta:
            if not m[1]:
                kk = (m[0][0], m[0][-1])
                HIST[kk] = HIST.get(kk, 0) + depth * depth
            break
    if depth > 0:
        flag = 1 if best >= beta else (0 if best > a0 else 2)
        TT[key] = (depth, flag, best, bm)
    return best


THINK_TIME = 1.5   # soniya (o'rta o'yinda). Ko'paytirsangiz kuchliroq, lekin sekinroq


def _think_time(b):
    n = sum(1 for row in b for x in row if x)
    if n >= 22:
        return min(0.6, THINK_TIME)    # ochilish: tez
    return THINK_TIME


def choose_move_py(b, excluded=(), tlimit=None, seen=None):
    ms = [m for m in gen_moves(b, 1) if tuple(m[0]) not in excluded]
    if not ms:
        return None
    if len(ms) == 1:
        return ms[0]
    tlimit = tlimit or _think_time(b)
    if len(TT) > 400000:
        TT.clear()
    HIST.clear()
    dl = time.time() + tlimit
    best = ms[0]
    for depth in range(1, 30):
        try:
            alpha = -10 ** 9
            cur = None
            scored = []
            for m in ms:
                nb = apply_move(b, m, 1)
                sc = -_negamax(nb, -1, depth - 1, -10 ** 9, -alpha, dl)
                if seen and sig(nb) in seen:
                    sc -= 25         # bir xil holatni takrorlamaslik
                scored.append((sc, m))
                if sc > alpha or cur is None:
                    alpha = max(alpha, sc)
                    cur = m
            best = cur
            scored.sort(key=lambda x: -x[0])
            ms = [m for _, m in scored]
            if scored[0][0] > 9000:     # g'alaba topildi
                break
        except Timeout:
            break
        if time.time() > dl:
            break
    return best


ENGINE = [None, False]   # [yo'l, qidirildi]


def engine_path():
    """Tez C dvigatelni (engine.exe) qidiradi. Topilmasa None (Python dvigateli ishlatiladi)."""
    if ENGINE[1]:
        return ENGINE[0]
    ENGINE[1] = True
    dirs = []
    if getattr(sys, "_MEIPASS", None):
        dirs.append(sys._MEIPASS)
    dirs.append(HERE)
    dirs.append(os.path.dirname(os.path.abspath(__file__)))
    for d in dirs:
        for n in ("engine.exe", "engine"):
            fp = os.path.join(d, n)
            if os.path.isfile(fp):
                ENGINE[0] = fp
                log("C dvigatel topildi: %s" % fp)
                return fp
    log("C dvigatel topilmadi, Python dvigateli ishlatiladi")
    return None


def board_str(b):
    m = {1: "o", 2: "O", -1: "x", -2: "X", 0: "."}
    return "".join(m[b[r][c]] for r in range(8) for c in range(8))


def _engine_move(b, out):
    """Dvigatel javobini (OK ...) qonuniy yurishga aylantiradi. Topilmasa None."""
    if len(out) < 6 or out[0] != "OK":
        return None
    try:
        sc, dep = int(out[1]), int(out[2])
        v = list(map(int, out[4:]))
        L = v[0]
        path = [(q // 8, q % 8) for q in v[1:1 + L]]
    except Exception:
        return None
    log("dvigatel: baho=%d chuqurlik=%d" % (sc, dep))
    legal = gen_moves(b, 1)
    for m in legal:                      # Python qoidalari bilan tekshirish
        if [tuple(x) for x in m[0]] == path:
            return m
    for m in legal:
        if m[0][0] == path[0] and m[0][-1] == path[-1]:
            return m
    return None


def _request_text(b, excluded, seen_str):
    ex = ["%d %d" % (k[0][0] * 8 + k[0][1], k[-1][0] * 8 + k[-1][1]) for k in excluded]
    return "%s\n%d %s\n%d %s\n" % (board_str(b), len(ex), " ".join(ex), len(seen_str), " ".join(seen_str))


class CEngine:
    """Tirik C dvigatel (engine --server): jadval yurishlar orasida saqlanadi va raqib o'ylayotganda
    ham o'ylaydi (ponder). Ishlamasa - avtomatik eski usulga (har yurishda yangi jarayon) o'tadi."""

    def __init__(self, exe):
        self.exe = exe
        self.p = None
        self.q = None
        self.bad = False
        self.fails = 0
        self.lock = threading.Lock()

    def alive(self):
        return self.p is not None and self.p.poll() is None

    @staticmethod
    def _reader(p, q):
        try:
            for line in p.stdout:
                q.put(line)
        except Exception:
            pass
        q.put(None)

    def _start(self):
        import subprocess
        kw = {}
        if os.name == "nt":
            kw["creationflags"] = 0x08000000   # qora oyna chiqmasin
        self.p = subprocess.Popen([self.exe, "--server"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1, **kw)
        self.q = queue.Queue()
        threading.Thread(target=self._reader, args=(self.p, self.q), daemon=True).start()
        log("dvigatel server rejimida ishga tushdi")

    def _ensure(self):
        if self.bad:
            return False
        if not self.alive():
            try:
                self._start()
            except Exception as e:
                log("server dvigatelni ishga tushirib bo'lmadi: %s" % e)
                self.bad = True
                return False
        return True

    def _send(self, text):
        self.p.stdin.write(text)
        self.p.stdin.flush()

    def _fail(self, why):
        self.fails += 1
        log("server dvigatel xatosi (%d): %s" % (self.fails, why))
        self.kill()
        if self.fails >= 2:
            self.bad = True
            log("server rejimi o'chirildi, eski usul ishlatiladi")

    def kill(self):
        try:
            if self.p is not None:
                self.p.kill()
        except Exception:
            pass
        self.p = None

    def newgame(self):
        with self.lock:
            if self.alive():
                try:
                    self._send("NEWGAME\n")
                except Exception:
                    self.kill()

    def ponder(self, b_after):
        """Mening yurishimdan keyingi taxta: raqib o'ylayotganda dvigatel ham o'ylaydi."""
        with self.lock:
            if not self.alive():
                return
            try:
                self._send("PONDER\n%s\n" % board_str(b_after))
            except Exception:
                self.kill()

    def go(self, b, excluded, tlimit, seen_str):
        with self.lock:
            if not self._ensure():
                return None
            try:
                while True:                       # eski qatorlarni tashlab yuboramiz
                    self.q.get_nowait()
            except queue.Empty:
                pass
            try:
                self._send("GO %d\n%s" % (int(tlimit * 1000), _request_text(b, excluded, seen_str)))
                line = self.q.get(timeout=tlimit + 5)
            except queue.Empty:
                self._fail("javob kelmadi")
                return None
            except Exception as e:
                self._fail(str(e))
                return None
            if line is None:
                self._fail("dvigatel to'xtab qoldi")
                return None
            mv = _engine_move(b, line.split())
            if mv is None and not line.strip().startswith(("OK", "NONE")):
                self._fail("noma'lum javob: %r" % line[:60])
            return mv

    def close(self):
        with self.lock:
            try:
                if self.alive():
                    self._send("QUIT\n")
                    try:
                        self.p.wait(timeout=1.0)
                    except Exception:
                        pass
            except Exception:
                pass
            self.kill()


SERVER = [None]


def get_server():
    exe = engine_path()
    if not exe:
        return None
    if SERVER[0] is None:
        SERVER[0] = CEngine(exe)
    return None if SERVER[0].bad else SERVER[0]


def server_newgame():
    srv = SERVER[0]
    if srv is not None and not srv.bad:
        srv.newgame()


def server_ponder(b_after):
    srv = SERVER[0]
    if srv is not None and not srv.bad:
        srv.ponder(b_after)


def server_close():
    srv = SERVER[0]
    if srv is not None:
        srv.close()


def c_choose_oneshot(b, excluded, tlimit, seen_str):
    """Eski usul: har yurish uchun dvigatelni yangidan ishga tushiradi (zaxira)."""
    exe = engine_path()
    if not exe:
        return None
    import subprocess
    inp = "BEST %d\n%s" % (int(tlimit * 1000), _request_text(b, excluded, seen_str))
    kw = {}
    if os.name == "nt":
        kw["creationflags"] = 0x08000000   # qora oyna chiqmasin
    try:
        out = subprocess.run([exe], input=inp, capture_output=True, text=True,
                             timeout=tlimit + 5, **kw).stdout.split()
    except Exception as e:
        log("C dvigatel xatosi: %s" % e)
        return None
    return _engine_move(b, out)


def c_choose(b, excluded, tlimit, seen_str):
    srv = get_server()
    if srv is not None:
        mv = srv.go(b, excluded, tlimit, seen_str)
        if mv is not None:
            return mv
    return c_choose_oneshot(b, excluded, tlimit, seen_str)   # zaxira: eski usul


def choose_move(b, excluded=(), tlimit=None, seen=None, seen_str=()):
    ms = [m for m in gen_moves(b, 1) if tuple(m[0]) not in excluded]
    if not ms:
        return None
    if len(ms) == 1:
        return ms[0]
    mv = c_choose(b, excluded, tlimit or _think_time(b), list(seen_str)[-200:])
    if mv is not None:
        return mv
    return choose_move_py(b, excluded, tlimit, seen)


def sig(b):
    return tuple(tuple((x > 0) - (x < 0) for x in row) for row in b)


def fmt_sq(rc):
    return "%s%d" % ("abcdefgh"[rc[1]], 8 - rc[0])


# ======================================================================
#  2) TAXTANI EKRANDAN O'ZI TOPISH
# ======================================================================
def beige_mask(img):
    r=img[...,0].astype(np.int16); g=img[...,1].astype(np.int16); b=img[...,2].astype(np.int16)
    return (r>150)&(r>=g)&(g>b)&(r-b>32)&(r-b<90)&(g>110)

def detect_board(img):
    m=beige_mask(img)
    H,W=m.shape; st=3
    H2,W2=(H//st)*st,(W//st)*st
    ms=m[:H2,:W2].reshape(H2//st,st,W2//st,st).any(axis=(1,3))
    for _ in range(2):
        p=np.pad(ms,1); d=ms.copy()
        for dy in (-1,0,1):
            for dx in (-1,0,1):
                d|=p[1+dy:1+dy+ms.shape[0],1+dx:1+dx+ms.shape[1]]
        ms=d
    h,w=ms.shape
    seen=np.zeros_like(ms); cands=[]
    ys,xs=np.nonzero(ms)
    for y0,x0 in zip(ys,xs):
        if seen[y0,x0]: continue
        q=[(y0,x0)]; seen[y0,x0]=True
        mny=mxy=y0; mnx=mxx=x0; cnt=0
        while q:
            y,x=q.pop(); cnt+=1
            if y<mny:mny=y
            if y>mxy:mxy=y
            if x<mnx:mnx=x
            if x>mxx:mxx=x
            for yy,xx in ((y+1,x),(y-1,x),(y,x+1),(y,x-1)):
                if 0<=yy<h and 0<=xx<w and ms[yy,xx] and not seen[yy,xx]:
                    seen[yy,xx]=True; q.append((yy,xx))
        bw,bh=(mxx-mnx+1),(mxy-mny+1)
        if bw>=30 and bh>=30 and 0.85<=bw/bh<=1.18:
            cands.append((bw*bh,(mnx*st,mny*st,(mxx+1)*st,(mxy+1)*st)))
    cands.sort(reverse=True)
    for _,(x0,y0,x1,y1) in cands[:5]:
        mx=14
        X0,Y0=max(0,x0-mx),max(0,y0-mx); X1,Y1=min(W,x1+mx),min(H,y1+mx)
        res=fit_grid(m[Y0:Y1,X0:X1])
        if res:
            gx,gy,gs,score=res
            return (X0+gx,Y0+gy,X0+gx+8*gs,Y0+gy+8*gs)
    return None


_RR,_CC=np.meshgrid(np.arange(8),np.arange(8),indexing="ij")
_LIGHT=((_RR+_CC)%2==0)


def _score(I,x0,y0,s):
    Hh,Ww=I.shape[0]-1,I.shape[1]-1
    xa=(x0+_CC*s+0.07*s).astype(int); xb=(x0+_CC*s+0.93*s).astype(int)
    ya=(y0+_RR*s+0.07*s).astype(int); yb=(y0+_RR*s+0.93*s).astype(int)
    if xb.max()>Ww or yb.max()>Hh or xa.min()<0 or ya.min()<0: return -1
    tot=I[yb,xb]-I[ya,xb]-I[yb,xa]+I[ya,xa]
    f=tot/np.maximum(1,(xb-xa)*(yb-ya))
    return float(f[_LIGHT].mean()-f[~_LIGHT].mean())


def fit_grid(reg):
    reg=reg.astype(np.float64)
    I=np.pad(reg.cumsum(0).cumsum(1),((1,0),(1,0)))
    h,w=reg.shape
    ext=min(h,w)
    if ext<100: return None
    best=(-1,0,0,0)
    for s in range(int(ext*0.75/8),int(ext/8)+2):
        if s<12: continue
        for y0 in range(0,max(1,h-8*s+1),2):
            for x0 in range(0,max(1,w-8*s+1),2):
                sc=_score(I,x0,y0,s)
                if sc>best[0]: best=(sc,x0,y0,s)
    sc,bx,by,bs=best
    for s in range(bs-3,bs+4):
        for y0 in range(by-4,by+5):
            for x0 in range(bx-4,bx+5):
                sc2=_score(I,x0,y0,s)
                if sc2>best[0]: best=(sc2,x0,y0,s)
    sc,x0,y0,s=best
    return (x0,y0,s,sc) if sc>0.8 else None


def find_board():
    """Barcha ekranlarda shashka taxtasini qidiradi. (x1,y1,x2,y2) yoki None."""
    import mss
    best = None
    with mss.mss() as sct:
        for mon in sct.monitors[1:]:
            shot = sct.grab(mon)
            img = np.array(shot)[:, :, :3][:, :, ::-1]
            bb = detect_board(img)
            if bb:
                x0, y0, x1, y1 = [int(v) for v in bb]
                size = x1 - x0
                cand = (size, (x0 + mon["left"], y0 + mon["top"],
                               x1 + mon["left"], y1 + mon["top"]))
                if best is None or size > best[0]:
                    best = cand
                log("taxta topildi: %s | monitor: %sx%s | masshtab koeffitsiyenti=%.3f" % (
                    cand[1], mon["width"], mon["height"], CUR_K[0]))
    return best[1] if best else None


# ======================================================================
#  3) KATAKLARNI TANISH
# ======================================================================
# Haqiqiy Gramora skrinshotlaridan (kompyuter va telefon) o'rganilgan ranglar
DEFAULT_REFS = {
    "e": {"mean": [0.124, 0.1131, 0.0975, 0.0423, 0.0, 0.9999], "tol": 0.142},
    "w": {"mean": [0.763, 0.7189, 0.6424, 0.7438, 0.7277, 0.0777], "tol": 0.144},
    "b": {"mean": [0.1371, 0.1366, 0.1353, 0.3403, 0.0012, 0.7322], "tol": 0.128},
}


def grab(bbox):
    import mss
    x1, y1, x2, y2 = bbox
    with mss.mss() as s:
        shot = s.grab({"left": int(x1), "top": int(y1),
                       "width": int(x2 - x1), "height": int(y2 - y1)})
    return np.array(shot)[:, :, :3][:, :, ::-1]  # BGRA -> RGB


def grab_all(bbox):
    """Taxta + uning ostidagi navbat yozuvi."""
    import mss
    x1, y1, x2, y2 = bbox
    Hb = int(y2 - y1)
    with mss.mss() as sct:
        mon = sct.monitors[0]
        y_end = min(int(y2 + Hb * 0.30), mon["top"] + mon["height"])
        shot = sct.grab({"left": int(x1), "top": int(y1),
                         "width": int(x2 - x1), "height": int(y_end - y1)})
    return np.array(shot)[:, :, :3][:, :, ::-1], Hb


def my_turn_flag(img_full, Hb):
    """Yashil belgi (SIZNING NAVBATINGIZ) ko'rinsa True; ko'rinmasa False; o'qib bo'lmasa None."""
    W = img_full.shape[1]
    band = img_full[int(Hb * 1.08):int(Hb * 1.26), int(W * 0.2):int(W * 0.8)]
    if band.size == 0:
        return None
    r = band[..., 0].astype(np.int16)
    g = band[..., 1].astype(np.int16)
    b = band[..., 2].astype(np.int16)
    green = (g > 110) & (g > r + 35) & (g > b + 35)
    return bool(green.mean() > 0.002)


def safe_turn(bbox):
    try:
        full, Hb = grab_all(bbox)
        return my_turn_flag(full, Hb)
    except Exception:
        return None


def ring_cells(img):
    """O'yin ko'rsatgan belgilar: (yurish mumkin kataklari [qizil], tanlangan shashka [sariq])."""
    H, W = img.shape[:2]
    ch, cw = H / 8.0, W / 8.0
    r = img[..., 0].astype(np.int16)
    g = img[..., 1].astype(np.int16)
    b = img[..., 2].astype(np.int16)
    red = (r > 170) & (g < 110) & (b < 140) & (r - g > 90)
    yel = (r > 190) & (g > 150) & (b < 120) & (r - b > 100)
    dest, sel = set(), set()
    for rr in range(8):
        for cc in range(8):
            if (rr + cc) % 2 == 0:
                continue
            ya, yb = int(rr * ch), int((rr + 1) * ch)
            xa, xb = int(cc * cw), int((cc + 1) * cw)
            if red[ya:yb, xa:xb].mean() > 0.04:
                dest.add((rr, cc))
            if yel[ya:yb, xa:xb].mean() > 0.06:
                sel.add((rr, cc))
    return dest, sel


def cell_features(img):
    H, W = img.shape[:2]
    ch, cw = H / 8.0, W / 8.0
    out = {}
    for r in range(8):
        for c in range(8):
            if (r + c) % 2 == 0:  # yorug' kataklar o'ynalmaydi
                continue
            y0 = int(r * ch + ch * 0.12)
            y1 = int((r + 1) * ch - ch * 0.12)
            x0 = int(c * cw + cw * 0.12)
            x1 = int((c + 1) * cw - cw * 0.12)
            raw = img[y0:y1, x0:x1]
            rr_ = raw[..., 0].astype(np.int16)
            ring = (rr_ - raw[..., 1] > 25) & (rr_ - raw[..., 2] > 25)   # qizil/sariq halqa va uning soyasi
            if ring.mean() > 0.015:
                keep = raw[~ring]
                if len(keep) >= 20:
                    raw = keep.reshape(-1, 1, 3)
            p = raw.astype(np.float32)
            g = p.mean(axis=2)
            out[(r, c)] = np.array([
                p[..., 0].mean() / 255.0, p[..., 1].mean() / 255.0,
                p[..., 2].mean() / 255.0, g.std() / 80.0,
                float((g > 170).mean()), float((g < 45).mean())], dtype=np.float32)
    return out


def classify_cells(img):
    """Kataklarni 3 ta ishonchli belgi bo'yicha taniydi (rang namunalari kerak emas):
       - yorqinlik: oq shashka juda yorqin (~0.7), qora va bo'sh katak qorong'i (~0.1-0.18)
       - silliqlik (tekstura): bo'sh katak silliq (<=0.09), qora shashka aniq shaklga ega (>=0.28)
       - toj: damka markazida oltin rang bor (boshqalarda yo'q)
       Natija: 'e' bo'sh, 'w'/'b' oddiy shashka, 'W'/'B' damka."""
    H, W = img.shape[:2]
    ch, cw = H / 8.0, W / 8.0
    out = {}
    for r in range(8):
        for c in range(8):
            if (r + c) % 2 == 0:  # yorug' kataklar o'ynalmaydi
                continue
            y0 = int(r * ch + ch * 0.12)
            y1 = int((r + 1) * ch - ch * 0.12)
            x0 = int(c * cw + cw * 0.12)
            x1 = int((c + 1) * cw - cw * 0.12)
            raw = img[y0:y1, x0:x1]
            rr = raw[..., 0].astype(np.int16)
            ring = (rr - raw[..., 1] > 25) & (rr - raw[..., 2] > 25)   # qizil/sariq halqa va uning soyasi
            if ring.mean() > 0.015 and int((~ring).sum()) >= 20:
                keep = raw[~ring].reshape(-1, 1, 3)
            else:
                keep = raw
            p = keep.astype(np.float32)
            mean = float(p.mean() / 255.0)
            std = float(p.mean(axis=-1).std() / 80.0)
            if mean > 0.45:
                kind = "w"
            elif std > 0.17:
                kind = "b"
            else:
                out[(r, c)] = "e"
                continue
            # toj (damka): katak markazida oltin rang
            cy0, cy1 = int(r * ch + ch * 0.30), int((r + 1) * ch - ch * 0.30)
            cx0, cx1 = int(c * cw + cw * 0.30), int((c + 1) * cw - cw * 0.30)
            ce = img[cy0:cy1, cx0:cx1]
            r2 = ce[..., 0].astype(np.int16)
            g2 = ce[..., 1].astype(np.int16)
            b2 = ce[..., 2].astype(np.int16)
            gold = ((r2 > 150) & (g2 > 100) & (b2 < 120) & (r2 - b2 > 70) & (g2 - b2 > 40)).mean()
            out[(r, c)] = kind.upper() if gold > 0.03 else kind
    return out


def learn_refs(img):
    """Boshlang'ich holatdagi taxtadan rang namunalarini o'rganadi (zaxira usul)."""
    F = cell_features(img)
    top = [F[k] for k in F if k[0] <= 2]
    bot = [F[k] for k in F if k[0] >= 5]
    mid = [F[k] for k in F if k[0] in (3, 4)]
    bt = float(np.mean([f[:3].mean() for f in top]))
    bb = float(np.mean([f[:3].mean() for f in bot]))
    if abs(bt - bb) < 0.2:
        raise ValueError("boshlang'ich holat emas")
    if float(np.mean([f[:3].mean() for f in mid])) > 0.3:
        raise ValueError("o'rta qatorlar bo'sh emas")
    white, black = (bot, top) if bb > bt else (top, bot)
    refs = {}
    for name, lst in (("e", mid), ("w", white), ("b", black)):
        a = np.array(lst)
        m = a.mean(axis=0)
        d = max(float(np.linalg.norm(x - m)) for x in a)
        refs[name] = {"mean": [float(v) for v in m], "tol": float(max(d * 1.8 + 0.04, 0.10))}
    return refs


def classify(f, refs):
    d = {k: float(np.linalg.norm(f - np.array(v["mean"]))) for k, v in refs.items()}
    n = min(d, key=d.get)
    if d[n] <= refs[n]["tol"]:
        return n
    if d["e"] <= min(d["w"], d["b"]):
        return "e"
    return "W" if d["w"] < d["b"] else "B"  # katta farq = damka


def read_board(bbox, refs):
    """(taxta, navbat) qaytaradi."""
    img = grab(bbox)
    cb = classify_cells(img)
    return cb, safe_turn(bbox)


def save_debug(bbox):
    """Muammo bo'lsa menga yuborish uchun taxta rasmini saqlaydi."""
    try:
        import mss
        import mss.tools
        x1, y1, x2, y2 = bbox
        with mss.mss() as sct:
            shot = sct.grab({"left": int(x1), "top": int(y1),
                             "width": int(x2 - x1), "height": int(y2 - y1)})
            mss.tools.to_png(shot.rgb, shot.size, output=os.path.join(HERE, "shashka_taxta.png"))
    except Exception:
        pass


def board_valid(cb):
    nw = sum(1 for v in cb.values() if v in "wW")
    nb = sum(1 for v in cb.values() if v in "bB")
    return 0 < nw <= 12 and 0 < nb <= 12


def is_initial(cb):
    mid = all(cb[k] == "e" for k in cb if k[0] in (3, 4))
    top = {cb[k] for k in cb if k[0] <= 2}
    bot = {cb[k] for k in cb if k[0] >= 5}
    return mid and len(top) == 1 and len(bot) == 1 and top != bot and top <= {"w", "b"} and bot <= {"w", "b"}


def _cnt(b, side):
    return sum(1 for row in b for x in row if x * side > 0)


def _sgn(x):
    return (x > 0) - (x < 0)


def reconcile(model, cb, mine):
    """Shashkalarning O'RNI va RANGI ekrandan olinadi, DAMKA ekanligi esa o'z hisobimizdan:
    shashka damkaga faqat oxirgi qatorga yetganda aylanadi, keyin yurishlarini kuzatib boramiz.
    Ekrandagi g'alati belgilar (bo'sh katakdagi nur, siljish) yangi shashka sifatida qabul qilinmaydi.
    Qaytaradi: (taxta, holat): holat = "same" | "move" | "fallback" """
    vis = to_engine(cb, mine)
    sn = [[_sgn(x) for x in row] for row in vis]
    sm = [[_sgn(x) for x in row] for row in model]
    if sn == sm:
        return [row[:] for row in model], "same"

    # 1) O'yin qoidalari bo'yicha: qaysi QONUNIY yurish ekrandagi manzarani beradi?
    #    (ekrandagi 1-2 ta ortiqcha belgi e'tiborga olinmaydi; damka holati modeldan olinadi)
    best = None
    for side in (-1, 1):
        for m in gen_moves(model, side):
            res = apply_move(model, m, side)
            sr = [[_sgn(x) for x in row] for row in res]
            extra, odd, ok = 0, 0, True
            for r in range(8):
                for c in range(8):
                    if sr[r][c] != sn[r][c]:
                        if sr[r][c] == 0 and sn[r][c] != 0:
                            extra += 1
                            if cb.get((r, c), "") not in ("W", "B"):
                                odd += 1          # ortiqcha belgi g'alati ko'rinmaydi: ehtimoli past
                        else:
                            ok = False
                            break
                if not ok:
                    break
            if ok and extra <= 2:
                key = (extra, odd, 0 if side == -1 else 1)
                if best is None or key < best[0]:
                    best = (key, res)
    if best is not None:
        return best[1], ("move" if best[0][0] == 0 else "move+shubhali")

    out = [row[:] for row in model]
    ok = True
    for side in (1, -1):
        lost = [(r, c) for r in range(8) for c in range(8) if sm[r][c] == side and sn[r][c] != side]
        gained = [(r, c) for r in range(8) for c in range(8) if sn[r][c] == side and sm[r][c] != side]
        if not gained:
            for r, c in lost:
                out[r][c] = 0
        elif len(gained) == 1 and len(lost) == 1:
            (r0, c0), (r1, c1) = lost[0], gained[0]
            p = model[r0][c0]
            out[r0][c0] = 0
            if abs(p) == 1 and r1 == (0 if side == 1 else 7):
                p = 2 * side
            out[r1][c1] = p
        else:
            ok = False
    if ok and [[_sgn(x) for x in row] for row in out] == sn:
        return out, "move"
    # zaxira: bor shashkalarni saqlaymiz, yangilarini faqat o'rin bo'lsa qo'shamiz
    out = [[0] * 8 for _ in range(8)]
    for side in (1, -1):
        kept = [(r, c) for r in range(8) for c in range(8) if sn[r][c] == side and sm[r][c] == side]
        cand = [(r, c) for r in range(8) for c in range(8) if sn[r][c] == side and sm[r][c] != side]
        lost = [(r, c) for r in range(8) for c in range(8) if sm[r][c] == side and sn[r][c] != side]
        for r, c in kept:
            out[r][c] = model[r][c]
        budget = max(0, _cnt(model, side) - len(kept))

        def pri(q):
            geo = any(abs(q[0] - l[0]) == abs(q[1] - l[1]) for l in lost)
            return (not geo, cb.get(q, "") in ("W", "B"))   # diagonal bo'yida va g'alati bo'lmagan oldin
        cand.sort(key=pri)
        for (r, c) in cand[:budget]:
            out[r][c] = side * (2 if r == (0 if side == 1 else 7) else 1)
    return out, "fallback"


def detect_color(cb):
    sc = {}
    for col in "wb":
        sc[col] = (sum(1 for k, v in cb.items() if v.lower() == col and k[0] >= 4)
                   - sum(1 for k, v in cb.items() if v.lower() == col and k[0] <= 3))
    return "w" if sc["w"] >= sc["b"] else "b"


def to_engine(cb, mine):
    b = [[0] * 8 for _ in range(8)]
    for (r, c), ch in cb.items():
        if ch == "e":
            continue
        k = 2 if ch.isupper() else 1
        b[r][c] = k if ch.lower() == mine else -k
    return b


# ======================================================================
#  4) SICHQONCHA
# ======================================================================
def esc_pressed():
    if os.name != "nt":
        return False
    return bool(ctypes.windll.user32.GetAsyncKeyState(0x1B) & 0x8000)


class _PT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


CUR_K = [1.0]   # ekran rasmi koordinatasi / sichqoncha koordinatasi (masshtab 125% bo'lsa 1.25)
CAL_DONE = [False]   # koeffitsiyent belgi yordamida o'lchandimi
DESK = [0, 0, 1920, 1080]   # virtual ish stoli: chap, tepa, eni, bo'yi (haqiqiy piksellarda)


def init_click_scale():
    """Ekran o'lchamini aniqlaydi. Sichqoncha butun ekran bo'yicha MUTLAQ koordinatada boshqariladi,
    shuning uchun Windows ekran masshtabi (125%, 150%) xato keltirmaydi."""
    try:
        import mss
        with mss.mss() as sct:
            m = sct.monitors[0]
            DESK[:] = [m["left"], m["top"], m["width"], m["height"]]
        u = ctypes.windll.user32
        smw = u.GetSystemMetrics(78)
        if not CAL_DONE[0] and smw > 0 and abs(DESK[2] - smw) > 2:
            CUR_K[0] = DESK[2] / float(smw)
        log("ekran (mss): chap=%s tepa=%s eni=%s bo'yi=%s | tizim: eni=%s bo'yi=%s | koeffitsiyent=%.3f" % (
            DESK[0], DESK[1], DESK[2], DESK[3], smw, u.GetSystemMetrics(79), CUR_K[0]))
    except Exception as e:
        log("ekranni aniqlab bo'lmadi: %s" % e)


def _set_pos(x, y):
    """Sichqonchani ekran rasmidagi (x, y) nuqtaga qo'yadi.
    Rasm koordinatasi sichqoncha koordinatasidan CUR_K marta farq qilishi mumkin (ekran masshtabi)."""
    k = CUR_K[0]
    ctypes.windll.user32.SetCursorPos(int(round(x / k)), int(round(y / k)))


def scale_from_markers(found, pts):
    """Ekranga qo'yilgan 2 ta belgining rasmda topilgan o'rni bo'yicha masshtabni hisoblaydi.
    found: rasmda topilgan (x, y); pts: sichqoncha koordinatasida qo'yilgan (x, y)."""
    dx = found[1][0] - found[0][0]
    dy = found[1][1] - found[0][1]
    kx = dx / float(pts[1][0] - pts[0][0])
    ky = dy / float(pts[1][1] - pts[0][1])
    return (kx + ky) / 2.0


SICHQONCHA_SEKINLIGI = 0.7    # 0.5 = juda tez, 0.7 = tez, 1.0 = o'rtacha, 1.6 = sekin
SLOW_MULT = [1.0]             # yurish o'tmasa, qayta urinishlar avtomatik sekinlashadi (Bot.loop o'rnatadi)


def _f():
    return SICHQONCHA_SEKINLIGI * SLOW_MULT[0]


def mouse_move_smooth(x, y, steps=None):
    """Sichqonchani odamdek sekin va yumshoq olib boradi (boshida va oxirida sekinlashadi)."""
    u = ctypes.windll.user32
    p = _PT()
    u.GetCursorPos(ctypes.byref(p))
    sx, sy = float(p.x) * CUR_K[0], float(p.y) * CUR_K[0]
    dist = ((x - sx) ** 2 + (y - sy) ** 2) ** 0.5
    if dist > 4000:      # boshlang'ich nuqta noma'lum bo'lsa, sakramasdan o'ta qoladi
        sx, sy = x, y
    dur = min(0.6, max(0.2, 0.2 + dist / 2500.0)) * _f()
    n = steps or max(14, int(dur / 0.012))
    for i in range(1, n + 1):
        t = i / float(n)
        e = t * t * (3 - 2 * t)          # yumshoq boshlanish va to'xtash
        _set_pos(sx + (x - sx) * e, sy + (y - sy) * e)
        time.sleep(dur / n)
    _set_pos(x, y)          # aniq nuqtaga o'rnatish
    time.sleep(0.04)


def mouse_click(x, y, slow=False):
    u = ctypes.windll.user32
    f = _f()
    mouse_move_smooth(x, y)
    time.sleep((0.4 if slow else 0.22) * f)   # hover
    u.mouse_event(0x0002, 0, 0, 0, 0)         # bosish
    time.sleep((0.2 if slow else 0.12) * f)   # ushlab turish
    u.mouse_event(0x0004, 0, 0, 0, 0)         # qo'yib yuborish


def mouse_drag(x1, y1, x2, y2):
    """Shashkani tutib, boshqa katakka sekin sudraydi."""
    u = ctypes.windll.user32
    f = _f()
    mouse_move_smooth(x1, y1)
    time.sleep(0.3 * f)
    u.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.2 * f)
    mouse_move_smooth(x2, y2)
    time.sleep(0.2 * f)
    u.mouse_event(0x0004, 0, 0, 0, 0)


def window_is_active(bbox):
    """Taxta turgan oyna hozir faolmi? (Faol bo'lsa, oldindan bosish kerak emas.)"""
    try:
        u = ctypes.windll.user32
        cx = int((bbox[0] + bbox[2]) / 2 / CUR_K[0])
        cy = int((bbox[1] + bbox[3]) / 2 / CUR_K[0])
        h = u.WindowFromPoint(_PT(cx, cy))
        if not h:
            return False
        root = u.GetAncestor(h, 2)
        return bool(root) and root == u.GetForegroundWindow()
    except Exception:
        return False


def cell_xy(bbox, r, c):
    x1, y1, x2, y2 = bbox
    return x1 + (c + 0.5) * (x2 - x1) / 8.0, y1 + (r + 0.5) * (y2 - y1) / 8.0


# ======================================================================
#  5) AVTOMATIK O'YNOVCHI (taxtani o'zi topadi, navbat kelsa o'zi yuradi)
# ======================================================================
MODES = ["click", "slow", "steps", "drag", "slow"]


class Bot(threading.Thread):
    def __init__(self, say, state):
        threading.Thread.__init__(self, daemon=True)
        self.say = say
        self.state = state
        self.stop_ev = threading.Event()
        self.refs = DEFAULT_REFS

    def stop(self):
        self.stop_ev.set()

    def read_stable(self, bbox):
        """Taxtani 3 marta o'qiydi. Taxta oxirgi ikki o'qishda bir xil bo'lsa - barqaror.
        Navbat belgisi yonib-o'chib tursa ham: bir martasi yashil bo'lsa = sizning navbatingiz."""
        samples = []
        for i in range(3):
            samples.append(read_board(bbox, self.refs))
            if i < 2:
                time.sleep(0.1)
        if samples[1][0] != samples[2][0]:
            return None
        turns = [t for _, t in samples]
        if True in turns:
            turn = True
        elif False in turns:
            turn = False
        else:
            turn = None
        return samples[2][0], turn

    def run(self):
        try:
            init_click_scale()
            self.loop()
        except Exception as e:
            self.say("Xato: %s" % e)
            try:
                with open(LOG_PATH, "a", encoding="utf-8") as f:
                    f.write(traceback.format_exc() + "\n")
            except Exception:
                pass
        server_close()
        self.state("stopped")

    def _diag(self, bbox, mv):
        """Faqat yozib qo'yadi (hech narsani o'zgartirmaydi): o'yin qaysi kataklarni ko'rsatdi?"""
        try:
            dest, sel = ring_cells(grab(bbox))
            end = mv[0][-1]
            log("tanlandi %s | o'yin ko'rsatgan kataklar: %s | tanlangan: %s | oxirgi katak %s belgilanganmi: %s" % (
                fmt_sq(mv[0][0]), [fmt_sq(d) for d in sorted(dest)], [fmt_sq(d) for d in sorted(sel)],
                fmt_sq(end), end in dest))
        except Exception:
            pass

    def play(self, bbox, mv, mode="click"):
        """mode: click (oddiy), slow (sekin), drag (sudrab)"""
        x1, y1, x2, y2 = bbox
        slow = mode == "slow"
        # Oynani faollashtirish: taxta ramkasiga (o'yinga ta'sir qilmaydigan joy) bir bosish
        if not window_is_active(bbox):
            fx = x1 - max(4.0, (x2 - x1) / 8.0 * 0.15)
            mouse_click(fx, (y1 + y2) / 2.0, slow)
            time.sleep(0.35)
        # Zanjirli urishda ham faqat BOSHLANG'ICH shashka va OXIRGI katak bosiladi (o'yin oradagisini o'zi o'tadi)
        if mode == "steps":      # zaxira: zanjirning barcha kataklarini birma-bir bosish
            pts = [cell_xy(bbox, r, c) for (r, c) in mv[0]]
        else:
            pts = [cell_xy(bbox, *mv[0][0]), cell_xy(bbox, *mv[0][-1])]
        if mode == "drag":
            for i in range(len(pts) - 1):
                if self.stop_ev.is_set() or esc_pressed():
                    return
                mouse_drag(pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1])
                time.sleep(0.6 * _f())
        else:
            for i, (x, y) in enumerate(pts):
                if self.stop_ev.is_set() or esc_pressed():
                    return
                mouse_click(x, y, slow)
                time.sleep(((0.7 if slow else 0.5) if i == 0 else (0.6 if slow else 0.35)) * _f())
                if i == 0:
                    self._diag(bbox, mv)
        _set_pos(bbox[2] + 25, (bbox[1] + bbox[3]) / 2.0)

    def loop(self):
        bbox = None
        mine = None
        last_before = None
        last_after = None
        sent_key = None
        fails = 0
        excluded = []
        t_sent = 0
        unstable = 0
        last_turn = "?"
        seen = set()
        seen_str = []
        model = None          # o'zimiz yuritib borayotgan taxta (damkalar bilan)
        before_model = None
        while not self.stop_ev.is_set():
            if esc_pressed():
                self.say("Esc bosildi. Pauza. Davom ettirish uchun tugmani bosing.")
                return

            # 1) Taxtani ekrandan topish
            if bbox is None:
                bbox = find_board()
                if bbox is None:
                    self.say("Gramora taxtasi ekranda ko'rinmayapti. Kutyapman...")
                    mine = None
                    model = None
                    last_before = last_after = None
                    time.sleep(1.5)
                    continue
                self.say("Taxta topildi. Kuzatyapman...")

            # 2) Taxtani o'qish
            st = self.read_stable(bbox)
            if st is None:
                unstable += 1
                if unstable == 4:
                    self.say("Taxta harakatda (animatsiya). Kutyapman...")
                continue
            unstable = 0
            cb, turn = st
            if turn != last_turn:
                log("navbat belgisi: %s" % turn)
                last_turn = turn
            if not board_valid(cb):
                nw = sum(1 for v in cb.values() if v in "wW")
                nb = sum(1 for v in cb.values() if v in "bB")
                self.say("Taxta o'qilmadi (oq=%d, qora=%d). Qayta qidiryapman..." % (nw, nb))
                save_debug(bbox)
                bbox = None
                time.sleep(1.5)
                continue

            if is_initial(cb) and (mine is None or last_after is not None):
                mine = detect_color(cb)
                last_before = last_after = None
                fails = 0
                excluded = []
                seen = set()
                seen_str = []
                model = None
                before_model = None
                server_newgame()
                self.say("Yangi o'yin. Siz: %s." % ("OQ" if mine == "w" else "QORA"))
            if mine is None:
                mine = detect_color(cb)
                self.say("O'yin o'rtasidan qo'shildim. Siz: %s." % ("OQ" if mine == "w" else "QORA"))
            vis = to_engine(cb, mine)
            cur = sig(vis)
            if model is None:
                eng = vis                                   # boshlang'ich holat (yoki o'rtadan qo'shilish)
            elif before_model is not None and last_before is not None and cur == last_before:
                eng = [row[:] for row in before_model]      # yurishim qabul bo'lmadi
            else:
                eng, how = reconcile(model, cb, mine)
                if how != "same":
                    log("taxta o'zgardi (%s): %s" % (how, board_str(eng)))
                if [[_sgn(x) for x in row] for row in eng] != cur:
                    log("ekrandagi shubhali belgi e'tiborga olinmadi")
                    cur = sig(eng)
            model = eng

            # ASOSIY QOIDA: faqat "SIZNING NAVBATINGIZ" yonib turganda yuriladi
            if turn is False:
                self.say("Raqib o'ylayapti. Kutyapman...")
                time.sleep(0.15)
                continue
            if turn is None:
                self.say("Navbat belgisi ko'rinmayapti (taxta ostidagi yozuv).")

            if last_after is not None:
                if cur == last_after:  # yurishim qabul bo'ldi, belgi hali yangilanmagan
                    time.sleep(0.3)
                    continue
                if cur == last_before:  # yurishim o'tmadi
                    fails += 1
                    log("yurish o'tmadi (%d-marta), usul: %s" % (fails, MODES[min(fails - 1, 4)]))
                    if fails >= 6:
                        self.say("Yurish qabul qilinmayapti (6 marta). Pauza. shashka_log.txt faylini yuboring.")
                        return
                    if fails == 4:
                        excluded.append(sent_key)
                    self.say("Yurish o'tmadi. Boshqa usul bilan qayta urinaman (%d)..." % fails)
                else:
                    fails = 0
                    excluded = []

            mv = choose_move(eng, excluded, seen=seen, seen_str=seen_str)
            if mv is None:
                self.say("Yurish qolmadi. O'yin tugagan bo'lishi mumkin.")
                time.sleep(2.0)
                continue
            server_ponder(apply_move(eng, mv, 1))   # sichqoncha yurayotgan va raqib o'ylayotgan vaqtda ham o'ylaymiz
            self.say("Yuryapman: " + " > ".join(fmt_sq(p) for p in mv[0]))
            mode = MODES[min(fails, 4)]
            SLOW_MULT[0] = 1.0 + 0.6 * fails      # o'tmasa: har urinishda sekinroq
            log("yuraman: %s | usul=%s | taxta: %s" % (" > ".join(fmt_sq(p) for p in mv[0]), mode, board_str(eng)))
            self.play(bbox, mv, mode)
            if self.stop_ev.is_set():
                return
            before_model = [row[:] for row in eng]
            model = apply_move(eng, mv, 1)
            last_before = cur
            last_after = sig(model)
            seen.add(last_after)
            seen_str.append(board_str(apply_move(eng, mv, 1)))
            sent_key = tuple(mv[0])
            t_sent = time.time()
            time.sleep(0.4)


# ======================================================================
#  6) KICHIK SUZUVCHI OYNA
# ======================================================================
BG = "#1e1e1e"


class App:
    def __init__(self):
        self.root = tk.Tk()
        r = self.root
        r.title("Shashka AI")
        r.overrideredirect(True)
        r.attributes("-topmost", True)
        r.configure(bg=BG)
        r.geometry("+%d+%d" % (r.winfo_screenwidth() - 330, 30))
        self.bot = None
        self.q = queue.Queue()
        self._dx = self._dy = 0

        self.full = tk.Frame(r, bg=BG)
        self.mini = tk.Label(r, text="\u265f", font=("Segoe UI", 18), fg="white",
                             bg="#555555", width=3, cursor="hand2")
        self.mini.bind("<Button-1>", lambda e: self.expand())

        bar = tk.Frame(self.full, bg="#2b2b2b")
        bar.pack(fill="x")
        title = tk.Label(bar, text="\u265f Shashka AI (avto)", fg="white", bg="#2b2b2b",
                         font=("Segoe UI", 10, "bold"))
        title.pack(side="left", padx=8, pady=4)
        tk.Button(bar, text="\u2715", command=self.close, bg="#2b2b2b", fg="white",
                  relief="flat", bd=0, padx=8).pack(side="right")
        tk.Button(bar, text="\u2013", command=self.collapse, bg="#2b2b2b", fg="white",
                  relief="flat", bd=0, padx=8).pack(side="right")
        for w in (bar, title):
            w.bind("<Button-1>", self._drag_start)
            w.bind("<B1-Motion>", self._drag_move)

        self.status = tk.Label(self.full, text="", fg="#dddddd", bg=BG, wraplength=270,
                               justify="left", font=("Segoe UI", 9))
        self.status.pack(padx=10, pady=(8, 6), anchor="w")
        row = tk.Frame(self.full, bg=BG)
        row.pack(fill="x", padx=10, pady=(0, 10))
        self.btn = tk.Button(row, text="\u23f8  Pauza", command=self.toggle,
                             bg="#b71c1c", fg="white", relief="flat", font=("Segoe UI", 10, "bold"))
        self.btn.pack(side="left", expand=True, fill="x", padx=2)
        tk.Button(row, text="Sinov", command=self.mouse_test, bg="#3a3a3a", fg="white",
                  relief="flat", font=("Segoe UI", 9)).pack(side="left", expand=True, fill="x", padx=2)
        tk.Button(row, text="Ko'rish", command=self.show_board, bg="#3a3a3a", fg="white",
                  relief="flat", font=("Segoe UI", 9)).pack(side="left", expand=True, fill="x", padx=2)
        self.full.pack()

        self.root.after(150, self.poll)
        self.root.after(300, self.calibrate_space)   # sichqoncha va ekran rasmi masshtabini o'lchash
        self.root.after(1500, self.start_bot)        # o'zi ishga tushadi
        self.root.after(3500, self.collapse)         # va kichik doiraga aylanadi

    # --- oyna ---
    def _drag_start(self, e):
        self._dx, self._dy = e.x_root - self.root.winfo_x(), e.y_root - self.root.winfo_y()

    def _drag_move(self, e):
        self.root.geometry("+%d+%d" % (e.x_root - self._dx, e.y_root - self._dy))

    def say(self, t):
        self.status.config(text=t)

    def collapse(self):
        self.full.pack_forget()
        running = self.bot is not None and self.bot.is_alive()
        self.mini.config(bg="#2e7d32" if running else "#555555")
        self.mini.pack()

    def expand(self):
        self.mini.pack_forget()
        self.full.pack()

    def close(self):
        if self.bot:
            self.bot.stop()
        self.root.destroy()

    def poll(self):
        try:
            while True:
                kind, val = self.q.get_nowait()
                if kind == "say":
                    self.say(val)
                elif kind == "state" and val == "stopped":
                    self.btn.config(text="\u25b6  Davom", bg="#2e7d32")
                    self.mini.config(bg="#555555")
                    self.expand()
        except queue.Empty:
            pass
        self.root.after(150, self.poll)

    # --- masshtabni o'lchash ---
    def calibrate_space(self):
        """Ekranga 2 ta rangli belgi qo'yib, ekran rasmida qayerda ko'rinishini o'lchaydi.
        Shu orqali sichqoncha koordinatasi bilan rasm koordinatasi orasidagi farq (masshtab) aniqlanadi."""
        try:
            import mss
            pts = [(200, 200), (900, 600)]
            found = []
            for (px, py) in pts:
                w = tk.Toplevel(self.root)
                w.overrideredirect(True)
                w.attributes("-topmost", True)
                w.configure(bg="#ff00ff")
                w.geometry("14x14+%d+%d" % (px, py))
                w.update()
                time.sleep(0.3)
                w.update()
                with mss.mss() as sct:
                    shot = sct.grab(sct.monitors[1])
                img = np.array(shot)[:, :, :3][:, :, ::-1]
                w.destroy()
                self.root.update()
                m = (img[..., 0] > 230) & (img[..., 1] < 40) & (img[..., 2] > 230)
                ys, xs = np.nonzero(m)
                if len(xs) < 20:
                    raise ValueError("belgi rasmda topilmadi")
                found.append((float(xs.mean()), float(ys.mean())))
            k = scale_from_markers(found, pts)
            if 0.5 < k < 4.0:
                CUR_K[0] = k
                CAL_DONE[0] = True
            log("masshtab o'lchandi: koeffitsiyent=%.3f (belgilar: %s)" % (k, found))
        except Exception as e:
            log("masshtabni o'lchab bo'lmadi: %s" % e)

    # --- sichqoncha sinovi ---
    def mouse_test(self):
        def work():
            try:
                self.q.put(("say", "Sinov: taxtani qidiryapman..."))
                bbox = find_board()
                if bbox is None:
                    self.q.put(("say", "Sinov: taxta ekranda topilmadi."))
                    return
                self.q.put(("say", "Sinov: sichqoncha taxtaning 4 burchagidagi kataklarga boradi. Qarab turing..."))
                for (r, c) in [(0, 1), (0, 7), (7, 6), (7, 0)]:
                    x, y = cell_xy(bbox, r, c)
                    mouse_move_smooth(x, y)
                    time.sleep(1.3)
                self.q.put(("say", "Sinov tugadi. Sichqoncha burchaklardagi kataklarga aniq bordimi? "
                                   "Oshib ketsa yoki chetga tushsa, shashka_log.txt faylini yuboring."))
            except Exception as e:
                self.q.put(("say", "Sinov xatosi: %s" % e))
        threading.Thread(target=work, daemon=True).start()

    # --- ishga tushirish / pauza ---
    def start_bot(self):
        if self.bot is not None and self.bot.is_alive():
            return
        self.bot = Bot(lambda m: self.q.put(("say", m)), lambda s: self.q.put(("state", s)))
        self.bot.start()
        self.btn.config(text="\u23f8  Pauza", bg="#b71c1c")
        self.mini.config(bg="#2e7d32")
        self.say("Ishlayapti. Taxtani o'zim topaman. Pauza: Esc.")

    def toggle(self):
        if self.bot is not None and self.bot.is_alive():
            self.bot.stop()
            self.say("Pauza qilinmoqda...")
        else:
            self.start_bot()
            self.root.after(1500, self.collapse)

    # --- ko'rish (tekshirish uchun) ---
    def show_board(self):
        try:
            self.root.withdraw()
            self.root.update()
            time.sleep(0.35)
            try:
                bbox = find_board()
                if bbox is None:
                    raise ValueError("Taxta ekranda topilmadi.")
                img = grab(bbox)
                turn = safe_turn(bbox)
            finally:
                self.root.deiconify()
            refs = self.bot.refs if self.bot else DEFAULT_REFS
            cb = classify_cells(img)
            save_debug(bbox)
        except Exception as e:
            self.say("Bo'lmadi: %s" % e)
            return
        sym = {"e": "\u00b7", "w": "o", "W": "O", "b": "x", "B": "X"}
        lines = []
        for r in range(8):
            lines.append(" ".join(sym[cb[(r, c)]] if (r, c) in cb else " " for c in range(8)))
        win = tk.Toplevel(self.root)
        win.title("Ilova taxtani shunday ko'ryapti")
        win.attributes("-topmost", True)
        tk.Label(win, text="\n".join(lines), font=("Consolas", 16), justify="left",
                 padx=20, pady=12).pack()
        tk.Label(win, text="o/O = oq (damka katta harf)   x/X = qora\n"
                           "Navbat: " + {True: "SIZNING NAVBATINGIZ", False: "raqibniki",
                                         None: "o'qib bo'lmadi"}[turn] + "\n"
                           "Yopish uchun bosing.", font=("Segoe UI", 9)).pack(pady=(0, 10))
        win.bind("<Button-1>", lambda e: win.destroy())


def main():
    try:
        App().root.mainloop()
    except Exception:
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(traceback.format_exc() + "\n")
        raise


if __name__ == "__main__":
    main()
