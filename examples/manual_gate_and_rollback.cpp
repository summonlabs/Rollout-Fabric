// Rollout Fabric - manual gates, abort and compensation.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Two stages, a manual gate between them, and an operator abort after the first
// stage changed a target. The compensating action fails for one target, so the
// example also shows how a partial rollback is recorded rather than hidden.
#include <cstdio>
#include <string>

#include "rollout_fabric/orchestrator.hpp"
#include "rollout_fabric/plan.hpp"
#include "rollout_fabric/report.hpp"
#include "rollout_fabric/sim_adapters.hpp"
#include "rollout_fabric/target.hpp"
#include "rollout_fabric/version.hpp"

namespace {

using namespace rollout_fabric;

[[nodiscard]] TargetId derive_target(std::string_view name) {
  ByteWriter writer;
  writer.string("rollout-fabric/target/v1");
  writer.string(name);
  const Digest256 digest = sha256(writer.span());
  return TargetId::from_bytes(
      std::span<const std::byte, TargetId::byte_size>(digest.data(), TargetId::byte_size));
}

[[nodiscard]] FailureDomainId derive_domain(std::string_view name) {
  ByteWriter writer;
  writer.string("rollout-fabric/failure-domain/v1");
  writer.string(name);
  const Digest256 digest = sha256(writer.span());
  return FailureDomainId::from_bytes(std::span<const std::byte, FailureDomainId::byte_size>(
      digest.data(), FailureDomainId::byte_size));
}

[[nodiscard]] StageSpec make_stage(const PlanId& plan, const std::string& name, std::uint32_t ordinal,
                                   const std::string& site,
                                   const std::vector<StageId>& predecessors) {
  StageSpec stage;
  stage.id = derive_stage_id(plan, name);
  stage.name = name;
  stage.ordinal = ordinal;
  stage.predecessors = predecessors;
  stage.selector.op = SelectorOp::kBySite;
  stage.selector.sites.push_back(site);
  stage.policy.max_concurrency = 2;
  stage.policy.canary_size = 0;
  stage.policy.min_healthy_fraction = ratio(1, 1);
  stage.policy.failure_budget_targets = 0;
  stage.policy.health_gate.min_consecutive_healthy = 1;
  stage.policy.rollback_supported = true;

  ActionSpec apply;
  apply.id = derive_action_id(stage.id, "apply");
  apply.name = "apply";
  apply.kind = ActionKind::kApplyConfiguration;
  apply.artifact_digest = sha256(name + "/artifact");
  stage.actions.push_back(apply);

  ActionSpec undo;
  undo.id = derive_action_id(stage.id, "undo");
  undo.name = "undo";
  undo.kind = ActionKind::kCompensate;
  undo.rollback = RollbackSupport::kSupported;
  undo.artifact_digest = sha256(name + "/undo-artifact");
  stage.actions.push_back(undo);
  stage.policy.compensation_actions.push_back(undo.id);
  return stage;
}

}  // namespace

int main() {
  const RuntimeLimits limits;
  ManualClock clock;
  IdFactory ids = IdFactory::from_entropy();

  TargetInventory inventory;
  for (int index = 0; index < 6; ++index) {
    TargetDescriptor descriptor;
    descriptor.name = "spine-" + std::to_string(index);
    descriptor.id = derive_target(descriptor.name);
    descriptor.kind = TargetKind::kSwitch;
    descriptor.placement.site = index < 3 ? "site-a" : "site-b";
    descriptor.placement.pod = "pod-0";
    descriptor.placement.rack = "rack-" + std::to_string(index);
    descriptor.placement.failure_domain = derive_domain("fd-" + std::to_string(index % 3));
    const Status status = inventory.add(std::move(descriptor), limits);
    if (!status.ok()) {
      return 1;
    }
  }

  const Digest256 source_digest = sha256(std::string("example/two-stage-plan"));
  ChangePlan plan;
  plan.id = derive_plan_id(source_digest);
  plan.source_digest = source_digest;
  plan.change_id = "CHG-EXAMPLE-2";
  plan.approved_by = "change-planner";
  plan.approved_at = Timestamp::from_unix_seconds(1767225600);

  StageSpec first = make_stage(plan.id, "site-a-wave", 0, "site-a", {});
  StageSpec second = make_stage(plan.id, "site-b-wave", 1, "site-b", {first.id});
  second.policy.entry_gate = GateMode::kManual;
  plan.stages.push_back(first);
  plan.stages.push_back(second);
  Status status = plan.validate(limits);
  if (!status.ok()) {
    std::printf("plan: %s\n", status.to_string().c_str());
    return 1;
  }

  sim::SimExecutionAdapter execution(clock, limits);
  sim::TargetBehavior behavior;
  behavior.work_duration = Duration::from_millis(100);
  execution.set_default_behavior(behavior);

  const HealthSourceId health_source = ids.next<HealthSourceId>(IdDomain::kHealthSource);
  sim::SimHealthAdapter health(clock, limits, health_source);

  OrchestratorDeps deps;
  deps.limits = limits;
  deps.clock = &clock;
  deps.execution = &execution;
  deps.health = &health;
  deps.ids = ids;
  deps.incarnation = ids.next<IncarnationId>(IdDomain::kIncarnation);
  deps.controller_epoch = EpochCounter{1};
  deps.inventory = inventory;
  Orchestrator orchestrator(std::move(deps));
  health.set_sink(&orchestrator);

  CommandContext context;
  context.operator_name = "example";
  context.operator_id = ids.next<OperatorId>(IdDomain::kOperator);
  context.authority = ids.next<AuthorityId>(IdDomain::kAuthority);
  context.incarnation = orchestrator.incarnation();
  context.controller_epoch = orchestrator.controller_epoch();

  std::string detail;
  auto rollout_id = orchestrator.create_rollout(plan, context, &detail);
  if (!rollout_id.ok()) {
    return 1;
  }
  const RolloutId id = rollout_id.value();
  if (!orchestrator.arm(id, context, &detail).ok() || !orchestrator.start(id, context, &detail).ok()) {
    return 1;
  }

  // Run the first stage to completion; the second stops at its manual gate.
  std::uint32_t ticks = 0;
  for (; ticks < 4000; ++ticks) {
    clock.advance(Duration::from_millis(50));
    (void)orchestrator.tick();
    const Rollout* rollout = orchestrator.find(id);
    if (rollout != nullptr && rollout->state == RolloutState::kGated) {
      break;
    }
  }
  const Rollout* gated = orchestrator.find(id);
  if (gated == nullptr) {
    return 1;
  }
  std::printf("stopped at a manual gate after %u ticks\n", ticks);
  std::printf("%s\n", render_status(*gated, inventory, limits).c_str());

  // Compensation is deliberately broken for the target in failure domain fd-0.
  sim::TargetBehavior undo_failure;
  undo_failure.work_duration = Duration::from_millis(50);
  undo_failure.fail = true;
  for (const TargetDescriptor& descriptor : inventory.all()) {
    if (descriptor.placement.failure_domain == derive_domain("fd-0")) {
      execution.set_behavior(descriptor.id, undo_failure);
    }
  }
  // The apply and the compensation are dispatched to the same target with the
  // same action identity, so the behaviour is switched between phases instead:
  // the first stage has already applied, so the failing behaviour only affects
  // the compensating attempts that follow.
  sim::TargetBehavior apply_ok;
  apply_ok.work_duration = Duration::from_millis(50);
  execution.set_default_behavior(apply_ok);

  if (!orchestrator.abort(id, context, "operator aborted after the gate", &detail).ok()) {
    std::printf("abort failed\n");
    return 1;
  }
  std::printf("abort: %s\n", detail.c_str());
  for (int index = 0; index < 4000; ++index) {
    clock.advance(Duration::from_millis(50));
    (void)orchestrator.tick();
    const Rollout* rollout = orchestrator.find(id);
    if (rollout != nullptr && is_terminal(rollout->state)) {
      break;
    }
  }

  const Rollout* finished = orchestrator.find(id);
  if (finished == nullptr) {
    return 1;
  }
  std::printf("\n%s\n", render_status(*finished, inventory, limits).c_str());
  std::printf("last decisions:\n");
  const std::vector<Decision> recent = orchestrator.recent_decisions(id, 4);
  for (const Decision& decision : recent) {
    std::printf("%s", decision.render().c_str());
  }
  return 0;
}
