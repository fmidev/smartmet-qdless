"""Production captures for the qdless documentation.

python3 prod.py [scene ...]   (no args = all scenes)
Output: prod/<name>.png (stills) and prod/<name>.webp (animations).
"""
import re
import sys
from common import *

PROD = os.environ.get('QDLESS_DOC_OUT', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'prod'))
os.makedirs(PROD, exist_ok=True)
MASALA = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'test', 'data',
                      'masala', 'masala', 'datasets')


def still(img, name, scale=0.8):
    if scale != 1.0:
        img = img.resize((int(img.width * scale), int(img.height * scale)), Image.LANCZOS)
    img = img.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    img.save(os.path.join(PROD, name + '.png'), optimize=True)


def anim(frames, name, fps=8, scale=0.5, quality=70, trim_blank=False):
    if trim_blank:
        # drop the tail once the process has exited and the screen is empty
        while len(frames) > 1 and not np.asarray(frames[-1].convert('L')).std() > 1.0:
            frames = frames[:-1]
    frames = [f.resize((int(f.width * scale), int(f.height * scale)), Image.LANCZOS)
              for f in frames]
    # collapse identical consecutive frames into longer durations
    out, durs = [], []
    for f in frames:
        if out and np.array_equal(np.asarray(out[-1]), np.asarray(f)):
            durs[-1] += int(1000 / fps)
        else:
            out.append(f)
            durs.append(int(1000 / fps))
    durs[-1] += 1200  # linger on the last frame before looping
    p = os.path.join(PROD, name + '.webp')
    out[0].save(p, save_all=True, append_images=out[1:], duration=durs, loop=0,
                quality=quality, method=4)
    print(f'{name}: {len(out)} frames, {os.path.getsize(p) // 1024} KB', flush=True)


def keys(s, pairs, t0=0.4, dt=0.25, gap=0.5):
    """[(key, count), ...] -> actions dict for Session.record"""
    acts, t = {}, t0
    for k, n in pairs:
        for _ in range(n):
            acts[round(t, 3)] = (lambda k=k: s.send(KEYS.get(k, k)))
            t += dt
        t += gap
    return acts, t


def click(s, x, y, b=0):
    s.send(f"\x1b[<{b};{x + 1};{y + 1}M")
    time.sleep(0.03)
    s.send(f"\x1b[<{b};{x + 1};{y + 1}m")


def captioned(frames_by_label, per=12):
    out = []
    for label, im in frames_by_label:
        out += [caption(im, label)] * per
    return out


SCENES = {}


def scene(f):
    SCENES[f.__name__] = f
    return f


# ------------------------------------------------------------------ 2D basics
@scene
def basics():
    s = start([H + 'fmi.sqd', '-p', 'Temperature', '-t', '8'], wait=2)
    still(s.snap(), 'qdless')
    s.key('p', quiet=0.8); still(s.snap(), 'params'); s.key('esc')
    s.key('g', quiet=0.8); still(s.snap(), 'legend'); s.key('esc')
    s.key('M', quiet=0.8); still(s.snap(), 'metadata'); s.key('esc')
    s.key('?', quiet=0.8); still(s.snap(), 'help'); s.key('esc')
    s.close()
    s = start([H + 'ecmwf.sqd', '-p', 'Temperature'], wait=3)
    still(s.snap(), 'global')
    s.close()
    s = start([H + 'meps_pressurelevels.sqd', '-p', 'Temperature'], wait=2)
    s.key('L', quiet=1); still(s.snap(), 'levels'); s.key('esc')
    s.close()


@scene
def zoom():
    s = start([H + 'fmi.sqd', '-p', 'Temperature', '-t', '8'], wait=2)
    acts, t = keys(s, [('+', 3), ('l', 2), ('j', 2), ('-', 2), ('0', 1)], dt=0.55)
    anim(s.record(t + 1, fps=8, actions=acts), 'zoom_pan')
    # double-click zoom at the cursor, right double-click back out
    def dbl(x, y, b):
        return lambda: [click(s, x, y, b), time.sleep(0.05), click(s, x, y, b)]
    acts = {0.5: dbl(100, 22, 0), 1.8: dbl(90, 20, 0), 3.2: dbl(90, 20, 2), 4.4: dbl(90, 20, 2)}
    anim(s.record(6, fps=8, actions=acts), 'dblclick_zoom')
    s.close()


@scene
def animation():
    s = start([H + 'fmi.sqd', '-p', 'Pressure'], wait=2)
    anim(s.record(8, fps=8, actions={0.2: lambda: s.send(' '), 7.8: lambda: s.send(' ')}),
         'animation')
    s.close()


@scene
def overlays():
    s = start([H + 'fmi.sqd', '-p', 'Temperature', '-t', '8'], wait=2)
    seq = [('default: braille coastlines, borders, graticule', s.snap())]
    for ks, label in [(['c'], '[c] thick coastlines'), (['c'], '[c] coastlines off'),
                      (['c', 'b'], '[b] thick borders'), (['b', 'b', 'n'], '[n] thick graticule'),
                      (['n'], '[n] graticule off'), (['n', 'i'], '[i] city overlay'),
                      (['pgdn'] * 3, '[PgDn] denser cities')]:
        for k in ks:
            s.key(k, quiet=0.4)
        seq.append((label, s.snap()))
    still(seq[-1][1], 'cities')
    anim(captioned(seq), 'overlays', fps=8)
    s.key('i')
    # cell styles: crop of the same area in all three styles, 2x nearest upscale
    crops = []
    for _ in range(3):
        c = crop(s.snap(), rows=(14, 26), cols=(56, 86))
        crops.append(c.resize((c.width * 2, c.height * 2), Image.NEAREST))
        s.key('t', quiet=0.6)
    side_by_side(crops, ['[t] sextants', 'triangles', 'squares']).save(
        os.path.join(PROD, 'cellstyles.png'), optimize=True)
    s.close()
    s = start([H + 'fmi.sqd', '-p', 'Temperature', '-t', '8'], wait=2)
    s.key('+'); s.key('+')
    s.key('w', quiet=1); still(s.snap(), 'wind')
    s.close()


@scene
def search():
    s = start([H + 'fmi.sqd', '-p', 'Temperature', '-t', '8'], wait=2)
    acts = {0.5: lambda: s.send('/')}
    t = 1.3
    for ch in 'Tampe':
        acts[t] = (lambda ch=ch: s.send(ch)); t += 0.35
    acts[t + 0.9] = lambda: s.send('\r')
    fr = s.record(t + 4.5, fps=8, actions=acts)
    anim(fr, 'search')
    still(fr[int((t + 0.5) * 8)], 'search')
    s.close()


@scene
def probe():
    s = start([H + 'fmi.sqd', '-p', 'Temperature'], wait=2)
    acts = {0.5: lambda: click(s, 92, 22)}
    t = 2.0
    for _ in range(6):
        acts[t] = lambda: s.send(KEYS['right']); t += 0.4
    acts[t + 0.3] = lambda: s.send(' ')
    acts[t + 4.5] = lambda: s.send(' ')
    acts[t + 5.2] = lambda: s.send('s')
    fr = s.record(t + 7.5, fps=8, actions=acts)
    anim(fr, 'probe')
    still(fr[int(1.8 * 8)], 'timeseries')
    still(fr[-1], 'probe_stats')
    s.close()


@scene
def crosssection():
    s = start([H + 'meps_pressurelevels.sqd', '-p', 'Temperature'], wait=2)
    acts = {0.4: lambda: s.send('x'), 1.2: lambda: click(s, 40, 30),
            2.2: lambda: click(s, 115, 12)}
    for i in range(30):
        acts[round(4.0 + i * 0.12, 3)] = (lambda x=int(12 + i * 1.5): s.send(f"\x1b[<35;{x + 1};4M"))
    for i in range(30):
        acts[round(7.8 + i * 0.12, 3)] = (lambda x=int(57 - i * 1.5): s.send(f"\x1b[<35;{x + 1};4M"))
    fr = s.record(12, fps=8, actions=acts)
    anim(fr, 'cross_section')
    still(fr[int(3.5 * 8)], 'pressurelevel_cross_section')
    s.key('y', quiet=1.5); still(s.snap(), 'cross_section_levels')
    s.key('y', quiet=1.0)
    s.key('H', quiet=2.0); still(s.snap(), 'hovmoller')
    s.close()
    s = start([H + 'meps_hybrid.sqd', '-p', 'WindSpeedMS'], wait=2)
    s.key('x'); click(s, 40, 30); s.settle(); click(s, 115, 12); s.settle(2)
    still(s.snap(), 'model_level_cross_section')
    s.close()


# ------------------------------------------------------------------ curtain
def curtain_session():
    s = start([H + 'meps_hybrid.sqd', '-p', 'WindSpeedMS'], wait=2)
    s.key('v', quiet=1.0)
    for _ in range(16):
        s.key('pgdn', quiet=0.15)
    s.settle(0.8)
    return s


@scene
def curtain():
    s = curtain_session()
    still(s.snap(), 'curtain')
    s.key('?', quiet=0.8); still(s.snap(), 'help_curtain'); s.key('esc', quiet=0.8)

    def rec(name, on, secs, off=None, fps=10):
        acts = {0.3: (lambda: [s.send(KEYS.get(k, k)) for k in on])}
        fr = s.record(secs, fps=fps, actions=acts)
        anim(fr, name, fps=fps)
        for k in (off if off is not None else on):
            s.key(k)
        s.settle(0.5)
        return fr

    rec('curtain_swing', ['s'], 5.5)
    rec('curtain_rotate', ['r'], 5.5)
    rec('curtain_orbit', ['o'], 5.5)
    rec('curtain_tilt', ['T'], 5.5)
    fr = rec('curtain_xcross', ['x', 'r'], 5.5, off=['r'])
    still(fr[-1], 'curtain_xcross')
    s.key('x')
    rec('curtain_combo', ['o', 'r', 's'], 7.0)
    s.close()


@scene
def curtain_edit():
    s = curtain_session()
    s.key('tab'); s.key('tab')  # A+B -> View -> A
    acts, t = keys(s, [('left', 5), ('down', 3), ('tab', 1), ('up', 4), ('right', 3),
                       ('tab', 1), ('left', 5), ('right', 5)])
    anim(s.record(t + 1, fps=8, actions=acts), 'curtain_edit')
    acts, t = keys(s, [('tab', 1), ('left', 8), ('down', 3), ('+', 3), ('right', 8), ('-', 3),
                       ('up', 3)], dt=0.3)
    anim(s.record(t + 1, fps=8, actions=acts), 'curtain_camera')
    s.key('0', quiet=0.8)
    s.key('tab'); s.key('tab')  # View -> A -> B ... back to A+B
    s.key('tab')
    acts, t = keys(s, [('pgup', 8), ('pgdn', 8)], dt=0.2)
    anim(s.record(t + 0.5, fps=8, actions=acts), 'curtain_ceiling')
    anim(s.record(9, fps=8, actions={0.3: lambda: s.send(' '), 8.5: lambda: s.send(' ')}),
         'curtain_time')
    s.close()


# ------------------------------------------------------------------ 3D point cloud
@scene
def pointcloud():
    s = start([H + 'pvol.h5'], wait=2)
    still(s.snap(), 'pvol_2d')
    s.key('L', quiet=0.8); still(s.snap(), 'pvol_levels'); s.key('esc')
    anim(s.record(4.5, fps=6, actions={round(0.2 + i * 0.4, 2): (lambda: s.send('.'))
                                        for i in range(10)}), 'pvol_elevations', fps=6)
    s.key('home')
    for _ in range(10):
        s.key(',', quiet=0.2)
    s.settle(1)
    s.key('x'); click(s, 40, 20); s.settle(); click(s, 110, 22); s.settle(1.5)
    still(s.snap(), 'pvol_rhi')
    s.key('y', quiet=1.5); still(s.snap(), 'pvol_rhi_angle')
    s.key('y', quiet=1.0); s.key('x', quiet=1.0)
    s.key('3', quiet=1.5); still(s.snap(), 'pvol_3d')
    acts, t = keys(s, [('l', 14), ('k', 3), (',', 3), ('.', 6), ('pgup', 3), ('h', 8)], dt=0.2)
    anim(s.record(t + 0.8, fps=8, actions=acts), 'pvol_3d')
    s.key('?', quiet=0.8); still(s.snap(), 'help_3d'); s.key('esc')
    s.close()
    s = start([H + 'meps_hybrid.sqd', '-p', 'TotalCloudCover'], wait=2)
    s.key('3', quiet=2)
    acts, t = keys(s, [('l', 16), ('k', 3), ('.', 3)], dt=0.3)
    fr = s.record(t + 0.8, fps=8, actions=acts)
    anim(fr, 'qd_3d', scale=0.45)
    still(fr[len(fr) // 2], 'qd_3d')
    s.close()
    s = start([H + 'meps_hybrid.sqd', '-p', 'Temperature'], wait=2)
    s.key('3', quiet=2)
    s.key('x', quiet=4.0); still(s.snap(), 'qd_3d_extrema')
    s.close()


@scene
def jet():
    # The jet stream as a 3D body: wind speed on MEPS hybrid levels. The wind
    # palette's bands (21-26 red, 26-32 red-orange, >32 orange) nest inside
    # each other, so raising the threshold peels the jet down to its core.
    # 3D sampling of this file takes ~0.5 s per frame, so keys are spaced to
    # give every step its own frame.
    s = start([H + 'meps_hybrid.sqd', '-p', 'WindSpeedMS'], wait=2)
    s.key('3', quiet=2)
    for _ in range(3):
        s.key('.', quiet=0.4)  # 0 -> 15 m/s
    for _ in range(2):
        s.key('k', quiet=0.4)  # a little more oblique
    s.settle(1.5)
    acts, t = keys(s, [('.', 5), (',', 2)], t0=1.0, dt=1.4, gap=1.4)  # 15 -> 40 -> 30
    fr = s.record(t + 1.5, fps=8, actions=acts)
    anim(fr, 'qd_3d_jet_threshold', scale=0.5)
    still(fr[-1], 'qd_3d_jet')
    acts, t = keys(s, [('l', 30), ('j', 3), ('h', 20), ('k', 3)], t0=0.5, dt=0.6, gap=0.6)
    anim(s.record(t + 1.0, fps=8, actions=acts), 'qd_3d_jet', scale=0.5)
    # The jet through one forecast day: fixed oblique camera, Space plays the
    # hourly steps; the valid time is stamped large on every frame since the
    # status line is unreadable at documentation size.
    s.key('0', quiet=1.5)
    for _ in range(2):
        s.key('k', quiet=0.5)
    for _ in range(4):
        s.key('l', quiet=0.5)
    s.key('-', quiet=1.0)  # keep the northern end of the jet in frame
    s.key('home', quiet=2.0)
    fr, texts = s.record(32, fps=8, with_text=True,
                         actions={0.3: lambda: s.send(' '), 31.6: lambda: s.send(' ')})
    stamped = []
    for f, txt in zip(fr, texts):
        m = next((re.search(r'(\d{4}-\d\d-\d\d \d\d:\d\d)', l) for l in txt if ' UTC' in l), None)
        stamped.append(caption(f, f'wind speed >= 30 m/s   {m.group(1) if m else ""} UTC',
                               h=64))
    anim(stamped, 'qd_3d_jet_time', scale=0.5)
    s.close()


# ------------------------------------------------------------------ globe + sun
@scene
def globe():
    s = start([H + 'ecmwf.sqd', '-p', 'Temperature'], wait=3)
    s.key('G', quiet=1.5); still(s.snap(), 'globe')
    acts, t = keys(s, [('l', 20), ('k', 4), ('j', 8), ('+', 3), ('-', 3)], dt=0.2)
    anim(s.record(t + 0.8, fps=8, actions=acts), 'globe_spin', scale=0.45)
    s.key('0', quiet=1)
    s.key('?', quiet=0.8); still(s.snap(), 'help_globe'); s.key('esc')
    s.key('u', quiet=1.0); still(s.snap(), 'globe_sun')
    anim(s.record(9, fps=8, actions={0.3: lambda: s.send(' '), 8.6: lambda: s.send(' ')}),
         'globe_sun', scale=0.45)
    s.key('G', quiet=1.5); still(s.snap(), 'sun_2d')
    s.close()
    s = start([H + 'fmi.sqd', '-p', 'Temperature', '-t', '8'], wait=2)
    s.key('G', quiet=1.5)
    for _ in range(3):
        s.key('+', quiet=0.4)
    s.settle(1)
    ims = []
    for _ in range(4):
        ims.append(crop(s.snap(), rows=(0, 43), cols=(20, 130)))
        s.key('o', quiet=1.0)
    ims = [i.resize((i.width * 2 // 5, i.height * 2 // 5), Image.LANCZOS) for i in ims]
    side_by_side(ims, ['[o] outline', 'ocean', 'land', 'both']).save(
        os.path.join(PROD, 'globe_surface.png'), optimize=True)
    s.close()


# ------------------------------------------------------------------ panels
@scene
def panels():
    s = start([H + 'fmi.sqd', '-p', 'Temperature,Pressure,WindSpeedMS,TotalCloudCover', '-t', '8'],
              wait=3)
    still(s.snap(), 'multipanel_quad')
    K = lambda k: (lambda: s.send(KEYS.get(k, k)))
    anim(s.record(8, fps=8, actions={0.3: K('tab'), 1.0: K('tab'), 1.7: K('1'), 2.4: K('f2'),
                                     3.6: K('f2'), 4.8: K('f2'), 5.6: K(' '), 7.8: K(' ')}),
         'multipanel', scale=0.45)
    s.close()


# ------------------------------------------------------------------ other sources
@scene
def sources():
    for name, args, w in [('grib_celsius', [H + 'interpolated_T-K.grib2'], 5),
                          ('netcdf_sst', [H + 'TSEA-C_height_0_ll_801_738_0_000.nc'], 3),
                          ('grib_tm', [H + 'WILDFIRES-KM2_ground_0_tm_760_1226_0_000.grib2'], 6),
                          ('image_png', [H + '2026041812_eu_analyysi_fi.png'], 2),
                          ('grib_global', [H + 'message_1884543755_0.grib'], 3)]:
        s = start(args, wait=w)
        s.settle(quiet=2, maxwait=60)
        still(s.snap(), name)
        if name == 'grib_celsius':
            click(s, 80, 25); s.settle(1.5); still(s.snap(), 'grib_probe')
        s.close()
    s = start(['--dir', H + 'radar_finland_rr1h_3067'], wait=3)
    anim(s.record(7, fps=8, actions={0.2: lambda: s.send(' ')}), 'radar_dir', scale=0.45)
    s.close()


@scene
def shapes():
    s = start([H + 'Meret/itameri_isot_alueet.shp'], wait=2)
    still(s.snap(), 'shape')
    buf = s.screen.buffer
    pt = next((x, y) for y in range(15, 30) for x in range(60, 100)
              if len(buf[y][x].bg) == 6 and buf[y][x].bg != '000000' and buf[y][x].data == ' ')
    click(s, *pt); s.settle(1); still(s.snap(), 'shape_click'); s.key(' ', quiet=0.5)
    s.key('a', quiet=1); still(s.snap(), 'shape_attrs')
    s.key('down'); s.key('down'); s.key('enter', quiet=1); s.key(' ', quiet=1)
    still(s.snap(), 'shape_highlight')
    s.key('r', quiet=1); still(s.snap(), 'shape_flat')
    s.close()


@scene
def catalog():
    s = start(['--catalog', MASALA], wait=2)
    frames = [s.snap()] * 8
    still(s.snap(), 'catalog_picker', scale=0.8)

    def step(k, q=0.8, n=8):
        s.key(k, quiet=q)
        frames.extend([s.snap()] * n)
    for k in ['down', 'down', 'right', 'left', 'right', 'right']:
        step(k)
    step('right', q=4, n=24)
    s.key('d', quiet=1); still(s.snap(), 'catalog_repick')
    s.close()
    anim(frames, 'catalog', scale=0.45)
    s = start(['--catalog', MASALA + '/131/202606130000/ECEUR0100', '-p', 'T-K'], wait=4)
    s.settle(quiet=2, maxwait=60)
    still(s.snap(), 'catalog_cube')
    s.close()


@scene
def exits():
    for eff in ['Tornado', 'Aurora', 'Hurricane Eye']:
        s = Session([QD, '--exit-effect', eff, H + 'fmi.sqd', '-p', 'Temperature', '-t', '8'],
                    cols=150, rows=46, cwd=H)
        s.settle(quiet=1.0, maxwait=60)
        fr = s.record(10, fps=10, actions={0.3: lambda: s.send('q')})
        anim(fr, 'exit_' + eff.lower().replace(' ', '_'), fps=10, scale=0.45, trim_blank=True)
        s.close()


@scene
def export():
    exp = os.path.join(PROD, 'export_tmp')
    os.makedirs(exp, exist_ok=True)
    s = start([H + 'fmi.sqd', '-p', 'Temperature', '-t', '8'], cwd=exp, wait=2)
    s.key('e', quiet=2)
    still(s.snap(), 'export_status')
    s.close()
    for f in os.listdir(exp):
        os.replace(os.path.join(exp, f), os.path.join(PROD, 'export_' + f))


if __name__ == '__main__':
    names = sys.argv[1:] or list(SCENES)
    for n in names:
        t = time.time()
        SCENES[n]()
        print(f'== {n} done in {time.time() - t:.0f}s', flush=True)
