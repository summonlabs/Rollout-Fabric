// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "rollout_fabric/rollout.hpp"

#include <algorithm>
#include <string>

#include "decode_util.hpp"

namespace rollout_fabric {
namespace {

using detail::dc_id;
using detail::dc_string;
using detail::dc_u32;
using detail::dc_u8;
using detail::wr_digest;
using detail::wr_id;

void encode_target_runtime(ByteWriter& writer, const TargetRuntime& value) {
  wr_id(writer, value.target);
  writer.u8(static_cast<std::uint8_t>(value.state));
  writer.u64(value_of(value.epoch));
  writer.u32(value.attempts_created);
  wr_id(writer, value.current_attempt);
  wr_id(writer, value.last_attempt);
  detail::wr_timestamp(writer, value.dispatched_at);
  detail::wr_timestamp(writer, value.last_change_at);
  detail::wr_timestamp(writer, value.completed_at);
  writer.boolean(value.healthy);
  writer.u32(value.consecutive_healthy);
  detail::wr_timestamp(writer, value.healthy_since);
  detail::wr_timestamp(writer, value.last_healthy_at);
  writer.boolean(value.last_health_origin_live);
  wr_id(writer, value.last_health_source);
  detail::wr_timestamp(writer, value.verification_observed_at);
  writer.boolean(value.verification_passed);
  wr_id(writer, value.verification_verifier);
  wr_id(writer, value.last_terminal_evidence);
  writer.u8(static_cast<std::uint8_t>(value.outcome));
  writer.u32(value.rollback_step);
  writer.string(value.last_reason);
}

[[nodiscard]] Status decode_target_runtime_body(ByteReader& reader, const RuntimeLimits& limits,
                                                TargetRuntime& value) {
  RF_TRY(dc_id(reader, "target_runtime.target", value.target));
  std::uint8_t state = 0;
  RF_TRY(dc_u8(reader, "target_runtime.state", state));
  if (state > static_cast<std::uint8_t>(TargetState::kUnknown)) {
    return make_status(StatusCode::kCorrupt, "unknown target state in rollout snapshot");
  }
  value.state = static_cast<TargetState>(state);
  std::uint64_t epoch = 0;
  RF_TRY(detail::dc_u64(reader, "target_runtime.epoch", epoch));
  value.epoch = AttemptEpoch{epoch};
  RF_TRY(dc_u32(reader, "target_runtime.attempts_created", value.attempts_created));
  RF_TRY(dc_id(reader, "target_runtime.current_attempt", value.current_attempt));
  RF_TRY(dc_id(reader, "target_runtime.last_attempt", value.last_attempt));
  RF_TRY(detail::dc_timestamp(reader, "target_runtime.dispatched_at", value.dispatched_at));
  RF_TRY(detail::dc_timestamp(reader, "target_runtime.last_change_at", value.last_change_at));
  RF_TRY(detail::dc_timestamp(reader, "target_runtime.completed_at", value.completed_at));
  RF_TRY(detail::dc_bool(reader, "target_runtime.healthy", value.healthy));
  RF_TRY(dc_u32(reader, "target_runtime.consecutive_healthy", value.consecutive_healthy));
  RF_TRY(detail::dc_timestamp(reader, "target_runtime.healthy_since", value.healthy_since));
  RF_TRY(detail::dc_timestamp(reader, "target_runtime.last_healthy_at", value.last_healthy_at));
  RF_TRY(detail::dc_bool(reader, "target_runtime.last_health_origin_live",
                         value.last_health_origin_live));
  RF_TRY(dc_id(reader, "target_runtime.last_health_source", value.last_health_source));
  RF_TRY(detail::dc_timestamp(reader, "target_runtime.verification_observed_at",
                              value.verification_observed_at));
  RF_TRY(detail::dc_bool(reader, "target_runtime.verification_passed", value.verification_passed));
  RF_TRY(dc_id(reader, "target_runtime.verification_verifier", value.verification_verifier));
  RF_TRY(dc_id(reader, "target_runtime.last_terminal_evidence", value.last_terminal_evidence));
  std::uint8_t outcome = 0;
  RF_TRY(dc_u8(reader, "target_runtime.outcome", outcome));
  if (outcome > static_cast<std::uint8_t>(EvidenceOutcome::kInconclusive)) {
    return make_status(StatusCode::kCorrupt, "unknown outcome in rollout snapshot");
  }
  value.outcome = static_cast<EvidenceOutcome>(outcome);
  RF_TRY(dc_u32(reader, "target_runtime.rollback_step", value.rollback_step));
  RF_TRY(dc_string(reader, "target_runtime.last_reason", limits.max_description_bytes,
                   value.last_reason));
  return Status::success();
}

void encode_stage_runtime(ByteWriter& writer, const StageRuntime& value) {
  wr_id(writer, value.id);
  writer.u8(static_cast<std::uint8_t>(value.state));
  writer.u32(value.ordinal);
  writer.u32(static_cast<std::uint32_t>(value.predecessors.size()));
  for (const StageId& predecessor : value.predecessors) {
    wr_id(writer, predecessor);
  }
  writer.u32(static_cast<std::uint32_t>(value.successors.size()));
  for (const StageId& successor : value.successors) {
    wr_id(writer, successor);
  }
  encode(writer, value.policy);
  // The cohort carries its own selector and member list; the policy digest is
  // recomputed on restore, so nothing here can drift silently.
  wr_id(writer, value.cohort.id);
  wr_id(writer, value.cohort.stage);
  wr_id(writer, value.cohort.generation);
  encode(writer, value.cohort.selector);
  wr_digest(writer, value.cohort.selector_digest);
  writer.u32(static_cast<std::uint32_t>(value.cohort.members.size()));
  for (const TargetId& member : value.cohort.members) {
    wr_id(writer, member);
  }
  wr_digest(writer, value.cohort.membership_digest);
  wr_digest(writer, value.cohort.inventory_digest);
  writer.u64(value_of(value.cohort.inventory_epoch));
  detail::wr_timestamp(writer, value.cohort.frozen_at);
  writer.boolean(value.cohort.frozen);

  writer.u32(static_cast<std::uint32_t>(value.actions.size()));
  for (const ActionSpec& action : value.actions) {
    encode(writer, action);
  }
  detail::wr_timestamp(writer, value.entered_at);
  detail::wr_timestamp(writer, value.started_at);
  detail::wr_timestamp(writer, value.soak_started_at);
  detail::wr_timestamp(writer, value.gated_at);
  detail::wr_timestamp(writer, value.completed_at);
  detail::wr_timestamp(writer, value.last_progress_at);
  writer.u32(value.dispatched_total);
  writer.u32(value.succeeded_total);
  writer.u32(value.failed_total);
  writer.u32(value.cancelled_total);
  writer.u32(value.rolled_back_total);
  writer.u32(value.rollback_failed_total);
  writer.u32(value.attempts_total);
  writer.boolean(value.canary_cleared);
  writer.u32(value.max_observed_changing);
  wr_id(writer, value.pending_gate);
  writer.u8(static_cast<std::uint8_t>(value.pending_gate_kind));
  detail::wr_timestamp(writer, value.pending_gate_since);

  writer.u32(static_cast<std::uint32_t>(value.targets.size()));
  for (const TargetRuntime& target : value.targets) {
    encode_target_runtime(writer, target);
  }
}

[[nodiscard]] Status decode_stage_runtime_body(ByteReader& reader, const RuntimeLimits& limits,
                                               StageRuntime& value) {
  RF_TRY(dc_id(reader, "stage_runtime.id", value.id));
  std::uint8_t state = 0;
  RF_TRY(dc_u8(reader, "stage_runtime.state", state));
  if (state > static_cast<std::uint8_t>(StageState::kSkipped)) {
    return make_status(StatusCode::kCorrupt, "unknown stage state in rollout snapshot");
  }
  value.state = static_cast<StageState>(state);
  RF_TRY(dc_u32(reader, "stage_runtime.ordinal", value.ordinal));
  RF_TRY(detail::dc_id_vector(reader, "stage_runtime.predecessors", limits.max_stages_per_plan,
                              value.predecessors));
  RF_TRY(detail::dc_id_vector(reader, "stage_runtime.successors", limits.max_stages_per_plan,
                              value.successors));
  auto policy = decode_stage_policy(reader, limits);
  if (!policy.ok()) {
    return policy.status();
  }
  value.policy = std::move(policy).value();

  RF_TRY(dc_id(reader, "cohort.id", value.cohort.id));
  RF_TRY(dc_id(reader, "cohort.stage", value.cohort.stage));
  RF_TRY(dc_id(reader, "cohort.generation", value.cohort.generation));
  auto selector = decode_selector_term(reader, limits);
  if (!selector.ok()) {
    return selector.status();
  }
  value.cohort.selector = std::move(selector).value();
  RF_TRY(detail::dc_digest(reader, "cohort.selector_digest", value.cohort.selector_digest));
  RF_TRY(detail::dc_id_vector(reader, "cohort.members", limits.max_targets_per_cohort,
                              value.cohort.members));
  RF_TRY(detail::dc_digest(reader, "cohort.membership_digest", value.cohort.membership_digest));
  RF_TRY(detail::dc_digest(reader, "cohort.inventory_digest", value.cohort.inventory_digest));
  std::uint64_t inventory_epoch = 0;
  RF_TRY(detail::dc_u64(reader, "cohort.inventory_epoch", inventory_epoch));
  value.cohort.inventory_epoch = EpochCounter{inventory_epoch};
  RF_TRY(detail::dc_timestamp(reader, "cohort.frozen_at", value.cohort.frozen_at));
  RF_TRY(detail::dc_bool(reader, "cohort.frozen", value.cohort.frozen));

  std::uint32_t action_count = 0;
  RF_TRY(dc_u32(reader, "stage_runtime.actions", action_count));
  if (action_count > limits.max_actions_per_stage) {
    return make_status(StatusCode::kLimitExceeded, "stage action count exceeds the bound");
  }
  value.actions.reserve(action_count);
  for (std::uint32_t index = 0; index < action_count; ++index) {
    auto action = decode_action(reader, limits);
    if (!action.ok()) {
      return action.status();
    }
    value.actions.push_back(std::move(action).value());
  }
  RF_TRY(detail::dc_timestamp(reader, "stage_runtime.entered_at", value.entered_at));
  RF_TRY(detail::dc_timestamp(reader, "stage_runtime.started_at", value.started_at));
  RF_TRY(detail::dc_timestamp(reader, "stage_runtime.soak_started_at", value.soak_started_at));
  RF_TRY(detail::dc_timestamp(reader, "stage_runtime.gated_at", value.gated_at));
  RF_TRY(detail::dc_timestamp(reader, "stage_runtime.completed_at", value.completed_at));
  RF_TRY(detail::dc_timestamp(reader, "stage_runtime.last_progress_at", value.last_progress_at));
  RF_TRY(dc_u32(reader, "stage_runtime.dispatched_total", value.dispatched_total));
  RF_TRY(dc_u32(reader, "stage_runtime.succeeded_total", value.succeeded_total));
  RF_TRY(dc_u32(reader, "stage_runtime.failed_total", value.failed_total));
  RF_TRY(dc_u32(reader, "stage_runtime.cancelled_total", value.cancelled_total));
  RF_TRY(dc_u32(reader, "stage_runtime.rolled_back_total", value.rolled_back_total));
  RF_TRY(dc_u32(reader, "stage_runtime.rollback_failed_total", value.rollback_failed_total));
  RF_TRY(dc_u32(reader, "stage_runtime.attempts_total", value.attempts_total));
  RF_TRY(detail::dc_bool(reader, "stage_runtime.canary_cleared", value.canary_cleared));
  RF_TRY(dc_u32(reader, "stage_runtime.max_observed_changing", value.max_observed_changing));
  RF_TRY(dc_id(reader, "stage_runtime.pending_gate", value.pending_gate));
  std::uint8_t gate_kind = 0;
  RF_TRY(dc_u8(reader, "stage_runtime.pending_gate_kind", gate_kind));
  if (gate_kind > static_cast<std::uint8_t>(GateKind::kRollback)) {
    return make_status(StatusCode::kCorrupt, "unknown gate kind in rollout snapshot");
  }
  value.pending_gate_kind = static_cast<GateKind>(gate_kind);
  RF_TRY(detail::dc_timestamp(reader, "stage_runtime.pending_gate_since", value.pending_gate_since));

  std::uint32_t target_count = 0;
  RF_TRY(dc_u32(reader, "stage_runtime.targets", target_count));
  if (target_count > limits.max_targets_per_cohort) {
    return make_status(StatusCode::kLimitExceeded,
                       "stage target count exceeds max_targets_per_cohort while decoding");
  }
  value.targets.reserve(target_count);
  for (std::uint32_t index = 0; index < target_count; ++index) {
    TargetRuntime target;
    const Status status = decode_target_runtime_body(reader, limits, target);
    if (!status.ok()) {
      return status;
    }
    value.targets.push_back(std::move(target));
  }
  return Status::success();
}

}  // namespace

bool TargetRuntime::counts_as_changing() const noexcept {
  return state == TargetState::kDispatched || state == TargetState::kUnknown ||
         state == TargetState::kRollingBack;
}

const TargetRuntime* StageRuntime::find_target(const TargetId& target) const noexcept {
  const auto found = std::lower_bound(targets.begin(), targets.end(), target,
                                      [](const TargetRuntime& entry, const TargetId& probe) {
                                        return entry.target < probe;
                                      });
  if (found == targets.end() || !(found->target == target)) {
    return nullptr;
  }
  return &*found;
}

TargetRuntime* StageRuntime::find_target(const TargetId& target) noexcept {
  const auto found = std::lower_bound(targets.begin(), targets.end(), target,
                                      [](const TargetRuntime& entry, const TargetId& probe) {
                                        return entry.target < probe;
                                      });
  if (found == targets.end() || !(found->target == target)) {
    return nullptr;
  }
  return &*found;
}

std::uint32_t StageRuntime::changing_count() const noexcept {
  std::uint32_t count = 0;
  for (const TargetRuntime& target : targets) {
    if (target.counts_as_changing()) {
      ++count;
    }
  }
  return count;
}

// Health is a separate axis from change state: a target that has not been changed
// yet is healthy if the health source says so. Counting only already-succeeded
// targets would make a healthy fraction of 1/1 unreachable during a percentage
// rollout, which is exactly the gate that is supposed to make the rollout safe.
std::uint32_t StageRuntime::healthy_count() const noexcept {
  std::uint32_t count = 0;
  for (const TargetRuntime& target : targets) {
    if (target.healthy) {
      ++count;
    }
  }
  return count;
}

std::uint32_t StageRuntime::failed_count() const noexcept {
  std::uint32_t count = 0;
  for (const TargetRuntime& target : targets) {
    if (target.state == TargetState::kFailed || target.state == TargetState::kExhausted ||
        target.state == TargetState::kRollbackFailed) {
      ++count;
    }
  }
  return count;
}

std::uint32_t StageRuntime::terminal_count() const noexcept {
  std::uint32_t count = 0;
  for (const TargetRuntime& target : targets) {
    if (is_terminal(target.state)) {
      ++count;
    }
  }
  return count;
}

std::uint32_t StageRuntime::verified_count() const noexcept {
  std::uint32_t count = 0;
  for (const TargetRuntime& target : targets) {
    if (!target.verification_observed_at.is_nil() && target.verification_passed) {
      ++count;
    }
  }
  return count;
}

std::uint32_t StageRuntime::pending_count() const noexcept {
  std::uint32_t count = 0;
  for (const TargetRuntime& target : targets) {
    if (target.state == TargetState::kPending) {
      ++count;
    }
  }
  return count;
}

StageRuntime* Rollout::find_stage(const StageId& stage_id) noexcept {
  for (StageRuntime& stage : stages) {
    if (stage.id == stage_id) {
      return &stage;
    }
  }
  return nullptr;
}

const StageRuntime* Rollout::find_stage(const StageId& stage_id) const noexcept {
  for (const StageRuntime& stage : stages) {
    if (stage.id == stage_id) {
      return &stage;
    }
  }
  return nullptr;
}

const StageRuntime* Rollout::current_stage() const noexcept {
  if (cursor >= stages.size()) {
    return nullptr;
  }
  return &stages[cursor];
}

StageRuntime* Rollout::current_stage() noexcept {
  if (cursor >= stages.size()) {
    return nullptr;
  }
  return &stages[cursor];
}

std::uint32_t Rollout::changing_count() const noexcept {
  std::uint32_t count = 0;
  for (const StageRuntime& stage : stages) {
    count += stage.changing_count();
  }
  return count;
}

std::uint32_t Rollout::changing_in_failure_domain(const FailureDomainId& domain,
                                                  const TargetInventory& inventory) const {
  std::uint32_t count = 0;
  for (const StageRuntime& stage : stages) {
    for (const TargetRuntime& target : stage.targets) {
      if (!target.counts_as_changing()) {
        continue;
      }
      const TargetDescriptor* descriptor = inventory.find(target.target);
      if (descriptor != nullptr && descriptor->placement.failure_domain == domain) {
        ++count;
      }
    }
  }
  return count;
}

bool Rollout::has_pending_manual_gate() const noexcept {
  for (const StageRuntime& stage : stages) {
    if (stage.state == StageState::kGated && !stage.pending_gate.is_nil()) {
      return true;
    }
  }
  return false;
}

void Rollout::record_decision(const Decision& decision, const RuntimeLimits& limits) {
  decisions.push_back(decision);
  if (decisions.size() > limits.max_decision_history) {
    const std::size_t excess = decisions.size() - limits.max_decision_history;
    decisions.erase(decisions.begin(), decisions.begin() + static_cast<std::ptrdiff_t>(excess));
  }
}

Status freeze_cohort(StageRuntime& stage, Timestamp now) {
  if (stage.cohort.frozen) {
    return make_status(StatusCode::kImmutable,
                       "cohort for stage " + stage.id.to_hex() +
                           " is already frozen; a new membership requires a new rollout generation");
  }
  if (stage.cohort.members.empty()) {
    return make_status(StatusCode::kPreconditionFailed,
                       "refusing to freeze an empty cohort; a stage with no members cannot be armed");
  }
  stage.cohort.frozen = true;
  stage.cohort.frozen_at = now;
  return Status::success();
}

void encode(ByteWriter& writer, const TargetRuntime& value) { encode_target_runtime(writer, value); }

Result<TargetRuntime> decode_target_runtime(ByteReader& reader, const RuntimeLimits& limits) {
  TargetRuntime value;
  const Status status = decode_target_runtime_body(reader, limits, value);
  if (!status.ok()) {
    return status;
  }
  return value;
}

void encode(ByteWriter& writer, const StageRuntime& value) { encode_stage_runtime(writer, value); }

Result<StageRuntime> decode_stage_runtime(ByteReader& reader, const RuntimeLimits& limits) {
  StageRuntime value;
  const Status status = decode_stage_runtime_body(reader, limits, value);
  if (!status.ok()) {
    return status;
  }
  return value;
}

void encode(ByteWriter& writer, const Rollout& value) {
  wr_id(writer, value.id);
  wr_id(writer, value.plan_id);
  wr_digest(writer, value.plan_digest);
  wr_digest(writer, value.source_digest);
  writer.string(value.change_id);
  writer.u32(value.schema_version);
  wr_id(writer, value.generation);
  writer.u64(value_of(value.generation_counter));
  writer.u64(value_of(value.revision));
  wr_id(writer, value.incarnation);
  writer.u64(value_of(value.controller_epoch));
  writer.u8(static_cast<std::uint8_t>(value.state));
  writer.u8(static_cast<std::uint8_t>(value.resume_state));
  writer.string(value.state_reason);
  detail::wr_timestamp(writer, value.created_at);
  detail::wr_timestamp(writer, value.updated_at);
  detail::wr_timestamp(writer, value.armed_at);
  detail::wr_timestamp(writer, value.completed_at);
  wr_id(writer, value.created_by);
  wr_id(writer, value.created_by_operator);
  writer.u32(static_cast<std::uint32_t>(value.stages.size()));
  for (const StageRuntime& stage : value.stages) {
    encode_stage_runtime(writer, stage);
  }
  writer.u32(value.cursor);
  writer.u32(value.outstanding_attempts);
  writer.u32(value.max_observed_changing_global);
  writer.u64(value.evidence_sequences);
  writer.u32(static_cast<std::uint32_t>(value.decisions.size()));
  for (const Decision& decision : value.decisions) {
    encode(writer, decision);
  }
}

Result<Rollout> decode_rollout(ByteReader& reader, const RuntimeLimits& limits) {
  Rollout value;
  RF_TRY(dc_id(reader, "rollout.id", value.id));
  RF_TRY(dc_id(reader, "rollout.plan_id", value.plan_id));
  RF_TRY(detail::dc_digest(reader, "rollout.plan_digest", value.plan_digest));
  RF_TRY(detail::dc_digest(reader, "rollout.source_digest", value.source_digest));
  RF_TRY(dc_string(reader, "rollout.change_id", limits.max_name_bytes, value.change_id));
  RF_TRY(dc_u32(reader, "rollout.schema_version", value.schema_version));
  if (value.schema_version != Rollout::kSchemaVersion) {
    return make_status(StatusCode::kCorrupt,
                       "rollout snapshot schema version " + std::to_string(value.schema_version) +
                           " is not the version this build reads");
  }
  RF_TRY(dc_id(reader, "rollout.generation", value.generation));
  std::uint64_t generation_counter = 0;
  RF_TRY(detail::dc_u64(reader, "rollout.generation_counter", generation_counter));
  value.generation_counter = GenerationCounter{generation_counter};
  std::uint64_t revision = 0;
  RF_TRY(detail::dc_u64(reader, "rollout.revision", revision));
  value.revision = Revision{revision};
  RF_TRY(dc_id(reader, "rollout.incarnation", value.incarnation));
  std::uint64_t controller_epoch = 0;
  RF_TRY(detail::dc_u64(reader, "rollout.controller_epoch", controller_epoch));
  value.controller_epoch = EpochCounter{controller_epoch};
  std::uint8_t state = 0;
  RF_TRY(dc_u8(reader, "rollout.state", state));
  if (state > static_cast<std::uint8_t>(RolloutState::kRetired)) {
    return make_status(StatusCode::kCorrupt, "unknown rollout state in snapshot");
  }
  value.state = static_cast<RolloutState>(state);
  std::uint8_t resume = 0;
  RF_TRY(dc_u8(reader, "rollout.resume_state", resume));
  if (resume > static_cast<std::uint8_t>(RolloutState::kRetired)) {
    return make_status(StatusCode::kCorrupt, "unknown resume state in snapshot");
  }
  value.resume_state = static_cast<RolloutState>(resume);
  RF_TRY(dc_string(reader, "rollout.state_reason", limits.max_description_bytes, value.state_reason));
  RF_TRY(detail::dc_timestamp(reader, "rollout.created_at", value.created_at));
  RF_TRY(detail::dc_timestamp(reader, "rollout.updated_at", value.updated_at));
  RF_TRY(detail::dc_timestamp(reader, "rollout.armed_at", value.armed_at));
  RF_TRY(detail::dc_timestamp(reader, "rollout.completed_at", value.completed_at));
  RF_TRY(dc_id(reader, "rollout.created_by", value.created_by));
  RF_TRY(dc_id(reader, "rollout.created_by_operator", value.created_by_operator));

  std::uint32_t stage_count = 0;
  RF_TRY(dc_u32(reader, "rollout.stages", stage_count));
  if (stage_count > limits.max_stages_per_plan) {
    return make_status(StatusCode::kLimitExceeded, "rollout stage count exceeds max_stages_per_plan");
  }
  value.stages.reserve(stage_count);
  for (std::uint32_t index = 0; index < stage_count; ++index) {
    StageRuntime stage;
    const Status status = decode_stage_runtime_body(reader, limits, stage);
    if (!status.ok()) {
      return status;
    }
    value.stages.push_back(std::move(stage));
  }
  RF_TRY(dc_u32(reader, "rollout.cursor", value.cursor));
  RF_TRY(dc_u32(reader, "rollout.outstanding_attempts", value.outstanding_attempts));
  RF_TRY(dc_u32(reader, "rollout.max_observed_changing_global", value.max_observed_changing_global));
  RF_TRY(detail::dc_u64(reader, "rollout.evidence_sequences", value.evidence_sequences));
  std::uint32_t decision_count = 0;
  RF_TRY(dc_u32(reader, "rollout.decisions", decision_count));
  if (decision_count > limits.max_decision_history) {
    return make_status(StatusCode::kLimitExceeded, "rollout decision history exceeds the bound");
  }
  value.decisions.reserve(decision_count);
  for (std::uint32_t index = 0; index < decision_count; ++index) {
    auto decision = decode_decision(reader, limits);
    if (!decision.ok()) {
      return decision.status();
    }
    value.decisions.push_back(std::move(decision).value());
  }
  if (value.cursor > value.stages.size()) {
    return make_status(StatusCode::kCorrupt, "rollout cursor is past the end of the stage list");
  }
  for (const StageRuntime& stage : value.stages) {
    for (std::size_t index = 1; index < stage.targets.size(); ++index) {
      if (!(stage.targets[index - 1].target < stage.targets[index].target)) {
        return make_status(StatusCode::kCorrupt,
                           "rollout snapshot target list is not in canonical identifier order");
      }
    }
    for (std::size_t index = 1; index < stage.cohort.members.size(); ++index) {
      if (!(stage.cohort.members[index - 1] < stage.cohort.members[index])) {
        return make_status(StatusCode::kCorrupt,
                           "rollout snapshot cohort is not in canonical identifier order");
      }
    }
  }
  return value;
}

Digest256 rollout_digest(const Rollout& value) {
  ByteWriter writer;
  writer.string("rollout-fabric/rollout-digest/v1");
  encode(writer, value);
  return sha256(writer.span());
}

}  // namespace rollout_fabric
