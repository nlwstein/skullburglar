// Synthetic mouse motion for testing the drift gate (B-269), as a gyro-as-mouse or a worn sensor would send it:
// whole counts per frame, no game or SDK types. Used by tests/mouse_gate_sim.cpp and, in the game, by the
// BOTS_MOUSE_FEED test aid, so both feed the real driver path the same motion.
//
// A spec is one or more parts joined with '+'; each part is `kind:value[:value]`, and the parts' counts add up:
//   steady:<cps>[:<dir>]   <cps> counts per second, evenly spaced, in direction <dir> (x by default; "y" for up/down)
//                          (a negative <cps> goes the other way). A drift, or a deliberate slow turn at a higher rate.
//   alt:<cps>              +1, -1, +1... at <cps> counts per second: noise around zero
//   noise:<p>[:<bias>]     on each frame with probability <p> one count, positive with probability <bias> (default
//                          0.5): random jitter, with a bias a sensor that leans one way
//   poisson:<cps>[:<bias>] counts arrive at random (a Poisson process) at <cps> per second, positive with probability
//                          <bias> (default 1): drift that is not evenly spaced
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace bots::input {

class MousePattern {
 public:
  struct Counts {
    double x = 0, y = 0;
  };

  // False (and an empty pattern) when the spec does not parse.
  bool Parse(const std::string& spec, uint32_t seed = 1) {
    parts_.clear();
    rng_ = seed ? seed : 1;
    t_ = 0;
    size_t at = 0;
    while (at <= spec.size()) {
      size_t end = spec.find('+', at);
      if (end == std::string::npos) end = spec.size();
      if (!ParsePart(spec.substr(at, end - at))) {
        parts_.clear();
        return false;
      }
      at = end + 1;
    }
    return !parts_.empty();
  }

  bool Empty() const { return parts_.empty(); }

  // The counts of the next frame, `dt` seconds long.
  Counts Next(double dt) {
    Counts c;
    const double t0 = t_;
    t_ += dt;
    for (auto& p : parts_) {
      double n = 0;
      switch (p.kind) {
        case Kind::kSteady: {
          const double was = std::floor(std::fabs(p.rate) * t0 + kEps), now = std::floor(std::fabs(p.rate) * t_ + kEps);
          n = (now - was) * (p.rate < 0 ? -1.0 : 1.0);
          break;
        }
        case Kind::kAlt: {
          const double was = std::floor(p.rate * t0 + kEps), now = std::floor(p.rate * t_ + kEps);
          for (double k = was; k < now; ++k) n += std::fmod(k, 2.0) == 0.0 ? 1.0 : -1.0;
          break;
        }
        case Kind::kNoise:
          if (Uniform() < p.rate) n = Uniform() < p.bias ? 1.0 : -1.0;
          break;
        case Kind::kPoisson: {
          // Knuth's method; fine for the small means a frame has.
          const double limit = std::exp(-p.rate * dt);
          double prod = Uniform();
          int k = 0;
          while (prod > limit && k < 1000) prod *= Uniform(), ++k;
          for (int i = 0; i < k; ++i) n += Uniform() < p.bias ? 1.0 : -1.0;
          break;
        }
      }
      (p.y ? c.y : c.x) += n;
    }
    return c;
  }

 private:
  static constexpr double kEps = 1e-9;  // frames add up to a round time only to within rounding
  enum class Kind { kSteady, kAlt, kNoise, kPoisson };
  struct Part {
    Kind kind = Kind::kSteady;
    double rate = 0, bias = 0.5;
    bool y = false;
  };

  bool ParsePart(const std::string& s) {
    std::vector<std::string> f;
    for (size_t at = 0; at <= s.size();) {
      size_t end = s.find(':', at);
      if (end == std::string::npos) end = s.size();
      f.push_back(s.substr(at, end - at));
      at = end + 1;
    }
    if (f.size() < 2 || f[1].empty()) return false;
    Part p;
    p.rate = std::atof(f[1].c_str());
    if (f[0] == "steady") {
      p.kind = Kind::kSteady;
      p.y = f.size() > 2 && f[2] == "y";
    } else if (f[0] == "alt") {
      p.kind = Kind::kAlt;
    } else if (f[0] == "noise") {
      p.kind = Kind::kNoise;
      p.bias = f.size() > 2 ? std::atof(f[2].c_str()) : 0.5;
    } else if (f[0] == "poisson") {
      p.kind = Kind::kPoisson;
      p.bias = f.size() > 2 ? std::atof(f[2].c_str()) : 1.0;
    } else {
      return false;
    }
    parts_.push_back(p);
    return true;
  }

  double Uniform() {  // xorshift32 in [0, 1)
    rng_ ^= rng_ << 13, rng_ ^= rng_ >> 17, rng_ ^= rng_ << 5;
    return rng_ / 4294967296.0;
  }

  std::vector<Part> parts_;
  uint32_t rng_ = 1;
  double t_ = 0;
};

}  // namespace bots::input
