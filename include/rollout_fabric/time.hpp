// Rollout Fabric - time, clocks and freshness.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Two distinct notions of time are used and never interchanged:
//
//   * Wall time (Timestamp) is what evidence ages are measured against. It can
//     move backwards, so every comparison against it is guarded.
//   * Monotonic time (Duration since an arbitrary origin) is what deadlines and
//     soak timers use, because it cannot move backwards.
//
// Every freshness evaluation returns a reason, so a gate can explain exactly
// why it refused to advance.
#pragma once

#include <cstdint>
#include <compare>
#include <string>
#include <string_view>

#include "rollout_fabric/status.hpp"

namespace rollout_fabric {

class Duration {
 public:
  constexpr Duration() noexcept = default;

  [[nodiscard]] static constexpr Duration from_nanos(std::int64_t nanos) noexcept { return Duration(nanos); }
  [[nodiscard]] static constexpr Duration from_micros(std::int64_t micros) noexcept {
    return Duration(micros * 1000);
  }
  [[nodiscard]] static constexpr Duration from_millis(std::int64_t millis) noexcept {
    return Duration(millis * 1000000);
  }
  [[nodiscard]] static constexpr Duration from_seconds(std::int64_t seconds) noexcept {
    return Duration(seconds * 1000000000);
  }

  [[nodiscard]] constexpr std::int64_t nanos() const noexcept { return nanos_; }
  [[nodiscard]] constexpr std::int64_t millis() const noexcept { return nanos_ / 1000000; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return nanos_ == 0; }
  [[nodiscard]] constexpr bool is_negative() const noexcept { return nanos_ < 0; }

  friend constexpr auto operator<=>(const Duration&, const Duration&) noexcept = default;
  friend constexpr Duration operator+(Duration lhs, Duration rhs) noexcept {
    return Duration(lhs.nanos_ + rhs.nanos_);
  }
  friend constexpr Duration operator-(Duration lhs, Duration rhs) noexcept {
    return Duration(lhs.nanos_ - rhs.nanos_);
  }

 private:
  explicit constexpr Duration(std::int64_t nanos) noexcept : nanos_(nanos) {}
  std::int64_t nanos_ = 0;
};

// Nanoseconds since the Unix epoch, UTC.
class Timestamp {
 public:
  constexpr Timestamp() noexcept = default;
  [[nodiscard]] static constexpr Timestamp from_unix_nanos(std::int64_t nanos) noexcept {
    return Timestamp(nanos);
  }
  [[nodiscard]] static constexpr Timestamp from_unix_millis(std::int64_t millis) noexcept {
    return Timestamp(millis * 1000000);
  }
  [[nodiscard]] static constexpr Timestamp from_unix_seconds(std::int64_t seconds) noexcept {
    return Timestamp(seconds * 1000000000);
  }

  [[nodiscard]] constexpr std::int64_t unix_nanos() const noexcept { return nanos_; }
  [[nodiscard]] constexpr std::int64_t unix_millis() const noexcept { return nanos_ / 1000000; }
  [[nodiscard]] constexpr bool is_nil() const noexcept { return nanos_ == 0; }

  friend constexpr auto operator<=>(const Timestamp&, const Timestamp&) noexcept = default;
  friend constexpr Timestamp operator+(Timestamp lhs, Duration rhs) noexcept {
    return Timestamp(lhs.nanos_ + rhs.nanos());
  }
  friend constexpr Timestamp operator-(Timestamp lhs, Duration rhs) noexcept {
    return Timestamp(lhs.nanos_ - rhs.nanos());
  }
  friend constexpr Duration operator-(Timestamp lhs, Timestamp rhs) noexcept {
    return Duration::from_nanos(lhs.nanos_ - rhs.nanos_);
  }

  // RFC 3339 with millisecond precision and a trailing Z.
  [[nodiscard]] std::string to_rfc3339() const;
  [[nodiscard]] static Result<Timestamp> parse_rfc3339(std::string_view text);

 private:
  explicit constexpr Timestamp(std::int64_t nanos) noexcept : nanos_(nanos) {}
  std::int64_t nanos_ = 0;
};

class Clock {
 public:
  Clock() = default;
  virtual ~Clock();
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  Clock(Clock&&) = delete;
  Clock& operator=(Clock&&) = delete;

  // Wall time. May jump in either direction; callers must tolerate that.
  [[nodiscard]] virtual Timestamp wall_now() const = 0;
  // Monotonic time. Never moves backwards for the lifetime of the process.
  [[nodiscard]] virtual Timestamp monotonic_now() const = 0;
};

class SystemClock final : public Clock {
 public:
  [[nodiscard]] Timestamp wall_now() const override;
  [[nodiscard]] Timestamp monotonic_now() const override;
};

// Deterministic clock for tests and for replay-based property checks.
class ManualClock final : public Clock {
 public:
  ManualClock() noexcept;
  explicit ManualClock(Timestamp start) noexcept : wall_(start), monotonic_(start) {}

  [[nodiscard]] Timestamp wall_now() const override { return wall_; }
  [[nodiscard]] Timestamp monotonic_now() const override { return monotonic_; }

  void advance(Duration delta) noexcept {
    wall_ = wall_ + delta;
    monotonic_ = monotonic_ + delta;
  }
  // Moves wall time only, which is how clock regression is injected.
  void jump_wall(Timestamp to) noexcept { wall_ = to; }
  void set_wall(Timestamp to) noexcept { wall_ = to; }

 private:
  Timestamp wall_{};
  Timestamp monotonic_{};
};

// Outcome of a freshness evaluation. Never a bare boolean: the reason is part
// of the decision trail.
struct Freshness {
  bool fresh = false;
  Duration age{};
  std::string reason;

  [[nodiscard]] explicit operator bool() const noexcept { return fresh; }
};

// A sample is fresh when it is not older than max_age and not implausibly far
// in the future. A negative age (the observer's clock is behind the sample's)
// is tolerated only up to max_future_skew; beyond that the sample is rejected
// as a clock anomaly rather than silently accepted.
[[nodiscard]] Freshness evaluate_freshness(Timestamp observed_at,
                                           Timestamp now,
                                           Duration max_age,
                                           Duration max_future_skew);

}  // namespace rollout_fabric
