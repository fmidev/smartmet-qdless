#pragma once

// Capsule-silhouette renderer for CMU mocap skeletons.
//
// Takes a BvhAnimation (see QdlessBvh.h), a frame index and a target
// screen rectangle, and composites a humanoid figure made of filled
// capsules over the destination raster. The skeleton's joint naming
// follows the CMU convention (Hips → LHipJoint → LeftUpLeg → ...) which
// is consistent across all subjects in the Carnegie Mellon mocap
// database.
//
// The user-picked style is "thick capsules around bones + torso shape":
// each limb is a tapered filled capsule, the torso is a wider capsule
// connecting Hips to Spine1, and the head is a filled circle at the
// Head joint. The effect is a thick, weighty marionette that reads
// clearly even at small sizes.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "QdlessBvh.h"
#include "QdlessRenderer.h"

namespace Qdless
{
struct MarionetteBone
{
  const char* fromJoint;
  const char* toJoint;
  float radius;  // multiplier on the figure's reference unit (see below)
};

// Standard humanoid bones rendered as capsules. Radii are in figure-
// height-relative units (the reference unit is set to 1/24 of the
// captured-bounding-box height — about the width of a real limb).
inline const std::vector<MarionetteBone>& cmuBones()
{
  static const std::vector<MarionetteBone> bones = {
      // Legs (thigh, shin, foot)
      {"LeftUpLeg",  "LeftLeg",     1.20F},
      {"LeftLeg",    "LeftFoot",    0.95F},
      {"LeftFoot",   "LeftToeBase", 0.80F},
      {"RightUpLeg", "RightLeg",    1.20F},
      {"RightLeg",   "RightFoot",   0.95F},
      {"RightFoot",  "RightToeBase",0.80F},
      // Arms (upper, forearm). Hand→Hand-end is left implicit; the
      // wrist joint is small enough at terminal resolution.
      {"LeftArm",       "LeftForeArm",  0.95F},
      {"LeftForeArm",   "LeftHand",     0.75F},
      {"RightArm",      "RightForeArm", 0.95F},
      {"RightForeArm",  "RightHand",    0.75F},
      // Neck (collar to head pivot — head itself drawn as a circle)
      {"Neck",  "Head", 1.05F},
  };
  return bones;
}

struct MarionettePose
{
  // World joint positions for one frame, indexed by BvhJoint index.
  std::vector<std::array<double, 3>> joints;
  // Bounding box in world coordinates (after centering on root).
  double minX = 0, maxX = 0, minY = 0, maxY = 0;
};

// Compute the rest-pose bounding box height to use as the universal
// scale reference. Done once per animation at load time so the figure
// stays the same size frame-to-frame.
inline double bvhReferenceHeight(const BvhAnimation& a)
{
  if (a.frameCount == 0) return 1.0;
  double minY = 1e30, maxY = -1e30;
  const int sample = std::min(a.frameCount, 30);  // first ~0.5 s
  for (int f = 0; f < sample; ++f)
  {
    auto pos = bvhJointPositions(a, f);
    for (const auto& p : pos)
    {
      if (p[1] < minY) minY = p[1];
      if (p[1] > maxY) maxY = p[1];
    }
  }
  const double h = maxY - minY;
  return (h > 1e-3) ? h : 1.0;
}

// Project a 3D joint coordinate to 2D pixel coordinates and return both
// the pixel position and the depth (for back-to-front sorting).
//
// We use a simple orthographic side-view projection: BVH X is screen X,
// BVH Y is up (so screen Y is inverted), BVH Z is depth. The figure is
// centred around its root XZ position and scaled to fit the target
// rectangle. yAspect compensates for the terminal's 2:1 cell aspect.
struct Projection
{
  double rootX = 0;
  double rootZ = 0;
  double rootY = 0;
  // BVH units to dst pixels. y is rows (so 1 BVH unit -> scaleY rows);
  // x is cols, and since dst cols are visually thinner than rows by a
  // factor of ya, we use scaleX = scaleY / ya so the figure's
  // anatomical aspect is preserved on screen.
  double scaleY = 1.0;
  double scaleX = 1.0;
  double cx = 0;
  double cy = 0;
  double ya = 0.5;
};

inline std::array<double, 3> projectJoint(const Projection& p,
                                          const std::array<double, 3>& w)
{
  const double dx = w[0] - p.rootX;
  const double dy = w[1] - p.rootY;
  const double dz = w[2] - p.rootZ;
  const double sx = p.cx + dx * p.scaleX;
  const double sy = p.cy - dy * p.scaleY;
  return {{sx, sy, dz}};
}

// Filled capsule from (x0, y0) radius r0 to (x1, y1) radius r1 with the
// given colour, into a destination raster of width w and height h. Used
// for limbs and torso both.
//
// Implementation: rasterise the convex hull of the two endpoint discs,
// scan-line by scan-line. The capsule body between the two centres is
// trapezoidal in profile; the caps add the two circular ends.
inline void drawCapsule(std::vector<Rgb>& dst, int w, int h, float ya,
                        double x0, double y0, double x1, double y1,
                        double r0, double r1, Rgb color)
{
  // Bounding box that contains both end discs.
  const double maxR = std::max(r0, r1);
  const int xmin = std::max(0, static_cast<int>(std::floor(std::min(x0, x1) - maxR)));
  const int xmax = std::min(w - 1, static_cast<int>(std::ceil(std::max(x0, x1) + maxR)));
  const int ymin = std::max(0, static_cast<int>(std::floor(std::min(y0, y1) - maxR / ya)));
  const int ymax = std::min(h - 1, static_cast<int>(std::ceil(std::max(y0, y1) + maxR / ya)));
  if (xmax < xmin || ymax < ymin) return;
  // Capsule rasterisation in metric space (where y pixels are scaled by
  // ya so a "round" capsule has equal radius in x and y). The metric
  // segment runs from A_m = (x0, y0*ya) to B_m = (x1, y1*ya); the
  // capsule is the set of points within `r` of any point on that
  // segment (linearly interpolated when r0 != r1).
  const double mdx = x1 - x0;            // metric_dx
  const double mdy = (y1 - y0) * ya;     // metric_dy
  const double L2m = mdx * mdx + mdy * mdy;
  if (L2m < 1e-12)
  {
    // Degenerate segment — draw a disc of radius max(r0, r1).
    const double r = std::max(r0, r1);
    const double r2 = r * r;
    for (int yy = ymin; yy <= ymax; ++yy)
    {
      const double yd = (yy - y0) * ya;
      for (int xx = xmin; xx <= xmax; ++xx)
      {
        const double xd = xx - x0;
        if (xd * xd + yd * yd <= r2)
          dst[static_cast<std::size_t>(yy) * w + xx] = color;
      }
    }
    return;
  }
  for (int yy = ymin; yy <= ymax; ++yy)
  {
    const double pmy = yy * ya;  // point y in metric
    for (int xx = xmin; xx <= xmax; ++xx)
    {
      // Vector from A_m to the sample point, in metric space.
      const double rx = xx - x0;
      const double ry = pmy - y0 * ya;
      // Project onto the segment, parameter t in [0, 1] along A → B.
      const double t = (rx * mdx + ry * mdy) / L2m;
      const double tc = std::clamp(t, 0.0, 1.0);
      // Residual to the closest point on the (clamped) segment.
      const double qx = tc * mdx;
      const double qy = tc * mdy;
      const double dxr = rx - qx;
      const double dyr = ry - qy;
      const double d2 = dxr * dxr + dyr * dyr;
      const double r = r0 + (r1 - r0) * tc;
      if (d2 <= r * r)
        dst[static_cast<std::size_t>(yy) * w + xx] = color;
    }
  }
}

// Filled convex quadrilateral by scan-line conversion. Used for the
// torso silhouette which connects the shoulders to the hips as one
// continuous shape — no caps, no balloon at the hip or shoulder joints
// where the limbs emerge.
inline void fillQuad(std::vector<Rgb>& dst, int w, int h,
                     double v0x, double v0y, double v1x, double v1y,
                     double v2x, double v2y, double v3x, double v3y,
                     Rgb color)
{
  const double xs[4] = {v0x, v1x, v2x, v3x};
  const double ys[4] = {v0y, v1y, v2y, v3y};
  double minY = ys[0], maxY = ys[0];
  for (int i = 1; i < 4; ++i)
  {
    if (ys[i] < minY) minY = ys[i];
    if (ys[i] > maxY) maxY = ys[i];
  }
  const int ymin = std::max(0, static_cast<int>(std::floor(minY)));
  const int ymax = std::min(h - 1, static_cast<int>(std::ceil(maxY)));
  for (int y = ymin; y <= ymax; ++y)
  {
    double xL = 1e30, xR = -1e30;
    for (int e = 0; e < 4; ++e)
    {
      const int e2 = (e + 1) % 4;
      const double y0 = ys[e];
      const double y1 = ys[e2];
      if (y0 == y1) continue;
      const double yLo = std::min(y0, y1);
      const double yHi = std::max(y0, y1);
      if (static_cast<double>(y) < yLo || static_cast<double>(y) > yHi) continue;
      const double t = (static_cast<double>(y) - y0) / (y1 - y0);
      const double x = xs[e] + t * (xs[e2] - xs[e]);
      if (x < xL) xL = x;
      if (x > xR) xR = x;
    }
    if (xR < xL) continue;
    const int xa = std::max(0, static_cast<int>(std::floor(xL)));
    const int xb = std::min(w - 1, static_cast<int>(std::ceil(xR)));
    for (int x = xa; x <= xb; ++x)
      dst[static_cast<std::size_t>(y) * w + x] = color;
  }
}

// Filled circle (used for the head).
inline void drawDisc(std::vector<Rgb>& dst, int w, int h, float ya,
                     double cx, double cy, double r, Rgb color)
{
  const int xmin = std::max(0, static_cast<int>(std::floor(cx - r)));
  const int xmax = std::min(w - 1, static_cast<int>(std::ceil(cx + r)));
  const int ymin = std::max(0, static_cast<int>(std::floor(cy - r / ya)));
  const int ymax = std::min(h - 1, static_cast<int>(std::ceil(cy + r / ya)));
  const double r2 = r * r;
  for (int yy = ymin; yy <= ymax; ++yy)
  {
    const double yd = (yy - cy) * ya;
    for (int xx = xmin; xx <= xmax; ++xx)
    {
      const double xd = xx - cx;
      if (xd * xd + yd * yd <= r2)
        dst[static_cast<std::size_t>(yy) * w + xx] = color;
    }
  }
}

// Render a full marionette figure into the raster.
//
// (cx, cy) is the on-screen anchor (the figure's hip centre). figureH
// is the target on-screen height in pixels — the BVH is scaled so its
// bounding-box height maps to figureH. unitRefH is the BVH reference
// height precomputed by bvhReferenceHeight() at load time.
// Optional output: caller passes a pointer to a vector<array<double,2>>
// to receive the screen (col, row) of every joint after projection.
// Used by effects that attach props to body parts — bowler hat above
// the head, briefcase at a wrist, boxing gloves at the hands, etc.
inline void drawMarionette(std::vector<Rgb>& dst, int w, int h, float ya,
                           const BvhAnimation& anim, int frameIdx,
                           double cx, double cy, double figureH,
                           double unitRefH, Rgb bodyCol,
                           std::vector<std::array<double, 2>>* jointScreenOut = nullptr)
{
  if (anim.frameCount == 0 || unitRefH <= 0.0) return;
  const auto worldPos = bvhJointPositions(anim, frameIdx);
  // Centre the figure on its hip joint so it doesn't wander out of the
  // viewport as the captured subject walks across the mocap stage.
  Projection p;
  const int hipsIdx = anim.jointIndex("Hips");
  if (hipsIdx >= 0)
  {
    p.rootX = worldPos[hipsIdx][0];
    p.rootZ = worldPos[hipsIdx][2];
    // For Y we use the lowest point so the feet sit on a fixed
    // baseline. Re-derive per frame from the actual joint positions.
    double minY = 1e30;
    for (const auto& j : worldPos)
      if (j[1] < minY) minY = j[1];
    p.rootY = minY;
  }
  // Scale: map unitRefH BVH units onto figureH dst rows; the x scale
  // is amplified by 1/ya so the figure's BVH width converts to the
  // right number of dst cols (dst cols are visually thinner than rows
  // by a factor of ya).
  p.scaleY = figureH / unitRefH;
  p.scaleX = p.scaleY / std::max(0.01, static_cast<double>(ya));
  p.cx = cx;
  p.cy = cy;
  p.ya = ya;

  // Optional: project every joint to screen space for the caller. Same
  // projection used internally below, so props attached to a joint by
  // the caller (hat above head, briefcase at wrist) land on the
  // pixel-accurate joint position.
  if (jointScreenOut)
  {
    jointScreenOut->resize(worldPos.size());
    for (std::size_t i = 0; i < worldPos.size(); ++i)
    {
      const auto pj = projectJoint(p, worldPos[i]);
      (*jointScreenOut)[i] = {{pj[0], pj[1]}};
    }
  }

  // Slender Prince-of-Persia-ish proportions: limb half-width is ~1/30
  // of figure height. The torso is a filled polygon (not a capsule), so
  // it doesn't add cap-balloons at the hip/shoulder where the limbs
  // emerge. Limbs are constant-width capsules — same radius at every
  // joint — so the rounded ends two bones share at a joint blend into
  // a single same-width disc instead of poking out perpendicular to
  // each bone.
  const double bodyUnit = std::max(1.0, figureH / 30.0);

  const int spineI   = anim.jointIndex("Spine1");
  const int lArmI    = anim.jointIndex("LeftArm");
  const int rArmI    = anim.jointIndex("RightArm");
  const int lLegI    = anim.jointIndex("LeftUpLeg");
  const int rLegI    = anim.jointIndex("RightUpLeg");

  // 1) Torso polygon: shoulders → hips. Drawn first so limbs overlay it.
  if (lArmI >= 0 && rArmI >= 0 && lLegI >= 0 && rLegI >= 0)
  {
    const auto sl = projectJoint(p, worldPos[lArmI]);
    const auto sr = projectJoint(p, worldPos[rArmI]);
    const auto hl = projectJoint(p, worldPos[lLegI]);
    const auto hr = projectJoint(p, worldPos[rLegI]);
    // Vertex order CW: left shoulder → right shoulder → right hip → left hip.
    fillQuad(dst, w, h,
             sl[0], sl[1], sr[0], sr[1], hr[0], hr[1], hl[0], hl[1],
             bodyCol);
  }

  // 2) Limb chains as constant-width capsules. Each pair shares the
  // same radius so the cap at any shared endpoint becomes an invisible
  // round corner inside the limb width.
  struct Chain
  {
    const char* a;
    const char* b;
    float widthMul;  // multiplier on bodyUnit, picked per body part
  };
  static constexpr Chain kChains[] = {
      // Legs: thigh, shin, foot (all same width within a leg).
      {"LeftUpLeg",   "LeftLeg",     1.00F},
      {"LeftLeg",     "LeftFoot",    1.00F},
      {"LeftFoot",    "LeftToeBase", 1.00F},
      {"RightUpLeg",  "RightLeg",    1.00F},
      {"RightLeg",    "RightFoot",   1.00F},
      {"RightFoot",   "RightToeBase",1.00F},
      // Arms: upper arm + forearm.
      {"LeftArm",     "LeftForeArm", 0.85F},
      {"LeftForeArm", "LeftHand",    0.85F},
      {"RightArm",    "RightForeArm",0.85F},
      {"RightForeArm","RightHand",   0.85F},
  };
  for (const auto& c : kChains)
  {
    const int ai = anim.jointIndex(c.a);
    const int bi = anim.jointIndex(c.b);
    if (ai < 0 || bi < 0) continue;
    const auto a = projectJoint(p, worldPos[ai]);
    const auto b = projectJoint(p, worldPos[bi]);
    const double r = bodyUnit * c.widthMul;
    drawCapsule(dst, w, h, ya, a[0], a[1], b[0], b[1], r, r, bodyCol);
  }

  // 3) Neck: from the midpoint of the shoulder line up to the head
  // joint, as a thin capsule. Spine1 is between the shoulders so we
  // anchor at its projection.
  const int headI = anim.jointIndex("Head");
  if (spineI >= 0 && headI >= 0)
  {
    const auto sp = projectJoint(p, worldPos[spineI]);
    const auto hh = projectJoint(p, worldPos[headI]);
    drawCapsule(dst, w, h, ya, sp[0], sp[1], hh[0], hh[1],
                bodyUnit * 0.7, bodyUnit * 0.7, bodyCol);
  }

  // 4) Head as a small disc.
  if (headI >= 0)
  {
    const auto hh = projectJoint(p, worldPos[headI]);
    drawDisc(dst, w, h, ya, hh[0], hh[1], bodyUnit * 1.6, bodyCol);
  }
}

// Convenience wrapper used by the dance-style exit effects (Riverdance,
// Ballet, Macarena, ...). Loads a CMU motion by short name from the
// data/cmu/ directory and bundles it with its reference height so the
// caller doesn't have to track both. ok=false means the motion file
// was missing or unparseable — callers should treat the dancer as
// invisible in that case.
struct DancerMotion
{
  BvhAnimation anim;
  double refH = 1.0;
  bool ok = false;
};

inline DancerMotion loadDancerMotion(const char* name)
{
  DancerMotion d;
  char rel[64];
  std::snprintf(rel, sizeof(rel), "cmu/%s.bvh", name);
  // findDataImage lives in Qdless::ee_detail (it ships with the
  // exit-effect helpers); resolve via qualified name so this header
  // doesn't depend on a using-directive at the call site.
  const std::string path = ee_detail::findDataImage(rel);
  if (path.empty()) return d;
  try
  {
    d.anim = loadBvhFile(path);
    d.refH = bvhReferenceHeight(d.anim);
    d.ok = true;
  }
  catch (const std::exception&) { d.ok = false; }
  return d;
}

// Draw one dancer at the given anchor. phaseFrame is a fractional frame
// index along the BVH (effects compute it from the beat and a per-
// dancer offset so each dancer is at a slightly different cycle phase).
inline void drawDancer(std::vector<Rgb>& dst, int w, int h, float ya,
                       double cx, double floorY, double figH,
                       const DancerMotion& d, double phaseFrame, Rgb color,
                       std::vector<std::array<double, 2>>* jointScreenOut = nullptr)
{
  if (!d.ok || d.anim.frameCount <= 0) return;
  const int n = d.anim.frameCount;
  const int fi = ((static_cast<int>(std::floor(phaseFrame)) % n) + n) % n;
  drawMarionette(dst, w, h, ya, d.anim, fi, cx, floorY, figH, d.refH, color,
                 jointScreenOut);
}
}  // namespace Qdless
