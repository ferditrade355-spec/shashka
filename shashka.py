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
            else:
                v = 340
                if r + c == 7:
                    v += 12          # bosh yo'l (a1-h8) - damka uchun eng kuchli
                elif 2 <= c <= 5 and 2 <= r <= 5:
                    v += 4
            s += v if p > 0 else -v
    return s


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


THINK_TIME = 2.0   # soniya (o'rta o'yinda). Ko'paytirsangiz kuchliroq, lekin sekinroq


def _think_time(b):
    n = sum(1 for row in b for x in row if x)
    if n >= 22:
        return min(1.2, THINK_TIME)    # ochilish: tez
    return THINK_TIME


def choose_move(b, excluded=(), tlimit=None, seen=None):
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
            p = img[y0:y1, x0:x1].astype(np.float32)
            g = p.mean(axis=2)
            out[(r, c)] = np.array([
                p[..., 0].mean() / 255.0, p[..., 1].mean() / 255.0,
                p[..., 2].mean() / 255.0, g.std() / 80.0,
                float((g > 170).mean()), float((g < 45).mean())], dtype=np.float32)
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
    F = cell_features(img)
    cb = {k: classify(f, refs) for k, f in F.items()}
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


CUR_K = [1.0]   # GetCursorPos -> haqiqiy piksel koeffitsiyenti (ekran masshtabi 125% bo'lsa 1.25)
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
        if smw > 0 and abs(DESK[2] - smw) > 2:
            CUR_K[0] = DESK[2] / float(smw)
        log("ekran (mss): chap=%s tepa=%s eni=%s bo'yi=%s | tizim: eni=%s bo'yi=%s | koeffitsiyent=%.3f" % (
            DESK[0], DESK[1], DESK[2], DESK[3], smw, u.GetSystemMetrics(79), CUR_K[0]))
    except Exception as e:
        log("ekranni aniqlab bo'lmadi: %s" % e)


def _set_pos(x, y):
    """Sichqonchani ekranning (x, y) haqiqiy pikseliga qo'yadi (masshtabga bog'liq emas)."""
    L, T, W, H = DESK
    nx = int((x - L + 0.5) * 65536.0 / W)
    ny = int((y - T + 0.5) * 65536.0 / H)
    nx = max(0, min(65535, nx))
    ny = max(0, min(65535, ny))
    # MOVE | ABSOLUTE | VIRTUALDESK
    ctypes.windll.user32.mouse_event(0x0001 | 0x8000 | 0x4000, nx, ny, 0, 0)


SICHQONCHA_SEKINLIGI = 1.6   # 1.0 = o'rtacha, 1.6 = sekin, 2.0+ = juda sekin


def mouse_move_smooth(x, y, steps=None):
    """Sichqonchani odamdek sekin va yumshoq olib boradi (boshida va oxirida sekinlashadi)."""
    u = ctypes.windll.user32
    p = _PT()
    u.GetCursorPos(ctypes.byref(p))
    sx, sy = float(p.x) * CUR_K[0], float(p.y) * CUR_K[0]
    dist = ((x - sx) ** 2 + (y - sy) ** 2) ** 0.5
    if dist > 4000:      # boshlang'ich nuqta noma'lum bo'lsa, sakramasdan o'ta qoladi
        sx, sy = x, y
    dur = min(0.9, max(0.35, 0.3 + dist / 1800.0)) * SICHQONCHA_SEKINLIGI
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
    f = SICHQONCHA_SEKINLIGI
    mouse_move_smooth(x, y)
    time.sleep((0.55 if slow else 0.4) * f)   # hover
    u.mouse_event(0x0002, 0, 0, 0, 0)         # bosish
    time.sleep((0.25 if slow else 0.18) * f)  # ushlab turish
    u.mouse_event(0x0004, 0, 0, 0, 0)         # qo'yib yuborish


def mouse_drag(x1, y1, x2, y2):
    """Shashkani tutib, boshqa katakka sekin sudraydi."""
    u = ctypes.windll.user32
    f = SICHQONCHA_SEKINLIGI
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
        cx = int((bbox[0] + bbox[2]) / 2)
        cy = int((bbox[1] + bbox[3]) / 2)
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
MODES = ["click", "slow", "drag", "slow"]


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
                time.sleep(0.2)
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
        self.state("stopped")

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
        pts = [cell_xy(bbox, *mv[0][0]), cell_xy(bbox, *mv[0][-1])]
        if mode == "drag":
            for i in range(len(pts) - 1):
                if self.stop_ev.is_set() or esc_pressed():
                    return
                mouse_drag(pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1])
                time.sleep(0.8 * SICHQONCHA_SEKINLIGI)
        else:
            for i, (x, y) in enumerate(pts):
                if self.stop_ev.is_set() or esc_pressed():
                    return
                mouse_click(x, y, slow)
                time.sleep(((0.9 if slow else 0.7) if i == 0 else (0.8 if slow else 0.6)) * SICHQONCHA_SEKINLIGI)
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
                # zaxira: rang namunalarini shu ekrandan o'rganib ko'rish
                try:
                    new_refs = learn_refs(grab(bbox))
                    self.refs = new_refs
                    self.say("Ranglarni qayta o'rgandim.")
                    continue
                except Exception:
                    pass
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
                self.say("Yangi o'yin. Siz: %s." % ("OQ" if mine == "w" else "QORA"))
            if mine is None:
                mine = detect_color(cb)
                self.say("O'yin o'rtasidan qo'shildim. Siz: %s." % ("OQ" if mine == "w" else "QORA"))
            eng = to_engine(cb, mine)
            cur = sig(eng)

            # ASOSIY QOIDA: faqat "SIZNING NAVBATINGIZ" yonib turganda yuriladi
            if turn is False:
                self.say("Raqib o'ylayapti. Kutyapman...")
                time.sleep(0.3)
                continue
            if turn is None:
                self.say("Navbat belgisi ko'rinmayapti (taxta ostidagi yozuv).")

            if last_after is not None:
                if cur == last_after:  # yurishim qabul bo'ldi, belgi hali yangilanmagan
                    time.sleep(0.3)
                    continue
                if cur == last_before:  # yurishim o'tmadi
                    fails += 1
                    log("yurish o'tmadi (%d-marta), usul: %s" % (fails, MODES[min(fails - 1, 3)]))
                    if fails >= 5:
                        self.say("Yurish qabul qilinmayapti (5 marta). Pauza. shashka_log.txt faylini yuboring.")
                        return
                    if fails == 3:
                        excluded.append(sent_key)
                    self.say("Yurish o'tmadi. Boshqa usul bilan qayta urinaman (%d)..." % fails)
                else:
                    fails = 0
                    excluded = []

            mv = choose_move(eng, excluded, seen=seen)
            if mv is None:
                self.say("Yurish qolmadi. O'yin tugagan bo'lishi mumkin.")
                time.sleep(2.0)
                continue
            self.say("Yuryapman: " + " > ".join(fmt_sq(p) for p in mv[0]))
            mode = MODES[min(fails, 3)]
            log("yuraman: %s | usul=%s | taxta=%s" % (" > ".join(fmt_sq(p) for p in mv[0]), mode, bbox))
            self.play(bbox, mv, mode)
            if self.stop_ev.is_set():
                return
            last_before = cur
            last_after = sig(apply_move(eng, mv, 1))
            seen.add(last_after)
            sent_key = tuple(mv[0])
            t_sent = time.time()
            time.sleep(0.9)


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
        tk.Button(row, text="Ko'rish", command=self.show_board, bg="#3a3a3a", fg="white",
                  relief="flat", font=("Segoe UI", 9)).pack(side="left", expand=True, fill="x", padx=2)
        self.full.pack()

        self.root.after(150, self.poll)
        self.root.after(500, self.start_bot)      # o'zi ishga tushadi
        self.root.after(2500, self.collapse)      # va kichik doiraga aylanadi

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
            F = cell_features(img)
            cb = {k: classify(f, refs) for k, f in F.items()}
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
