// Rollout Fabric - attempts, fencing tokens and idempotency.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// An attempt is the unit of dispatched work. It is fenced by four independent
// values, and a report that gets any of them wrong is rejected:
//
//   * the rollout generation, which changes only when a new cohort snapshot is
//     created;
//   * the attempt epoch, which increments every time a target is retried within
//     a stage;
//   * the controller incarnation that dispatched it;
//   * the action digest, which pins the exact approved bytes.
//
// The idempotency key is derived from the same inputs and is stable across a
// controller restart, which is what lets a restarted controller ask the
// executor "did you already do this?" instead of doing it again.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rollout_fabric/codec.hpp"
#include "rollout_fabric/digest.hpp"
#include "rollout_fabric/evidence_types.hpp"
#include "rollout_fabric/ids.hpp"
#include "rollout_fabric/limits.hpp"
#include "rollout_fabric/lifecycle.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/time.hpp"

namespace rollout_fabric {

// The complete fence an attempt report must present.
struct AttemptFence {
  RolloutId rollout{};
  GenerationId generation{};
  StageId stage{};
  AttemptId attempt{};
  AttemptEpoch epoch{};
  IncarnationId incarnation{};

  friend bool operator==(const AttemptFence&, const AttemptFence&) = default;
  [[nodiscard]] Digest256 fence_digest() const;
};

// Stable identity of the logical effect an attempt performs. Two attempts that
// share an idempotency key must have the same effect or none at all.
//
// The attempt epoch is deliberately NOT part of the key. Retrying a target
// after a timeout creates a new attempt with a new epoch, but it names the same
// logical effect, so an executor that already performed it must report the
// recorded outcome instead of performing it a second time. That is what makes
// "restart does not redispatch an effect that already happened" true across
// timeouts, controller restarts and reconnects alike.
[[nodiscard]] Digest256 derive_idempotency_key(const RolloutId& rollout,
                                               const GenerationId& generation,
                                               const StageId& stage,
                                               const TargetId& target,
                                               const ActionId& action) noexcept;

struct Attempt {
  AttemptId id{};
  RolloutId rollout{};
  GenerationId generation{};
  StageId stage{};
  CohortId cohort{};
  TargetId target{};
  ActionId action{};
  AttemptEpoch epoch{};
  std::uint32_t attempt_number = 1;
  IncarnationId incarnation{};
  Digest256 action_digest{};
  Digest256 artifact_digest{};
  Digest256 idempotency_key{};
  Timestamp created_at{};
  Timestamp dispatched_at{};
  Timestamp deadline{};
  Timestamp last_evidence_at{};
  TargetState state = TargetState::kPending;
  // Set once terminal execution evidence is admitted. Dispatch alone never sets
  // this, which is the invariant the whole runtime is built around.
  bool completed = false;
  EvidenceOutcome outcome = EvidenceOutcome::kInconclusive;
  AttemptEpoch cancelled_epoch{};  // non-zero when a cancel was requested
  Sequence highest_sequence{};
  std::uint32_t accepted_evidence = 0;
  std::uint32_t duplicate_evidence = 0;
  std::uint32_t out_of_order_evidence = 0;

  [[nodiscard]] AttemptFence fence() const noexcept {
    return AttemptFence{rollout, generation, stage, id, epoch, incarnation};
  }
  // Named finished() rather than is_terminal() so that it cannot collide with
  // the free predicate over TargetState.
  [[nodiscard]] bool finished() const noexcept { return rollout_fabric::is_terminal(state); }
  // True when the attempt was dispatched but no terminal evidence has arrived.
  [[nodiscard]] bool is_in_flight() const noexcept {
    return dispatched_at.unix_nanos() != 0 && !finished();
  }
};

void encode(ByteWriter& writer, const Attempt& value);
[[nodiscard]] Result<Attempt> decode_attempt(ByteReader& reader, const RuntimeLimits& limits);

}  // namespace rollout_fabric
