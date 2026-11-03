#include "motion.h"

#include <cmath>

namespace sstk {

namespace {

constexpr double kTwoPi = 6.283185307179586;
constexpr int kSegments = 16;

struct SpringState {
  double value;
  double velocity;
};

// Displacement from the target after `time` seconds, from the analytic solution of a damped oscillator.
SpringState Evaluate(const SpringSpec& spec, double displacement, double velocity, double time) {
  const double omega = kTwoPi / spec.response;
  const double zeta = spec.damping;
  if (zeta < 0.999) {
    const double damped = omega * std::sqrt(1.0 - zeta * zeta);
    const double decay = std::exp(-zeta * omega * time);
    const double b = (velocity + zeta * omega * displacement) / damped;
    const double cosine = std::cos(damped * time);
    const double sine = std::sin(damped * time);
    const double wave = displacement * cosine + b * sine;
    const double slope = -displacement * damped * sine + b * damped * cosine;
    return {decay * wave, decay * (slope - zeta * omega * wave)};
  }
  const double decay = std::exp(-omega * time);
  const double b = velocity + omega * displacement;
  return {(displacement + b * time) * decay, (b - omega * (displacement + b * time)) * decay};
}

}  // namespace

double SpringSettleTime(const SpringSpec& spec) {
  const double omega = kTwoPi / spec.response;
  return spec.damping < 0.999 ? 7.6 / (spec.damping * omega) : 9.5 / omega;
}

void SpringValue::Snap(float to) {
  value = target = to;
  velocity = 0.0f;
}

void SpringValue::Step(const SpringSpec& spec, float dt) {
  const SpringState state = Evaluate(spec, value - target, velocity, dt);
  value = target + static_cast<float>(state.value);
  velocity = static_cast<float>(state.velocity);
}

bool SpringValue::Settled(float tolerance) const {
  return std::fabs(value - target) < tolerance && std::fabs(velocity) < tolerance * 12.0f;
}

void AnimFloat::Bind(void* object, Setter setter, float bias) { sinks_[sink_count_++] = Sink{object, setter, bias}; }

void AnimFloat::Sample(double time, float* value, float* velocity) const {
  if (time >= end_time_) {
    *value = target_;
    *velocity = 0.0f;
    return;
  }
  if (time < start_time_) {
    *value = target_ + displacement_;
    *velocity = 0.0f;
    return;
  }
  SpringState state = Evaluate(spec_, displacement_, initial_velocity_, time - start_time_);
  *value = target_ + static_cast<float>(state.value);
  *velocity = static_cast<float>(state.velocity);
}

float AnimFloat::ValueAt(double time) const {
  float value;
  float velocity;
  Sample(time, &value, &velocity);
  return value;
}

void AnimFloat::Snap(float value) {
  target_ = value;
  displacement_ = 0.0f;
  initial_velocity_ = 0.0f;
  start_time_ = end_time_ = 0.0;
  for (int i = 0; i < sink_count_; ++i) sinks_[i].setter(sinks_[i].object, value + sinks_[i].bias, nullptr);
}

void AnimFloat::To(IDCompositionDevice* device, const TimeBase& now, float target, const SpringSpec& spec, double delay) {
  float value;
  float velocity;
  Sample(now.seconds, &value, &velocity);
  // Re-issuing the same destination must not restart a curve already heading there.
  if (std::fabs(target - target_) < 1e-3f) return;
  Start(device, now, value, velocity, target, spec, delay);
}

void AnimFloat::To(IDCompositionDevice* device, const TimeBase& now, float target, const SpringSpec& spec, double delay,
                   float initial_velocity) {
  float value;
  float velocity;
  Sample(now.seconds, &value, &velocity);
  Start(device, now, value, initial_velocity, target, spec, delay);
}

void AnimFloat::Start(IDCompositionDevice* device, const TimeBase& now, float start, float velocity, float target,
                      const SpringSpec& spec, double delay) {
  target_ = target;
  displacement_ = start - target;
  initial_velocity_ = velocity;
  spec_ = spec;
  start_time_ = now.seconds + delay;
  const double duration = SpringSettleTime(spec);
  end_time_ = start_time_ + duration;

  for (int i = 0; i < sink_count_; ++i) {
    const Sink& sink = sinks_[i];
    ComPtr<IDCompositionAnimation> animation;
    if (FAILED(device->CreateAnimation(&animation))) {
      sink.setter(sink.object, target + sink.bias, nullptr);
      continue;
    }
    animation->SetAbsoluteBeginTime(now.ticks);
    if (delay > 0.0) animation->AddCubic(0.0, start + sink.bias, 0.0f, 0.0f, 0.0f);

    // Hermite cubics over a schedule that packs segments into the fast early part of the curve.
    double t0 = 0.0;
    SpringState s0 = Evaluate(spec, displacement_, velocity, 0.0);
    for (int k = 1; k <= kSegments; ++k) {
      const double fraction = static_cast<double>(k) / kSegments;
      const double t1 = duration * std::pow(fraction, 1.5);
      SpringState s1 = k == kSegments ? SpringState{0.0, 0.0} : Evaluate(spec, displacement_, velocity, t1);
      const double h = t1 - t0;
      const double c2 = (3.0 * (s1.value - s0.value) / h - 2.0 * s0.velocity - s1.velocity) / h;
      const double c3 = (2.0 * (s0.value - s1.value) / h + s0.velocity + s1.velocity) / (h * h);
      animation->AddCubic(delay + t0, static_cast<float>(s0.value + target + sink.bias),
                          static_cast<float>(s0.velocity), static_cast<float>(c2), static_cast<float>(c3));
      t0 = t1;
      s0 = s1;
    }
    animation->End(delay + duration, target + sink.bias);
    sink.setter(sink.object, 0.0f, animation.Get());
  }
}

}  // namespace sstk
