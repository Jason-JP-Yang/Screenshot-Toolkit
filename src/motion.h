// Spring-driven values that run inside the system compositor.
//
// A damped spring has a closed-form solution, so each value is uploaded once as a chain of cubic
// segments and DWM evaluates it at the display's refresh rate. This process stays asleep during
// animation, while still knowing the exact position and velocity for retargeting mid-flight.
#pragma once

#include "platform.h"

#include <dcomp.h>

#include <array>

namespace sstk {

struct SpringSpec {
  float response;  // Seconds for one undamped oscillation; smaller is faster.
  float damping;   // 1 is critically damped; below 1 overshoots slightly.
};

// A shared instant for everything started by one input event, expressed on the DWM timeline.
struct TimeBase {
  double seconds = 0.0;
  LARGE_INTEGER ticks{};
};

double SpringSettleTime(const SpringSpec& spec);

class AnimFloat {
 public:
  // Applies either a constant or an animation to one composition property.
  using Setter = void (*)(void* object, float value, IDCompositionAnimation* animation);

  // The bias is added to everything the sink receives, for geometry that tracks another edge at a fixed distance.
  void Bind(void* object, Setter setter, float bias = 0.0f);

  void Snap(float value);
  void To(IDCompositionDevice* device, const TimeBase& now, float target, const SpringSpec& spec, double delay = 0.0);
  void To(IDCompositionDevice* device, const TimeBase& now, float target, const SpringSpec& spec, double delay,
          float initial_velocity);

  float ValueAt(double time) const;
  float target() const { return target_; }
  double end_time() const { return end_time_; }

 private:
  struct Sink {
    void* object;
    Setter setter;
    float bias;
  };

  void Sample(double time, float* value, float* velocity) const;
  void Start(IDCompositionDevice* device, const TimeBase& now, float start, float velocity, float target,
             const SpringSpec& spec, double delay);

  std::array<Sink, 3> sinks_{};
  int sink_count_ = 0;
  float target_ = 0.0f;
  float displacement_ = 0.0f;
  float initial_velocity_ = 0.0f;
  double start_time_ = 0.0;
  double end_time_ = 0.0;
  SpringSpec spec_{1.0f, 1.0f};
};

// Builds an AnimFloat::Setter for a DirectComposition property with overloaded float/animation setters.
#define SSTK_SETTER(Interface, Method)                                     \
  [](void* object, float value, IDCompositionAnimation* animation) {       \
    auto* typed = static_cast<Interface*>(object);                         \
    if (animation != nullptr) typed->Method(animation);                    \
    else typed->Method(value);                                             \
  }

}  // namespace sstk
