#include "QdlessExitEffect.h"

#include "QdlessImageSource.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/dll/runtime_symbol_info.hpp>

#include <poll.h>
#include <unistd.h>

#include "QdlessExitEffectCommon.h"
#include "QdlessExitEffectEffects.h"

namespace Qdless
{
using namespace ee_detail;

int exitEffectCount()
{
  return kEffectCount;
}

const char* exitEffectName(int effectIndex)
{
  // Alphabetical roster (movie/book title case: short prepositions/articles
  // lowercased mid-string; acronyms preserved). The switch in playExitEffect
  // is dispatched by this index, so the two lists must stay in lockstep.
  static const char* const kNames[kEffectCount] = {
      "Accretion Disk"      , "Acid Trip"           , "ACME Anvil"          , "Akira",
      "AMOC"                , "Andromeda"           , "Anubis"              , "Apocalypse",
      "Apollo 11"           , "Atlas"               , "Aurora"              , "Auroral Oval",
      "Avogadro"            , "Ballet"              , "Banksy Balloon"      , "Bass Spiral",
      "Beagle"              , "Beethoven Fifth"     , "Benzene"             , "Berlin Wall",
      "Big Bang"            , "Big Keyboard"        , "Black Hole"          , "Bohr Atom",
      "Bok Globule"         , "Bond Barrel"         , "Bone Chandelier"     , "Bone Cut",
      "Boulder"             , "Bouncing Ball"       , "Brachistochrone"     , "Brownian",
      "Buckyball"           , "Butterfly"           , "Cambrian"            , "Cassini Finale",
      "Catalyst"            , "Ceci n'est pas"      , "Cell Divides"        , "Cepheid",
      "Cetacean"            , "Cezanne Still"       , "Chinese Dragon"      , "Chladni",
      "Chromatography"      , "Clockwork"           , "Close Encounters"    , "CMB Glow",
      "Co-evolution"        , "Columbus"            , "Comet Tail"          , "Conductor Baton",
      "Coriolis"            , "Cosmic Web"          , "Countdown"           , "Crab",
      "Crab Nebula"         , "Crown"               , "CRT Off"             , "Crystal Ball",
      "Damocles Foot"       , "Darwin"              , "Day Terminator"      , "Deep Field",
      "DeLorean"            , "Derecho"             , "Dial M"              , "Dictator Globe",
      "Dissolve"            , "DNA"                 , "Dodecahedron"        , "Double Slit",
      "Doves"               , "Dyson Sphere"        , "E.T."                , "Eden",
      "Eiffel Tower"        , "El Nino"             , "Electrolysis"        , "End Card",
      "Endosymbiosis"       , "Euler Identity"      , "Europa Ice"          , "Excalibur",
      "Explode"             , "Eye Wink"            , "Fade"                , "Fall to Pieces",
      "Feather"             , "Film Burn"           , "Finches"             , "Fire",
      "Fireworks"           , "Fish"                , "Flame Test"          , "Flatland",
      "Fogbow"              , "Foot Stomp"          , "Foucault Pendulum"   , "Fourier",
      "Galapagos"           , "Galaxy Collision"    , "Galileo Telescope"   , "Galileo Tower",
      "Game of Life"        , "Garuda"              , "Gas Giant"           , "Glow Stick",
      "Golden Spiral"       , "Gotcha"              , "Gravity Lens"        , "Greek Dance",
      "Guillotine"          , "Gunshot"             , "Gutenberg"           , "Hadley Cell",
      "HAL 9000"            , "HAL Stare"           , "Halley"              , "Hawking Radiation",
      "Helix Nebula"        , "Hendrix Guitar"      , "Hitchcock"           , "Hokusai Wave",
      "HR Diagram"          , "Hubble Expansion"    , "Hubble Telescope"    , "Hurricane Eye",
      "Hurricane Tracks"    , "Implode + Ring"      , "Inception"           , "Indy Idol",
      "Interstellar"        , "Iris Out"            , "ISS Track"           , "ITCZ",
      "Jaws"                , "Jellyfish"           , "Jet Stream"          , "Jurassic",
      "JWST"                , "Kenney"              , "King Kong"           , "Koyaanisqatsi",
      "Krakatoa"            , "Lagrange Points"     , "Lava Lamp"           , "Lawrence",
      "Lebowski"            ,
      "Liberty Torch"       , "Liesegang"           , "LIGO Chirp"          , "Lorenz Attractor",
      "Lucy"                , "Lunar Eclipse"       , "Macarena"            , "Magna Carta",
      "Magnetar"            , "Magnetosphere"       , "Magritte Bowler"     , "Mammatus",
      "Mandelbrot"          , "March of Progress"   , "Marionette"          , "Mars Rover",
      "Mary Poppins"        ,
      "Matrix Drop"         , "Memento"             , "Mendel"              , "Mendeleev",
      "Mentos"              , "Mitochondrial Eve"   , "MJO"                 , "Mjolnir",
      "Mobius"              , "Mona Lisa"           , "Monolith"            , "Monsoon",
      "Monty Python"        , "Moon Flag"           , "Moon Phases"         , "Moon Rocket",
      "Munch Scream"        , "Mutation"            , "Muybridge"           , "NaCl Lattice",
      "Napoleon"            , "Neo"                 , "Neutron Star"        , "Newspaper",
      "Newton"              ,
      "Newton Cradle"       , "Nosferatu"           , "Olympic Torch"       , "Olympus Mons",
      "Opera Curtain"       , "Oscar Statue"        , "Out of Africa"       , "Ozone Hole",
      "Pac-Man"             , "Pac-Man Duel"        , "Pandora"             , "Pandora Foot",
      "Pangaea"             , "Parker Probe"        , "Parthenon"           , "Peacock",
      "Pegasus"             , "Pendulum Waves"      , "Peppered Moth"       , "Periodic Table",
      "pH Strip"            , "Phase Transition"    , "Phoenix"             , "Pi",
      "Piano Keys"          , "Pillars of Creation" , "Pink Panther"        , "Pleasantville",
      "Polar Vortex"        , "Pompeii"             , "Pride Rock"          , "Psycho",
      "Pulp Briefcase"      , "Pulp Fiction"        , "Pulsar"              , "Punctuated",
      "Pyramids"            , "Pythagoras"          , "Python Cut"          , "Python Wars",
      "Quetzalcoatl"        , "Ragnarok"            , "Red Balloon"         , "Ring of Fire",
      "Riverdance"          , "Rocky"               , "Rosebud"             , "Rubik",
      "Russian Dance"       , "Sagittarius A"       , "Saharan Dust"        , "Saturn",
      "Saturn Rings"        , "Schrodinger"         , "Sea Ice"             , "Selection",
      "Shawshank"           , "Sheet Music"         , "Shining"             , "Shiver",
      "Sierpinski"          , "Silly Walk"          , "Singin'"             , "Sistine",
      "Skeleton Wave"       , "Snow Tree"           , "Snowfall"            , "Soap Bubble",
      "Solar Eclipse"       , "Solar Flare"         , "Solar System"        , "Sound of Music",
      "Spaghettify"         , "Spider"              , "Spiral"              , "Spirited Train",
      "Sputnik"             , "Standing Wave"       , "Standoff"            , "Star Gate",
      "Star Wars"           , "Stephenson"          , "Stingray"            , "Stonehenge",
      "Stradivarius"        , "Strangelove"         , "Submarine"           , "Sun Dogs",
      "Sunspot Cycle"       , "Supercell"           , "Supernova"           , "Tatooine",
      "Teardrop"            , "Tears in Rain"       , "Test Card"           , "Thanos Snap",
      "That's All Folks"    , "The Birds"           , "The End"             , "Thelma & Louise",
      "Theremin"            , "Thriller"            , "Thunderstorm"        , "Titanic",
      "Top Hat"             , "Tornado"             , "Tornado Duel"        , "Totoro",
      "Tracks"              , "Train"               , "Tree of Life"        , "Trojan Foot",
      "Trojan Horse"        , "Tron"                , "Trophy"              , "Truman",
      "Tsunami"             , "Tunnel"              , "UFO"                 , "Up",
      "Utopia"              , "Valkyrie Ride"       , "Vertigo"             , "Viking Longboat",
      "Vinyl Spin"          , "Voyager"             , "Walker Cell"         , "Warhol Banana",
      "Warp"                , "Wildfire Smoke"      , "William Tell"        , "Wizard of Oz",
      "Word Reveal"         , "Wormhole"            , "Wright Flyer"        , "Yggdrasil",
      "YMCA"                , "Yorick"};
  if (effectIndex < 0 || effectIndex >= kEffectCount)
    return "random";
  return kNames[effectIndex];
}

int exitThemeCount()
{
  return kThemeCount;
}

const char* exitThemeName(int themeIndex)
{
  if (themeIndex < 0 || themeIndex >= kThemeCount)
    return "";
  return kThemeNames[themeIndex];
}

int exitEffectTheme(int effectIndex)
{
  if (effectIndex < 0 || effectIndex >= kEffectCount)
    return -1;
  return static_cast<int>(kThemes[effectIndex]);
}

int exitWordlineCount()
{
  return kExitWordlineCount;
}

const char* exitWordline(int index)
{
  return (index >= 0 && index < kExitWordlineCount) ? kExitWordlines[index] : "";
}

int exitEffectIndexByName(const std::string& name)
{
  // Normalise to lowercase alphanumerics so spacing / case / punctuation
  // differences don't matter.
  auto norm = [](const std::string& s)
  {
    std::string o;
    for (char c : s)
      if (std::isalnum(static_cast<unsigned char>(c)) != 0)
        o.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return o;
  };
  const std::string target = norm(name);
  if (target.empty())
    return -1;
  for (int i = 0; i < kEffectCount; ++i)
    if (norm(exitEffectName(i)) == target)
      return i;
  return -1;
}

ExitEffectPlay playExitEffect(const Renderer& renderer,
                              std::vector<Rgb> frame,
                              int subW,
                              int subH,
                              int effectIndex,
                              unsigned seed,
                              std::string words,
                              const std::vector<Rgb>* linesFrame,
                              const std::vector<char>* swedenMask)
{
  // A nonzero seed always survives the round-trip, so a stored {index, seed}
  // can be replayed verbatim by the repeat key. 0 means "pick a fresh one".
  if (seed == 0)
  {
    seed = std::random_device{}();
    if (seed == 0)
      seed = 1U;
  }

  if (subW < 2 || subH < 2 ||
      frame.size() < static_cast<std::size_t>(subW) * static_cast<std::size_t>(subH))
    return {effectIndex < 0 ? 0 : effectIndex % kEffectCount, seed};

  std::mt19937 rng(seed);
  // Always draw once for the (possibly overridden) effect choice, so the rng
  // stream the effect then consumes is identical whether the index was random
  // or forced — that's what makes a replayed seed reproduce the exact frame.
  const int pick = static_cast<int>(rng() % static_cast<unsigned>(kEffectCount));
  const int e = (effectIndex < 0) ? pick : (effectIndex % kEffectCount);

  // Reset any leftover interrupt state, then roll for a surprise Monty Python
  // stomp. Both draws happen unconditionally so the rng stream the effect then
  // consumes stays identical for a replayed seed. One exit in five is cut short
  // — except the stomp-based effects, which already end on a foot.
  g_stompArmed = false;
  g_stompFired = false;
  const bool stompRoll = (rng() % 5U) == 0U;
  const double stompDelayMs = 350.0 + static_cast<double>(rng() % 700U);
  // Coin-flip on which foot the random stomp uses. Done outside the
  // conditional so the rng stream stays identical when a replayed seed
  // didn't pick a stomp on this run.
  const bool useTransfoot = (rng() % 2U) == 0U;
  std::unique_ptr<ImageSource> stompFoot;
  // Foot Stomp (97), Monty Python (176), Python Wars (227) — these already
  // end on a foot, so don't double-stomp them.
  if (stompRoll && e != 97 && e != 176 && e != 228)
  {
    std::size_t sfw = 0;
    std::size_t sfh = 0;
    // Random pick between the renaissance-background foot.png and the
    // alpha-clean transfoot.png. Fall back to whichever is available.
    if (useTransfoot)
      stompFoot = loadDataImage("transfoot.png", sfw, sfh);
    if (!stompFoot)
      stompFoot = loadFootImage(sfw, sfh);
    if (stompFoot)
    {
      g_stompArmed = true;
      g_stompTriggerMs = stompDelayMs;
      g_stompArmStart = std::chrono::steady_clock::now();
    }
  }

  // Disable autowrap so the bottom-right cell can be painted without
  // scrolling the screen.
  std::fputs("\x1b[?7l", stdout);

  // Cases follow the alphabetical kNames[] order above.
  switch (e)
  {
    case 0: effectAccretionDisk(renderer, frame, subW, subH); break;
    case 1: effectAcidTrip(renderer, frame, subW, subH); break;
    case 2: effectAcmeAnvil(renderer, frame, subW, subH); break;
    case 3: effectAkira(renderer, frame, subW, subH); break;
    case 4: effectAmoc(renderer, frame, subW, subH); break;
    case 5: effectAndromeda(renderer, frame, subW, subH); break;
    case 6: effectAnubis(renderer, frame, subW, subH); break;
    case 7: effectApocalypse(renderer, frame, subW, subH); break;
    case 8: effectApollo11(renderer, frame, subW, subH); break;
    case 9: effectAtlas(renderer, frame, subW, subH); break;
    case 10: effectAurora(renderer, frame, subW, subH, rng); break;
    case 11: effectAuroralOval(renderer, frame, subW, subH); break;
    case 12: effectAvogadro(renderer, frame, subW, subH); break;
    case 13: effectBallet(renderer, frame, subW, subH); break;
    case 14: effectBanksyBalloon(renderer, frame, subW, subH); break;
    case 15: effectBassSpiral(renderer, frame, subW, subH); break;
    case 16: effectBeagle(renderer, frame, subW, subH); break;
    case 17: effectBeethovenFifth(renderer, frame, subW, subH); break;
    case 18: effectBenzene(renderer, frame, subW, subH); break;
    case 19: effectBerlinWall(renderer, frame, subW, subH); break;
    case 20: effectBigBang(renderer, frame, subW, subH); break;
    case 21: effectBigKeyboard(renderer, frame, subW, subH); break;
    case 22: effectBlackHole(renderer, frame, subW, subH); break;
    case 23: effectBohrAtom(renderer, frame, subW, subH); break;
    case 24: effectBokGlobule(renderer, frame, subW, subH); break;
    case 25: effectBond(renderer, frame, subW, subH); break;
    case 26: effectBoneChandelier(renderer, frame, subW, subH); break;
    case 27: effectBoneCut(renderer, frame, subW, subH); break;
    case 28: effectBoulder(renderer, frame, subW, subH); break;
    case 29: effectBall(renderer, frame, subW, subH); break;
    case 30: effectBrachistochrone(renderer, frame, subW, subH); break;
    case 31: effectBrownian(renderer, frame, subW, subH); break;
    case 32: effectBuckyball(renderer, frame, subW, subH); break;
    case 33: effectButterfly(renderer, frame, subW, subH); break;
    case 34: effectCambrian(renderer, frame, subW, subH); break;
    case 35: effectCassiniFinale(renderer, frame, subW, subH); break;
    case 36: effectCatalyst(renderer, frame, subW, subH); break;
    case 37: effectCeciNestPas(renderer, frame, subW, subH); break;
    case 38: effectCellDivides(renderer, frame, subW, subH); break;
    case 39: effectCepheid(renderer, frame, subW, subH); break;
    case 40: effectCetacean(renderer, frame, subW, subH); break;
    case 41: effectCezanneStill(renderer, frame, subW, subH); break;
    case 42: effectChineseDragon(renderer, frame, subW, subH); break;
    case 43: effectChladni(renderer, frame, subW, subH); break;
    case 44: effectChromatography(renderer, frame, subW, subH); break;
    case 45: effectClockwork(renderer, frame, subW, subH); break;
    case 46: effectCloseEncounters(renderer, frame, subW, subH); break;
    case 47: effectCmbGlow(renderer, frame, subW, subH); break;
    case 48: effectCoEvolution(renderer, frame, subW, subH); break;
    case 49: effectColumbus(renderer, frame, subW, subH); break;
    case 50: effectCometTail(renderer, frame, subW, subH); break;
    case 51: effectConductorBaton(renderer, frame, subW, subH); break;
    case 52: effectCoriolis(renderer, frame, subW, subH); break;
    case 53: effectCosmicWeb(renderer, frame, subW, subH); break;
    case 54: effectCountdown(renderer, frame, subW, subH); break;
    case 55: effectCrab(renderer, frame, subW, subH); break;
    case 56: effectCrabNebula(renderer, frame, subW, subH); break;
    case 57: effectCrown(renderer, frame, subW, subH); break;
    case 58: effectCrtOff(renderer, frame, subW, subH); break;
    case 59: effectCrystalBall(renderer, frame, subW, subH); break;
    case 60: effectDamoclesFoot(renderer, frame, subW, subH); break;
    case 61: effectDarwin(renderer, frame, subW, subH); break;
    case 62: effectDayTerminator(renderer, frame, subW, subH); break;
    case 63: effectDeepField(renderer, frame, subW, subH); break;
    case 64: effectDeLorean(renderer, frame, subW, subH); break;
    case 65: effectDerecho(renderer, frame, subW, subH); break;
    case 66: effectDialM(renderer, frame, subW, subH); break;
    case 67: effectGlobeDance(renderer, frame, subW, subH); break;
    case 68: effectDissolve(renderer, frame, subW, subH, rng); break;
    case 69: effectDNA(renderer, frame, subW, subH); break;
    case 70: effectDodecahedron(renderer, frame, subW, subH); break;
    case 71: effectDoubleSlit(renderer, frame, subW, subH); break;
    case 72: effectDoves(renderer, frame, subW, subH, rng); break;
    case 73: effectDysonSphere(renderer, frame, subW, subH); break;
    case 74: effectET(renderer, frame, subW, subH); break;
    case 75: effectEden(renderer, frame, subW, subH); break;
    case 76: effectEiffel(renderer, frame, subW, subH); break;
    case 77: effectElNino(renderer, frame, subW, subH); break;
    case 78: effectElectrolysis(renderer, frame, subW, subH); break;
    case 79: effectEndCard(renderer, frame, subW, subH); break;
    case 80: effectEndosymbiosis(renderer, frame, subW, subH); break;
    case 81: effectEulerIdentity(renderer, frame, subW, subH); break;
    case 82: effectEuropaIce(renderer, frame, subW, subH); break;
    case 83: effectExcalibur(renderer, frame, subW, subH); break;
    case 84: effectExplode(renderer, frame, subW, subH); break;
    case 85: effectEyewink(renderer, frame, subW, subH); break;
    case 86: effectFade(renderer, frame, subW, subH); break;
    case 87: effectFallToPieces(renderer, frame, subW, subH); break;
    case 88: effectFeather(renderer, frame, subW, subH); break;
    case 89: effectFilmBurn(renderer, frame, subW, subH); break;
    case 90: effectFinches(renderer, frame, subW, subH); break;
    case 91: effectFire(renderer, frame, subW, subH, rng); break;
    case 92: effectFireworks(renderer, frame, subW, subH, rng); break;
    case 93: effectFish(renderer, frame, subW, subH); break;
    case 94: effectFlameTest(renderer, frame, subW, subH); break;
    case 95: effectFlatland(renderer, frame, subW, subH, rng); break;
    case 96: effectFogbow(renderer, frame, subW, subH); break;
    case 97: effectFoot(renderer, frame, subW, subH); break;
    case 98: effectFoucaultPendulum(renderer, frame, subW, subH); break;
    case 99: effectFourier(renderer, frame, subW, subH); break;
    case 100: effectGalapagos(renderer, frame, subW, subH); break;
    case 101: effectGalaxyCollision(renderer, frame, subW, subH); break;
    case 102: effectGalileoTelescope(renderer, frame, subW, subH); break;
    case 103: effectGalileoTower(renderer, frame, subW, subH); break;
    case 104: effectGameOfLife(renderer, frame, subW, subH); break;
    case 105: effectGaruda(renderer, frame, subW, subH); break;
    case 106: effectGasGiant(renderer, frame, subW, subH); break;
    case 107: effectGlowStick(renderer, frame, subW, subH); break;
    case 108: effectGoldenSpiral(renderer, frame, subW, subH); break;
    case 109: effectGotcha(renderer, frame, subW, subH); break;
    case 110: effectGravityLens(renderer, frame, subW, subH); break;
    case 111: effectGreekDance(renderer, frame, subW, subH); break;
    case 112: effectGuillotine(renderer, frame, subW, subH); break;
    case 113: effectGunshot(renderer, frame, subW, subH, rng); break;
    case 114: effectGutenberg(renderer, frame, subW, subH); break;
    case 115: effectHadleyCell(renderer, frame, subW, subH); break;
    case 116: effectHal9000(renderer, frame, subW, subH); break;
    case 117: effectHalStare(renderer, frame, subW, subH); break;
    case 118: effectHalley(renderer, frame, subW, subH); break;
    case 119: effectHawkingRadiation(renderer, frame, subW, subH); break;
    case 120: effectHelixNebula(renderer, frame, subW, subH); break;
    case 121: effectHendrixGuitar(renderer, frame, subW, subH); break;
    case 122: effectHitchcock(renderer, frame, subW, subH); break;
    case 123: effectHokusaiWave(renderer, frame, subW, subH); break;
    case 124: effectHRDiagram(renderer, frame, subW, subH); break;
    case 125: effectHubbleExpansion(renderer, frame, subW, subH); break;
    case 126: effectHubbleTelescope(renderer, frame, subW, subH); break;
    case 127: effectHurricaneEye(renderer, frame, subW, subH); break;
    case 128: effectHurricaneTracks(renderer, frame, subW, subH); break;
    case 129: effectImplodeRing(renderer, frame, subW, subH); break;
    case 130: effectInception(renderer, frame, subW, subH); break;
    case 131: effectIndyIdol(renderer, frame, subW, subH); break;
    case 132: effectInterstellar(renderer, frame, subW, subH); break;
    case 133: effectIrisOut(renderer, frame, subW, subH); break;
    case 134: effectIssTrack(renderer, frame, subW, subH); break;
    case 135: effectItcz(renderer, frame, subW, subH); break;
    case 136: effectJaws(renderer, frame, subW, subH); break;
    case 137: effectJellyfish(renderer, frame, subW, subH); break;
    case 138: effectJetStream(renderer, frame, subW, subH); break;
    case 139: effectJurassic(renderer, frame, subW, subH); break;
    case 140: effectJWST(renderer, frame, subW, subH); break;
    case 141: effectKenney(renderer, frame, subW, subH); break;
    case 142: effectKong(renderer, frame, subW, subH); break;
    case 143: effectKoyaanisqatsi(renderer, frame, subW, subH); break;
    case 144: effectKrakatoa(renderer, frame, subW, subH); break;
    case 145: effectLagrangePoints(renderer, frame, subW, subH); break;
    case 146: effectLavaLamp(renderer, frame, subW, subH); break;
    case 147: effectLawrence(renderer, frame, subW, subH); break;
    case 148: effectLebowski(renderer, frame, subW, subH); break;
    case 149: effectLibertyTorch(renderer, frame, subW, subH); break;
    case 150: effectLiesegang(renderer, frame, subW, subH); break;
    case 151: effectLigoChirp(renderer, frame, subW, subH); break;
    case 152: effectLorenzAttractor(renderer, frame, subW, subH); break;
    case 153: effectLucy(renderer, frame, subW, subH); break;
    case 154: effectLunarEclipse(renderer, frame, subW, subH); break;
    case 155: effectMacarena(renderer, frame, subW, subH); break;
    case 156: effectMagnaCarta(renderer, frame, subW, subH); break;
    case 157: effectMagnetar(renderer, frame, subW, subH); break;
    case 158: effectMagnetosphere(renderer, frame, subW, subH); break;
    case 159: effectMagritteBowler(renderer, frame, subW, subH); break;
    case 160: effectMammatus(renderer, frame, subW, subH); break;
    case 161: effectMandelbrot(renderer, frame, subW, subH); break;
    case 162: effectMarchOfProgress(renderer, frame, subW, subH); break;
    case 163: effectMarionette(renderer, frame, subW, subH); break;
    case 164: effectMarsRover(renderer, frame, subW, subH); break;
    case 165: effectMaryPoppins(renderer, frame, subW, subH); break;
    case 166: effectMatrix(renderer, frame, subW, subH, rng); break;
    case 167: effectMemento(renderer, frame, subW, subH); break;
    case 168: effectMendel(renderer, frame, subW, subH); break;
    case 169: effectMendeleev(renderer, frame, subW, subH); break;
    case 170: effectMentos(renderer, frame, subW, subH); break;
    case 171: effectMitochondrialEve(renderer, frame, subW, subH); break;
    case 172: effectMjo(renderer, frame, subW, subH); break;
    case 173: effectMjolnir(renderer, frame, subW, subH); break;
    case 174: effectMobius(renderer, frame, subW, subH); break;
    case 175: effectMonaLisa(renderer, frame, subW, subH); break;
    case 176: effectMonolith(renderer, frame, subW, subH); break;
    case 177: effectMonsoon(renderer, frame, subW, subH); break;
    case 178: effectMontyPython(renderer, frame, subW, subH); break;
    case 179: effectMoonFlag(renderer, frame, subW, subH); break;
    case 180: effectMoonPhases(renderer, frame, subW, subH); break;
    case 181: effectMoonRocket(renderer, frame, subW, subH); break;
    case 182: effectMunchScream(renderer, frame, subW, subH); break;
    case 183: effectMutation(renderer, frame, subW, subH); break;
    case 184: effectMuybridge(renderer, frame, subW, subH); break;
    case 185: effectNaClLattice(renderer, frame, subW, subH); break;
    case 186: effectNapoleon(renderer, frame, subW, subH); break;
    case 187: effectNeo(renderer, frame, subW, subH); break;
    case 188: effectNeutronStar(renderer, frame, subW, subH); break;
    case 189: effectNewspaper(renderer, frame, subW, subH); break;
    case 190: effectNewton(renderer, frame, subW, subH); break;
    case 191: effectNewtonCradle(renderer, frame, subW, subH); break;
    case 192: effectNosferatu(renderer, frame, subW, subH); break;
    case 193: effectOlympicTorch(renderer, frame, subW, subH); break;
    case 194: effectOlympusMons(renderer, frame, subW, subH); break;
    case 195: effectOperaCurtain(renderer, frame, subW, subH); break;
    case 196: effectOscarStatue(renderer, frame, subW, subH); break;
    case 197: effectOutOfAfrica(renderer, frame, subW, subH); break;
    case 198: effectOzoneHole(renderer, frame, subW, subH); break;
    case 199: effectPacman(renderer, frame, subW, subH); break;
    case 200: effectPacmanDuel(renderer, frame, subW, subH); break;
    case 201: effectPandora(renderer, frame, subW, subH); break;
    case 202: effectPandoraFoot(renderer, frame, subW, subH); break;
    case 203: effectPangaea(renderer, frame, subW, subH); break;
    case 204: effectParkerProbe(renderer, frame, subW, subH); break;
    case 205: effectParthenon(renderer, frame, subW, subH); break;
    case 206: effectPeacock(renderer, frame, subW, subH); break;
    case 207: effectPegasus(renderer, frame, subW, subH); break;
    case 208: effectPendulumWaves(renderer, frame, subW, subH); break;
    case 209: effectPepperedMoth(renderer, frame, subW, subH); break;
    case 210: effectPeriodicTable(renderer, frame, subW, subH); break;
    case 211: effectPhStrip(renderer, frame, subW, subH); break;
    case 212: effectPhaseTransition(renderer, frame, subW, subH); break;
    case 213: effectPhoenix(renderer, frame, subW, subH); break;
    case 214: effectPi(renderer, frame, subW, subH); break;
    case 215: effectPianoKeys(renderer, frame, subW, subH); break;
    case 216: effectPillarsOfCreation(renderer, frame, subW, subH); break;
    case 217: effectPinkPanther(renderer, frame, subW, subH); break;
    case 218: effectPleasantville(renderer, frame, subW, subH); break;
    case 219: effectPolarVortex(renderer, frame, subW, subH); break;
    case 220: effectPompeii(renderer, frame, subW, subH); break;
    case 221: effectPrideRock(renderer, frame, subW, subH); break;
    case 222: effectPsycho(renderer, frame, subW, subH); break;
    case 223: effectPulpBriefcase(renderer, frame, subW, subH); break;
    case 224: effectPulp(renderer, frame, subW, subH); break;
    case 225: effectPulsar(renderer, frame, subW, subH); break;
    case 226: effectPunctuated(renderer, frame, subW, subH); break;
    case 227: effectPyramids(renderer, frame, subW, subH); break;
    case 228: effectPythagoras(renderer, frame, subW, subH); break;
    case 229: effectPythonCut(renderer, frame, subW, subH); break;
    case 230: effectPythonWars(renderer, frame, subW, subH); break;
    case 231: effectQuetzalcoatl(renderer, frame, subW, subH); break;
    case 232: effectRagnarok(renderer, frame, subW, subH); break;
    case 233: effectRedBalloon(renderer, frame, subW, subH); break;
    case 234: effectRingOfFire(renderer, frame, subW, subH); break;
    case 235: effectRiverdance(renderer, frame, subW, subH); break;
    case 236: effectRocky(renderer, frame, subW, subH); break;
    case 237: effectRosebud(renderer, frame, subW, subH, rng); break;
    case 238: effectRubik(renderer, frame, subW, subH, rng); break;
    case 239: effectRussianDance(renderer, frame, subW, subH); break;
    case 240: effectSagittariusA(renderer, frame, subW, subH); break;
    case 241: effectSaharanDust(renderer, frame, subW, subH); break;
    case 242: effectSaturn(renderer, frame, subW, subH); break;
    case 243: effectSaturnRings(renderer, frame, subW, subH); break;
    case 244: effectSchrodinger(renderer, frame, subW, subH); break;
    case 245: effectSeaIce(renderer, frame, subW, subH); break;
    case 246: effectSelection(renderer, frame, subW, subH); break;
    case 247: effectShawshank(renderer, frame, subW, subH); break;
    case 248: effectSheetMusic(renderer, frame, subW, subH); break;
    case 249: effectShining(renderer, frame, subW, subH); break;
    case 250: effectShiver(renderer, frame, subW, subH, rng); break;
    case 251: effectSierpinski(renderer, frame, subW, subH); break;
    case 252: effectSillyWalk(renderer, frame, subW, subH); break;
    case 253: effectSinginRain(renderer, frame, subW, subH); break;
    case 254: effectSistine(renderer, frame, subW, subH); break;
    case 255: effectSkeletonWave(renderer, frame, subW, subH); break;
    case 256: effectSnowTree(renderer, frame, subW, subH); break;
    case 257: effectSnowflakes(renderer, frame, subW, subH, rng); break;
    case 258: effectSoapBubble(renderer, frame, subW, subH); break;
    case 259: effectSolarEclipse(renderer, frame, subW, subH); break;
    case 260: effectSolarFlare(renderer, frame, subW, subH); break;
    case 261: effectSolarSystem(renderer, frame, subW, subH, rng); break;
    case 262: effectSoundOfMusic(renderer, frame, subW, subH); break;
    case 263: effectSpaghettify(renderer, frame, subW, subH); break;
    case 264: effectSpider(renderer, frame, subW, subH); break;
    case 265: effectSpiral(renderer, frame, subW, subH); break;
    case 266: effectSpiritedTrain(renderer, frame, subW, subH); break;
    case 267: effectSputnik(renderer, frame, subW, subH); break;
    case 268: effectStandingWave(renderer, frame, subW, subH); break;
    case 269: effectStandoff(renderer, frame, subW, subH); break;
    case 270: effectStarGate(renderer, frame, subW, subH); break;
    case 271: effectStarWars(renderer, frame, subW, subH); break;
    case 272: effectStephenson(renderer, frame, subW, subH); break;
    case 273: effectStingray(renderer, frame, subW, subH); break;
    case 274: effectStonehenge(renderer, frame, subW, subH); break;
    case 275: effectStradivarius(renderer, frame, subW, subH); break;
    case 276: effectStrangelove(renderer, frame, subW, subH); break;
    case 277: effectSubmarine(renderer, frame, subW, subH, rng); break;
    case 278: effectSunDogs(renderer, frame, subW, subH); break;
    case 279: effectSunspotCycle(renderer, frame, subW, subH); break;
    case 280: effectSupercell(renderer, frame, subW, subH); break;
    case 281: effectSupernova(renderer, frame, subW, subH); break;
    case 282: effectTatooine(renderer, frame, subW, subH); break;
    case 283: effectTeardrop(renderer, frame, subW, subH); break;
    case 284: effectTearsInRain(renderer, frame, subW, subH, rng); break;
    case 285: effectTestCard(renderer, frame, subW, subH, rng); break;
    case 286: effectThanos(renderer, frame, subW, subH, rng); break;
    case 287: effectThatsAllFolks(renderer, frame, subW, subH); break;
    case 288: effectBirds(renderer, frame, subW, subH); break;
    case 289: effectTheEnd(renderer, frame, subW, subH, rng); break;
    case 290: effectThelma(renderer, frame, subW, subH); break;
    case 291: effectTheremin(renderer, frame, subW, subH); break;
    case 292: effectThriller(renderer, frame, subW, subH); break;
    case 293: effectThunderstorm(renderer, frame, subW, subH, rng); break;
    case 294: effectTitanic(renderer, frame, subW, subH); break;
    case 295: effectTopHat(renderer, frame, subW, subH); break;
    case 296: effectTornado(renderer, frame, subW, subH); break;
    case 297: effectTornadoDuel(renderer, frame, subW, subH); break;
    case 298: effectTotoro(renderer, frame, subW, subH); break;
    case 299: effectStandByMe(renderer, frame, subW, subH); break;
    case 300: effectTrain(renderer, frame, subW, subH); break;
    case 301: effectTreeOfLife(renderer, frame, subW, subH); break;
    case 302: effectTrojanFoot(renderer, frame, subW, subH); break;
    case 303: effectTrojanHorse(renderer, frame, subW, subH); break;
    case 304: effectTron(renderer, frame, subW, subH); break;
    case 305: effectTrophy(renderer, frame, subW, subH); break;
    case 306: effectTruman(renderer, frame, subW, subH); break;
    case 307: effectTsunami(renderer, frame, subW, subH); break;
    case 308: effectTunnel(renderer, frame, subW, subH); break;
    case 309: effectUFO(renderer, frame, subW, subH); break;
    case 310: effectUp(renderer, frame, subW, subH); break;
    case 311: effectUtopia(renderer, frame, subW, subH, rng, linesFrame, swedenMask); break;
    case 312: effectValkyrieRide(renderer, frame, subW, subH); break;
    case 313: effectVertigo(renderer, frame, subW, subH); break;
    case 314: effectVikingLongboat(renderer, frame, subW, subH); break;
    case 315: effectVinylSpin(renderer, frame, subW, subH); break;
    case 316: effectVoyager(renderer, frame, subW, subH); break;
    case 317: effectWalkerCell(renderer, frame, subW, subH); break;
    case 318: effectWarholBanana(renderer, frame, subW, subH); break;
    case 319: effectWarp(renderer, frame, subW, subH); break;
    case 320: effectWildfireSmoke(renderer, frame, subW, subH); break;
    case 321: effectWilliamTell(renderer, frame, subW, subH); break;
    case 322: effectOz(renderer, frame, subW, subH); break;
    case 323: effectWordReveal(renderer, frame, subW, subH, rng, words); break;
    case 324: effectWormhole(renderer, frame, subW, subH); break;
    case 325: effectWrightFlyer(renderer, frame, subW, subH); break;
    case 326: effectYggdrasil(renderer, frame, subW, subH); break;
    case 327: effectYMCA(renderer, frame, subW, subH); break;
    case 328: effectYorick(renderer, frame, subW, subH); break;
    default: effectFade(renderer, frame, subW, subH); break;
  }

  // If a surprise stomp fired mid-effect, crush the snapshot under the foot —
  // even faster than the usual stomp (slam in ~75 ms), then hold the planted
  // foot a beat so the gag registers before the program exits.
  g_stompArmed = false;
  if (stompFoot && g_stompFired)
  {
    g_stompFired = false;  // let the stomp's own runFrames run to completion
    auto footPixel = [&](float u, float v) -> Rgb
    { return stompFoot->pixelAtUV(std::clamp(u, 0.0F, 0.999F), std::clamp(v, 0.0F, 0.999F)); };
    std::vector<Rgb> view = std::move(g_stompCapture);
    runFrames(renderer,
              subW,
              subH,
              500,
              [&](float t, std::vector<Rgb>& dst)
              { stompFrame(dst, view, subW, subH, std::min(1.0F, t / 0.15F), footPixel); });
  }

  // Guaranteed clean end state: restore autowrap, reset attributes, clear.
  std::fputs("\x1b[?7h\x1b[0m\x1b[2J\x1b[H", stdout);
  std::fflush(stdout);
  return {e, seed};
}
}  // namespace Qdless
