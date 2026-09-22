// Rollout Fabric - the rollout aggregate.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A rollout is the durable orchestration state: which stage is current, what
// every target in every stage has proved, which attempt is outstanding, how
// much of the correlated population is changing right now, and which decisions
// brought it here. It is a value: it is encoded, digested, journaled and
// restored whole, and it never holds a pointer into a live adapter.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rollout_fabric/attempt.hpp"
#include "rollout_fabric/codec.hpp"
#include "rollout_fabric/decision.hpp"
#include "rollout_fabric/digest.hpp"
#include "rollout_fabric/evidence.hpp"
#include "rollout_fabric/ids.hpp"
#include "rollout_fabric/limits.hpp"
#include "rollout_fabric/lifecycle.hpp"
#include "rollout_fabric/plan.hpp"
#include "rollout_fabric/policy.hpp"
#include "rollout_fabric/selector.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/time.hpp"

namespace rollout_fabric {

// Durable per-target orchestration state.
struct TargetRuntime {
  TargetId target{};
  TargetState state = TargetState::kPending;
  // Epoch of the most recent attempt for this target inside this stage. Every
  // new attempt increments it, which fences reports from superseded attempts.
  AttemptEpoch epoch{};
  std::uint32_t attempts_created = 0;
  AttemptId current_attempt{};
  AttemptId last_attempt{};
  Timestamp dispatched_at{};
  Timestamp last_change_at{};
  Timestamp completed_at{};
  bool healthy = false;
  std::uint32_t consecutive_healthy = 0;
  Timestamp healthy_since{};
  Timestamp last_healthy_at{};
  // Health is only usable by a gate when the observation that produced it was
  // live and named an attesting source. Both facts are part of the durable
  // record so that a restart cannot silently promote restored telemetry.
  bool last_health_origin_live = false;
  HealthSourceId last_health_source{};
  // Independent verification of the post-change state, when the stage requires
  // it. Absent verification is not the same as passing verification.
  Timestamp verification_observed_at{};
  bool verification_passed = false;
  VerifierId verification_verifier{};
  EvidenceId last_terminal_evidence{};
  EvidenceOutcome outcome = EvidenceOutcome::kInconclusive;
  // Position in the stage's compensation sequence while a rollback is running.
  std::uint32_t rollback_step = 0;
  std::string last_reason;

  [[nodiscard]] bool counts_as_changing() const noexcept;
};

struct StageRuntime {
  StageId id{};
  StageState state = StageState::kPending;
  std::uint32_t ordinal = 0;
  std::vector<StageId> predecessors;
  std::vector<StageId> successors;
  StagePolicy policy{};
  TargetEvidenceRule evidence_rule{};
  std::vector<ActionSpec> actions;
  CohortMembership cohort{};
  std::vector<TargetRuntime> targets;

  Timestamp entered_at{};
  Timestamp started_at{};
  Timestamp soak_started_at{};
  Timestamp gated_at{};
  Timestamp completed_at{};
  Timestamp last_progress_at{};

  std::uint32_t dispatched_total = 0;
  std::uint32_t succeeded_total = 0;
  std::uint32_t failed_total = 0;
  std::uint32_t cancelled_total = 0;
  std::uint32_t rolled_back_total = 0;
  std::uint32_t rollback_failed_total = 0;
  std::uint32_t attempts_total = 0;
  bool canary_cleared = false;

  // Blast-radius high-water mark, retained as evidence that the configured
  // limit was never exceeded rather than merely asserted.
  std::uint32_t max_observed_changing = 0;

  // Pending manual gate, if the stage is stopped at one.
  GateId pending_gate{};
  GateKind pending_gate_kind = GateKind::kStageEntry;
  Timestamp pending_gate_since{};

  GateEvaluation last_health_gate{};
  GateEvaluation last_evidence_gate{};
  GateEvaluation last_advance_gate{};

  [[nodiscard]] const TargetRuntime* find_target(const TargetId& target) const noexcept;
  [[nodiscard]] TargetRuntime* find_target(const TargetId& target) noexcept;
  [[nodiscard]] std::uint32_t changing_count() const noexcept;
  [[nodiscard]] std::uint32_t healthy_count() const noexcept;
  [[nodiscard]] std::uint32_t failed_count() const noexcept;
  [[nodiscard]] std::uint32_t terminal_count() const noexcept;
  [[nodiscard]] std::uint32_t verified_count() const noexcept;
  [[nodiscard]] std::uint32_t pending_count() const noexcept;
};

struct Rollout {
  static constexpr std::uint32_t kSchemaVersion = 1;

  RolloutId id{};
  PlanId plan_id{};
  Digest256 plan_digest{};
  Digest256 source_digest{};
  std::string change_id;
  std::uint32_t schema_version = kSchemaVersion;

  GenerationId generation{};
  GenerationCounter generation_counter{0};
  Revision revision{0};
  // The controller incarnation currently entitled to mutate this rollout, and
  // the epoch it claimed the journal at. Every mutation checks both.
  IncarnationId incarnation{};
  EpochCounter controller_epoch{0};

  RolloutState state = RolloutState::kCreated;
  // Where resume returns to. Pause is the only transition that rewrites it.
  RolloutState resume_state = RolloutState::kCreated;
  std::string state_reason;

  Timestamp created_at{};
  Timestamp updated_at{};
  Timestamp armed_at{};
  Timestamp completed_at{};
  AuthorityId created_by{};
  OperatorId created_by_operator{};

  std::vector<StageRuntime> stages;
  // Index into the plan's deterministic execution order of the stage currently
  // being worked on. A stage is only ever entered through this cursor.
  std::uint32_t cursor = 0;
  std::uint32_t outstanding_attempts = 0;
  std::uint32_t max_observed_changing_global = 0;
  std::uint64_t evidence_sequences = 0;
  std::vector<Decision> decisions;  // bounded ring, newest last

  [[nodiscard]] StageRuntime* find_stage(const StageId& id) noexcept;
  [[nodiscard]] const StageRuntime* find_stage(const StageId& id) const noexcept;
  [[nodiscard]] const StageRuntime* current_stage() const noexcept;
  [[nodiscard]] StageRuntime* current_stage() noexcept;

  [[nodiscard]] std::uint32_t changing_count() const noexcept;
  [[nodiscard]] std::uint32_t changing_in_failure_domain(const FailureDomainId& domain,
                                                         const TargetInventory& inventory) const;
  [[nodiscard]] bool has_pending_manual_gate() const noexcept;

  void record_decision(const Decision& decision, const RuntimeLimits& limits);
};

// The cohort of a stage is immutable from the moment the stage is armed. This
// is enforced here rather than by convention.
[[nodiscard]] Status freeze_cohort(StageRuntime& stage, Timestamp now);

void encode(ByteWriter& writer, const TargetRuntime& value);
[[nodiscard]] Result<TargetRuntime> decode_target_runtime(ByteReader& reader, const RuntimeLimits& limits);
void encode(ByteWriter& writer, const StageRuntime& value);
[[nodiscard]] Result<StageRuntime> decode_stage_runtime(ByteReader& reader, const RuntimeLimits& limits);
void encode(ByteWriter& writer, const Rollout& value);
[[nodiscard]] Result<Rollout> decode_rollout(ByteReader& reader, const RuntimeLimits& limits);

[[nodiscard]] Digest256 rollout_digest(const Rollout& value);

}  // namespace rollout_fabric
