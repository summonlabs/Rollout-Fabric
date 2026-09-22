// Rollout Fabric - evidence, its classes, and gate evaluation.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The central rule of this runtime: dispatch is not completion. An attempt that
// a worker accepted has proved exactly one thing - that the worker accepted it.
// Only terminal execution evidence from the executor can complete an attempt,
// and only live, fresh, correctly-attested telemetry can satisfy a health gate.
//
// Evidence carries its own provenance: who attested it, when the underlying
// fact was observed, and whether it was read live or restored from the journal.
// Restored evidence is never silently promoted to fresh.
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
#include "rollout_fabric/policy.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/time.hpp"

namespace rollout_fabric {

struct Metric {
  std::string name;
  std::int64_t value_milli = 0;
  std::string unit;
};

struct EvidenceRecord {
  EvidenceId id{};
  RolloutId rollout{};
  GenerationId generation{};
  StageId stage{};
  CohortId cohort{};
  TargetId target{};
  AttemptId attempt{};
  AttemptEpoch attempt_epoch{};
  // Controller incarnation that dispatched the attempt. Evidence naming a
  // superseded incarnation for a live attempt is fenced.
  IncarnationId incarnation{};
  EvidenceKind kind = EvidenceKind::kExecutionCompleted;
  EvidenceOutcome outcome = EvidenceOutcome::kInconclusive;
  EvidenceAuthority authority = EvidenceAuthority::kWorkerSelfReported;
  EvidenceOrigin origin = EvidenceOrigin::kLive;
  // Per-attempt, strictly increasing. Duplicates are detected by (sequence,
  // kind) after a restart; out-of-order arrivals are held, not applied.
  Sequence sequence{};
  Timestamp observed_at{};
  Timestamp recorded_at{};
  // Optional explicit lifetime. Zero means "use the policy's max age".
  Duration ttl{};
  HealthSourceId health_source{};
  VerifierId verifier{};
  Digest256 payload_digest{};
  std::vector<Metric> metrics;

  [[nodiscard]] Digest256 record_digest() const;
  [[nodiscard]] Status validate(const RuntimeLimits& limits) const;
  [[nodiscard]] bool matches(const AttemptId& attempt_id, AttemptEpoch epoch) const noexcept {
    return attempt == attempt_id && attempt_epoch == epoch;
  }
};

void encode(ByteWriter& writer, const EvidenceRecord& value);
[[nodiscard]] Result<EvidenceRecord> decode_evidence_record(ByteReader& reader, const RuntimeLimits& limits);

// Decision of the evidence admission filter. Every rejection has a distinct
// disposition so tests and explanations can name the exact fence that fired.
enum class EvidenceAdmission : std::uint8_t {
  kAccepted = 0,
  kDuplicate = 1,
  kOutOfOrder = 2,
  kStaleAttempt = 3,
  kStaleGeneration = 4,
  kStaleIncarnation = 5,
  kUnknownAttempt = 6,
  kUntrustedAuthority = 7,
  kExpired = 8,
  kClockAnomaly = 9,
  kMalformed = 10,
  kBackpressure = 11,
};

[[nodiscard]] std::string_view to_string(EvidenceAdmission admission) noexcept;

struct AdmissionResult {
  EvidenceAdmission disposition = EvidenceAdmission::kAccepted;
  std::string reason;

  [[nodiscard]] bool accepted() const noexcept { return disposition == EvidenceAdmission::kAccepted; }
};

// A single gate evaluation. Deterministic given the same inputs.
struct GateEvaluation {
  GateId id{};
  GateKind kind = GateKind::kAdvance;
  StageId stage{};
  bool satisfied = false;
  std::string reason;
  std::vector<std::string> blocking;  // bounded, deterministic order
  std::uint32_t total_targets = 0;
  std::uint32_t healthy_targets = 0;
  std::uint32_t failed_targets = 0;
  std::uint32_t pending_targets = 0;
  std::uint32_t verified_targets = 0;
  Digest256 evidence_digest{};
  Timestamp evaluated_at{};
};

// Deterministic health evaluation over a cohort.
struct HealthSnapshot {
  HealthSourceId source{};
  Timestamp observed_at{};
  std::uint32_t healthy = 0;
  std::uint32_t unhealthy = 0;
  std::uint32_t unknown = 0;
  Digest256 digest{};
  bool live = false;
};

}  // namespace rollout_fabric
