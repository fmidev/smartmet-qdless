"""Drive qdless in a pty, emulate the terminal with pyte, rasterise frames.

Block / sextant / quadrant / braille / small-triangle glyphs are drawn
geometrically (so they tile seamlessly like in a real terminal); all other
characters go through Noto Sans Mono.
"""
import os, sys, pty, time, select, fcntl, termios, struct, signal
import pyte  # pip install pyte
import numpy as np
from PIL import Image, ImageDraw, ImageFont

CW, CH = 12, 24  # cell pixel size
FONT_SIZE = 19
JB = "/usr/share/fonts/jetbrains-mono-fonts/JetBrainsMono-{}.otf"
FONT = ImageFont.truetype(JB.format("Regular"), FONT_SIZE)
FONT_BOLD = ImageFont.truetype(JB.format("Bold"), FONT_SIZE)
_font_for = {}


def font_for(ch, bold):
    """JetBrains Mono when it has the glyph, else whatever fontconfig finds."""
    import subprocess
    key = (ch, bold)
    if key in _font_for:
        return _font_for[key]
    f = FONT_BOLD if bold else FONT
    if ord(ch) >= 0x2000:
        out = subprocess.run(["fc-list", f":charset={ord(ch):x}", "file"], capture_output=True,
                             text=True).stdout.split("\n")
        files = [l.split(":")[0] for l in out if l.strip()]
        if not any("JetBrainsMono" in x for x in files):
            files.sort(key=lambda x: (0 if "Mono" in x else 1, 0 if "Regular" in x else 1, x))
            f = ImageFont.truetype(files[0], FONT_SIZE) if files else f
    _font_for[key] = f
    return f


DEFAULT_FG = (0xd0, 0xd0, 0xd0)
DEFAULT_BG = (0x14, 0x16, 0x1a)
ANSI = {
    "black": (0, 0, 0), "red": (205, 49, 49), "green": (13, 188, 121), "brown": (229, 229, 16),
    "yellow": (229, 229, 16), "blue": (36, 114, 200), "magenta": (188, 63, 188),
    "cyan": (17, 168, 205), "white": (229, 229, 229),
    "brightblack": (102, 102, 102), "brightred": (241, 76, 76), "brightgreen": (35, 209, 139),
    "brightbrown": (245, 245, 67), "brightyellow": (245, 245, 67), "brightblue": (59, 142, 234),
    "brightmagenta": (214, 112, 214), "brightcyan": (41, 184, 219), "brightwhite": (255, 255, 255),
}


def color(c, default):
    if c == "default":
        return default
    if c in ANSI:
        return ANSI[c]
    if len(c) == 6:
        try:
            return (int(c[0:2], 16), int(c[2:4], 16), int(c[4:6], 16))
        except ValueError:
            pass
    return default


# ---------------------------------------------------------------- glyph masks
_mask_cache = {}


def _rect(m, x0, y0, x1, y1):
    m[int(round(y0 * CH)):int(round(y1 * CH)), int(round(x0 * CW)):int(round(x1 * CW))] = 1.0


def _poly(pts):
    im = Image.new("L", (CW * 4, CH * 4), 0)
    ImageDraw.Draw(im).polygon([(x * CW * 4, y * CH * 4) for x, y in pts], fill=255)
    return np.asarray(im.resize((CW, CH), Image.BOX), dtype=np.float32) / 255.0


def glyph_mask(ch, bold=False):
    key = (ch, bold)
    if key in _mask_cache:
        return _mask_cache[key]
    m = np.zeros((CH, CW), dtype=np.float32)
    cp = ord(ch) if len(ch) == 1 else 0
    if ch == " " or cp == 0:
        pass
    elif 0x1FB00 <= cp <= 0x1FB3B:  # sextants
        n = cp - 0x1FB00 + 1
        if n >= 21:
            n += 1
        if n >= 42:
            n += 1
        for b in range(6):
            if n & (1 << b):
                sx, sy = b % 2, b // 2
                _rect(m, sx / 2, sy / 3, (sx + 1) / 2, (sy + 1) / 3)
    elif cp in (0x1FB3C, 0x1FB47, 0x1FB57, 0x1FB62):
        tri = {0x1FB3C: [(0, 2 / 3), (0, 1), (0.5, 1)],
               0x1FB47: [(0.5, 1), (1, 1), (1, 2 / 3)],
               0x1FB57: [(0, 0), (0.5, 0), (0, 1 / 3)],
               0x1FB62: [(0.5, 0), (1, 0), (1, 1 / 3)]}[cp]
        m = _poly(tri)
    elif 0x2580 <= cp <= 0x259F:  # block elements
        if cp == 0x2580: _rect(m, 0, 0, 1, 0.5)
        elif 0x2581 <= cp <= 0x2588: _rect(m, 0, 1 - (cp - 0x2580) / 8, 1, 1)
        elif 0x2589 <= cp <= 0x258F: _rect(m, 0, 0, (0x2590 - cp) / 8, 1)
        elif cp == 0x2590: _rect(m, 0.5, 0, 1, 1)
        elif cp in (0x2591, 0x2592, 0x2593): m[:] = {0x2591: .25, 0x2592: .5, 0x2593: .75}[cp]
        elif cp == 0x2594: _rect(m, 0, 0, 1, 1 / 8)
        elif cp == 0x2595: _rect(m, 7 / 8, 0, 1, 1)
        else:
            q = {0x2596: 4, 0x2597: 8, 0x2598: 1, 0x2599: 1 | 4 | 8, 0x259A: 1 | 8,
                 0x259B: 1 | 2 | 4, 0x259C: 1 | 2 | 8, 0x259D: 2, 0x259E: 2 | 4,
                 0x259F: 2 | 4 | 8}[cp]
            for b in range(4):
                if q & (1 << b):
                    sx, sy = b % 2, b // 2
                    _rect(m, sx / 2, sy / 2, (sx + 1) / 2, (sy + 1) / 2)
    elif 0x2800 <= cp <= 0x28FF:  # braille
        bits = cp - 0x2800
        pos = [(0, 0), (0, 1), (0, 2), (1, 0), (1, 1), (1, 2), (0, 3), (1, 3)]
        im = Image.new("L", (CW * 4, CH * 4), 0)
        d = ImageDraw.Draw(im)
        r = CW * 4 * 0.16
        for b, (px, py) in enumerate(pos):
            if bits & (1 << b):
                cx = (px + 0.5) / 2 * CW * 4
                cy = (py + 0.5) / 4 * CH * 4
                d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
        m = np.asarray(im.resize((CW, CH), Image.BOX), dtype=np.float32) / 255.0
    elif 0x2500 <= cp <= 0x257F and ch in BOX:
        # light box drawing: (up, down, left, right) arms, heavy flag
        up, dn, lf, rt, w = BOX[ch]
        cx, cy = CW // 2, CH // 2
        t = w
        if up: m[0:cy + t // 2 + 1, cx - t // 2:cx - t // 2 + t] = 1
        if dn: m[cy - t // 2:CH, cx - t // 2:cx - t // 2 + t] = 1
        if lf: m[cy - t // 2:cy - t // 2 + t, 0:cx + t // 2 + 1] = 1
        if rt: m[cy - t // 2:cy - t // 2 + t, cx - t // 2:CW] = 1
    else:
        im = Image.new("L", (CW * 2, CH), 0)
        d = ImageDraw.Draw(im)
        f = font_for(ch, bold)
        try:
            d.text((CW / 2, CH * 0.78), ch, font=f, fill=255, anchor="ms")
        except Exception:
            pass
        a = np.asarray(im, dtype=np.float32) / 255.0
        m = a[:, CW // 2:CW // 2 + CW].copy()
        if a[:, :CW // 2].any() or a[:, CW // 2 + CW:].any():
            # glyph wider than the cell (e.g. emoji fallback): squeeze
            m = np.asarray(im.resize((CW, CH), Image.BOX), dtype=np.float32) / 255.0
    _mask_cache[key] = m
    return m


BOX = {}
for chars, w in (("─│┌┐└┘├┤┬┴┼", 1), ("━┃┏┓┗┛┣┫┳┻╋", 2)):
    arms = [(0, 0, 1, 1), (1, 1, 0, 0), (0, 1, 0, 1), (0, 1, 1, 0), (1, 0, 0, 1), (1, 0, 1, 0),
            (1, 1, 0, 1), (1, 1, 1, 0), (0, 1, 1, 1), (1, 0, 1, 1), (1, 1, 1, 1)]
    for c, a in zip(chars, arms):
        BOX[c] = (*a, w)
for c, a in zip("╭╮╰╯", [(0, 1, 0, 1), (0, 1, 1, 0), (1, 0, 0, 1), (1, 0, 1, 0)]):
    BOX[c] = (*a, 1)


def render_screen(screen):
    rows, cols = screen.lines, screen.columns
    img = np.empty((rows * CH, cols * CW, 3), dtype=np.float32)
    buf = screen.buffer
    for y in range(rows):
        line = buf[y]
        for x in range(cols):
            c = line[x]
            fg = color(c.fg, DEFAULT_FG)
            bg = color(c.bg, DEFAULT_BG)
            if c.reverse:
                fg, bg = bg, fg
            if c.bold and c.fg in ANSI and not c.fg.startswith("bright"):
                fg = ANSI.get("bright" + c.fg, fg)
            m = glyph_mask(c.data or " ", c.bold)
            fga = np.asarray(fg, dtype=np.float32)
            bga = np.asarray(bg, dtype=np.float32)
            img[y * CH:(y + 1) * CH, x * CW:(x + 1) * CW] = bga + m[..., None] * (fga - bga)
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB")


# ---------------------------------------------------------------- session
KEYS = {
    "left": "\x1bOD", "right": "\x1bOC", "up": "\x1bOA", "down": "\x1bOB",
    "home": "\x1bOH", "end": "\x1bOF", "pgup": "\x1b[5~", "pgdn": "\x1b[6~",
    "sleft": "\x1b[1;2D", "sright": "\x1b[1;2C", "sup": "\x1b[1;2A", "sdown": "\x1b[1;2B",
    "tab": "\t", "btab": "\x1b[Z", "enter": "\r", "esc": "\x1b", "bs": "\x7f",
    "f2": "\x1bOQ", "f8": "\x1b[19~", "f9": "\x1b[20~", "f10": "\x1b[21~",
    "f11": "\x1b[23~", "f12": "\x1b[24~",
}


class Session:
    def __init__(self, argv, cols=150, rows=46, cwd=None, env=None):
        self.cols, self.rows = cols, rows
        self.screen = pyte.Screen(cols, rows)
        self.stream = pyte.ByteStream(self.screen)
        e = dict(os.environ)
        e.update({"TERM": "xterm-256color", "COLORTERM": "truecolor", "LANG": "C.UTF-8",
                  "LC_ALL": "C.UTF-8", "ESCDELAY": "25"})
        e.pop("TERM_PROGRAM", None)
        e.pop("KITTY_WINDOW_ID", None)
        if env:
            e.update(env)
        pid, fd = pty.fork()
        if pid == 0:
            if cwd:
                os.chdir(cwd)
            os.execvpe(argv[0], argv, e)
        self.pid, self.fd = pid, fd
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, rows * CH, cols * CW))
        self.alive = True
        self.raw = bytearray()

    def _answer(self, data):
        # answer the capability probe: DA1 without sixel, cell size, no kitty
        if b"\x1b[c" in data:
            os.write(self.fd, b"\x1b[?62;22c")
        if b"\x1b[16t" in data:
            os.write(self.fd, f"\x1b[6;{CH};{CW}t".encode())
        if b"\x1b[14t" in data:
            os.write(self.fd, f"\x1b[4;{self.rows*CH};{self.cols*CW}t".encode())

    def pump(self, secs=0.0):
        end = time.time() + secs
        while True:
            t = max(0.0, end - time.time())
            r, _, _ = select.select([self.fd], [], [], t)
            if not r:
                if time.time() >= end:
                    return
                continue
            try:
                data = os.read(self.fd, 1 << 20)
            except OSError:
                self.alive = False
                return
            if not data:
                self.alive = False
                return
            self._answer(data)
            self.stream.feed(data)

    def settle(self, quiet=0.35, maxwait=20.0):
        """pump until no output for `quiet` seconds"""
        start = time.time()
        last = time.time()
        while time.time() - start < maxwait:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if r:
                try:
                    data = os.read(self.fd, 1 << 20)
                except OSError:
                    self.alive = False
                    return
                if not data:
                    self.alive = False
                    return
                self._answer(data)
                self.stream.feed(data)
                last = time.time()
            elif time.time() - last > quiet:
                return

    def send(self, s):
        if isinstance(s, str):
            s = s.encode()
        os.write(self.fd, s)

    def key(self, k, settle=True, quiet=0.35):
        self.send(KEYS.get(k, k))
        if settle:
            self.settle(quiet)

    def mouse(self, x, y, button=0, release=True, settle=True):
        # x, y are 0-based cells
        self.send(f"\x1b[<{button};{x+1};{y+1}M")
        if release:
            time.sleep(0.03)
            self.send(f"\x1b[<{button};{x+1};{y+1}m")
        if settle:
            self.settle()

    def snap(self):
        return render_screen(self.screen)

    def text(self):
        return "\n".join(self.screen.display)

    def record(self, secs, fps=8, actions=None, with_text=False):
        """Log raw output for `secs` (actions: {seconds: callable}), then render
        frames at exact 1/fps instants by replaying into a copy of the screen."""
        import copy
        base = copy.deepcopy(self.screen)
        log = []
        start = time.time()
        pending = sorted((actions or {}).items())
        while time.time() - start < secs:
            now = time.time() - start
            while pending and pending[0][0] <= now:
                pending.pop(0)[1]()
            r, _, _ = select.select([self.fd], [], [], 0.01)
            if r:
                try:
                    data = os.read(self.fd, 1 << 20)
                except OSError:
                    break
                if not data:
                    break
                self._answer(data)
                log.append((time.time() - start, data))
        # replay
        scr = base
        st = pyte.ByteStream(scr)
        frames = []
        texts = []
        i = 0
        n = int(secs * fps)
        for k in range(n):
            t = (k + 1) / fps
            fed = False
            # only stop at a quiet boundary (next chunk >= 25 ms later) so a
            # frame never shows a half-drawn screen
            j = i
            last_ok = i
            while j < len(log) and log[j][0] <= t:
                j += 1
                if j >= len(log) or log[j][0] - log[j - 1][0] >= 0.025:
                    last_ok = j
            while i < last_ok:
                st.feed(log[i][1])
                i += 1
                fed = True
            frames.append(render_screen(scr) if fed or not frames else frames[-1])
            texts.append(list(scr.display))
        # bring the live screen up to date
        for _, d in log:
            self.stream.feed(d)
        return (frames, texts) if with_text else frames

    def close(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
        except Exception:
            pass


def crop(img, rows=None, cols=None):
    if rows is None and cols is None:
        return img
    r0, r1 = rows or (0, img.height // CH)
    c0, c1 = cols or (0, img.width // CW)
    return img.crop((c0 * CW, r0 * CH, c1 * CW, r1 * CH))
