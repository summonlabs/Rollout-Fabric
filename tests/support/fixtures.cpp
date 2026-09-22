// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fixtures.hpp"

#include <algorithm>
#include <string>

namespace rf_test {

using rollout_fabric::ChangePlan;
using rollout_fabric::CommandContext;
using rollout_fabric::CommandKind;
using rollout_fabric::Duration;
using rollout_fabric::FailureDomainId;
using rollout_fabric::IdDomain;
using rollout_fabric::Orchestrator;
using rollout_fabric::OrchestratorDeps;
using rollout_fabric::Ratio;
using rollout_fabric::SelectorOp;
using rollout_fabric::SelectorTerm;
using rollout_fabric::StagePolicy;
using rollout_fabric::StageSpec;
using rollout_fabric::TargetDescriptor;
using rollout_fabric::TargetId;
using rollout_fabric::TargetKind;

namespace {

[[nodiscard]] rollout_fabric::Digest256 derive(const std::string_view domain,
                                               const std::string_view name) {
  rollout_fabric::ByteWriter writer;
  writer.string(domain);
  writer.string(name);
  return rollout_fabric::sha256(writer.span());
}

}  // namespace

TargetId target_id(std::string_view name) {
  const auto digest = derive("rollout-fabric/target/v1", name);
  return TargetId::from_bytes(
      std::span<const std::byte, TargetId::byte_size>(digest.data(), TargetId::byte_size));
}

FailureDomainId failure_domain_id(std::string_view name) {
  const auto digest = derive("rollout-fabric/failure-domain/v1", name);
  return FailureDomainId::from_bytes(std::span<const std::byte, FailureDomainId::byte_size>(
      digest.data(), FailureDomainId::byte_size));
}

SelectorTerm all_switches() {
  SelectorTerm term;
  term.op = SelectorOp::kByKind;
  term.kinds.push_back(TargetKind::kSwitch);
  return term;
}

SelectorTerm switches_in_site(std::string site) {
  SelectorTerm term;
  term.op = SelectorOp::kBySite;
  term.sites.push_back(std::move(site));
  return term;
}

SelectorTerm switches_in_domain(std::string domain) {
  SelectorTerm term;
  term.op = SelectorOp::kByFailureDomain;
  term.failure_domains.push_back(failure_domain_id(domain));
  return term;
}

StagePolicy quick_policy(std::uint32_t concurrency, std::uint32_t canary) {
  StagePolicy policy;
  policy.max_concurrency = concurrency;
  policy.canary_size = canary;
  policy.min_healthy_fraction = rollout_fabric::ratio(1, 1);
  policy.failure_budget_targets = 0;
  policy.failure_budget_fraction = rollout_fabric::ratio(0, 1);
  policy.health_gate.enabled = true;
  policy.health_gate.min_consecutive_healthy = 1;
  policy.health_gate.max_age = Duration::from_seconds(30);
  policy.health_gate.required_source = rollout_fabric::HealthSourceId{};
  policy.evidence_gate.mode = rollout_fabric::GateMode::kAutomatic;
  policy.evidence_gate.min_passing_samples = 1;
  policy.evidence_gate.require_full_coverage = false;
  policy.evidence_gate.min_passing_fraction = rollout_fabric::ratio(0, 1);
  policy.attempt_deadline = Duration::from_seconds(60);
  policy.evidence_max_age = Duration::from_seconds(120);
  policy.blast_radius.max_targets_changing_global = 64;
  policy.blast_radius.max_targets_changing_per_stage = 64;
  policy.blast_radius.max_targets_changing_per_failure_domain = 64;
  policy.blast_radius.max_targets_changing_per_rack = 64;
  policy.blast_radius.max_targets_changing_per_pod = 64;
  policy.blast_radius.max_targets_changing_per_site = 64;
  policy.blast_radius.max_failure_domains_changing = 64;
  return policy;
}

const rollout_fabric::StageRuntime* only_stage(const rollout_fabric::Rollout& rollout) {
  if (rollout.stages.size() != 1) {
    return nullptr;
  }
  return &rollout.stages.front();
}

std::uint32_t count_state(const rollout_fabric::StageRuntime& stage,
                          rollout_fabric::TargetState state) {
  std::uint32_t count = 0;
  for (const rollout_fabric::TargetRuntime& target : stage.targets) {
    if (target.state == state) {
      ++count;
    }
  }
  return count;
}

Fixture::Fixture()
    : limits(),
      ids(rollout_fabric::IdFactory::from_entropy()),
      execution(clock, limits),
      health(clock, limits, rollout_fabric::HealthSourceId{}) {
  health_source = ids.next<rollout_fabric::HealthSourceId>(IdDomain::kHealthSource);
  health.set_source(health_source);
  build_inventory(6, 2, 2, 2, 2);
}

void Fixture::build_inventory(std::uint32_t count, std::uint32_t sites, std::uint32_t pods_per_site,
                              std::uint32_t racks_per_pod, std::uint32_t domains) {
  inventory = rollout_fabric::TargetInventory{};
  for (std::uint32_t index = 0; index < count; ++index) {
    TargetDescriptor descriptor;
    const std::string name = "leaf-" + std::to_string(index);
    descriptor.id = target_id(name);
    descriptor.name = name;
    descriptor.kind = TargetKind::kSwitch;
    descriptor.placement.site = "s" + std::to_string(index % sites);
    descriptor.placement.pod =
        "p" + std::to_string((index / std::max<std::uint32_t>(sites, 1u)) % pods_per_site);
    descriptor.placement.rack = "r" + std::to_string(index % std::max<std::uint32_t>(racks_per_pod, 1u));
    descriptor.placement.failure_domain =
        failure_domain_id("fd-" + std::to_string(index % std::max<std::uint32_t>(domains, 1u)));
    const auto status = inventory.add(std::move(descriptor), limits);
    (void)status;
  }
}

void Fixture::build_inventory(const std::vector<TargetSpec>& specs) {
  inventory = rollout_fabric::TargetInventory{};
  for (const TargetSpec& spec : specs) {
    TargetDescriptor descriptor;
    descriptor.id = target_id(spec.name);
    descriptor.name = spec.name;
    descriptor.kind = spec.kind;
    descriptor.placement.site = spec.site;
    descriptor.placement.pod = spec.pod;
    descriptor.placement.rack = spec.rack;
    descriptor.placement.failure_domain = failure_domain_id(spec.failure_domain);
    const auto status = inventory.add(std::move(descriptor), limits);
    (void)status;
  }
}

void Fixture::build_orchestrator(rollout_fabric::JournalWriter* journal, bool attach_health) {
  OrchestratorDeps deps;
  deps.limits = limits;
  deps.clock = &clock;
  deps.journal = journal;
  deps.execution = &execution;
  deps.health = attach_health ? &health : nullptr;
  deps.ids = ids;
  deps.incarnation = ids.next<rollout_fabric::IncarnationId>(IdDomain::kIncarnation);
  deps.controller_epoch = rollout_fabric::EpochCounter{1};
  deps.inventory = inventory;
  orchestrator = std::make_unique<Orchestrator>(std::move(deps));
  execution.set_sink(orchestrator.get());
  if (attach_health) {
    health.set_sink(orchestrator.get());
  }
}

CommandContext Fixture::context() {
  CommandContext context;
  context.operator_name = "test-operator";
  context.operator_id = ids.next<rollout_fabric::OperatorId>(IdDomain::kOperator);
  context.authority = ids.next<rollout_fabric::AuthorityId>(IdDomain::kAuthority);
  context.incarnation = orchestrator->incarnation();
  context.controller_epoch = orchestrator->controller_epoch();
  return context;
}

void Fixture::run(std::uint32_t ticks, std::int64_t advance_millis) {
  for (std::uint32_t index = 0; index < ticks; ++index) {
    clock.advance(Duration::from_millis(advance_millis));
    const rollout_fabric::TickReport report = orchestrator->tick();
    (void)report;
  }
}

std::uint32_t Fixture::run_until(const std::function<bool()>& predicate, std::uint32_t tick_budget,
                                 std::int64_t advance_millis) {
  for (std::uint32_t index = 0; index < tick_budget; ++index) {
    if (predicate()) {
      return index;
    }
    clock.advance(Duration::from_millis(advance_millis));
    const rollout_fabric::TickReport report = orchestrator->tick();
    (void)report;
  }
  return tick_budget;
}

PlanBuilder::PlanBuilder() {
  plan_.id = rollout_fabric::derive_plan_id(rollout_fabric::sha256(std::string("fixture-plan")));
  plan_.source_digest = rollout_fabric::sha256(std::string("fixture-plan-source"));
  plan_.change_id = "CHG-FIXTURE";
  plan_.approved_by = "change-planner";
  plan_.approved_at = rollout_fabric::Timestamp::from_unix_seconds(1767225600);
}

StageSpec* PlanBuilder::find(const std::string& name) {
  for (StageSpec& stage : plan_.stages) {
    if (stage.name == name) {
      return &stage;
    }
  }
  return nullptr;
}

const StageSpec* PlanBuilder::find(const std::string& name) const {
  for (const StageSpec& stage : plan_.stages) {
    if (stage.name == name) {
      return &stage;
    }
  }
  return nullptr;
}

PlanBuilder& PlanBuilder::add_stage(const std::string& name, std::uint32_t ordinal,
                                    const SelectorTerm& selector, const StagePolicy& policy) {
  StageSpec stage;
  stage.id = rollout_fabric::derive_stage_id(plan_.id, name);
  stage.name = name;
  stage.ordinal = ordinal;
  stage.selector = selector;
  stage.policy = policy;
  plan_.stages.push_back(std::move(stage));
  names_.push_back(name);
  return *this;
}

PlanBuilder& PlanBuilder::predecessor(const std::string& stage, const std::string& predecessor_name) {
  StageSpec* spec = find(stage);
  const StageSpec* previous = find(predecessor_name);
  if (spec != nullptr && previous != nullptr) {
    spec->predecessors.push_back(previous->id);
  }
  return *this;
}

PlanBuilder& PlanBuilder::action(const std::string& stage, const std::string& action_name,
                                 rollout_fabric::ActionKind kind,
                                 rollout_fabric::RollbackSupport rollback) {
  StageSpec* spec = find(stage);
  if (spec != nullptr) {
    rollout_fabric::ActionSpec action;
    action.id = rollout_fabric::derive_action_id(spec->id, action_name);
    action.name = action_name;
    action.kind = kind;
    action.rollback = rollback;
    action.artifact_digest = rollout_fabric::sha256(action_name + "@fixture");
    action.artifact = rollout_fabric::ArtifactId{};
    spec->actions.push_back(std::move(action));
  }
  return *this;
}

PlanBuilder& PlanBuilder::compensation(const std::string& stage, const std::string& action_name) {
  StageSpec* spec = find(stage);
  if (spec != nullptr) {
    spec->policy.rollback_supported = true;
    spec->policy.compensation_actions.push_back(rollout_fabric::derive_action_id(spec->id, action_name));
  }
  return *this;
}

PlanBuilder& PlanBuilder::evidence_rule(const std::string& stage,
                                        const rollout_fabric::TargetEvidenceRule& rule) {
  StageSpec* spec = find(stage);
  if (spec != nullptr) {
    spec->evidence_rule = rule;
  }
  return *this;
}

PlanBuilder& PlanBuilder::change_id(std::string value) {
  plan_.change_id = std::move(value);
  return *this;
}

ChangePlan PlanBuilder::build() const {
  ChangePlan plan = plan_;
  const auto status = plan.validate(rollout_fabric::default_limits());
  (void)status;
  return plan;
}

}  // namespace rf_test
