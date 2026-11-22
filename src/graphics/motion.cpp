#include "sstk/graphics/motion.h"

#include <algorithm>
#include <cmath>
#include <vector>

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

int AnimFloat::Leg(double progress) const {
  int leg = 0;
  while (leg + 2 < path_count_ && progress >= path_at_[static_cast<size_t>(leg) + 1]) ++leg;
  return leg;
}

double AnimFloat::Slope(int leg) const {
  const size_t i = static_cast<size_t>(leg);
  return (path_[i + 1] - path_[i]) / std::max(static_cast<double>(path_at_[i + 1] - path_at_[i]), 1e-6);
}

double AnimFloat::Along(int leg, double progress) const {
  const size_t i = static_cast<size_t>(leg);
  return path_[i] + Slope(leg) * (progress - path_at_[i]);
}

void AnimFloat::Sample(double time, float* value, float* velocity) const {
  if (time >= end_time_) {
    *value = target_;
    *velocity = 0.0f;
    return;
  }
  if (path_count_ > 0) {
    const SpringState state = Evaluate(spec_, -1.0, initial_velocity_, std::max(time - start_time_, 0.0));
    const double progress = 1.0 + state.value;
    const int leg = Leg(progress);
    *value = static_cast<float>(Along(leg, progress));
    *velocity = static_cast<float>(Slope(leg) * state.velocity);
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

float AnimFloat::VelocityAt(double time) const {
  float value;
  float velocity;
  Sample(time, &value, &velocity);
  return velocity;
}

void AnimFloat::Snap(float value) {
  target_ = value;
  path_count_ = 0;
  displacement_ = 0.0f;
  initial_velocity_ = 0.0f;
  start_time_ = end_time_ = 0.0;
  // DirectComposition drops a constant equal to the last one it took, or to the default before any, even once an
  // animation has replaced it, and the property stays wherever that animation was. An animation holding still takes.
  const bool dropped = animated_ && (!constant_known_ || value == constant_);
  for (int i = 0; i < sink_count_; ++i) {
    const Sink& sink = sinks_[i];
    ComPtr<IDCompositionAnimation> hold;
    if (dropped && SUCCEEDED(device_->CreateAnimation(&hold))) {
      hold->AddCubic(0.0, value + sink.bias, 0.0f, 0.0f, 0.0f);
      sink.setter(sink.object, 0.0f, hold.Get());
    } else {
      sink.setter(sink.object, value + sink.bias, nullptr);
    }
  }
  if (dropped) return;
  constant_ = value;
  constant_known_ = true;
  animated_ = false;
}

void AnimFloat::Play(IDCompositionDevice* device, IDCompositionAnimation* animation) {
  device_ = device;
  animated_ = true;
  for (int i = 0; i < sink_count_; ++i) sinks_[i].setter(sinks_[i].object, 0.0f, animation);
}

void AnimFloat::To(IDCompositionDevice* device, const TimeBase& now, float target, const SpringSpec& spec, double delay) {
  float value;
  float velocity;
  Sample(now.seconds, &value, &velocity);
  // Re-issuing the same destination must not restart a curve already heading there.
  if (std::fabs(target - target_) < 1e-3f) return;
  Start(device, now, value, value, velocity, target, spec, delay);
}

void AnimFloat::To(IDCompositionDevice* device, const TimeBase& now, float target, const SpringSpec& spec, double delay,
                   float initial_velocity) {
  float value;
  float velocity;
  Sample(now.seconds, &value, &velocity);
  Start(device, now, value, value, initial_velocity, target, spec, delay);
}

void AnimFloat::Pulse(IDCompositionDevice* device, const TimeBase& now, float peak, double rise, const SpringSpec& fall) {
  Start(device, now, ValueAt(now.seconds), peak, 0.0f, 0.0f, fall, rise);
}

void AnimFloat::Through(IDCompositionDevice* device, const TimeBase& now, const float* stops, const float* at, int count, const SpringSpec& spec,
                        float progress_velocity) {
  float value;
  float velocity;
  Sample(now.seconds, &value, &velocity);
  count = std::clamp(count, 1, static_cast<int>(path_.size()) - 1);
  path_[0] = value;
  path_at_[0] = 0.0f;
  for (int i = 0; i < count; ++i) {
    path_[static_cast<size_t>(i) + 1] = stops[i];
    path_at_[static_cast<size_t>(i) + 1] = at[i];
  }
  path_count_ = count + 1;
  target_ = stops[count - 1];
  displacement_ = -1.0f;
  initial_velocity_ = progress_velocity;
  spec_ = spec;
  start_time_ = now.seconds;
  const double duration = SpringSettleTime(spec);
  end_time_ = start_time_ + duration;
  device_ = device;
  animated_ = true;

  const auto progress_at = [&](double time) {
    if (time >= duration) return SpringState{1.0, 0.0};
    const SpringState state = Evaluate(spec, -1.0, progress_velocity, time);
    return SpringState{1.0 + state.value, state.velocity};
  };
  // The schedule packs segments into the fast early part of the curve, and breaks where each stop is passed, so that
  // no segment turns a corner.
  std::vector<double> times;
  for (int k = 0; k <= kSegments; ++k) times.push_back(duration * std::pow(static_cast<double>(k) / kSegments, 1.5));
  for (int i = 1; i < count; ++i) {
    double low = 0.0;
    double high = duration;
    for (int step = 0; step < 48; ++step) {
      const double middle = (low + high) * 0.5;
      (progress_at(middle).value < path_at_[static_cast<size_t>(i)] ? low : high) = middle;
    }
    times.push_back(high);
  }
  std::sort(times.begin(), times.end());

  for (int s = 0; s < sink_count_; ++s) {
    const Sink& sink = sinks_[s];
    ComPtr<IDCompositionAnimation> animation;
    if (FAILED(device->CreateAnimation(&animation))) {
      sink.setter(sink.object, target_ + sink.bias, nullptr);
      continue;
    }
    animation->SetAbsoluteBeginTime(now.ticks);
    for (size_t k = 0; k + 1 < times.size(); ++k) {
      const double t0 = times[k];
      const double t1 = times[k + 1];
      const double h = t1 - t0;
      if (h < 1e-6) continue;
      const SpringState p0 = progress_at(t0);
      const SpringState p1 = progress_at(t1);
      const int leg = Leg(progress_at((t0 + t1) * 0.5).value);
      const double slope = Slope(leg);
      const double v0 = Along(leg, p0.value);
      const double v1 = Along(leg, p1.value);
      const double d0 = slope * p0.velocity;
      const double d1 = slope * p1.velocity;
      const double c2 = (3.0 * (v1 - v0) / h - 2.0 * d0 - d1) / h;
      const double c3 = (2.0 * (v0 - v1) / h + d0 + d1) / (h * h);
      animation->AddCubic(t0, static_cast<float>(v0 + sink.bias), static_cast<float>(d0), static_cast<float>(c2), static_cast<float>(c3));
    }
    animation->End(duration, target_ + sink.bias);
    sink.setter(sink.object, 0.0f, animation.Get());
  }
}

void AnimFloat::Start(IDCompositionDevice* device, const TimeBase& now, float lead, float start, float velocity, float target,
                      const SpringSpec& spec, double delay) {
  path_count_ = 0;
  target_ = target;
  displacement_ = start - target;
  initial_velocity_ = velocity;
  spec_ = spec;
  start_time_ = now.seconds + delay;
  const double duration = SpringSettleTime(spec);
  end_time_ = start_time_ + duration;
  device_ = device;
  animated_ = true;

  for (int i = 0; i < sink_count_; ++i) {
    const Sink& sink = sinks_[i];
    ComPtr<IDCompositionAnimation> animation;
    if (FAILED(device->CreateAnimation(&animation))) {
      sink.setter(sink.object, target + sink.bias, nullptr);
      continue;
    }
    animation->SetAbsoluteBeginTime(now.ticks);
    if (delay > 0.0) {
      const double climb = start - lead;
      animation->AddCubic(0.0, lead + sink.bias, 0.0f, static_cast<float>(3.0 * climb / (delay * delay)),
                          static_cast<float>(-2.0 * climb / (delay * delay * delay)));
    }

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
