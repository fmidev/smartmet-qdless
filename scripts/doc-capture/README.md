# doc-capture

Regenerates the screenshots and animations in `docs/images/` from a live
qdless session. Each scene starts `qdless` in a pseudo-terminal, sends
keystrokes and mouse events on a schedule, emulates the terminal with
[pyte](https://github.com/selectel/pyte), and rasterises the screen.
Sextant, quadrant, braille and box-drawing glyphs are drawn geometrically so
they tile the way a real terminal draws them; all other text uses JetBrains
Mono. Animations log the raw terminal output with timestamps and are rendered
afterwards at exact frame times, so they play at real speed however slow the
rasteriser is.

```bash
pip install pyte pillow numpy
make -C ../.. -j                                 # the scenes use ../../qdless
QDLESS_DOC_DATA=~/hub python3 prod.py            # all scenes → prod/
QDLESS_DOC_DATA=~/hub python3 prod.py curtain    # one scene
```

The scenes expect these sample files in `$QDLESS_DOC_DATA`: `fmi.sqd`,
`ecmwf.sqd`, `meps_pressurelevels.sqd`, `meps_hybrid.sqd`, `pvol.h5`,
`interpolated_T-K.grib2`, `message_1884543755_0.grib`,
`WILDFIRES-KM2_ground_0_tm_760_1226_0_000.grib2`,
`TSEA-C_height_0_ll_801_738_0_000.nc`, `2026041812_eu_analyysi_fi.png`,
`radar_finland_rr1h_3067/` and `Meret/itameri_isot_alueet.shp`. The catalog
scene uses the local masala sample in `test/data/masala` (unpacked from
`masala-sample.tgz`, not in git). Before copying the output into `docs/images/`,
resize stills to 900 px wide; the published animations are the `.webp` files
exactly as produced.

Other environment variables: `QDLESS_BIN` (the binary to drive) and
`QDLESS_DOC_OUT` (the output directory).
