// Rollout Fabric - stage policy, gates and blast-radius controls.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Policy is data, not code. Every threshold that can change an orchestration
// outcome lives here, is validated once, is hashed into a policy digest, and is
// echoed verbatim in the explanation of every decision it governs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rollout_fabric/codec.hpp"
#include "rollout_fabric/digest.hpp"
#include "rollout_fabric/ids.hpp"
#include "rollout_fabric/limits.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/time.hpp"

namespace rollout_fabric {

// An exact non-negative rational. Floating point is deliberately absent from
// every threshold in this runtime: "two thirds healthy" must mean the same
// thing on every host and in every replay.
struct Ratio {
  std::uint32_t numerator = 0;
  std::uint32_t denominator = 1;

  [[nodiscard]] Status validate() const;
  [[nodiscard]] bool is_zero() const noexcept { return numerator == 0; }
  [[nodiscard]] bool is_one() const noexcept { return numerator == denominator; }

  // floor(numerator * value / denominator) and ceil(...). Validated ratios have
  // denominator >= numerator >= 0, so neither can exceed value and neither can
  // overflow for any 64-bit input.
  [[nodiscard]] std::uint64_t apply_floor(std::uint64_t value) const noexcept;
  [[nodiscard]] std::uint64_t apply_ceil(std::uint64_t value) const noexcept;

  // True when part/whole is at least this ratio.
  [[nodiscard]] bool satisfied_by(std::uint64_t part, std::uint64_t whole) const noexcept;

  [[nodiscard]] int compare(const Ratio& other) const noexcept;

  friend bool operator==(const Ratio&, const Ratio&) = default;
  [[nodiscard]] std::string to_string() const;
};

[[nodiscard]] Ratio ratio(std::uint32_t numerator, std::uint32_t denominator) noexcept;

// Gate semantics. Automatic gates are evaluated by the runtime and proceed on
// their own. Manual gates stop the rollout in a Gated state and require an
// explicit, single-use, generation- and revision-fenced approval.
enum class GateMode : std::uint8_t {
  kAutomatic = 0,
  kManual = 1,
};

[[nodiscard]] std::string_view to_string(GateMode mode) noexcept;
[[nodiscard]] std::optional<GateMode> parse_gate_mode(std::string_view text) noexcept;

enum class GateKind : std::uint8_t {
  kStageEntry = 0,   // may the stage start at all
  kEvidence = 1,     // soak plus verifier evidence
  kHealth = 2,       // live health of the changed and unchanged population
  kAdvance = 3,      // may the rollout leave this stage
  kRollback = 4,     // may compensation begin
};

[[nodiscard]] std::string_view to_string(GateKind kind) noexcept;

// What happens to work that is already dispatched when a pause is applied.
enum class InFlightPolicy : std::uint8_t {
  // Attempts already dispatched keep running and their evidence is still
  // accepted; no new dispatch happens. This is the default.
  kLetFinish = 0,
  // Attempts already dispatched are told to stop and their terminal evidence is
  // recorded but never counted as success.
  kCancel = 1,
};

[[nodiscard]] std::string_view to_string(InFlightPolicy policy) noexcept;
[[nodiscard]] std::optional<InFlightPolicy> parse_in_flight_policy(std::string_view text) noexcept;

struct HealthGatePolicy {
  bool enabled = true;
  // Maximum age of a health sample. A sample older than this cannot satisfy the
  // gate no matter what it says.
  Duration max_age = Duration::from_seconds(30);
  // Which attestor is allowed to satisfy this gate. A sample from any other
  // source is recorded but is not evidence.
  HealthSourceId required_source{};
  // Consecutive healthy samples required before the gate opens. One lucky probe
  // is not a gate.
  std::uint32_t min_consecutive_healthy = 2;
  // Fraction of the cohort that must currently be healthy.
  Ratio min_healthy_fraction = ratio(1, 1);
  // Failures inside the cohort that are tolerated before the gate closes.
  std::uint32_t failure_budget_targets = 0;
  Ratio failure_budget_fraction = ratio(0, 1);
};

struct EvidenceGatePolicy {
  GateMode mode = GateMode::kAutomatic;
  // Minimum time between the first completion in the stage and gate evaluation.
  Duration soak = Duration::from_seconds(0);
  // Maximum age of verifier evidence.
  Duration max_age = Duration::from_seconds(300);
  // Verifier that must have attested. A nil id means "any registered verifier".
  VerifierId required_verifier{};
  // Minimum number of passing verification samples, and the fraction of the
  // cohort that must have passed verification.
  std::uint32_t min_passing_samples = 1;
  Ratio min_passing_fraction = ratio(1, 1);
  // When true, verification must cover every member; a partial cohort can never
  // advance on a fraction alone.
  bool require_full_coverage = true;
};

// Limits on how much of a correlated population may be changing at once.
// Exceeding any of these is a hard refusal to dispatch, never a warning.
struct BlastRadiusPolicy {
  std::uint32_t max_targets_changing_global = 64;
  std::uint32_t max_targets_changing_per_stage = 64;
  std::uint32_t max_targets_changing_per_failure_domain = 1;
  std::uint32_t max_targets_changing_per_rack = 1;
  std::uint32_t max_targets_changing_per_pod = 1;
  std::uint32_t max_targets_changing_per_site = 2;
  // Total distinct failure domains allowed to have any target changing.
  std::uint32_t max_failure_domains_changing = 2;
};

struct StagePolicy {
  // Dispatch concurrency for this stage.
  std::uint32_t max_concurrency = 1;
  // Fraction of the cohort that must be healthy for the stage to continue
  // dispatching. Evaluated before every batch.
  Ratio min_healthy_fraction = ratio(1, 1);
  // Failure budget: absolute count and fraction of the cohort. A stage that
  // exceeds either is failed.
  std::uint32_t failure_budget_targets = 0;
  Ratio failure_budget_fraction = ratio(0, 1);
  // Size of the first batch. Zero means "no canary": the first dispatch uses the
  // full allowed concurrency.
  std::uint32_t canary_size = 1;
  // When true the canary batch must clear the health gate before the remainder
  // of the cohort is admitted, and the clear is recorded as its own gate.
  bool canary_requires_health_gate = true;

  HealthGatePolicy health_gate{};
  EvidenceGatePolicy evidence_gate{};

  // Gate that guards entry into this stage. Manual entry gates are how an
  // operator says "yes, proceed to the next wave".
  GateMode entry_gate = GateMode::kAutomatic;
  // Gate that guards leaving this stage for its successors.
  GateMode advance_gate = GateMode::kAutomatic;

  BlastRadiusPolicy blast_radius{};

  // Per-attempt deadline measured on the monotonic clock.
  Duration attempt_deadline = Duration::from_seconds(120);
  // Attempts allowed per target within this stage. On exhaustion the target is
  // marked permanently failed for the stage.
  std::uint32_t max_attempts_per_target = 3;
  // Evidence older than this cannot mark an attempt complete after a restart.
  Duration evidence_max_age = Duration::from_seconds(300);

  InFlightPolicy pause_in_flight = InFlightPolicy::kLetFinish;

  // Whether this stage's changes can be undone by the plan's compensation
  // actions. Rollback is only ever attempted where this says it is supported.
  bool rollback_supported = false;
  // Actions to run, in order, when compensation is triggered for a target.
  std::vector<ActionId> compensation_actions{};

  // When false a stage may start even if a predecessor neither completed nor
  // failed, which is only meaningful for stages with no successors depending on
  // that predecessor's outcome.
  bool require_predecessors_succeeded = true;

  // Relative weight used when a stage has no explicit spread constraint.
  std::uint32_t weight = 1;

  [[nodiscard]] Status validate(const RuntimeLimits& limits) const;
  [[nodiscard]] Digest256 policy_digest() const;

  [[nodiscard]] std::string describe() const;
};

void encode(ByteWriter& writer, const Ratio& value);
[[nodiscard]] Result<Ratio> decode_ratio(ByteReader& reader);
void encode(ByteWriter& writer, const StagePolicy& value);
[[nodiscard]] Result<StagePolicy> decode_stage_policy(ByteReader& reader, const RuntimeLimits& limits);

}  // namespace rollout_fabric
