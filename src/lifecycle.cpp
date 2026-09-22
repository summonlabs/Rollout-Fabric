// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "rollout_fabric/lifecycle.hpp"

namespace rollout_fabric {

std::string_view to_string(RolloutState state) noexcept {
  switch (state) {
    case RolloutState::kCreated: return "created";
    case RolloutState::kValidated: return "validated";
    case RolloutState::kArmed: return "armed";
    case RolloutState::kRunning: return "running";
    case RolloutState::kGated: return "gated";
    case RolloutState::kSoaking: return "soaking";
    case RolloutState::kAdvancing: return "advancing";
    case RolloutState::kPaused: return "paused";
    case RolloutState::kAborting: return "aborting";
    case RolloutState::kRollingBack: return "rolling_back";
    case RolloutState::kFailed: return "failed";
    case RolloutState::kCompleted: return "completed";
    case RolloutState::kRetired: return "retired";
  }
  return "unknown";
}

std::string_view to_string(StageState state) noexcept {
  switch (state) {
    case StageState::kPending: return "pending";
    case StageState::kBlocked: return "blocked";
    case StageState::kArmed: return "armed";
    case StageState::kRunning: return "running";
    case StageState::kGated: return "gated";
    case StageState::kSoaking: return "soaking";
    case StageState::kAdvancing: return "advancing";
    case StageState::kSucceeded: return "succeeded";
    case StageState::kFailed: return "failed";
    case StageState::kAborted: return "aborted";
    case StageState::kRolledBack: return "rolled_back";
    case StageState::kSkipped: return "skipped";
  }
  return "unknown";
}

std::string_view to_string(TargetState state) noexcept {
  switch (state) {
    case TargetState::kPending: return "pending";
    case TargetState::kDispatched: return "dispatched";
    case TargetState::kSucceeded: return "succeeded";
    case TargetState::kFailed: return "failed";
    case TargetState::kCancelled: return "cancelled";
    case TargetState::kRollingBack: return "rolling_back";
    case TargetState::kRolledBack: return "rolled_back";
    case TargetState::kRollbackFailed: return "rollback_failed";
    case TargetState::kExhausted: return "exhausted";
    case TargetState::kUnknown: return "unknown";
  }
  return "unknown";
}

bool is_terminal(RolloutState state) noexcept {
  return state == RolloutState::kFailed || state == RolloutState::kCompleted ||
         state == RolloutState::kRetired;
}

bool is_terminal(StageState state) noexcept {
  return state == StageState::kSucceeded || state == StageState::kFailed ||
         state == StageState::kAborted || state == StageState::kRolledBack ||
         state == StageState::kSkipped;
}

bool is_terminal(TargetState state) noexcept {
  return state == TargetState::kSucceeded || state == TargetState::kFailed ||
         state == TargetState::kCancelled || state == TargetState::kRolledBack ||
         state == TargetState::kRollbackFailed || state == TargetState::kExhausted;
}

bool is_suspended(RolloutState state) noexcept {
  return state == RolloutState::kPaused || state == RolloutState::kGated ||
         state == RolloutState::kSoaking || state == RolloutState::kAborting ||
         state == RolloutState::kRollingBack;
}

bool admits_dispatch(RolloutState state) noexcept {
  return state == RolloutState::kRunning || state == RolloutState::kSoaking ||
         state == RolloutState::kAdvancing;
}

bool can_pause(RolloutState state) noexcept {
  return state == RolloutState::kArmed || state == RolloutState::kRunning ||
         state == RolloutState::kGated || state == RolloutState::kSoaking ||
         state == RolloutState::kAdvancing;
}

bool legal_transition(RolloutState from, RolloutState to) noexcept {
  if (from == to) {
    return false;
  }
  switch (from) {
    case RolloutState::kCreated:
      return to == RolloutState::kValidated || to == RolloutState::kRetired;
    case RolloutState::kValidated:
      return to == RolloutState::kArmed || to == RolloutState::kRetired;
    case RolloutState::kArmed:
      return to == RolloutState::kRunning || to == RolloutState::kGated ||
             to == RolloutState::kPaused || to == RolloutState::kAborting ||
             to == RolloutState::kFailed || to == RolloutState::kRetired;
    case RolloutState::kRunning:
      return to == RolloutState::kGated || to == RolloutState::kSoaking ||
             to == RolloutState::kAdvancing || to == RolloutState::kPaused ||
             to == RolloutState::kAborting || to == RolloutState::kRollingBack ||
             to == RolloutState::kFailed || to == RolloutState::kCompleted;
    case RolloutState::kGated:
      // A stage stopped at a manual gate can still be the last one, so
      // completion is reachable from here as well as from running.
      return to == RolloutState::kRunning || to == RolloutState::kAdvancing ||
             to == RolloutState::kPaused || to == RolloutState::kAborting ||
             to == RolloutState::kRollingBack || to == RolloutState::kFailed ||
             to == RolloutState::kCompleted;
    case RolloutState::kSoaking:
      // A soak is a state a rollout can finish from: the soak ends and the final
      // stage advances without passing back through running.
      return to == RolloutState::kRunning || to == RolloutState::kAdvancing ||
             to == RolloutState::kGated || to == RolloutState::kPaused ||
             to == RolloutState::kAborting || to == RolloutState::kRollingBack ||
             to == RolloutState::kFailed || to == RolloutState::kCompleted;
    case RolloutState::kAdvancing:
      return to == RolloutState::kRunning || to == RolloutState::kGated ||
             to == RolloutState::kPaused || to == RolloutState::kAborting ||
             to == RolloutState::kRollingBack || to == RolloutState::kFailed ||
             to == RolloutState::kCompleted;
    case RolloutState::kPaused:
      // Resume returns to whichever state was interrupted; the exact target is
      // checked against resume_state by the state machine.
      return to == RolloutState::kArmed || to == RolloutState::kRunning ||
             to == RolloutState::kGated || to == RolloutState::kSoaking ||
             to == RolloutState::kAdvancing || to == RolloutState::kAborting ||
             to == RolloutState::kRollingBack || to == RolloutState::kFailed ||
             to == RolloutState::kRetired;
    case RolloutState::kAborting:
      return to == RolloutState::kRollingBack || to == RolloutState::kFailed ||
             to == RolloutState::kCompleted || to == RolloutState::kRetired;
    case RolloutState::kRollingBack:
      return to == RolloutState::kFailed || to == RolloutState::kCompleted ||
             to == RolloutState::kRetired;
    case RolloutState::kFailed:
      return to == RolloutState::kRetired;
    case RolloutState::kCompleted:
      return to == RolloutState::kRetired;
    case RolloutState::kRetired:
      return false;
  }
  return false;
}

bool legal_transition(StageState from, StageState to) noexcept {
  if (from == to) {
    return false;
  }
  switch (from) {
    case StageState::kPending:
      // A stage may stop at a manual entry gate before it ever runs.
      return to == StageState::kBlocked || to == StageState::kArmed ||
             to == StageState::kGated || to == StageState::kSkipped;
    case StageState::kBlocked:
      return to == StageState::kArmed || to == StageState::kAborted || to == StageState::kSkipped;
    case StageState::kArmed:
      return to == StageState::kRunning || to == StageState::kGated ||
             to == StageState::kAborted || to == StageState::kRolledBack ||
             to == StageState::kSkipped;
    case StageState::kRunning:
      return to == StageState::kGated || to == StageState::kSoaking ||
             to == StageState::kAdvancing || to == StageState::kSucceeded ||
             to == StageState::kFailed || to == StageState::kAborted ||
             to == StageState::kRolledBack;
    case StageState::kGated:
      return to == StageState::kRunning || to == StageState::kAdvancing ||
             to == StageState::kFailed || to == StageState::kAborted ||
             to == StageState::kRolledBack;
    case StageState::kSoaking:
      return to == StageState::kRunning || to == StageState::kAdvancing ||
             to == StageState::kFailed || to == StageState::kAborted ||
             to == StageState::kRolledBack;
    case StageState::kAdvancing:
      return to == StageState::kRunning || to == StageState::kSucceeded ||
             to == StageState::kFailed || to == StageState::kAborted ||
             to == StageState::kRolledBack;
    case StageState::kSucceeded:
      return to == StageState::kRolledBack;
    case StageState::kFailed:
      return to == StageState::kRolledBack;
    case StageState::kAborted:
      return to == StageState::kRolledBack;
    case StageState::kRolledBack:
      return false;
    case StageState::kSkipped:
      return false;
  }
  return false;
}

bool legal_transition(TargetState from, TargetState to) noexcept {
  if (from == to) {
    return false;
  }
  switch (from) {
    case TargetState::kPending:
      return to == TargetState::kDispatched || to == TargetState::kCancelled ||
             to == TargetState::kExhausted || to == TargetState::kUnknown;
    case TargetState::kDispatched:
      return to == TargetState::kPending || to == TargetState::kSucceeded ||
             to == TargetState::kFailed ||
             to == TargetState::kCancelled || to == TargetState::kUnknown ||
             to == TargetState::kExhausted;
    case TargetState::kUnknown:
      // Reconciliation may conclude that the effect never started, in which
      // case the same attempt becomes dispatchable again.
      return to == TargetState::kPending || to == TargetState::kDispatched ||
             to == TargetState::kSucceeded ||
             to == TargetState::kFailed || to == TargetState::kCancelled ||
             to == TargetState::kExhausted;
    case TargetState::kSucceeded:
      return to == TargetState::kRollingBack;
    case TargetState::kFailed:
      return to == TargetState::kRollingBack || to == TargetState::kExhausted;
    case TargetState::kCancelled:
      return to == TargetState::kRollingBack || to == TargetState::kExhausted;
    case TargetState::kRollingBack:
      return to == TargetState::kRolledBack || to == TargetState::kRollbackFailed;
    case TargetState::kRolledBack:
      return false;
    case TargetState::kRollbackFailed:
      return false;
    case TargetState::kExhausted:
      return false;
  }
  return false;
}

}  // namespace rollout_fabric
