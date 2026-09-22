// Rollout Fabric - the approved Change Planner plan.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Rollout Fabric does not invent intent. A plan arrives already approved, with
// an upstream digest, from the Change Planner. This runtime validates it,
// orders it deterministically and executes it. The plan is the only place where
// "what change" and "can it be undone" are answered; the runtime answers "when"
// and "where", and refuses to guess the rest.
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
#include "rollout_fabric/selector.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/time.hpp"

namespace rollout_fabric {

enum class ActionKind : std::uint8_t {
  kApplyConfiguration = 0,
  kRestart = 1,
  kDrain = 2,
  kUpgrade = 3,
  kVerify = 4,
  kRollback = 5,
  kCompensate = 6,
  kCustom = 7,
};

[[nodiscard]] std::string_view to_string(ActionKind kind) noexcept;
[[nodiscard]] std::optional<ActionKind> parse_action_kind(std::string_view text) noexcept;

// What the upstream plan says about undoing this action. The runtime never
// attempts compensation that the plan does not declare, and never claims a
// rollback succeeded without evidence from the compensating action itself.
enum class RollbackSupport : std::uint8_t {
  kUnsupported = 0,
  kSupported = 1,
  kBestEffort = 2,
};

[[nodiscard]] std::string_view to_string(RollbackSupport support) noexcept;
[[nodiscard]] std::optional<RollbackSupport> parse_rollback_support(std::string_view text) noexcept;

struct ActionSpec {
  ActionId id{};
  std::string name;
  ActionKind kind = ActionKind::kApplyConfiguration;
  // Digest of the deployable artifact or action payload produced upstream. The
  // runtime treats it as opaque content identity: it never renders or rewrites
  // configuration, it only proves that the bytes a worker received are the
  // bytes the plan approved.
  Digest256 artifact_digest{};
  ArtifactId artifact{};
  RollbackSupport rollback = RollbackSupport::kUnsupported;
  // Opaque, bounded parameters carried verbatim to the worker.
  std::vector<std::byte> parameters;

  [[nodiscard]] Status validate(const RuntimeLimits& limits) const;
  [[nodiscard]] Digest256 action_digest() const;
};

// Exactly what counts as success or failure for one target of this stage.
// There is no implicit "the dispatch went out, so it worked" path: unless these
// rules are satisfied by admitted evidence, the target is pending.
struct TargetEvidenceRule {
  // Authority that may attest terminal success and terminal failure.
  EvidenceAuthority success_authority = EvidenceAuthority::kExecutionAdapterAttested;
  EvidenceAuthority failure_authority = EvidenceAuthority::kExecutionAdapterAttested;
  // Independent verification, when the change requires it.
  bool require_verification = false;
  VerifierId verifier{};
  // The target must additionally be observed healthy for this long after its
  // terminal success before it counts toward the stage's healthy population.
  Duration health_settle = Duration::from_seconds(0);
  bool require_health_after_success = false;
};

struct StageSpec {
  StageId id{};
  std::string name;
  // Author-declared ordering hint. Ties in the deterministic topological sort
  // are broken by this value, then by identifier.
  std::uint32_t ordinal = 0;
  std::vector<StageId> predecessors;
  SelectorTerm selector{};
  StagePolicy policy{};
  TargetEvidenceRule evidence_rule{};
  std::vector<ActionSpec> actions;

  [[nodiscard]] Status validate(const RuntimeLimits& limits) const;
  [[nodiscard]] Digest256 stage_digest() const;
};

// Deterministic identity derivation. Two processes reading the same plan
// document derive exactly the same identifiers, which is what lets the CLI
// name a stage that the daemon independently derived.
[[nodiscard]] PlanId derive_plan_id(const Digest256& source_digest) noexcept;
[[nodiscard]] StageId derive_stage_id(const PlanId& plan, std::string_view stage_name) noexcept;
[[nodiscard]] ActionId derive_action_id(const StageId& stage, std::string_view action_name) noexcept;

struct ChangePlan {
  PlanId id{};
  // Digest of the upstream approved plan document. Recorded so that an operator
  // can prove which approval this rollout is executing.
  Digest256 source_digest{};
  std::string change_id;
  std::string approved_by;
  Timestamp approved_at{};
  std::string summary;
  std::vector<StageSpec> stages;
  // Canonical execution order produced by validate(). Stage identifiers in the
  // order the runtime is allowed to consider them.
  std::vector<StageId> order;

  [[nodiscard]] const StageSpec* find_stage(const StageId& id) const noexcept;
  [[nodiscard]] const ActionSpec* find_action(const StageId& stage, const ActionId& action) const noexcept;

  // Validates structure, then computes the deterministic order. A plan that
  // fails validation never reaches a rollout.
  [[nodiscard]] Status validate(const RuntimeLimits& limits);
  [[nodiscard]] const std::vector<StageId>& execution_order() const noexcept { return order; }

  // Digest over everything that can change execution: source digest, stage
  // order, selectors, policies and action payloads.
  [[nodiscard]] Digest256 plan_digest() const;
};

void encode(ByteWriter& writer, const ActionSpec& value);
[[nodiscard]] Result<ActionSpec> decode_action(ByteReader& reader, const RuntimeLimits& limits);
void encode(ByteWriter& writer, const StageSpec& value);
[[nodiscard]] Result<StageSpec> decode_stage(ByteReader& reader, const RuntimeLimits& limits);
void encode(ByteWriter& writer, const ChangePlan& value);
[[nodiscard]] Result<ChangePlan> decode_plan(ByteReader& reader, const RuntimeLimits& limits);

}  // namespace rollout_fabric
