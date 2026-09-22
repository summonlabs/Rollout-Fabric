// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Deterministic rendering. Two runs over the same value produce byte-identical
// output: nothing here iterates an unordered container, nothing reads the wall
// clock, and nothing depends on the process environment.
#include "rollout_fabric/report.hpp"

#include <string>

namespace rollout_fabric {
namespace {

[[nodiscard]] std::string short_hex(const Digest256& digest) { return to_hex(digest).substr(0, 16); }

[[nodiscard]] std::string short_hex(const RolloutId& id) { return id.to_hex().substr(0, 12); }

[[nodiscard]] std::string short_hex(const StageId& id) { return id.to_hex().substr(0, 12); }

[[nodiscard]] std::string short_hex(const TargetId& id) { return id.to_hex().substr(0, 12); }

[[nodiscard]] std::string short_hex(const CohortId& id) { return id.to_hex().substr(0, 12); }

[[nodiscard]] std::string short_hex(const GenerationId& id) { return id.to_hex().substr(0, 12); }

[[nodiscard]] std::string short_hex(const IncarnationId& id) { return id.to_hex().substr(0, 12); }
[[nodiscard]] std::string short_hex(const PlanId& id) { return id.to_hex().substr(0, 12); }

[[nodiscard]] std::string hex_or_none(const Digest256& digest) {
  return is_nil(digest) ? std::string("(none)") : to_hex(digest).substr(0, 16);
}

[[nodiscard]] std::string json_number(std::uint64_t value) { return std::to_string(value); }

}  // namespace

std::string render_target_brief(const TargetDescriptor& descriptor) {
  std::string text = descriptor.name.empty() ? short_hex(descriptor.id) : descriptor.name;
  text.append(" [").append(to_string(descriptor.kind)).append("]");
  text.append(" site=").append(descriptor.placement.site.empty() ? "-" : descriptor.placement.site);
  text.append(" pod=").append(descriptor.placement.pod.empty() ? "-" : descriptor.placement.pod);
  text.append(" rack=").append(descriptor.placement.rack.empty() ? "-" : descriptor.placement.rack);
  text.append(" domain=").append(descriptor.placement.failure_domain.to_hex().substr(0, 12));
  return text;
}

std::string render_gate_evaluation(const GateEvaluation& evaluation, std::string_view label) {
  std::string text;
  text.append("  ").append(label).append(": ").append(to_string(evaluation.kind));
  text.append(" satisfied=").append(evaluation.satisfied ? "yes" : "no");
  text.append(" healthy=").append(std::to_string(evaluation.healthy_targets));
  text.append(" failed=").append(std::to_string(evaluation.failed_targets));
  text.append(" pending=").append(std::to_string(evaluation.pending_targets));
  text.append(" verified=").append(std::to_string(evaluation.verified_targets));
  text.append(" of=").append(std::to_string(evaluation.total_targets));
  text.append("\n    evidence=").append(hex_or_none(evaluation.evidence_digest));
  text.append("\n    reason: ").append(evaluation.reason).append("\n");
  std::size_t shown = 0;
  for (const std::string& blocking : evaluation.blocking) {
    if (shown >= 16) {
      text.append("    ... ").append(std::to_string(evaluation.blocking.size() - shown))
          .append(" more\n");
      break;
    }
    text.append("    blocking: ").append(blocking).append("\n");
    ++shown;
  }
  return text;
}

std::string render_status(const Rollout& rollout, const TargetInventory& inventory,
                          const RuntimeLimits& limits) {
  (void)limits;
  std::string text;
  text.append("rollout ").append(short_hex(rollout.id));
  text.append("  change=").append(rollout.change_id.empty() ? "(none)" : rollout.change_id);
  text.append("\n  state=").append(to_string(rollout.state));
  text.append("  reason=").append(rollout.state_reason);
  text.append("\n  generation=").append(short_hex(rollout.generation));
  text.append(" (").append(std::to_string(value_of(rollout.generation_counter))).append(")");
  text.append("  revision=").append(std::to_string(value_of(rollout.revision)));
  text.append("  incarnation=").append(short_hex(rollout.incarnation));
  text.append("  controller_epoch=").append(std::to_string(value_of(rollout.controller_epoch)));
  text.append("\n  plan=").append(short_hex(rollout.plan_id));
  text.append("  plan_digest=").append(short_hex(rollout.plan_digest));
  text.append("  source_digest=").append(short_hex(rollout.source_digest));
  text.append("\n  cursor=").append(std::to_string(rollout.cursor)).append("/")
      .append(std::to_string(rollout.stages.size()));
  text.append("  changing=").append(std::to_string(rollout.changing_count()));
  text.append("  outstanding_attempts=").append(std::to_string(rollout.outstanding_attempts));
  text.append("  max_observed_changing=")
      .append(std::to_string(rollout.max_observed_changing_global));
  text.append("\n  created=").append(rollout.created_at.to_rfc3339());
  if (!rollout.armed_at.is_nil()) {
    text.append("  armed=").append(rollout.armed_at.to_rfc3339());
  }
  if (!rollout.completed_at.is_nil()) {
    text.append("  completed=").append(rollout.completed_at.to_rfc3339());
  }
  text.append("\n  digest=").append(short_hex(rollout_digest(rollout)));
  text.append("\n  stages:\n");

  for (std::size_t index = 0; index < rollout.stages.size(); ++index) {
    const StageRuntime& stage = rollout.stages[index];
    text.append("    [").append(std::to_string(index)).append("] ")
        .append(short_hex(stage.id))
        .append("  state=").append(to_string(stage.state));
    text.append("  ordinal=").append(std::to_string(stage.ordinal));
    text.append("  members=").append(std::to_string(stage.cohort.size()));
    text.append("  frozen=").append(stage.cohort.frozen ? "yes" : "no");
    text.append("\n        dispatched=").append(std::to_string(stage.dispatched_total));
    text.append(" succeeded=").append(std::to_string(stage.succeeded_total));
    text.append(" failed=").append(std::to_string(stage.failed_total));
    text.append(" cancelled=").append(std::to_string(stage.cancelled_total));
    text.append(" rolled_back=").append(std::to_string(stage.rolled_back_total));
    text.append(" rollback_failed=").append(std::to_string(stage.rollback_failed_total));
    text.append(" changing=").append(std::to_string(stage.changing_count()));
    text.append(" max_changing=").append(std::to_string(stage.max_observed_changing));
    if (!stage.pending_gate.is_nil()) {
      // The full identifier, so that an operator can approve exactly this gate.
      text.append("\n        pending_gate=").append(stage.pending_gate.to_hex());
      text.append(" kind=").append(to_string(stage.pending_gate_kind));
      text.append(" since=").append(stage.pending_gate_since.to_rfc3339());
    }
    text.append("\n        policy: ").append(stage.policy.describe()).append("\n");
  }

  if (!rollout.decisions.empty()) {
    const Decision& latest = rollout.decisions.back();
    text.append("  latest_decision=").append(to_string(latest.kind));
    text.append(" at=").append(latest.decided_at.to_rfc3339());
    text.append("\n    selected: ").append(latest.selected).append("\n");
  }
  (void)inventory;
  return text;
}

std::string render_stage(const StageRuntime& stage, const TargetInventory& inventory) {
  std::string text;
  text.append("stage ").append(short_hex(stage.id));
  text.append("  state=").append(to_string(stage.state));
  text.append("  ordinal=").append(std::to_string(stage.ordinal));
  text.append("  members=").append(std::to_string(stage.cohort.size()));
  text.append("  frozen=").append(stage.cohort.frozen ? "yes" : "no");
  text.append("\n  dispatched=").append(std::to_string(stage.dispatched_total));
  text.append(" succeeded=").append(std::to_string(stage.succeeded_total));
  text.append(" failed=").append(std::to_string(stage.failed_total));
  text.append(" attempts=").append(std::to_string(stage.attempts_total));
  text.append(" changing=").append(std::to_string(stage.changing_count()));
  text.append(" max_observed_changing=").append(std::to_string(stage.max_observed_changing));
  text.append("\n  policy: ").append(stage.policy.describe());
  text.append("\n  membership_digest=").append(short_hex(stage.cohort.membership_digest));
  text.append("  inventory_digest=").append(short_hex(stage.cohort.inventory_digest)).append("\n");

  text.append(render_gate_evaluation(stage.last_health_gate, "health"));
  text.append(render_gate_evaluation(stage.last_evidence_gate, "evidence"));
  text.append(render_gate_evaluation(stage.last_advance_gate, "advance"));

  text.append("  targets:\n");
  for (const TargetRuntime& target : stage.targets) {
    const TargetDescriptor* descriptor = inventory.find(target.target);
    text.append("    ").append(short_hex(target.target));
    if (descriptor != nullptr) {
      text.append("  ").append(descriptor->name);
      text.append("  domain=")
          .append(descriptor->placement.failure_domain.to_hex().substr(0, 12));
    }
    text.append("  state=").append(to_string(target.state));
    text.append("  attempts=").append(std::to_string(target.attempts_created));
    text.append("  epoch=").append(std::to_string(value_of(target.epoch)));
    text.append("  healthy=").append(target.healthy ? "yes" : "no");
    text.append("(").append(std::to_string(target.consecutive_healthy)).append(")");
    if (target.state == TargetState::kRollingBack) {
      text.append("  rollback_step=").append(std::to_string(target.rollback_step));
    }
    if (!target.last_reason.empty()) {
      text.append("\n        reason: ").append(target.last_reason);
    }
    text.append("\n");
  }
  return text;
}

std::string render_cohort_membership(const CohortMembership& membership,
                                     const TargetInventory& inventory) {
  std::string text;
  text.append("cohort ").append(short_hex(membership.id));
  text.append("  stage=").append(short_hex(membership.stage));
  text.append("  generation=").append(short_hex(membership.generation));
  text.append("  members=").append(std::to_string(membership.size()));
  text.append("  frozen=").append(membership.frozen ? "yes" : "no");
  text.append("\n  selector: ").append(membership.selector.describe());
  text.append("\n  selector_digest=").append(hex_or_none(membership.selector_digest));
  text.append("\n  membership_digest=").append(hex_or_none(membership.membership_digest));
  text.append("\n  inventory_digest=").append(hex_or_none(membership.inventory_digest));
  text.append("  inventory_epoch=").append(std::to_string(value_of(membership.inventory_epoch)));
  text.append("\n  frozen_at=").append(membership.frozen_at.to_rfc3339()).append("\n");
  for (const TargetId& member : membership.members) {
    const TargetDescriptor* descriptor = inventory.find(member);
    text.append("    ").append(short_hex(member));
    if (descriptor == nullptr) {
      text.append("  (not present in the current inventory)\n");
      continue;
    }
    text.append("  ").append(render_target_brief(*descriptor)).append("\n");
  }
  return text;
}

std::string render_decision(const Decision& decision) { return decision.render(); }

std::string render_decisions(const std::vector<Decision>& decisions) {
  std::string text;
  for (const Decision& decision : decisions) {
    if (!text.empty()) {
      text.append("\n");
    }
    text.append(decision.render());
  }
  return text;
}

JsonValue status_to_json(const Rollout& rollout, const TargetInventory& inventory) {
  (void)inventory;
  JsonValue root = JsonValue::make_object();
  root.set("kind", JsonValue::make_string("rollout_fabric.status"));
  root.set("schema_version", JsonValue::make_number(1));
  root.set("rollout", JsonValue::make_string(rollout.id.to_hex()));
  root.set("change_id", JsonValue::make_string(rollout.change_id));
  root.set("state", JsonValue::make_string(std::string(to_string(rollout.state))));
  root.set("state_reason", JsonValue::make_string(rollout.state_reason));
  root.set("generation", JsonValue::make_string(rollout.generation.to_hex()));
  root.set("generation_counter",
           JsonValue::make_number(static_cast<std::int64_t>(value_of(rollout.generation_counter))));
  root.set("revision",
           JsonValue::make_number(static_cast<std::int64_t>(value_of(rollout.revision))));
  root.set("incarnation", JsonValue::make_string(rollout.incarnation.to_hex()));
  root.set("controller_epoch",
           JsonValue::make_number(static_cast<std::int64_t>(value_of(rollout.controller_epoch))));
  root.set("plan_id", JsonValue::make_string(rollout.plan_id.to_hex()));
  root.set("plan_digest", JsonValue::make_string(to_hex(rollout.plan_digest)));
  root.set("source_digest", JsonValue::make_string(to_hex(rollout.source_digest)));
  root.set("cursor", JsonValue::make_number(rollout.cursor));
  root.set("outstanding_attempts", JsonValue::make_number(rollout.outstanding_attempts));
  root.set("changing", JsonValue::make_number(rollout.changing_count()));
  root.set("max_observed_changing",
           JsonValue::make_number(rollout.max_observed_changing_global));
  root.set("created_at", JsonValue::make_string(rollout.created_at.to_rfc3339()));
  root.set("digest", JsonValue::make_string(to_hex(rollout_digest(rollout))));

  JsonValue stages = JsonValue::make_array();
  for (const StageRuntime& stage : rollout.stages) {
    JsonValue entry = JsonValue::make_object();
    entry.set("id", JsonValue::make_string(stage.id.to_hex()));
    entry.set("state", JsonValue::make_string(std::string(to_string(stage.state))));
    entry.set("ordinal", JsonValue::make_number(stage.ordinal));
    entry.set("members", JsonValue::make_number(stage.cohort.size()));
    entry.set("frozen", JsonValue::make_bool(stage.cohort.frozen));
    entry.set("dispatched", JsonValue::make_number(stage.dispatched_total));
    entry.set("succeeded", JsonValue::make_number(stage.succeeded_total));
    entry.set("failed", JsonValue::make_number(stage.failed_total));
    entry.set("cancelled", JsonValue::make_number(stage.cancelled_total));
    entry.set("rolled_back", JsonValue::make_number(stage.rolled_back_total));
    entry.set("rollback_failed", JsonValue::make_number(stage.rollback_failed_total));
    entry.set("changing", JsonValue::make_number(stage.changing_count()));
    entry.set("max_observed_changing", JsonValue::make_number(stage.max_observed_changing));
    entry.set("policy", JsonValue::make_string(stage.policy.describe()));
    if (!stage.pending_gate.is_nil()) {
      entry.set("pending_gate", JsonValue::make_string(stage.pending_gate.to_hex()));
      entry.set("pending_gate_kind",
                JsonValue::make_string(std::string(to_string(stage.pending_gate_kind))));
    }
    stages.push_back(std::move(entry));
  }
  root.set("stages", std::move(stages));
  return root;
}

JsonValue decision_to_json(const Decision& decision) {
  JsonValue root = JsonValue::make_object();
  root.set("kind", JsonValue::make_string("rollout_fabric.decision"));
  root.set("schema_version", JsonValue::make_number(1));
  root.set("id", JsonValue::make_string(decision.id.to_hex()));
  root.set("decision_kind", JsonValue::make_string(std::string(to_string(decision.kind))));
  root.set("rollout", JsonValue::make_string(decision.rollout.to_hex()));
  root.set("generation", JsonValue::make_string(decision.generation.to_hex()));
  root.set("revision",
           JsonValue::make_number(static_cast<std::int64_t>(value_of(decision.revision))));
  root.set("controller_epoch",
           JsonValue::make_number(static_cast<std::int64_t>(value_of(decision.controller_epoch))));
  root.set("incarnation", JsonValue::make_string(decision.incarnation.to_hex()));
  root.set("authority", JsonValue::make_string(decision.authority.to_hex()));
  root.set("stage", JsonValue::make_string(decision.stage.to_hex()));
  root.set("target", JsonValue::make_string(decision.target.to_hex()));
  root.set("attempt", JsonValue::make_string(decision.attempt.to_hex()));
  root.set("decided_at", JsonValue::make_string(decision.decided_at.to_rfc3339()));
  root.set("inputs_digest", JsonValue::make_string(to_hex(decision.inputs_digest)));
  root.set("policy_digest", JsonValue::make_string(to_hex(decision.policy_digest)));
  root.set("evidence_digest", JsonValue::make_string(to_hex(decision.evidence_digest)));
  root.set("selected", JsonValue::make_string(decision.selected));
  JsonValue rejected = JsonValue::make_array();
  for (const RejectedAlternative& alternative : decision.rejected) {
    JsonValue entry = JsonValue::make_object();
    entry.set("action", JsonValue::make_string(alternative.action));
    entry.set("reason", JsonValue::make_string(alternative.reason));
    rejected.push_back(std::move(entry));
  }
  root.set("rejected", std::move(rejected));
  root.set("rationale", JsonValue::make_string(decision.rationale));
  root.set("digest", JsonValue::make_string(to_hex(decision.decision_digest())));
  return root;
}

JsonValue cohort_to_json(const CohortMembership& membership, const TargetInventory& inventory) {
  JsonValue root = JsonValue::make_object();
  root.set("kind", JsonValue::make_string("rollout_fabric.cohort"));
  root.set("schema_version", JsonValue::make_number(1));
  root.set("id", JsonValue::make_string(membership.id.to_hex()));
  root.set("stage", JsonValue::make_string(membership.stage.to_hex()));
  root.set("generation", JsonValue::make_string(membership.generation.to_hex()));
  root.set("selector", JsonValue::make_string(membership.selector.describe()));
  root.set("selector_digest", JsonValue::make_string(to_hex(membership.selector_digest)));
  root.set("membership_digest", JsonValue::make_string(to_hex(membership.membership_digest)));
  root.set("inventory_digest", JsonValue::make_string(to_hex(membership.inventory_digest)));
  root.set("inventory_epoch",
           JsonValue::make_number(static_cast<std::int64_t>(value_of(membership.inventory_epoch))));
  root.set("frozen", JsonValue::make_bool(membership.frozen));
  root.set("frozen_at", JsonValue::make_string(membership.frozen_at.to_rfc3339()));
  root.set("member_count", JsonValue::make_number(membership.size()));
  JsonValue members = JsonValue::make_array();
  for (const TargetId& member : membership.members) {
    JsonValue entry = JsonValue::make_object();
    entry.set("id", JsonValue::make_string(member.to_hex()));
    const TargetDescriptor* descriptor = inventory.find(member);
    if (descriptor != nullptr) {
      entry.set("name", JsonValue::make_string(descriptor->name));
      entry.set("kind", JsonValue::make_string(std::string(to_string(descriptor->kind))));
      entry.set("site", JsonValue::make_string(descriptor->placement.site));
      entry.set("pod", JsonValue::make_string(descriptor->placement.pod));
      entry.set("rack", JsonValue::make_string(descriptor->placement.rack));
      entry.set("failure_domain",
                JsonValue::make_string(descriptor->placement.failure_domain.to_hex()));
    }
    members.push_back(std::move(entry));
  }
  root.set("members", std::move(members));
  return root;
}

}  // namespace rollout_fabric
