#!/bin/bash
#
# collect-masala-sample.sh
#
# Collect a small but representative slice of the /masala forecast archive for
# qdless development and rendering tests. See docs/masala-catalog-plan.md for the
# full analysis of the on-disk layout.
#
# We sample each axis independently rather than the full cross-product, because
# the level axis (~137 hybrid levels) is what blows up the size. Parameter names
# are DISCOVERED from the actual data (not hardcoded), and for each geometry we
# pick the instance with the fullest leaf -- i.e. the real model, not a sparse
# producer that merely shares the geometry name.
#
# Run this ON THE /masala host. Total output is typically ~300-400 MB.
# Real relative paths are preserved so the catalog parser sees the genuine
# producer/reftime/geometry/leadtime/file structure.
#
# Usage:
#   ./collect-masala-sample.sh                 # -> masala-sample.tgz in $PWD
#   ROOT=/some/other/datasets ./collect-masala-sample.sh
#   KEEP=1 ./collect-masala-sample.sh          # keep the staging dir too

set -u

ROOT=${ROOT:-/masala/datasets}
OUT=$(mktemp -d /tmp/masala-sample.XXXX)
COPIED=0

# copy WITHOUT preserving ownership/mode (we are not root); keep timestamps only.
CP="cp --preserve=timestamps"

# mirror a file's /masala-relative path under $OUT
copy_into() {
  local rel=${1#/}
  mkdir -p "$OUT/$(dirname "$rel")"
  if $CP "$1" "$OUT/$rel" 2>/dev/null; then COPIED=$((COPIED+1)); fi
}
# copy every existing match of one or more globs (quiet; counts via $COPIED)
copy_glob() { local f; for f in "$@"; do [ -e "$f" ] && copy_into "$f"; done; }

# first existing leaf (lead-time dir) under a geometry dir: prefer "0"
first_leaf() { [ -d "$1/0" ] && { echo "$1/0"; return; }
  find "$1" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort | head -1; }

# among ALL dirs named $1, pick the one whose leaf holds the most files
# (the full model, not a sparse producer that reuses the geometry name)
richest_geom() {
  local best="" bestn=-1 d leaf n
  while IFS= read -r d; do
    leaf=$(first_leaf "$d"); [ -n "$leaf" ] || continue
    n=$(ls -1 "$leaf" 2>/dev/null | wc -l)
    [ "$n" -gt "$bestn" ] && { bestn=$n; best=$d; }
  done < <(find "$ROOT" -maxdepth 3 -type d -name "$1" 2>/dev/null)
  echo "$best"
}

# param prefixes in a leaf that have many files of a given leveltype (multi-level)
multilevel_params() { ls -1 "$1" 2>/dev/null | grep "_$2_" | sed 's/_.*//' | sort -u; }
# single-level (surface) param prefixes: those appearing exactly once in the leaf
surface_params() { ls -1 "$1" 2>/dev/null | sed 's/_.*//' | sort | uniq -c | awk '$1==1{print $2}'; }
# echo "PARAM LEVELTYPE" for the (param,leveltype) combo with the most files in a
# leaf -- i.e. the dominant 3D field, whatever it happens to be named.
richest_cube() { ls -1 "$1" 2>/dev/null \
  | sed -E 's/^([^_]+)_([A-Za-z]+)_.*/\1 \2/' | sort | uniq -c | sort -rn \
  | awk 'NR==1 && $1>1 {print $2, $3}'; }
# a lead-time leaf past the analysis step (so static analysis-only fields drop out)
later_leaf() { local c; for c in 6 3 12 1; do [ -d "$1/$c" ] && { echo "$1/$c"; return; }; done
  first_leaf "$1"; }
# how many files this block added
block() { echo "      copied $((COPIED-MARK)) file(s)"; }

echo "scanning $ROOT ..."

# ---- (A) full Z axis: ALL levels of ONE param, ONE lead time (~40 MB) -------
G=$(richest_geom ECEUR0100)                      # rll hybrid model (ECMWF)
if [ -n "$G" ]; then
  L=$(first_leaf "$G")
  ZP=$(multilevel_params "$L" hybrid | grep -xE 'T-K' || true)
  [ -z "$ZP" ] && ZP=$(multilevel_params "$L" hybrid | head -1)
  echo "(A) Z-axis sample: param '$ZP' (all hybrid+pressure levels) from $L"
  MARK=$COPIED; copy_glob "$L/${ZP}_hybrid_"* "$L/${ZP}_pressure_"*; block
else
  echo "(A) skipped: no ECEUR0100 geometry found"
fi

# ---- (B) full T axis: a few surface params across ALL lead times ------------
if [ -n "$G" ]; then
  LB=$(later_leaf "$G")                          # discover from a non-analysis step
  SP=$(surface_params "$LB" | head -4 | tr '\n' ' ')
  echo "(B) T-axis sample: surface params [$SP] across all lead times of $G"
  MARK=$COPIED
  for lt in "$G"/*/; do for p in $SP; do copy_glob "$lt${p}_"*; done; done
  block
fi

# ---- (C) a small browseable cube: top params x all levels x 3 lead times ----
if [ -n "$G" ]; then
  CP4=$(multilevel_params "$L" hybrid | head -4 | tr '\n' ' ')
  echo "(C) small cube: params [$CP4] x levels x lead times {0,3,6} of $G"
  MARK=$COPIED
  for lt in 0 3 6; do for p in $CP4; do copy_glob "$G/$lt/${p}_hybrid_"* "$G/$lt/${p}_pressure_"*; done; done
  block
fi

# ---- (D) other modalities, discovered the same way --------------------------
GO=$(richest_geom COPERNICUSNEMO); [ -n "$GO" ] || GO=$(richest_geom NEMO801738_UV)
if [ -n "$GO" ]; then L=$(first_leaf "$GO"); read -r DP DL <<<"$(richest_cube "$L")"
  echo "(D) ocean: param '$DP' $DL levels from $L"; MARK=$COPIED
  [ -n "$DP" ] && copy_glob "$L/${DP}_${DL}_"*; block; fi

GW=$(richest_geom WAMEC)                # wave 2D: just grab the leaf
if [ -n "$GW" ]; then L=$(first_leaf "$GW")
  echo "(D) wave: all fields from $L"; MARK=$COPIED
  for f in $(ls -1 "$L" 2>/dev/null | head -20); do copy_into "$L/$f"; done; block; fi

GF=$(richest_geom GFS0250)              # GFS pressure levels
if [ -n "$GF" ]; then L=$(first_leaf "$GF"); FP=$(multilevel_params "$L" pressure | head -1)
  echo "(D) GFS: param '$FP' pressure levels from $L"; MARK=$COPIED; copy_glob "$L/${FP}_pressure_"*; block; fi

GI=$(richest_geom MEPS2500D_ICING3D)    # 3D icing vertical slabs
if [ -n "$GI" ]; then L=$(first_leaf "$GI"); read -r IP IL <<<"$(richest_cube "$L")"
  echo "(D) 3D icing: param '$IP' $IL slabs from $L"; MARK=$COPIED
  [ -n "$IP" ] && copy_glob "$L/${IP}_${IL}_"*; block; fi

# radar GeoTIFF time series (small): newest ~24 frames
echo "(D) radar GeoTIFF frames (newest 24)"; MARK=$COPIED
find "$ROOT" -name '*fmippn*det*.tif' 2>/dev/null | sort | tail -24 | while read -r f; do copy_into "$f"; done
# (subshell pipe: recount from disk)
COPIED=$(find "$OUT" -type f | wc -l); block

# ONE monolithic multi-message file (to test the grid-files backend)
echo "(D) one monolithic GFS file"
find "$ROOT" -maxdepth 2 -name 'gfs.t*z.pgrb2full*.f024' 2>/dev/null | tail -1 | while read -r f; do copy_into "$f"; done
COPIED=$(find "$OUT" -type f | wc -l)

# ---- pack -------------------------------------------------------------------
echo
echo "staged $COPIED file(s) in $OUT"
echo "sample size:"; du -sh "$OUT"
tar czf masala-sample.tgz -C "$OUT" .
echo "wrote masala-sample.tgz  ($(du -h masala-sample.tgz | cut -f1))"

if [ "${KEEP:-0}" = 1 ]; then echo "staging dir kept at $OUT (KEEP=1)"; else rm -rf "$OUT"; fi

# Notes:
#  * richest_geom() picks, among all dirs sharing a geometry NAME, the instance
#    with the fullest leaf -- so it lands on the real model (e.g. ECMWF 131),
#    not a sparse producer (e.g. 243) that merely reuses the geometry name.
#  * Param prefixes are discovered from the actual leaf, so the script adapts to
#    whatever params/levels a given model writes. If a block still reports
#    "copied 0", that geometry/leveltype is absent on your mount -- harmless.
#  * We copy without preserving ownership (cp --preserve=timestamps): the files
#    are owned by other users, so `cp -a` would fail with "failed to preserve
#    ownership: Operation not permitted".
