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
CFG_PATH = os.path.join(HERE, "shashka_config.json")
LOG_PATH = os.path.join(HERE, "shashka_xato.txt")


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


def _order(ms, ttm):
    def k(m):
        if ttm is not None and m[0] == ttm[0]:
            return -1000
        return -len(m[1]) * 10 - (5 if m[2] else 0)
    return sorted(ms, key=k)


def _negamax(b, side, depth, alpha, beta, dl):
    _nodes[0] += 1
    if _nodes[0] % 256 == 0 and time.time() > dl:
        raise Timeout()
    ms = gen_moves(b, side)
    if not ms:
        return -10000 - depth
    if depth <= 0 and not ms[0][1]:
        return side * evaluate(b)
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
            break
    if depth > 0:
        flag = 1 if best >= beta else (0 if best > a0 else 2)
        TT[key] = (depth, flag, best, bm)
    return best


THINK_TIME = 3.0   # soniya (o'rta o'yinda). Ko'paytirsangiz kuchliroq, lekin sekinroq


def _think_time(b):
    n = sum(1 for row in b for x in row if x)
    if n >= 22:
        return min(1.2, THINK_TIME)    # ochilish: tez
    return THINK_TIME


def choose_move(b, excluded=(), tlimit=None):
    ms = [m for m in gen_moves(b, 1) if tuple(m[0]) not in excluded]
    if not ms:
        return None
    if len(ms) == 1:
        return ms[0]
    tlimit = tlimit or _think_time(b)
    if len(TT) > 400000:
        TT.clear()
    dl = time.time() + tlimit
    best = ms[0]
    for depth in range(1, 30):
        try:
            alpha = -10 ** 9
            cur = None
            scored = []
            for m in ms:
                sc = -_negamax(apply_move(b, m, 1), -1, depth - 1, -10 ** 9, -alpha, dl)
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
#  2) EKRANNI O'QISH
# ======================================================================
def grab(bbox):
    import mss
    x1, y1, x2, y2 = bbox
    with mss.mss() as s:
        shot = s.grab({"left": int(x1), "top": int(y1),
                       "width": int(x2 - x1), "height": int(y2 - y1)})
    return np.array(shot)[:, :, :3][:, :, ::-1]  # BGRA -> RGB


def grab_all(bbox):
    """Taxta + uning ostidagi navbat yozuvi (SIZNING NAVBATINGIZ / Raqib o'ylamoqda)."""
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
    """Yashil belgi ko'rinsa = sizning navbatingiz. True / False / None(o'qib bo'lmadi)"""
    W = img_full.shape[1]
    band = img_full[int(Hb * 1.08):int(Hb * 1.26), int(W * 0.2):int(W * 0.8)]
    if band.size == 0:
        return None
    r = band[..., 0].astype(np.int16)
    g = band[..., 1].astype(np.int16)
    b = band[..., 2].astype(np.int16)
    green = (g > 110) & (g > r + 35) & (g > b + 35)
    return bool(green.mean() > 0.003)


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
    """Boshlang'ich holatdagi taxtadan: bo'sh / oq / qora shashka namunalarini o'rganadi."""
    F = cell_features(img)
    top = [F[k] for k in F if k[0] <= 2]
    bot = [F[k] for k in F if k[0] >= 5]
    mid = [F[k] for k in F if k[0] in (3, 4)]
    bt = float(np.mean([f[:3].mean() for f in top]))
    bb = float(np.mean([f[:3].mean() for f in bot]))
    if abs(bt - bb) < 0.2:
        raise ValueError("Boshlang'ich holat emas. Yangi o'yin boshida (shashkalar joyida) o'rgating.")
    if float(np.mean([f[:3].mean() for f in mid])) > 0.3:
        raise ValueError("O'rtadagi 2 qator bo'sh emas. Taxta aniq belgilanganmi? Qayta «1 Taxta».")
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


def safe_turn(bbox):
    """Navbat belgisi. Xato bo'lsa None qaytaradi (taxtani o'qishga halaqit bermaydi)."""
    try:
        full, Hb = grab_all(bbox)
        return my_turn_flag(full, Hb)
    except Exception:
        return None


def read_board(bbox, refs):
    """(taxta, navbat) qaytaradi. Taxta avvalgi usulda o'qiladi."""
    img = grab(bbox)
    F = cell_features(img)
    cb = {k: classify(f, refs) for k, f in F.items()}
    return cb, safe_turn(bbox)


def save_debug(bbox):
    """Taxta rasmini saqlaydi (muammo bo'lsa menga yuborish uchun)."""
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
#  3) SICHQONCHA
# ======================================================================
def esc_pressed():
    if os.name != "nt":
        return False
    return bool(ctypes.windll.user32.GetAsyncKeyState(0x1B) & 0x8000)


def mouse_click(x, y):
    u = ctypes.windll.user32
    u.SetCursorPos(int(x), int(y))
    time.sleep(0.08)
    u.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.06)
    u.mouse_event(0x0004, 0, 0, 0, 0)


def cell_xy(bbox, r, c):
    x1, y1, x2, y2 = bbox
    return x1 + (c + 0.5) * (x2 - x1) / 8.0, y1 + (r + 0.5) * (y2 - y1) / 8.0


# ======================================================================
#  4) O'YNOVCHI ISH JARAYONI
# ======================================================================
class Bot(threading.Thread):
    def __init__(self, cfg, say, state):
        threading.Thread.__init__(self, daemon=True)
        self.cfg = cfg
        self.say = say
        self.state = state
        self.stop_ev = threading.Event()

    def stop(self):
        self.stop_ev.set()

    def read_stable(self):
        a = read_board(self.cfg["bbox"], self.cfg["refs"])
        time.sleep(0.45)
        b = read_board(self.cfg["bbox"], self.cfg["refs"])
        return b if a == b else None

    def run(self):
        try:
            self.loop()
        except Exception as e:
            self.say("Xato: %s" % e)
            try:
                with open(LOG_PATH, "a", encoding="utf-8") as f:
                    f.write(traceback.format_exc() + "\n")
            except Exception:
                pass
        self.state("stopped")

    def play(self, mv):
        bbox = self.cfg["bbox"]
        for i, (r, c) in enumerate(mv[0]):
            if self.stop_ev.is_set() or esc_pressed():
                return
            x, y = cell_xy(bbox, r, c)
            mouse_click(x, y)
            time.sleep(0.35 if i else 0.3)
        mouse_click_free = bbox[2] + 25
        ctypes.windll.user32.SetCursorPos(int(mouse_click_free), int((bbox[1] + bbox[3]) / 2))

    def loop(self):
        mine = None
        last_before = None
        last_after = None
        sent_key = None
        fails = 0
        excluded = []
        t_sent = 0
        self.say("Taxtani kuzatyapman. To'xtatish: Esc.")
        while not self.stop_ev.is_set():
            if esc_pressed():
                self.say("Esc bosildi. To'xtadim.")
                return
            st = self.read_stable()
            if st is None:
                continue
            cb, turn = st
            if not board_valid(cb):
                nw = sum(1 for v in cb.values() if v in "wW")
                nb = sum(1 for v in cb.values() if v in "bB")
                self.say("Taxta noto'g'ri o'qildi (oq=%d, qora=%d). Rasm: shashka_taxta.png. "
                         "«1 Taxta» va «2 O'rgatish» ni qayta bajaring." % (nw, nb))
                save_debug(self.cfg["bbox"])
                time.sleep(2.0)
                continue
            if is_initial(cb) and (mine is None or last_after is not None):
                mine = detect_color(cb)
                last_before = last_after = None
                fails = 0
                excluded = []
                self.say("Yangi o'yin. Siz: %s." % ("OQ" if mine == "w" else "QORA"))
            if mine is None:
                mine = detect_color(cb)
                self.say("O'yin o'rtasidan boshladim. Siz: %s." % ("OQ" if mine == "w" else "QORA"))
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
                if cur == last_after:  # raqib hali yurmadi
                    if time.time() - t_sent > 5:
                        self.say("Raqib o'ylayapti...")
                    time.sleep(0.3)
                    continue
                if cur == last_before:  # mening yurishim qabul bo'lmadi
                    fails += 1
                    excluded.append(sent_key)
                    if fails >= 3:
                        self.say("Yurish qabul qilinmayapti (3 marta). To'xtadim. Taxtani «Ko'rish» bilan tekshiring.")
                        return
                    self.say("Yurish o'tmadi, boshqasini sinayman...")
                else:
                    fails = 0
                    excluded = []
            else:
                if is_initial(cb) and mine == "b":
                    self.say("Oq birinchi yuradi. Raqibni kutyapman...")
                    time.sleep(0.4)
                    continue

            mv = choose_move(eng, excluded)
            if mv is None:
                self.say("Yurish qolmadi. O'yin tugagan bo'lishi mumkin.")
                return
            self.say("Yuryapman: " + " > ".join(fmt_sq(p) for p in mv[0]))
            self.play(mv)
            if self.stop_ev.is_set():
                return
            last_before = cur
            last_after = sig(apply_move(eng, mv, 1))
            sent_key = tuple(mv[0])
            t_sent = time.time()
            time.sleep(0.9)


# ======================================================================
#  5) KICHIK SUZUVCHI OYNA
# ======================================================================
def load_cfg():
    try:
        with open(CFG_PATH, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def save_cfg(cfg):
    with open(CFG_PATH, "w", encoding="utf-8") as f:
        json.dump(cfg, f)


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
        self.cfg = load_cfg()
        self.bot = None
        self.q = queue.Queue()
        self._dx = self._dy = 0

        self.full = tk.Frame(r, bg=BG)
        self.mini = tk.Label(r, text="\u265f", font=("Segoe UI", 18), fg="white",
                             bg="#555555", width=3, cursor="hand2")
        self.mini.bind("<Button-1>", lambda e: self.expand())

        bar = tk.Frame(self.full, bg="#2b2b2b")
        bar.pack(fill="x")
        title = tk.Label(bar, text="\u265f Shashka AI", fg="white", bg="#2b2b2b",
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
        self.btn = tk.Button(self.full, text="\u25b6  Boshlash", command=self.toggle,
                             bg="#2e7d32", fg="white", relief="flat", font=("Segoe UI", 10, "bold"))
        self.btn.pack(fill="x", padx=10, pady=(0, 6))
        row = tk.Frame(self.full, bg=BG)
        row.pack(fill="x", padx=10, pady=(0, 10))
        for txt, cmd in (("1  Taxta", self.calibrate), ("2  O'rgatish", self.do_learn),
                         ("Ko'rish", self.show_board)):
            tk.Button(row, text=txt, command=cmd, bg="#3a3a3a", fg="white", relief="flat",
                      font=("Segoe UI", 9)).pack(side="left", expand=True, fill="x", padx=2)
        self.full.pack()

        if not self.cfg.get("bbox"):
            self.say("Birinchi marta: avval «1 Taxta» tugmasini bosing.")
        elif not self.cfg.get("refs"):
            self.say("Endi yangi o'yin boshida (shashkalar joyida) «2 O'rgatish» ni bosing.")
        else:
            self.say("Tayyor. «Boshlash» ni bosing.")
        self.root.after(150, self.poll)

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
                    self.btn.config(text="\u25b6  Boshlash", bg="#2e7d32")
                    self.mini.config(bg="#555555")
                    self.expand()
        except queue.Empty:
            pass
        self.root.after(150, self.poll)

    # --- boshlash / to'xtatish ---
    def toggle(self):
        if self.bot is not None and self.bot.is_alive():
            self.bot.stop()
            self.say("To'xtatilyapti...")
            return
        if not self.cfg.get("bbox"):
            self.say("Avval «1 Taxta» tugmasini bosing.")
            return
        if not self.cfg.get("refs"):
            self.say("Avval yangi o'yin boshida «2 O'rgatish» ni bosing.")
            return
        self.bot = Bot(self.cfg, lambda m: self.q.put(("say", m)),
                       lambda s: self.q.put(("state", s)))
        self.bot.start()
        self.btn.config(text="\u25a0  To'xtatish", bg="#b71c1c")
        self.say("Ishlayapti. To'xtatish: Esc.")
        self.root.after(900, self.collapse)

    # --- taxtani belgilash ---
    def calibrate(self):
        self.root.withdraw()
        self.root.update()
        time.sleep(0.2)
        ov = tk.Toplevel()
        ov.attributes("-fullscreen", True)
        ov.attributes("-topmost", True)
        ov.attributes("-alpha", 0.4)
        ov.configure(bg="black")
        W, H = ov.winfo_screenwidth(), ov.winfo_screenheight()
        cv = tk.Canvas(ov, bg="black", highlightthickness=0, cursor="crosshair")
        cv.pack(fill="both", expand=True)
        txt = cv.create_text(W // 2, 70, fill="yellow", font=("Segoe UI", 22, "bold"),
                             text="1/2: Katak maydonining CHAP-YUQORI burchagini bosing")
        cv.create_text(W // 2, 120, fill="white", font=("Segoe UI", 13),
                       text="(yog'och ramka ichidagi birinchi katakning burchagi. Bekor qilish: Esc)")
        pts = []

        def finish(ok):
            ov.destroy()
            self.root.deiconify()
            if ok:
                save_cfg(self.cfg)
                self.say("Taxta belgilandi. Endi yangi o'yin boshida «2 O'rgatish» ni bosing.")
            else:
                self.say("Belgilash bekor qilindi.")

        def click(e):
            pts.append((e.x_root, e.y_root))
            if len(pts) == 1:
                cv.create_oval(e.x - 6, e.y - 6, e.x + 6, e.y + 6, outline="red", width=3)
                cv.itemconfig(txt, text="2/2: Katak maydonining O'NG-PASTKI burchagini bosing")
                return
            (ax, ay), (bx, by) = pts[0], pts[1]
            x1, x2 = sorted((ax, bx))
            y1, y2 = sorted((ay, by))
            w, h = x2 - x1, y2 - y1
            if w < 80 or h < 80 or abs(w - h) > 0.08 * max(w, h):
                pts.clear()
                cv.delete("all")
                txt2 = cv.create_text(W // 2, 70, fill="red", font=("Segoe UI", 22, "bold"),
                                      text="Burchaklar noto'g'ri. Qaytadan: chap-yuqori burchakni bosing")
                return
            self.cfg["bbox"] = [x1, y1, x2, y2]
            for i in range(9):  # tekshirish uchun to'r
                cv.create_line(x1 - 0 + i * w / 8.0 - ov.winfo_rootx(), y1 - ov.winfo_rooty(),
                               x1 + i * w / 8.0 - ov.winfo_rootx(), y2 - ov.winfo_rooty(),
                               fill="cyan", width=2)
                cv.create_line(x1 - ov.winfo_rootx(), y1 + i * h / 8.0 - ov.winfo_rooty(),
                               x2 - ov.winfo_rootx(), y1 + i * h / 8.0 - ov.winfo_rooty(),
                               fill="cyan", width=2)
            cv.itemconfig(txt, text="Yaxshi! To'r kataklarga mos tushdimi? (yopilmoqda...)")
            ov.after(2200, lambda: finish(True))

        cv.bind("<Button-1>", click)
        ov.bind("<Escape>", lambda e: finish(False))
        ov.focus_force()

    # --- o'rgatish ---
    def _grab_hidden(self):
        self.root.withdraw()
        self.root.update()
        time.sleep(0.35)
        try:
            return grab(self.cfg["bbox"])
        finally:
            self.root.deiconify()

    def do_learn(self):
        if not self.cfg.get("bbox"):
            self.say("Avval «1 Taxta» tugmasini bosing.")
            return
        try:
            img = self._grab_hidden()
            self.cfg["refs"] = learn_refs(img)
            save_cfg(self.cfg)
            self.say("O'rgandim \u2713. Endi «Ko'rish» bilan tekshirib, «Boshlash» ni bosing.")
        except Exception as e:
            self.say("Bo'lmadi: %s" % e)

    # --- ko'rish ---
    def show_board(self):
        if not self.cfg.get("refs"):
            self.say("Avval «2 O'rgatish» ni bosing.")
            return
        try:
            self.root.withdraw()
            self.root.update()
            time.sleep(0.35)
            try:
                img = grab(self.cfg["bbox"])
                turn = safe_turn(self.cfg["bbox"])
            finally:
                self.root.deiconify()
            F = cell_features(img)
            cb = {k: classify(f, self.cfg["refs"]) for k, f in F.items()}
            save_debug(self.cfg["bbox"])
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
                           "Ekrandagi taxta bilan bir xilmi?  Yopish uchun bosing.",
                 font=("Segoe UI", 9)).pack(pady=(0, 10))
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
