// Rollout Fabric - lifecycle states and the legal transition table.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// States are explicit values, transitions are a closed table, and an illegal
// transition is a fenced error rather than a silent state write. "Advancing" is
// a real, observable state that a rollout occupies while it moves from one
// stage to the next, so a controller killed at that instant is distinguishable
// on restart from one killed mid-stage.
#pragma once

#include <cstdint>
#include <string_view>

namespace rollout_fabric {

enum class RolloutState : std::uint8_t {
  kCreated = 0,
  kValidated = 1,
  kArmed = 2,
  kRunning = 3,
  kGated = 4,
  kSoaking = 5,
  kAdvancing = 6,
  kPaused = 7,
  kAborting = 8,
  kRollingBack = 9,
  kFailed = 10,
  kCompleted = 11,
  kRetired = 12,
};

enum class StageState : std::uint8_t {
  kPending = 0,
  kBlocked = 1,
  kArmed = 2,
  kRunning = 3,
  kGated = 4,
  kSoaking = 5,
  kAdvancing = 6,
  kSucceeded = 7,
  kFailed = 8,
  kAborted = 9,
  kRolledBack = 10,
  kSkipped = 11,
};

enum class TargetState : std::uint8_t {
  kPending = 0,
  kDispatched = 1,
  kSucceeded = 2,
  kFailed = 3,
  kCancelled = 4,
  kRollingBack = 5,
  kRolledBack = 6,
  kRollbackFailed = 7,
  kExhausted = 8,
  kUnknown = 9,
};

[[nodiscard]] std::string_view to_string(RolloutState state) noexcept;
[[nodiscard]] std::string_view to_string(StageState state) noexcept;
[[nodiscard]] std::string_view to_string(TargetState state) noexcept;

[[nodiscard]] bool is_terminal(RolloutState state) noexcept;
[[nodiscard]] bool is_terminal(StageState state) noexcept;
[[nodiscard]] bool is_terminal(TargetState state) noexcept;

// A state a rollout occupies while the operator has suspended forward progress
// but nothing has been torn down.
[[nodiscard]] bool is_suspended(RolloutState state) noexcept;

// Whether new dispatch may happen in this state.
[[nodiscard]] bool admits_dispatch(RolloutState state) noexcept;

[[nodiscard]] bool legal_transition(RolloutState from, RolloutState to) noexcept;
[[nodiscard]] bool legal_transition(StageState from, StageState to) noexcept;
[[nodiscard]] bool legal_transition(TargetState from, TargetState to) noexcept;

// Pause remembers where it came from so that resume returns to the exact state
// that was interrupted, including Gated and Advancing.
[[nodiscard]] bool can_pause(RolloutState state) noexcept;

}  // namespace rollout_fabric
