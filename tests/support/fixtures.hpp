// Rollout Fabric - shared test fixtures.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Fixtures build orchestration state directly rather than through the JSON
// document format, so a test that is about the state machine does not fail
// because a document parser changed.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rollout_fabric/orchestrator.hpp"
#include "rollout_fabric/plan.hpp"
#include "rollout_fabric/report.hpp"
#include "rollout_fabric/sim_adapters.hpp"
#include "rollout_fabric/target.hpp"

namespace rf_test {

// The identifier derivation used by the inventory document loader, repeated
// here so that fixtures mint exactly the identifiers an operator's inventory
// would produce.
[[nodiscard]] rollout_fabric::TargetId target_id(std::string_view name);
[[nodiscard]] rollout_fabric::FailureDomainId failure_domain_id(std::string_view name);

struct TargetSpec {
  std::string name;
  rollout_fabric::TargetKind kind = rollout_fabric::TargetKind::kSwitch;
  std::string site;
  std::string pod;
  std::string rack;
  std::string failure_domain;
};

class Fixture {
 public:
  Fixture();

  rollout_fabric::ManualClock clock;
  rollout_fabric::RuntimeLimits limits;
  rollout_fabric::IdFactory ids;
  rollout_fabric::sim::SimExecutionAdapter execution;
  rollout_fabric::sim::SimHealthAdapter health;
  rollout_fabric::TargetInventory inventory;
  rollout_fabric::HealthSourceId health_source;
  std::unique_ptr<rollout_fabric::Orchestrator> orchestrator;

  // Builds an inventory of 'count' switches laid out over 'sites' sites,
  // 'pods_per_site' pods in each site, 'racks_per_pod' racks in each pod and
  // 'domains' distinct failure domains, deterministically.
  void build_inventory(std::uint32_t count, std::uint32_t sites, std::uint32_t pods_per_site,
                       std::uint32_t racks_per_pod, std::uint32_t domains);

  void build_inventory(const std::vector<TargetSpec>& specs);

  // Creates the orchestrator over the current inventory and adapters. The
  // journal is optional; without it the orchestrator runs purely in memory.
  void build_orchestrator(rollout_fabric::JournalWriter* journal = nullptr,
                          bool attach_health = true);

  // Mints a fresh operator and authority identity on every call, so commands
  // issued by a test are attributable without sharing state between tests.
  [[nodiscard]] rollout_fabric::CommandContext context();

  // Advances the clock and ticks the orchestrator.
  void run(std::uint32_t ticks, std::int64_t advance_millis = 5);

  // Advances the clock and ticks until the predicate holds or the tick budget
  // is spent. Returns the number of ticks used, or the budget when it never
  // held (the caller asserts on that).
  [[nodiscard]] std::uint32_t run_until(const std::function<bool()>& predicate,
                                        std::uint32_t tick_budget = 4000,
                                        std::int64_t advance_millis = 5);
};

// A plan builder that mirrors the document format's structure with defaults
// that produce a runnable single-stage rollout.
class PlanBuilder {
 public:
  PlanBuilder();

  PlanBuilder& add_stage(const std::string& name, std::uint32_t ordinal,
                         const rollout_fabric::SelectorTerm& selector,
                         const rollout_fabric::StagePolicy& policy);
  PlanBuilder& predecessor(const std::string& stage, const std::string& predecessor);
  PlanBuilder& action(const std::string& stage, const std::string& action_name,
                      rollout_fabric::ActionKind kind = rollout_fabric::ActionKind::kApplyConfiguration,
                      rollout_fabric::RollbackSupport rollback =
                          rollout_fabric::RollbackSupport::kUnsupported);
  PlanBuilder& compensation(const std::string& stage, const std::string& action_name);
  PlanBuilder& evidence_rule(const std::string& stage, const rollout_fabric::TargetEvidenceRule& rule);
  PlanBuilder& change_id(std::string value);

  [[nodiscard]] rollout_fabric::ChangePlan build() const;

 private:
  [[nodiscard]] rollout_fabric::StageSpec* find(const std::string& name);
  [[nodiscard]] const rollout_fabric::StageSpec* find(const std::string& name) const;

  rollout_fabric::ChangePlan plan_{};
  std::vector<std::string> names_;
};

// Helpers used across suites.
[[nodiscard]] rollout_fabric::StagePolicy quick_policy(std::uint32_t concurrency = 2,
                                                       std::uint32_t canary = 1);
[[nodiscard]] rollout_fabric::SelectorTerm all_switches();
[[nodiscard]] rollout_fabric::SelectorTerm switches_in_site(std::string site);
[[nodiscard]] rollout_fabric::SelectorTerm switches_in_domain(std::string domain);

// Convenience accessors with assertions-friendly returns.
[[nodiscard]] const rollout_fabric::StageRuntime* only_stage(const rollout_fabric::Rollout& rollout);
[[nodiscard]] std::uint32_t count_state(const rollout_fabric::StageRuntime& stage,
                                        rollout_fabric::TargetState state);

}  // namespace rf_test
