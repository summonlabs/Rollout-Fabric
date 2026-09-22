// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "rollout_fabric/decision.hpp"

#include <string>

#include "decode_util.hpp"

namespace rollout_fabric {
namespace {

using detail::dc_id;
using detail::dc_string;
using detail::dc_u32;
using detail::dc_u8;
using detail::wr_id;

}  // namespace

std::string_view to_string(DecisionKind kind) noexcept {
  switch (kind) {
    case DecisionKind::kValidatePlan: return "validate_plan";
    case DecisionKind::kArmRollout: return "arm_rollout";
    case DecisionKind::kStartStage: return "start_stage";
    case DecisionKind::kDispatchBatch: return "dispatch_batch";
    case DecisionKind::kHoldStage: return "hold_stage";
    case DecisionKind::kOpenGate: return "open_gate";
    case DecisionKind::kRefuseGate: return "refuse_gate";
    case DecisionKind::kAdvanceStage: return "advance_stage";
    case DecisionKind::kCompleteRollout: return "complete_rollout";
    case DecisionKind::kPauseRollout: return "pause_rollout";
    case DecisionKind::kResumeRollout: return "resume_rollout";
    case DecisionKind::kAbortRollout: return "abort_rollout";
    case DecisionKind::kBeginRollback: return "begin_rollback";
    case DecisionKind::kFailStage: return "fail_stage";
    case DecisionKind::kFailRollout: return "fail_rollout";
    case DecisionKind::kRetireRollout: return "retire_rollout";
    case DecisionKind::kRetryTarget: return "retry_target";
    case DecisionKind::kCancelAttempt: return "cancel_attempt";
    case DecisionKind::kReconcile: return "reconcile";
    case DecisionKind::kRejectEvidence: return "reject_evidence";
    case DecisionKind::kAdmitAttempt: return "admit_attempt";
  }
  return "unknown";
}

Digest256 Decision::decision_digest() const {
  ByteWriter writer;
  writer.string("rollout-fabric/decision/v1");
  wr_id(writer, id);
  writer.u8(static_cast<std::uint8_t>(kind));
  wr_id(writer, rollout);
  wr_id(writer, generation);
  writer.u64(value_of(revision));
  writer.u64(value_of(controller_epoch));
  wr_id(writer, incarnation);
  wr_id(writer, authority);
  wr_id(writer, stage);
  wr_id(writer, target);
  wr_id(writer, attempt);
  writer.i64(decided_at.unix_nanos());
  writer.raw(std::span<const std::byte>(inputs_digest.data(), inputs_digest.size()));
  writer.raw(std::span<const std::byte>(policy_digest.data(), policy_digest.size()));
  writer.raw(std::span<const std::byte>(evidence_digest.data(), evidence_digest.size()));
  writer.string(selected);
  writer.u32(static_cast<std::uint32_t>(rejected.size()));
  for (const RejectedAlternative& alternative : rejected) {
    writer.string(alternative.action);
    writer.string(alternative.reason);
  }
  writer.string(rationale);
  return sha256(writer.span());
}

std::string Decision::render() const {
  std::string text;
  text.append("decision ").append(id.to_hex().substr(0, 12));
  text.append(" kind=").append(to_string(kind));
  text.append(" at=").append(decided_at.to_rfc3339());
  text.append("\n  rollout=").append(rollout.to_hex().substr(0, 12));
  text.append(" generation=").append(generation.to_hex().substr(0, 12));
  text.append(" revision=").append(std::to_string(value_of(revision)));
  text.append(" controller_epoch=").append(std::to_string(value_of(controller_epoch)));
  text.append(" incarnation=").append(incarnation.to_hex().substr(0, 12));
  if (!stage.is_nil()) {
    text.append("\n  stage=").append(stage.to_hex().substr(0, 12));
  }
  if (!target.is_nil()) {
    text.append(" target=").append(target.to_hex().substr(0, 12));
  }
  if (!attempt.is_nil()) {
    text.append(" attempt=").append(attempt.to_hex().substr(0, 12));
  }
  text.append("\n  inputs=").append(is_nil(inputs_digest) ? std::string("(none)")
                                                          : to_hex(inputs_digest).substr(0, 16));
  text.append(" policy=").append(is_nil(policy_digest) ? std::string("(none)")
                                                       : to_hex(policy_digest).substr(0, 16));
  text.append(" evidence=").append(is_nil(evidence_digest) ? std::string("(none)")
                                                           : to_hex(evidence_digest).substr(0, 16));
  text.append("\n  selected: ").append(selected);
  for (const RejectedAlternative& alternative : rejected) {
    text.append("\n  rejected: ").append(alternative.action).append(" - ").append(alternative.reason);
  }
  text.append("\n  rationale: ").append(rationale);
  text.append("\n");
  return text;
}

void encode(ByteWriter& writer, const Decision& value) {
  wr_id(writer, value.id);
  writer.u8(static_cast<std::uint8_t>(value.kind));
  wr_id(writer, value.rollout);
  wr_id(writer, value.generation);
  writer.u64(value_of(value.revision));
  writer.u64(value_of(value.controller_epoch));
  wr_id(writer, value.incarnation);
  wr_id(writer, value.authority);
  wr_id(writer, value.stage);
  wr_id(writer, value.target);
  wr_id(writer, value.attempt);
  writer.i64(value.decided_at.unix_nanos());
  detail::wr_digest(writer, value.inputs_digest);
  detail::wr_digest(writer, value.policy_digest);
  detail::wr_digest(writer, value.evidence_digest);
  writer.string(value.selected);
  writer.u32(static_cast<std::uint32_t>(value.rejected.size()));
  for (const RejectedAlternative& alternative : value.rejected) {
    writer.string(alternative.action);
    writer.string(alternative.reason);
  }
  writer.string(value.rationale);
}

Result<Decision> decode_decision(ByteReader& reader, const RuntimeLimits& limits) {
  Decision value;
  RF_TRY(dc_id(reader, "decision.id", value.id));
  std::uint8_t kind = 0;
  RF_TRY(dc_u8(reader, "decision.kind", kind));
  if (kind > static_cast<std::uint8_t>(DecisionKind::kAdmitAttempt)) {
    return make_status(StatusCode::kCorrupt, "unknown decision kind " + std::to_string(kind));
  }
  value.kind = static_cast<DecisionKind>(kind);
  RF_TRY(dc_id(reader, "decision.rollout", value.rollout));
  RF_TRY(dc_id(reader, "decision.generation", value.generation));
  std::uint64_t revision = 0;
  RF_TRY(detail::dc_u64(reader, "decision.revision", revision));
  value.revision = Revision{revision};
  std::uint64_t epoch = 0;
  RF_TRY(detail::dc_u64(reader, "decision.controller_epoch", epoch));
  value.controller_epoch = EpochCounter{epoch};
  RF_TRY(dc_id(reader, "decision.incarnation", value.incarnation));
  RF_TRY(dc_id(reader, "decision.authority", value.authority));
  RF_TRY(dc_id(reader, "decision.stage", value.stage));
  RF_TRY(dc_id(reader, "decision.target", value.target));
  RF_TRY(dc_id(reader, "decision.attempt", value.attempt));
  RF_TRY(detail::dc_timestamp(reader, "decision.decided_at", value.decided_at));
  RF_TRY(detail::dc_digest(reader, "decision.inputs_digest", value.inputs_digest));
  RF_TRY(detail::dc_digest(reader, "decision.policy_digest", value.policy_digest));
  RF_TRY(detail::dc_digest(reader, "decision.evidence_digest", value.evidence_digest));
  RF_TRY(dc_string(reader, "decision.selected", limits.max_rationale_bytes, value.selected));
  std::uint32_t rejected_count = 0;
  RF_TRY(dc_u32(reader, "decision.rejected", rejected_count));
  if (rejected_count > limits.max_rejected_alternatives) {
    return make_status(StatusCode::kLimitExceeded,
                       "decision rejected-alternative count exceeds max_rejected_alternatives");
  }
  value.rejected.reserve(rejected_count);
  for (std::uint32_t index = 0; index < rejected_count; ++index) {
    RejectedAlternative alternative;
    RF_TRY(dc_string(reader, "decision.rejected[].action", limits.max_rationale_bytes,
                     alternative.action));
    RF_TRY(dc_string(reader, "decision.rejected[].reason", limits.max_rationale_bytes,
                     alternative.reason));
    value.rejected.push_back(std::move(alternative));
  }
  RF_TRY(dc_string(reader, "decision.rationale", limits.max_rationale_bytes, value.rationale));
  return value;
}

DecisionBuilder::DecisionBuilder(DecisionKind kind, const RolloutId& rollout,
                                 const GenerationId& generation, Revision revision,
                                 EpochCounter controller_epoch, const IncarnationId& incarnation) {
  decision_.kind = kind;
  decision_.rollout = rollout;
  decision_.generation = generation;
  decision_.revision = revision;
  decision_.controller_epoch = controller_epoch;
  decision_.incarnation = incarnation;
  input_bytes_.string("rollout-fabric/decision-inputs/v1");
}

void DecisionBuilder::append_input(std::string_view name, std::string_view value) {
  input_bytes_.string(name);
  input_bytes_.string(value);
}

void DecisionBuilder::input(std::string_view name, std::string_view value) { append_input(name, value); }

void DecisionBuilder::input_digest(std::string_view name, const Digest256& digest) {
  input_bytes_.string(name);
  input_bytes_.raw(std::span<const std::byte>(digest.data(), digest.size()));
}

void DecisionBuilder::input_u64(std::string_view name, std::uint64_t value) {
  input_bytes_.string(name);
  input_bytes_.u64(value);
}

void DecisionBuilder::select(std::string selected) { decision_.selected = std::move(selected); }

void DecisionBuilder::reject(std::string action, std::string reason) {
  decision_.rejected.push_back(RejectedAlternative{std::move(action), std::move(reason)});
}

void DecisionBuilder::rationale(std::string text) { decision_.rationale = std::move(text); }

Decision DecisionBuilder::finish(IdFactory& ids, Timestamp decided_at,
                                 const RuntimeLimits& limits) {
  decision_.decided_at = decided_at;
  decision_.inputs_digest = sha256(input_bytes_.span());
  decision_.id = ids.next<DecisionId>(IdDomain::kDecision);
  if (decision_.rejected.size() > limits.max_rejected_alternatives) {
    decision_.rejected.resize(limits.max_rejected_alternatives);
  }
  for (RejectedAlternative& alternative : decision_.rejected) {
    if (alternative.action.size() > limits.max_rationale_bytes) {
      alternative.action.resize(limits.max_rationale_bytes);
    }
    if (alternative.reason.size() > limits.max_rationale_bytes) {
      alternative.reason.resize(limits.max_rationale_bytes);
    }
  }
  if (decision_.rationale.size() > limits.max_rationale_bytes) {
    decision_.rationale.resize(limits.max_rationale_bytes);
  }
  if (decision_.selected.size() > limits.max_rationale_bytes) {
    decision_.selected.resize(limits.max_rationale_bytes);
  }
  return decision_;
}

}  // namespace rollout_fabric
