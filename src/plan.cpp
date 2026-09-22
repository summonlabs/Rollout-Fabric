// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "rollout_fabric/plan.hpp"

#include <algorithm>
#include <string>

#include "decode_util.hpp"

namespace rollout_fabric {
namespace {

using detail::dc_bytes;
using detail::dc_digest;
using detail::dc_id;
using detail::dc_string;
using detail::dc_u32;
using detail::dc_u8;
using detail::wr_digest;
using detail::wr_id;

[[nodiscard]] Digest256 derive(const std::string_view domain, std::span<const std::byte> material) {
  ByteWriter writer;
  writer.string(domain);
  writer.raw(material);
  return sha256(writer.span());
}

void encode_evidence_rule(ByteWriter& writer, const TargetEvidenceRule& rule) {
  writer.u8(static_cast<std::uint8_t>(rule.success_authority));
  writer.u8(static_cast<std::uint8_t>(rule.failure_authority));
  writer.boolean(rule.require_verification);
  wr_id(writer, rule.verifier);
  detail::wr_duration(writer, rule.health_settle);
  writer.boolean(rule.require_health_after_success);
}

[[nodiscard]] Status decode_evidence_rule(ByteReader& reader, const RuntimeLimits& limits,
                                          TargetEvidenceRule& rule) {
  (void)limits;
  std::uint8_t success = 0;
  RF_TRY(dc_u8(reader, "evidence_rule.success_authority", success));
  if (success > static_cast<std::uint8_t>(EvidenceAuthority::kOperatorAttested)) {
    return make_status(StatusCode::kCorrupt, "unknown success authority in evidence rule");
  }
  rule.success_authority = static_cast<EvidenceAuthority>(success);
  std::uint8_t failure = 0;
  RF_TRY(dc_u8(reader, "evidence_rule.failure_authority", failure));
  if (failure > static_cast<std::uint8_t>(EvidenceAuthority::kOperatorAttested)) {
    return make_status(StatusCode::kCorrupt, "unknown failure authority in evidence rule");
  }
  rule.failure_authority = static_cast<EvidenceAuthority>(failure);
  RF_TRY(detail::dc_bool(reader, "evidence_rule.require_verification", rule.require_verification));
  RF_TRY(dc_id(reader, "evidence_rule.verifier", rule.verifier));
  RF_TRY(detail::dc_duration(reader, "evidence_rule.health_settle", rule.health_settle));
  RF_TRY(detail::dc_bool(reader, "evidence_rule.require_health_after_success",
                         rule.require_health_after_success));
  return Status::success();
}

[[nodiscard]] const ActionSpec* find_action_in(const StageSpec& stage, const ActionId& id) noexcept {
  for (const ActionSpec& action : stage.actions) {
    if (action.id == id) {
      return &action;
    }
  }
  return nullptr;
}

[[nodiscard]] Status validate_evidence_rule(const TargetEvidenceRule& rule) {
  if (rule.success_authority == EvidenceAuthority::kWorkerSelfReported) {
    return make_status(StatusCode::kPolicyViolation,
                       "a target may not be declared successful purely on a worker's own report; "
                       "name an attesting authority");
  }
  if (rule.health_settle.is_negative()) {
    return make_status(StatusCode::kInvalidArgument, "evidence_rule.health_settle must not be negative");
  }
  if (rule.require_health_after_success && rule.health_settle.is_zero()) {
    return make_status(StatusCode::kInvalidArgument,
                       "require_health_after_success needs a non-zero health_settle so that the "
                       "observation has a defined duration");
  }
  return Status::success();
}

}  // namespace

std::string_view to_string(ActionKind kind) noexcept {
  switch (kind) {
    case ActionKind::kApplyConfiguration: return "apply_configuration";
    case ActionKind::kRestart: return "restart";
    case ActionKind::kDrain: return "drain";
    case ActionKind::kUpgrade: return "upgrade";
    case ActionKind::kVerify: return "verify";
    case ActionKind::kRollback: return "rollback";
    case ActionKind::kCompensate: return "compensate";
    case ActionKind::kCustom: return "custom";
  }
  return "unknown";
}

std::optional<ActionKind> parse_action_kind(std::string_view text) noexcept {
  if (text == "apply_configuration" || text == "apply-configuration") return ActionKind::kApplyConfiguration;
  if (text == "restart") return ActionKind::kRestart;
  if (text == "drain") return ActionKind::kDrain;
  if (text == "upgrade") return ActionKind::kUpgrade;
  if (text == "verify") return ActionKind::kVerify;
  if (text == "rollback") return ActionKind::kRollback;
  if (text == "compensate") return ActionKind::kCompensate;
  if (text == "custom") return ActionKind::kCustom;
  return std::nullopt;
}

std::string_view to_string(RollbackSupport support) noexcept {
  switch (support) {
    case RollbackSupport::kUnsupported: return "unsupported";
    case RollbackSupport::kSupported: return "supported";
    case RollbackSupport::kBestEffort: return "best_effort";
  }
  return "unknown";
}

std::optional<RollbackSupport> parse_rollback_support(std::string_view text) noexcept {
  if (text == "unsupported" || text == "none") return RollbackSupport::kUnsupported;
  if (text == "supported") return RollbackSupport::kSupported;
  if (text == "best_effort" || text == "best-effort") return RollbackSupport::kBestEffort;
  return std::nullopt;
}

PlanId derive_plan_id(const Digest256& source_digest) noexcept {
  const Digest256 digest = derive("rollout-fabric/plan/v1",
                                  std::span<const std::byte>(source_digest.data(), source_digest.size()));
  return PlanId::from_bytes(std::span<const std::byte, PlanId::byte_size>(digest.data(),
                                                                          PlanId::byte_size));
}

StageId derive_stage_id(const PlanId& plan, std::string_view stage_name) noexcept {
  ByteWriter writer;
  writer.raw(plan.span());
  writer.string(stage_name);
  const Digest256 digest = derive("rollout-fabric/stage/v1", writer.span());
  return StageId::from_bytes(std::span<const std::byte, StageId::byte_size>(digest.data(),
                                                                           StageId::byte_size));
}

ActionId derive_action_id(const StageId& stage, std::string_view action_name) noexcept {
  ByteWriter writer;
  writer.raw(stage.span());
  writer.string(action_name);
  const Digest256 digest = derive("rollout-fabric/action/v1", writer.span());
  return ActionId::from_bytes(std::span<const std::byte, ActionId::byte_size>(digest.data(),
                                                                            ActionId::byte_size));
}

Status ActionSpec::validate(const RuntimeLimits& limits) const {
  if (id.is_nil()) {
    return make_status(StatusCode::kInvalidArgument, "action identifier is nil");
  }
  if (name.empty()) {
    return make_status(StatusCode::kInvalidArgument, "action name must not be empty");
  }
  if (name.size() > limits.max_name_bytes) {
    return make_status(StatusCode::kLimitExceeded, "action name exceeds max_name_bytes");
  }
  if (!is_valid_utf8(name)) {
    return make_status(StatusCode::kInvalidArgument, "action name is not valid UTF-8");
  }
  if (parameters.size() > limits.max_message_bytes) {
    return make_status(StatusCode::kLimitExceeded,
                       "action '" + name + "' carries " + std::to_string(parameters.size()) +
                           " parameter bytes, above max_message_bytes");
  }
  if (is_nil(artifact_digest)) {
    return make_status(StatusCode::kInvalidArgument,
                       "action '" + name + "' has no artifact digest; the runtime only executes "
                                            "content it can pin");
  }
  return Status::success();
}

Digest256 ActionSpec::action_digest() const {
  ByteWriter writer;
  writer.string("rollout-fabric/action/v1");
  wr_id(writer, id);
  writer.string(name);
  writer.u8(static_cast<std::uint8_t>(kind));
  wr_digest(writer, artifact_digest);
  wr_id(writer, artifact);
  writer.u8(static_cast<std::uint8_t>(rollback));
  writer.bytes(parameters);
  return sha256(writer.span());
}

Status StageSpec::validate(const RuntimeLimits& limits) const {
  if (id.is_nil()) {
    return make_status(StatusCode::kInvalidArgument, "stage identifier is nil");
  }
  if (name.empty()) {
    return make_status(StatusCode::kInvalidArgument, "stage name must not be empty");
  }
  if (name.size() > limits.max_name_bytes) {
    return make_status(StatusCode::kLimitExceeded, "stage name exceeds max_name_bytes");
  }
  if (actions.size() > limits.max_actions_per_stage) {
    return make_status(StatusCode::kLimitExceeded,
                       "stage '" + name + "' declares " + std::to_string(actions.size()) +
                           " actions, above max_actions_per_stage");
  }
  Status status = selector.validate(limits);
  if (!status.ok()) {
    return make_status(status.code(), "stage '" + name + "' selector: " + status.message());
  }
  status = policy.validate(limits);
  if (!status.ok()) {
    return make_status(status.code(), "stage '" + name + "' policy: " + status.message());
  }
  status = validate_evidence_rule(evidence_rule);
  if (!status.ok()) {
    return make_status(status.code(), "stage '" + name + "' evidence rule: " + status.message());
  }
  for (const ActionSpec& action : actions) {
    status = action.validate(limits);
    if (!status.ok()) {
      return make_status(status.code(), "stage '" + name + "': " + status.message());
    }
  }
  for (const ActionId& compensation : policy.compensation_actions) {
    if (find_action_in(*this, compensation) == nullptr) {
      return make_status(StatusCode::kInvalidArgument,
                         "stage '" + name + "' names a compensation action that it does not declare");
    }
  }
  return Status::success();
}

Digest256 StageSpec::stage_digest() const {
  ByteWriter writer;
  writer.string("rollout-fabric/stage/v1");
  wr_id(writer, id);
  writer.string(name);
  writer.u32(ordinal);
  std::vector<Digest256> predecessor_bytes;
  predecessor_bytes.reserve(predecessors.size());
  for (const StageId& predecessor : predecessors) {
    Digest256 entry{};
    std::copy(predecessor.bytes().begin(), predecessor.bytes().end(), entry.begin());
    predecessor_bytes.push_back(entry);
  }
  std::sort(predecessor_bytes.begin(), predecessor_bytes.end());
  writer.u32(static_cast<std::uint32_t>(predecessor_bytes.size()));
  for (const Digest256& entry : predecessor_bytes) {
    writer.raw(std::span<const std::byte>(entry.data(), entry.size()));
  }
  wr_digest(writer, selector.selector_digest());
  wr_digest(writer, policy.policy_digest());
  encode_evidence_rule(writer, evidence_rule);
  writer.u32(static_cast<std::uint32_t>(actions.size()));
  for (const ActionSpec& action : actions) {
    wr_digest(writer, action.action_digest());
  }
  return sha256(writer.span());
}

const StageSpec* ChangePlan::find_stage(const StageId& stage_id) const noexcept {
  for (const StageSpec& stage : stages) {
    if (stage.id == stage_id) {
      return &stage;
    }
  }
  return nullptr;
}

const ActionSpec* ChangePlan::find_action(const StageId& stage_id, const ActionId& action_id) const noexcept {
  const StageSpec* spec = find_stage(stage_id);
  if (spec == nullptr) {
    return nullptr;
  }
  return find_action_in(*spec, action_id);
}

Status ChangePlan::validate(const RuntimeLimits& limits) {
  order.clear();
  if (id.is_nil()) {
    return make_status(StatusCode::kInvalidArgument, "plan identifier is nil");
  }
  if (stages.empty()) {
    return make_status(StatusCode::kInvalidArgument, "a plan must declare at least one stage");
  }
  if (stages.size() > limits.max_stages_per_plan) {
    return make_status(StatusCode::kLimitExceeded,
                       "plan declares " + std::to_string(stages.size()) +
                           " stages, above max_stages_per_plan");
  }
  if (change_id.empty()) {
    return make_status(StatusCode::kInvalidArgument, "plan must name the upstream change identifier");
  }
  if (is_nil(source_digest)) {
    return make_status(StatusCode::kInvalidArgument,
                       "plan must carry the digest of the approved upstream document");
  }

  for (const StageSpec& stage : stages) {
    const Status status = stage.validate(limits);
    if (!status.ok()) {
      return status;
    }
  }

  // Identifier uniqueness. Without this a duplicated stage would make the
  // execution order ambiguous in a way the digest could not detect.
  for (std::size_t outer = 0; outer < stages.size(); ++outer) {
    for (std::size_t inner = outer + 1; inner < stages.size(); ++inner) {
      if (stages[outer].id == stages[inner].id) {
        return make_status(StatusCode::kAlreadyExists,
                           "two stages share the identifier " + stages[outer].id.to_hex() +
                               "; stage identities must be distinct");
      }
    }
  }

  const std::size_t count = stages.size();
  std::vector<std::vector<std::size_t>> successors(count);
  std::vector<std::uint32_t> indegree(count, 0);
  for (std::size_t index = 0; index < count; ++index) {
    for (const StageId& predecessor : stages[index].predecessors) {
      if (predecessor == stages[index].id) {
        return make_status(StatusCode::kInvalidArgument,
                           "stage '" + stages[index].name + "' lists itself as a predecessor");
      }
      std::size_t found = count;
      for (std::size_t probe = 0; probe < count; ++probe) {
        if (stages[probe].id == predecessor) {
          found = probe;
          break;
        }
      }
      if (found == count) {
        return make_status(StatusCode::kNotFound,
                           "stage '" + stages[index].name + "' names an unknown predecessor " +
                               predecessor.to_hex());
      }
      successors[found].push_back(index);
      ++indegree[index];
    }
  }

  // Deterministic topological order: among ready stages the one with the lowest
  // ordinal wins, ties broken by identifier bytes.
  std::vector<std::size_t> ready;
  for (std::size_t index = 0; index < count; ++index) {
    if (indegree[index] == 0) {
      ready.push_back(index);
    }
  }
  const auto order_less = [&](std::size_t lhs, std::size_t rhs) {
    if (stages[lhs].ordinal != stages[rhs].ordinal) {
      return stages[lhs].ordinal < stages[rhs].ordinal;
    }
    return stages[lhs].id < stages[rhs].id;
  };
  std::sort(ready.begin(), ready.end(), order_less);
  while (!ready.empty()) {
    const std::size_t current = ready.front();
    ready.erase(ready.begin());
    order.push_back(stages[current].id);
    for (const std::size_t successor : successors[current]) {
      --indegree[successor];
      if (indegree[successor] == 0) {
        ready.push_back(successor);
      }
    }
    std::sort(ready.begin(), ready.end(), order_less);
  }

  if (order.size() != count) {
    return make_status(StatusCode::kInvalidArgument,
                       "the stage dependency graph contains a cycle; no stage ordering exists that "
                       "satisfies the declared predecessors");
  }
  return Status::success();
}

Digest256 ChangePlan::plan_digest() const {
  ByteWriter writer;
  writer.string("rollout-fabric/change-plan/v1");
  wr_id(writer, id);
  wr_digest(writer, source_digest);
  writer.string(change_id);
  writer.string(approved_by);
  writer.i64(approved_at.unix_nanos());
  writer.u32(static_cast<std::uint32_t>(order.size()));
  for (const StageId& stage_id : order) {
    const StageSpec* spec = find_stage(stage_id);
    if (spec == nullptr) {
      writer.raw(std::span<const std::byte>(stage_id.bytes().data(), stage_id.bytes().size()));
      continue;
    }
    wr_digest(writer, spec->stage_digest());
  }
  return sha256(writer.span());
}

void encode(ByteWriter& writer, const ActionSpec& value) {
  wr_id(writer, value.id);
  writer.string(value.name);
  writer.u8(static_cast<std::uint8_t>(value.kind));
  wr_digest(writer, value.artifact_digest);
  wr_id(writer, value.artifact);
  writer.u8(static_cast<std::uint8_t>(value.rollback));
  writer.bytes(value.parameters);
}

Result<ActionSpec> decode_action(ByteReader& reader, const RuntimeLimits& limits) {
  ActionSpec value;
  RF_TRY(dc_id(reader, "action.id", value.id));
  RF_TRY(dc_string(reader, "action.name", limits.max_name_bytes, value.name));
  std::uint8_t kind = 0;
  RF_TRY(dc_u8(reader, "action.kind", kind));
  if (kind > static_cast<std::uint8_t>(ActionKind::kCustom)) {
    return make_status(StatusCode::kCorrupt, "unknown action kind " + std::to_string(kind));
  }
  value.kind = static_cast<ActionKind>(kind);
  RF_TRY(dc_digest(reader, "action.artifact_digest", value.artifact_digest));
  RF_TRY(dc_id(reader, "action.artifact", value.artifact));
  std::uint8_t rollback = 0;
  RF_TRY(dc_u8(reader, "action.rollback", rollback));
  if (rollback > static_cast<std::uint8_t>(RollbackSupport::kBestEffort)) {
    return make_status(StatusCode::kCorrupt, "unknown rollback support value");
  }
  value.rollback = static_cast<RollbackSupport>(rollback);
  RF_TRY(dc_bytes(reader, "action.parameters", limits.max_message_bytes, value.parameters));
  const Status status = value.validate(limits);
  if (!status.ok()) {
    return status;
  }
  return value;
}

void encode(ByteWriter& writer, const StageSpec& value) {
  wr_id(writer, value.id);
  writer.string(value.name);
  writer.u32(value.ordinal);
  writer.u32(static_cast<std::uint32_t>(value.predecessors.size()));
  for (const StageId& predecessor : value.predecessors) {
    wr_id(writer, predecessor);
  }
  encode(writer, value.selector);
  encode(writer, value.policy);
  encode_evidence_rule(writer, value.evidence_rule);
  writer.u32(static_cast<std::uint32_t>(value.actions.size()));
  for (const ActionSpec& action : value.actions) {
    encode(writer, action);
  }
}

Result<StageSpec> decode_stage(ByteReader& reader, const RuntimeLimits& limits) {
  StageSpec value;
  RF_TRY(dc_id(reader, "stage.id", value.id));
  RF_TRY(dc_string(reader, "stage.name", limits.max_name_bytes, value.name));
  RF_TRY(dc_u32(reader, "stage.ordinal", value.ordinal));
  RF_TRY(detail::dc_id_vector(reader, "stage.predecessors", limits.max_stages_per_plan,
                              value.predecessors));
  auto selector = decode_selector_term(reader, limits);
  if (!selector.ok()) {
    return selector.status();
  }
  value.selector = std::move(selector).value();
  auto policy = decode_stage_policy(reader, limits);
  if (!policy.ok()) {
    return policy.status();
  }
  value.policy = std::move(policy).value();
  RF_TRY(decode_evidence_rule(reader, limits, value.evidence_rule));
  std::uint32_t action_count = 0;
  RF_TRY(dc_u32(reader, "stage.actions", action_count));
  if (action_count > limits.max_actions_per_stage) {
    return make_status(StatusCode::kLimitExceeded, "stage action count exceeds max_actions_per_stage");
  }
  value.actions.reserve(action_count);
  for (std::uint32_t index = 0; index < action_count; ++index) {
    auto action = decode_action(reader, limits);
    if (!action.ok()) {
      return action.status();
    }
    value.actions.push_back(std::move(action).value());
  }
  const Status status = value.validate(limits);
  if (!status.ok()) {
    return status;
  }
  return value;
}

void encode(ByteWriter& writer, const ChangePlan& value) {
  wr_id(writer, value.id);
  wr_digest(writer, value.source_digest);
  writer.string(value.change_id);
  writer.string(value.approved_by);
  writer.i64(value.approved_at.unix_nanos());
  writer.string(value.summary);
  writer.u32(static_cast<std::uint32_t>(value.stages.size()));
  for (const StageSpec& stage : value.stages) {
    encode(writer, stage);
  }
  writer.u32(static_cast<std::uint32_t>(value.order.size()));
  for (const StageId& stage_id : value.order) {
    wr_id(writer, stage_id);
  }
}

Result<ChangePlan> decode_plan(ByteReader& reader, const RuntimeLimits& limits) {
  ChangePlan value;
  RF_TRY(dc_id(reader, "plan.id", value.id));
  RF_TRY(dc_digest(reader, "plan.source_digest", value.source_digest));
  RF_TRY(dc_string(reader, "plan.change_id", limits.max_name_bytes, value.change_id));
  RF_TRY(dc_string(reader, "plan.approved_by", limits.max_name_bytes, value.approved_by));
  RF_TRY(detail::dc_timestamp(reader, "plan.approved_at", value.approved_at));
  RF_TRY(dc_string(reader, "plan.summary", limits.max_description_bytes, value.summary));
  std::uint32_t stage_count = 0;
  RF_TRY(dc_u32(reader, "plan.stages", stage_count));
  if (stage_count > limits.max_stages_per_plan) {
    return make_status(StatusCode::kLimitExceeded, "plan stage count exceeds max_stages_per_plan");
  }
  value.stages.reserve(stage_count);
  for (std::uint32_t index = 0; index < stage_count; ++index) {
    auto stage = decode_stage(reader, limits);
    if (!stage.ok()) {
      return stage.status();
    }
    value.stages.push_back(std::move(stage).value());
  }
  std::vector<StageId> stored_order;
  RF_TRY(detail::dc_id_vector(reader, "plan.order", limits.max_stages_per_plan, stored_order));
  const Status status = value.validate(limits);
  if (!status.ok()) {
    return status;
  }
  if (stored_order.size() != value.order.size()) {
    return make_status(StatusCode::kCorrupt, "stored execution order does not match the plan");
  }
  for (std::size_t index = 0; index < stored_order.size(); ++index) {
    if (!(stored_order[index] == value.order[index])) {
      return make_status(StatusCode::kCorrupt,
                         "stored execution order is not the deterministic order this plan requires");
    }
  }
  return value;
}

}  // namespace rollout_fabric
