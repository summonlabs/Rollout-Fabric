// Rollout Fabric - a complete single-stage rollout.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This example drives the real orchestrator through its public API with the
// in-process simulator. It prints the decisions the runtime made and why, which
// is the part of the system a reader should take away.
#include <cstdio>
#include <memory>
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

}  // namespace

int main() {
  std::printf("%s\n\n", version_banner().c_str());

  const RuntimeLimits limits;
  ManualClock clock;
  IdFactory ids = IdFactory::from_entropy();

  // --- inventory ---------------------------------------------------------
  TargetInventory inventory;
  for (int index = 0; index < 6; ++index) {
    TargetDescriptor descriptor;
    descriptor.name = "leaf-" + std::to_string(index);
    descriptor.id = derive_target(descriptor.name);
    descriptor.kind = TargetKind::kSwitch;
    descriptor.placement.site = "site-a";
    descriptor.placement.pod = "pod-" + std::to_string(index % 2);
    descriptor.placement.rack = "rack-" + std::to_string(index % 3);
    descriptor.placement.failure_domain = derive_domain("fd-" + std::to_string(index % 3));
    const Status status = inventory.add(std::move(descriptor), limits);
    if (!status.ok()) {
      std::printf("inventory: %s\n", status.to_string().c_str());
      return 1;
    }
  }
  std::printf("inventory: %zu switches in 3 failure domains\n", inventory.size());

  // --- plan (normally produced by the Change Planner) ---------------------
  const Digest256 source_digest = sha256(std::string("example/approved-plan"));
  ChangePlan plan;
  plan.id = derive_plan_id(source_digest);
  plan.source_digest = source_digest;
  plan.change_id = "CHG-EXAMPLE-1";
  plan.approved_by = "change-planner";
  plan.approved_at = Timestamp::from_unix_seconds(1767225600);

  StageSpec stage;
  stage.id = derive_stage_id(plan.id, "canary");
  stage.name = "canary";
  stage.ordinal = 0;
  stage.selector.op = SelectorOp::kByKind;
  stage.selector.kinds.push_back(TargetKind::kSwitch);
  stage.policy.max_concurrency = 2;
  stage.policy.canary_size = 1;
  stage.policy.min_healthy_fraction = ratio(1, 1);
  stage.policy.failure_budget_targets = 0;
  stage.policy.evidence_gate.soak = Duration::from_seconds(2);
  stage.policy.blast_radius.max_targets_changing_global = 2;
  stage.policy.blast_radius.max_targets_changing_per_failure_domain = 1;
  stage.policy.blast_radius.max_failure_domains_changing = 2;
  ActionSpec action;
  action.id = derive_action_id(stage.id, "apply-config");
  action.name = "apply-config";
  action.kind = ActionKind::kApplyConfiguration;
  action.rollback = RollbackSupport::kUnsupported;
  action.artifact_digest = sha256(std::string("example/artifact"));
  stage.actions.push_back(action);
  plan.stages.push_back(stage);
  Status status = plan.validate(limits);
  if (!status.ok()) {
    std::printf("plan: %s\n", status.to_string().c_str());
    return 1;
  }

  // --- adapters ----------------------------------------------------------
  sim::SimExecutionAdapter execution(clock, limits);
  sim::TargetBehavior behavior;
  behavior.work_duration = Duration::from_millis(250);
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
    std::printf("create: %s\n", rollout_id.status().to_string().c_str());
    return 1;
  }
  std::printf("created %s\n", detail.c_str());
  if (!orchestrator.arm(rollout_id.value(), context, &detail).ok()) {
    return 1;
  }
  std::printf("armed:   %s\n", detail.c_str());
  if (!orchestrator.start(rollout_id.value(), context, &detail).ok()) {
    return 1;
  }
  std::printf("started: %s\n\n", detail.c_str());

  // --- drive -------------------------------------------------------------
  for (int tick = 0; tick < 4000; ++tick) {
    clock.advance(Duration::from_millis(50));
    const TickReport report = orchestrator.tick();
    (void)report;
    const Rollout* rollout = orchestrator.find(rollout_id.value());
    if (rollout != nullptr && is_terminal(rollout->state)) {
      break;
    }
  }

  const Rollout* rollout = orchestrator.find(rollout_id.value());
  if (rollout == nullptr) {
    return 1;
  }
  std::printf("%s\n", render_status(*rollout, inventory, limits).c_str());
  std::printf("decisions (%zu retained):\n\n", rollout->decisions.size());
  for (const Decision& decision : rollout->decisions) {
    std::printf("%s", decision.render().c_str());
  }
  std::printf("\nexecution adapter: dispatched=%llu accepted=%llu refused=%llu\n",
              static_cast<unsigned long long>(execution.stats().dispatched),
              static_cast<unsigned long long>(execution.stats().dispatch_accepted),
              static_cast<unsigned long long>(execution.stats().dispatch_refused));
  std::printf("orchestrator counters: accepted=%llu rejected=%llu duplicate=%llu out_of_order=%llu\n",
              static_cast<unsigned long long>(orchestrator.counters().evidence_accepted),
              static_cast<unsigned long long>(orchestrator.counters().evidence_rejected),
              static_cast<unsigned long long>(orchestrator.counters().evidence_duplicate),
              static_cast<unsigned long long>(orchestrator.counters().evidence_out_of_order));
  return rollout->state == RolloutState::kCompleted ? 0 : 1;
}
