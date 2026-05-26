#pragma once

#include "QdlessPalette.h"
#include "QdlessRenderer.h"

#include <string>
#include <vector>

namespace Qdless
{
// Plays a randomly-chosen "exit" animation over the whole terminal and ends
// on a blank screen. Each effect carries its own wall-clock duration (~1.4 s
// to ~10 s for the "tears in rain" homage) — there is no global cap. `frame`
// is a subW×subH Rgb raster — the snapshot of the current view produced by
// App::sampleSlice at full-terminal resolution.
//
// All drawing goes straight to stdout via raw ANSI (through `renderer`),
// bypassing ncurses exactly like App::drawMap does. Autowrap is toggled
// off for the duration so painting the bottom-right cell can't scroll the
// view; the terminal is left cleared with attributes reset. The caller
// still owns endwin().
//
// What playExitEffect actually played: the resolved effect index and the RNG
// seed it used. Feed both back in (effectIndex >= 0, the same seed) to
// reproduce the exact same animation — used by the repeat key.
struct ExitEffectPlay
{
  int index;
  unsigned seed;
};

// `frame` is taken by value because the effects mutate / resample it.
// `effectIndex` < 0 picks one at random; 0..exitEffectCount()-1 forces a
// specific effect (preview / repeat keys). `seed` == 0 generates a fresh seed;
// a nonzero seed makes the run reproducible. `words` is the caller-supplied
// text for the word-reveal effect (ignored by the others). Any key pressed
// during the animation aborts it. Returns the index + seed used.
ExitEffectPlay playExitEffect(const Renderer& renderer,
                              std::vector<Rgb> frame,
                              int subW,
                              int subH,
                              int effectIndex = -1,
                              unsigned seed = 0,
                              std::string words = {});

// Number of distinct effects, and a short human-readable name for each
// (for the preview status message). name() returns "random" out of range.
int exitEffectCount();
const char* exitEffectName(int effectIndex);

// Resolve a name to its effect index, or -1 if no match. Case-insensitive and
// punctuation/space-insensitive, so "tears in rain", "Tears-In-Rain" and
// "tearsinrain" all match. Used to parse --exit-effect.
int exitEffectIndexByName(const std::string& name);

// The word-reveal anthology: the closing lines the effect draws from. Exposed
// so the App can cycle / menu-pick a specific line for testing. exitWordline()
// returns "" out of range.
int exitWordlineCount();
const char* exitWordline(int index);
}  // namespace Qdless
