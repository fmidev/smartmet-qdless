#pragma once

// Minimal BVH (BioVision Hierarchy) loader + forward-kinematics solver.
//
// Used by the Marionette exit effect to render Carnegie Mellon mocap data
// at the terminal's redraw rate. BVH is a plain-text format with two
// sections:
//
//   HIERARCHY
//     ROOT Hips { OFFSET ...; CHANNELS 6 Xposition Yposition Zposition
//                Zrotation Yrotation Xrotation; JOINT LHipJoint { ... } ... }
//   MOTION
//     Frames: N
//     Frame Time: 0.0166666
//     <N lines of channel values, one row per frame, space-separated>
//
// Channels: most CMU joints use ZYX rotation order; the root additionally
// has XYZ position channels. We support arbitrary channel orderings and
// 'End Site' leaves (which have no channels but contribute an OFFSET).
//
// Coordinate convention: BVH stores Y-up, with the subject typically
// facing +Z. The Euler rotations are intrinsic — i.e. each channel is
// applied around the joint's current local axis. Concretely, for "ZYX":
//
//   R_local = Rz(theta_z) * Ry(theta_y) * Rx(theta_x)
//
// applied in that order; equivalently you can read it as "rotate X first,
// then Y, then Z in the parent frame".

#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace Qdless
{
struct BvhJoint
{
  std::string name;
  int parent = -1;
  std::array<double, 3> offset{0, 0, 0};
  // Channel encoding: one char per channel, where lower-case xyz means
  // position-along-axis and upper-case XYZ means rotation-around-axis.
  // The order in the string matches the order of values in each frame's
  // row, which is also the order the rotations are MULTIPLIED in (so
  // "ZYX" → R = Rz * Ry * Rx applied to the local axes).
  std::string channels;
  int channelStart = 0;
};

struct BvhAnimation
{
  std::vector<BvhJoint> joints;
  int totalChannels = 0;
  int frameCount = 0;
  double frameTime = 0;
  std::vector<double> frames;  // size = frameCount * totalChannels

  int jointIndex(const std::string& name) const
  {
    for (std::size_t i = 0; i < joints.size(); ++i)
      if (joints[i].name == name)
        return static_cast<int>(i);
    return -1;
  }
};

namespace BvhDetail
{
inline std::string tokenize_pop(std::istringstream& iss)
{
  std::string s;
  iss >> s;
  return s;
}

inline char channelChar(const std::string& token)
{
  if (token == "Xposition") return 'x';
  if (token == "Yposition") return 'y';
  if (token == "Zposition") return 'z';
  if (token == "Xrotation") return 'X';
  if (token == "Yrotation") return 'Y';
  if (token == "Zrotation") return 'Z';
  throw std::runtime_error("unknown BVH channel: " + token);
}

inline void parseHierarchy(std::ifstream& in, BvhAnimation& out)
{
  std::vector<int> stack;  // parent-index stack
  std::string line;
  while (std::getline(in, line))
  {
    std::istringstream iss(line);
    std::string tok;
    if (!(iss >> tok))
      continue;
    if (tok == "ROOT" || tok == "JOINT")
    {
      std::string name;
      iss >> name;
      BvhJoint j;
      j.name = name;
      j.parent = stack.empty() ? -1 : stack.back();
      out.joints.push_back(j);
      stack.push_back(static_cast<int>(out.joints.size() - 1));
    }
    else if (tok == "End")
    {
      // "End Site" — a leaf with only an OFFSET, no channels.
      BvhJoint j;
      j.name = "EndSite";
      j.parent = stack.empty() ? -1 : stack.back();
      out.joints.push_back(j);
      stack.push_back(static_cast<int>(out.joints.size() - 1));
    }
    else if (tok == "{")
    {
      // scope open — nothing to do
    }
    else if (tok == "}")
    {
      if (!stack.empty())
        stack.pop_back();
    }
    else if (tok == "OFFSET")
    {
      auto& j = out.joints.back();
      iss >> j.offset[0] >> j.offset[1] >> j.offset[2];
    }
    else if (tok == "CHANNELS")
    {
      int n = 0;
      iss >> n;
      auto& j = out.joints.back();
      j.channels.clear();
      for (int i = 0; i < n; ++i)
      {
        std::string c;
        iss >> c;
        j.channels.push_back(channelChar(c));
      }
      j.channelStart = out.totalChannels;
      out.totalChannels += n;
    }
    else if (tok == "MOTION")
    {
      return;  // hierarchy block done
    }
  }
  throw std::runtime_error("BVH: no MOTION section found");
}

inline void parseMotion(std::ifstream& in, BvhAnimation& out)
{
  std::string line;
  while (std::getline(in, line))
  {
    std::istringstream iss(line);
    std::string tok;
    if (!(iss >> tok))
      continue;
    if (tok == "Frames:")
    {
      iss >> out.frameCount;
    }
    else if (tok == "Frame")
    {
      std::string time;
      iss >> time;  // "Time:"
      iss >> out.frameTime;
      break;
    }
  }
  out.frames.resize(static_cast<std::size_t>(out.frameCount) * out.totalChannels);
  std::size_t idx = 0;
  for (int f = 0; f < out.frameCount; ++f)
  {
    if (!std::getline(in, line))
      throw std::runtime_error("BVH: motion data ended early");
    std::istringstream iss(line);
    for (int c = 0; c < out.totalChannels; ++c)
    {
      if (!(iss >> out.frames[idx++]))
        throw std::runtime_error("BVH: malformed frame row");
    }
  }
}
}  // namespace BvhDetail

inline BvhAnimation loadBvhFile(const std::string& path)
{
  std::ifstream in(path);
  if (!in)
    throw std::runtime_error("cannot open BVH: " + path);
  BvhAnimation out;
  BvhDetail::parseHierarchy(in, out);
  BvhDetail::parseMotion(in, out);
  return out;
}

// 3×3 row-major matrix utilities for forward kinematics.
struct Mat3
{
  double m[9];
  static Mat3 identity()
  {
    return {{1, 0, 0, 0, 1, 0, 0, 0, 1}};
  }
  static Mat3 rotX(double a)
  {
    const double c = std::cos(a), s = std::sin(a);
    return {{1, 0, 0, 0, c, -s, 0, s, c}};
  }
  static Mat3 rotY(double a)
  {
    const double c = std::cos(a), s = std::sin(a);
    return {{c, 0, s, 0, 1, 0, -s, 0, c}};
  }
  static Mat3 rotZ(double a)
  {
    const double c = std::cos(a), s = std::sin(a);
    return {{c, -s, 0, s, c, 0, 0, 0, 1}};
  }
  Mat3 operator*(const Mat3& o) const
  {
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j)
      {
        double s = 0;
        for (int k = 0; k < 3; ++k)
          s += m[i * 3 + k] * o.m[k * 3 + j];
        r.m[i * 3 + j] = s;
      }
    return r;
  }
  std::array<double, 3> apply(const std::array<double, 3>& v) const
  {
    return {{m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
             m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
             m[6] * v[0] + m[7] * v[1] + m[8] * v[2]}};
  }
};

// Forward kinematics: returns one (x, y, z) world position per joint.
//
// The BVH semantics for ZYX channels say: build a local rotation
// matrix by applying X first, then Y, then Z. That is R = Rz * Ry * Rx
// (because matrix multiplication composes right-to-left when applied to
// vectors). We honour whatever channel order the file specified by
// multiplying matrices in the order they appear: each channel is
// applied AFTER the previously-built rotation, i.e. accumR = Rch * accumR.
inline std::vector<std::array<double, 3>> bvhJointPositions(
    const BvhAnimation& a, int frameIdx)
{
  const std::size_t N = a.joints.size();
  std::vector<std::array<double, 3>> worldPos(N);
  std::vector<Mat3> worldRot(N);
  const double deg2rad = M_PI / 180.0;
  if (a.frameCount == 0)
    return worldPos;
  const double* row = a.frames.data() +
                      static_cast<std::size_t>(std::max(0, std::min(frameIdx, a.frameCount - 1))) *
                          a.totalChannels;
  for (std::size_t i = 0; i < N; ++i)
  {
    const BvhJoint& j = a.joints[i];
    std::array<double, 3> localPos = j.offset;
    Mat3 localRot = Mat3::identity();
    int c = j.channelStart;
    for (char ch : j.channels)
    {
      const double v = row[c++];
      switch (ch)
      {
        case 'x': localPos[0] += v; break;
        case 'y': localPos[1] += v; break;
        case 'z': localPos[2] += v; break;
        // BVH rotations are intrinsic: each channel rotates around the
        // joint's CURRENT local axis (i.e. after all prior channels have
        // been applied). The composite matrix that takes local-frame
        // vectors to parent-frame is Rfirst * Rsecond * Rthird, read
        // left-to-right in channel order. Equivalently, multiply each
        // new rotation on the RIGHT of the accumulator.
        case 'X': localRot = localRot * Mat3::rotX(v * deg2rad); break;
        case 'Y': localRot = localRot * Mat3::rotY(v * deg2rad); break;
        case 'Z': localRot = localRot * Mat3::rotZ(v * deg2rad); break;
        default: break;
      }
    }
    if (j.parent < 0)
    {
      worldPos[i] = localPos;
      worldRot[i] = localRot;
    }
    else
    {
      // world_p = parent_world_p + parent_world_R * local_offset
      auto rotatedOffset = worldRot[j.parent].apply(localPos);
      worldPos[i] = {{worldPos[j.parent][0] + rotatedOffset[0],
                      worldPos[j.parent][1] + rotatedOffset[1],
                      worldPos[j.parent][2] + rotatedOffset[2]}};
      worldRot[i] = worldRot[j.parent] * localRot;
    }
  }
  return worldPos;
}
}  // namespace Qdless
