"""Shared helpers for the documentation capture scenes (see prod.py)."""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qdcap import *  # noqa: F401,F403  Session, KEYS, crop, Image, np, ...

HERE = os.path.dirname(os.path.abspath(__file__))
# The qdless binary under test (default: the build-tree binary) and the
# directory holding the sample data files the scenes open.
QD = os.environ.get('QDLESS_BIN', os.path.join(HERE, '..', '..', 'qdless'))
H = os.environ.get('QDLESS_DOC_DATA', os.path.expanduser('~/hub')).rstrip('/') + '/'

CAPFONT = ImageFont.truetype(JB.format("Bold"), 30)


def start(args, cols=150, rows=46, wait=1.0, **kw):
    """Launch qdless in a pty and wait until its first frame is drawn."""
    s = Session([QD, '--no-exit-effect'] + args, cols=cols, rows=rows, cwd=kw.pop('cwd', H), **kw)
    s.settle(quiet=wait, maxwait=60)
    return s


def caption(img, text, h=46):
    """Add a label bar above an image."""
    out = Image.new('RGB', (img.width, img.height + h), (32, 36, 44))
    out.paste(img, (0, h))
    d = ImageDraw.Draw(out)
    d.text((16, h // 2), text, font=CAPFONT, fill=(255, 214, 102), anchor='lm')
    return out


def side_by_side(imgs, labels=None, gap=8):
    """Join images horizontally, optionally captioned."""
    if labels:
        imgs = [caption(i, l) for i, l in zip(imgs, labels)]
    w = sum(i.width for i in imgs) + gap * (len(imgs) - 1)
    h = max(i.height for i in imgs)
    out = Image.new('RGB', (w, h), (20, 22, 26))
    x = 0
    for i in imgs:
        out.paste(i, (x, 0))
        x += i.width + gap
    return out
