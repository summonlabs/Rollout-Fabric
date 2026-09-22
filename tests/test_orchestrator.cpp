// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Orchestration semantics: lifecycle, gating, fencing, blast radius, pause,
// abort, compensation and restart reconciliation, all driven through the public
// API with the in-process simulator. The distributed claims are proved
// separately by the process-based suites.
#include <string>

#include "fixtures.hpp"
#include "rollout_fabric/persistence.hpp"
#include "test_harness.hpp"

namespace {

using namespace rollout_fabric;

RolloutId create_single_stage(rf_test::Fixture& fixture, const StagePolicy& policy,
                              const SelectorTerm& selector = rf_test::all_switches(),
                              bool attach_health = true,
                              const TargetEvidenceRule& rule = TargetEvidenceRule{}) {
  fixture.build_orchestrator(nullptr, attach_health);
  rf_test::PlanBuilder builder;
  builder.add_stage("wave", 0, selector, policy)
      .action("wave", "apply")
      .evidence_rule("wave", rule);
  const ChangePlan plan = builder.build();
  std::string detail;
  auto id = fixture.orchestrator->create_rollout(plan, fixture.context(), &detail);
  RF_CHECK(id.ok());
  if (!id.ok()) {
    return RolloutId{};
  }
  return id.value();
}

RolloutId create_two_stage(rf_test::Fixture& fixture, const StagePolicy& policy) {
  fixture.build_orchestrator(nullptr, true);
  rf_test::PlanBuilder builder;
  builder.add_stage("first", 0, rf_test::switches_in_site("s0"), policy)
      .add_stage("second", 1, rf_test::switches_in_site("s1"), policy)
      .action("first", "apply")
      .action("second", "apply")
      .predecessor("second", "first");
  const ChangePlan plan = builder.build();
  std::string detail;
  auto id = fixture.orchestrator->create_rollout(plan, fixture.context(), &detail);
  RF_CHECK(id.ok());
  if (!id.ok()) {
    return RolloutId{};
  }
  return id.value();
}

std::uint32_t ticks_to_complete(rf_test::Fixture& fixture, const RolloutId& id,
                                std::uint32_t budget = 20000) {
  return fixture.run_until(
      [&]() {
        const Rollout* rollout = fixture.orchestrator->find(id);
        return rollout != nullptr && is_terminal(rollout->state);
      },
      budget);
}

}  // namespace

RF_TEST(rollout_lifecycle_reaches_completed) {
  rf_test::Fixture fixture;
  StagePolicy policy = rf_test::quick_policy(3, 1);
  const RolloutId id = create_single_stage(fixture, policy);

  const Rollout* created = fixture.orchestrator->find(id);
  RF_REQUIRE(created != nullptr);
  RF_CHECK_EQ(created->state, RolloutState::kValidated);
  RF_CHECK_EQ(created->stages.size(), std::size_t{1});
  RF_CHECK_EQ(created->stages.front().cohort.size(), std::size_t{6});
  RF_CHECK(!created->stages.front().cohort.frozen);

  const CommandContext context = fixture.context();
  std::string detail;
  RF_CHECK(fixture.orchestrator->arm(id, context, &detail).ok());
  const Rollout* armed = fixture.orchestrator->find(id);
  RF_REQUIRE(armed != nullptr);
  RF_CHECK_EQ(armed->state, RolloutState::kArmed);
  RF_CHECK(armed->stages.front().cohort.frozen);

  RF_CHECK(fixture.orchestrator->start(id, context, &detail).ok());
  const std::uint32_t used = ticks_to_complete(fixture, id);
  RF_CHECK(used < 20000);
  const Rollout* finished = fixture.orchestrator->find(id);
  RF_REQUIRE(finished != nullptr);
  RF_CHECK_EQ(finished->state, RolloutState::kCompleted);
  RF_CHECK_EQ(finished->stages.front().succeeded_total, std::uint32_t{6});
  RF_CHECK_EQ(finished->stages.front().failed_total, std::uint32_t{0});
  RF_CHECK_EQ(finished->outstanding_attempts, std::uint32_t{0});
  RF_CHECK(fixture.orchestrator->storage_status().ok());
}

RF_TEST(dispatch_is_not_completion) {
  rf_test::Fixture fixture;
  StagePolicy policy = rf_test::quick_policy(2, 0);
  policy.canary_size = 0;
  const RolloutId id = create_single_stage(fixture, policy);
  // Every target accepts the dispatch and never reports a terminal outcome.
  sim::TargetBehavior behavior;
  behavior.never_complete = true;
  fixture.execution.set_default_behavior(behavior);

  const CommandContext context = fixture.context();
  std::string detail;
  RF_REQUIRE(fixture.orchestrator->arm(id, context, &detail).ok());
  RF_REQUIRE(fixture.orchestrator->start(id, context, &detail).ok());
  fixture.run(40);

  const Rollout* rollout = fixture.orchestrator->find(id);
  RF_REQUIRE(rollout != nullptr);
  const StageRuntime& stage = rollout->stages.front();
  RF_CHECK(stage.dispatched_total > 0);
  RF_CHECK_EQ(stage.succeeded_total, std::uint32_t{0});
  RF_CHECK_EQ(stage.changing_count(), std::uint32_t{2});
  RF_CHECK_EQ(rollout->state, RolloutState::kRunning);
  RF_CHECK(!is_terminal(rollout->state));
}

RF_TEST(later_stage_cannot_start_before_predecessor_gate) {
  rf_test::Fixture fixture;
  StagePolicy policy = rf_test::quick_policy(2, 0);
  policy.canary_size = 0;
  const RolloutId id = create_two_stage(fixture, policy);
  const CommandContext context = fixture.context();
  std::string detail;
  RF_REQUIRE(fixture.orchestrator->arm(id, context, &detail).ok());
  RF_REQUIRE(fixture.orchestrator->start(id, context, &detail).ok());

  // The second stage must remain pending while the first is running.
  bool violated = false;
  for (int tick = 0; tick < 60; ++tick) {
    fixture.run(1);
    const Rollout* rollout = fixture.orchestrator->find(id);
    RF_REQUIRE(rollout != nullptr);
    if (rollout->stages.size() == 2) {
      const StageRuntime& second = rollout->stages[1];
      if (rollout->stages[0].state != StageState::kSucceeded &&
          second.state != StageState::kPending) {
        violated = true;
      }
    }
  }
  RF_CHECK(!violated);
  RF_CHECK(ticks_to_complete(fixture, id) < 20000);
  const Rollout* finished = fixture.orchestrator->find(id);
  RF_REQUIRE(finished != nullptr);
  RF_CHECK_EQ(finished->state, RolloutState::kCompleted);
  RF_CHECK_EQ(finished->stages[0].state, StageState::kSucceeded);
  RF_CHECK_EQ(finished->stages[1].state, StageState::kSucceeded);
}

RF_TEST(blast_radius_is_never_exceeded) {
  rf_test::Fixture fixture;
  StagePolicy policy = rf_test::quick_policy(8, 0);
  policy.canary_size = 0;
  policy.blast_radius.max_targets_changing_global = 3;
  policy.blast_radius.max_targets_changing_per_stage = 3;
  policy.blast_radius.max_targets_changing_per_failure_domain = 1;
  policy.blast_radius.max_failure_domains_changing = 2;
  const RolloutId id = create_single_stage(fixture, policy);
  const CommandContext context = fixture.context();
  std::string detail;
  RF_REQUIRE(fixture.orchestrator->arm(id, context, &detail).ok());
  RF_REQUIRE(fixture.orchestrator->start(id, context, &detail).ok());

  bool exceeded = false;
  for (int tick = 0; tick < 400; ++tick) {
    fixture.run(1);
    const Rollout* rollout = fixture.orchestrator->find(id);
    RF_REQUIRE(rollout != nullptr);
    if (rollout->changing_count() > 3) {
      exceeded = true;
    }
    if (rollout->stages.front().changing_count() > 3) {
      exceeded = true;
    }
  }
  RF_CHECK(!exceeded);
  const Rollout* rollout = fixture.orchestrator->find(id);
  RF_REQUIRE(rollout != nullptr);
  RF_CHECK(rollout->max_observed_changing_global <= 3);
  RF_CHECK(rollout->stages.front().max_observed_changing <= 3);
  RF_CHECK_EQ(rollout->state, RolloutState::kCompleted);
}

RF_TEST(manual_entry_gate_is_single_use_and_fenced) {
  rf_test::Fixture fixture;
  StagePolicy policy = rf_test::quick_policy(2, 0);
  policy.canary_size = 0;
  policy.entry_gate = GateMode::kManual;
  const RolloutId id = create_single_stage(fixture, policy);
  const CommandContext context = fixture.context();
  std::string detail;
  RF_REQUIRE(fixture.orchestrator->arm(id, context, &detail).ok());
  RF_REQUIRE(fixture.orchestrator->start(id, context, &detail).ok());
  fixture.run(4);

  const Rollout* gated = fixture.orchestrator->find(id);
  RF_REQUIRE(gated != nullptr);
  RF_CHECK_EQ(gated->state, RolloutState::kGated);
  const GateId gate = gated->stages.front().pending_gate;
  RF_CHECK(!gate.is_nil());
  RF_CHECK_EQ(gated->stages.front().state, StageState::kGated);

  // No work may be admitted while the gate is open.
  fixture.run(20);
  const Rollout* still_gated = fixture.orchestrator->find(id);
  RF_REQUIRE(still_gated != nullptr);
  RF_CHECK_EQ(still_gated->stages.front().dispatched_total, std::uint32_t{0});

  // A stale generation must be refused.
  CommandContext stale = fixture.context();
  stale.expected_generation = GenerationId{};
  stale.checked_generation = true;
  RF_CHECK_EQ(fixture.orchestrator->approve_gate(id, gate, stale, &detail).code(),
              StatusCode::kStaleGeneration);

  RF_CHECK(fixture.orchestrator->approve_gate(id, gate, fixture.context(), &detail).ok());
  RF_CHECK_EQ(fixture.orchestrator->approve_gate(id, gate, fixture.context(), &detail).code(),
              StatusCode::kNotFound);
  RF_CHECK(ticks_to_complete(fixture, id) < 20000);
  RF_CHECK_EQ(fixture.orchestrator->find(id)->state, RolloutState::kCompleted);
}

RF_TEST(stale_generation_evidence_is_rejected) {
  rf_test::Fixture fixture;
  StagePolicy policy = rf_test::quick_policy(1, 0);
  policy.canary_size = 0;
  const RolloutId id = create_single_stage(fixture, policy);
  const CommandContext context = fixture.context();
  std::string detail;
  RF_REQUIRE(fixture.orchestrator->arm(id, context, &detail).ok());
  RF_REQUIRE(fixture.orchestrator->start(id, context, &detail).ok());
  fixture.run(3);

  const Rollout* rollout = fixture.orchestrator->find(id);
  RF_REQUIRE(rollout != nullptr);
  const StageRuntime& stage = rollout->stages.front();
  RF_REQUIRE(!stage.targets.empty());

  // Forge evidence for a superseded generation.
  EvidenceRecord forged;
  forged.id = fixture.ids.next<EvidenceId>(IdDomain::kEvidence);
  forged.rollout = id;
  forged.generation = fixture.ids.next<GenerationId>(IdDomain::kGeneration);
  forged.stage = stage.id;
  forged.cohort = stage.cohort.id;
  forged.target = stage.targets.front().target;
  forged.attempt = stage.targets.front().current_attempt;
  forged.attempt_epoch = stage.targets.front().epoch;
  forged.incarnation = fixture.orchestrator->incarnation();
  forged.kind = EvidenceKind::kExecutionCompleted;
  forged.outcome = EvidenceOutcome::kSucceeded;
  forged.authority = EvidenceAuthority::kExecutionAdapterAttested;
  forged.sequence = Sequence{99};
  forged.observed_at = fixture.clock.wall_now();
  forged.recorded_at = forged.observed_at;
  forged.payload_digest = sha256(std::string("forged"));

  const std::uint64_t before = fixture.orchestrator->counters().evidence_stale_generation;
  fixture.orchestrator->on_evidence(forged);
  fixture.run(1);
  RF_CHECK_EQ(fixture.orchestrator->counters().evidence_stale_generation, before + 1);
  RF_CHECK(fixture.orchestrator->find(id)->stages.front().succeeded_total == 0 ||
           fixture.orchestrator->find(id)->stages.front().succeeded_total <= 1);
}

RF_TEST(stale_attempt_epoch_evidence_is_rejected) {
  rf_test::Fixture fixture;
  StagePolicy policy = rf_test::quick_policy(1, 0);
  policy.canary_size = 0;
  const RolloutId id = create_single_stage(fixture, policy, rf_test::all_switches(), true,
                                           TargetEvidenceRule{});
  const CommandContext context = fixture.context();
  std::string detail;
  RF_REQUIRE(fixture.orchestrator->arm(id, context, &detail).ok());
  RF_REQUIRE(fixture.orchestrator->start(id, context, &detail).ok());
  fixture.run(3);

  const Rollout* rollout = fixture.orchestrator->find(id);
  RF_REQUIRE(rollout != nullptr);
  const StageRuntime& stage = rollout->stages.front();
  const TargetRuntime& target = stage.targets.front();
  RF_REQUIRE(!target.current_attempt.is_nil());

  EvidenceRecord forged;
  forged.id = fixture.ids.next<EvidenceId>(IdDomain::kEvidence);
  forged.rollout = id;
  forged.generation = rollout->generation;
  forged.stage = stage.id;
  forged.cohort = stage.cohort.id;
  forged.target = target.target;
  forged.attempt = target.current_attempt;
  forged.attempt_epoch = AttemptEpoch{value_of(target.epoch) + 7};
  forged.incarnation = fixture.orchestrator->incarnation();
  forged.kind = EvidenceKind::kExecutionCompleted;
  forged.outcome = EvidenceOutcome::kSucceeded;
  forged.authority = EvidenceAuthority::kExecutionAdapterAttested;
  forged.sequence = Sequence{50};
  forged.observed_at = fixture.clock.wall_now();
  forged.recorded_at = forged.observed_at;
  forged.payload_digest = sha256(std::string("forged epoch"));

  const std::uint64_t before = fixture.orchestrator->counters().evidence_stale_attempt;
  fixture.orchestrator->on_evidence(forged);
  fixture.run(1);
  RF_CHECK_EQ(fixture.orchestrator->counters().evidence_stale_attempt, before + 1);
}

RF_TEST(duplicate_and_reordered_evidence_change_nothing) {
  rf_test::Fixture fixture;
  StagePolicy policy = rf_test::quick_policy(1, 0);
  policy.canary_size = 0;
  const RolloutId id = create_single_stage(fixture, policy);
  sim::TargetBehavior behavior;
  behavior.work_duration = Duration::from_millis(10);
  behavior.duplicate_report = true;
  fixture.execution.set_default_behavior(behavior);

  const CommandContext context = fixture.context();
  std::string detail;
  RF_REQUIRE(fixture.orchestrator->arm(id, context, &detail).ok());
  RF_REQUIRE(fixture.orchestrator->start(id, context, &detail).ok());
  RF_CHECK(ticks_to_complete(fixture, id) < 20000);
  const Rollout* rollout = fixture.orchestrator->find(id);
  RF_REQUIRE(rollout != nullptr);
  RF_CHECK_EQ(rollout->stages.front().succeeded_total, std::uint32_t{6});
  RF_CHECK(fixture.orchestrator->counters().evidence_duplicate > 0);

  rf_test::Fixture reordered;
  const RolloutId second = create_single_stage(reordered, policy);
  sim::TargetBehavior reorder_behavior;
  reorder_behavior.work_duration = Duration::from_millis(10);
  reorder_behavior.reorder_report = true;
  reordered.execution.set_default_behavior(reorder_behavior);
  const CommandContext second_context = reordered.context();
  RF_REQUIRE(reordered.orchestrator->arm(second, second_context, &detail).ok());
  RF_REQUIRE(reordered.orchestrator->start(second, second_context, &detail).ok());
  RF_CHECK(ticks_to_complete(reordered, second) < 20000);
  RF_CHECK(reordered.orchestrator->find(second)->stages.front().succeeded_total == 6);
  RF_CHECK(reordered.orchestrator->counters().evidence_out_of_order > 0);
}

RF_TEST(stale_health_cannot_satisfy_a_gate) {
  rf_test::Fixture fixture;
  StagePolicy policy = rf_test::quick_policy(2, 0);
  policy.canary_size = 0;
  policy.health_gate.max_age = Duration::from_millis(500);
  const RolloutId id = create_single_stage(fixture, policy);
  const CommandContext context = fixture.context();
  std::string detail;
  RF_REQUIRE(fixture.orchestrator->arm(id, context, &detail).ok());
  RF_REQUIRE(fixture.orchestrator->start(id, context, &detail).ok());
  RF_CHECK(ticks_to_complete(fixture, id) < 20000);
  RF_CHECK_EQ(fixture.orchestrator->find(id)->state, RolloutState::kCompleted);

  // The same rollout with telemetry that is always older than the gate accepts.
  rf_test::Fixture stale;
  StagePolicy stale_policy = rf_test::quick_policy(2, 0);
  stale_policy.canary_size = 0;
  stale_policy.health_gate.max_age = Duration::from_millis(50);
  const RolloutId stale_id = create_single_stage(stale, stale_policy);
  stale.health.set_health_age(Duration::from_seconds(3600));
  const CommandContext stale_context = stale.context();
  RF_REQUIRE(stale.orchestrator->arm(stale_id, stale_context, &detail).ok());
  RF_REQUIRE(stale.orchestrator->start(stale_id, stale_context, &detail).ok());
  stale.run(200);
  const Rollout* stuck = stale.orchestrator->find(stale_id);
  RF_REQUIRE(stuck != nullptr);
  RF_CHECK(!is_terminal(stuck->state));
  // Stale telemetry cannot satisfy the gate, so dispatch stops once the first
  // successes exist and the rollout never reaches a terminal state.
  RF_CHECK(stuck->stages.front().succeeded_total > 0);
  RF_CHECK(stuck->stages.front().succeeded_total < 6);
  RF_CHECK(!stuck->stages.front().last_health_gate.satisfied);
  RF_CHECK(!stuck->stages.front().last_health_gate.reason.empty());
}

RF_TEST(pause_stops_new_work_and_lets_in_flight_finish) {
  rf_test::Fixture fixture;
  StagePolicy policy = rf_test::quick_policy(2, 0);
  policy.canary_size = 0;
  policy.pause_in_flight = InFlightPolicy::kLetFinish;
  policy.attempt_deadline = Duration::from_seconds(3600);
  const RolloutId id = create_single_stage(fixture, policy);
  sim::TargetBehavior behavior;
  behavior.work_duration = Duration::from_millis(100);
  fixture.execution.set_default_behavior(behavior);

  const CommandContext context = fixture.context();
  std::string detail;
  RF_REQUIRE(fixture.orchestrator->arm(id, context, &detail).ok());
  RF_REQUIRE(fixture.orchestrator->start(id, context, &detail).ok());
  fixture.run(3);
  const std::uint32_t dispatched_before =
      fixture.orchestrator->find(id)->stages.front().dispatched_total;
  RF_CHECK(dispatched_before > 0);
  RF_REQUIRE(fixture.orchestrator->pause(id, fixture.context(), "operator pause", &detail).ok());
  const std::uint32_t dispatched_at_pause =
      fixture.orchestrator->find(id)->stages.front().dispatched_total;

  // Long enough for the in-flight attempts to finish under the let-finish
  // policy, and short enough that nothing new could have been admitted.
  fixture.run(40);
  const Rollout* paused = fixture.orchestrator->find(id);
  RF_REQUIRE(paused != nullptr);
  RF_CHECK_EQ(paused->state, RolloutState::kPaused);
  // No new dispatch happened while paused.
  RF_CHECK_EQ(paused->stages.front().dispatched_total, dispatched_at_pause);
  // In-flight work was allowed to finish and its evidence was applied.
  RF_CHECK(paused->stages.front().succeeded_total > 0);

  RF_REQUIRE(fixture.orchestrator->resume(id, fixture.context(), &detail).ok());
  RF_CHECK(ticks_to_complete(fixture, id) < 20000);
  RF_CHECK_EQ(fixture.orchestrator->find(id)->state, RolloutState::kCompleted);
}

RF_TEST(pause_cancel_policy_never_counts_a_cancelled_attempt_as_success) {
  rf_test::Fixture fixture;
  StagePolicy policy = rf_test::quick_policy(2, 0);
  policy.canary_size = 0;
  policy.pause_in_flight = InFlightPolicy::kCancel;
  policy.attempt_deadline = Duration::from_seconds(3600);
  const RolloutId id = create_single_stage(fixture, policy);
  sim::TargetBehavior behavior;
  behavior.work_duration = Duration::from_millis(200);
  fixture.execution.set_default_behavior(behavior);

  const CommandContext context = fixture.context();
  std::string detail;
  RF_REQUIRE(fixture.orchestrator->arm(id, context, &detail).ok());
  RF_REQUIRE(fixture.orchestrator->start(id, context, &detail).ok());
  fixture.run(3);
  RF_REQUIRE(fixture.orchestrator->pause(id, fixture.context(), "operator pause", &detail).ok());
  fixture.run(20);
  const Rollout* paused = fixture.orchestrator->find(id);
  RF_REQUIRE(paused != nullptr);
  RF_CHECK_EQ(paused->stages.front().succeeded_total, std::uint32_t{0});
  RF_CHECK(paused->stages.front().cancelled_total > 0);
}

RF_TEST(abort_without_compensation_fails_the_rollout) {
  rf_test::Fixture fixture;
  StagePolicy policy = rf_test::quick_policy(2, 0);
  policy.canary_size = 0;
  policy.attempt_deadline = Duration::from_seconds(3600);
  const RolloutId id = create_single_stage(fixture, policy);
  sim::TargetBehavior behavior;
  behavior.work_duration = Duration::from_seconds(30);
  fixture.execution.set_default_behavior(behavior);

  const CommandContext context = fixture.context();
  std::string detail;
  RF_REQUIRE(fixture.orchestrator->arm(id, context, &detail).ok());
  RF_REQUIRE(fixture.orchestrator->start(id, context, &detail).ok());
  fixture.run(3);
  RF_REQUIRE(fixture.orchestrator->abort(id, fixture.context(), "change window closed", &detail).ok());
  const Rollout* aborted = fixture.orchestrator->find(id);
  RF_REQUIRE(aborted != nullptr);
  RF_CHECK_EQ(aborted->state, RolloutState::kFailed);
  RF_CHECK_EQ(aborted->stages.front().state, StageState::kAborted);
  RF_CHECK(!aborted->state_reason.empty());
}

RF_TEST(abort_with_compensation_runs_rollback_and_records_failures) {
  rf_test::Fixture fixture;
  StagePolicy policy = rf_test::quick_policy(4, 0);
  policy.canary_size = 0;
  policy.rollback_supported = true;
  const RolloutId id = [&]() {
    fixture.build_orchestrator(nullptr, true);
    rf_test::PlanBuilder builder;
    builder.add_stage("wave", 0, rf_test::all_switches(), policy)
        .action("wave", "apply")
        .action("wave", "undo", ActionKind::kCompensate, RollbackSupport::kSupported)
        .compensation("wave", "undo");
    const ChangePlan plan = builder.build();
    std::string detail;
    auto created = fixture.orchestrator->create_rollout(plan, fixture.context(), &detail);
    return created.ok() ? created.value() : RolloutId{};
  }();
  RF_REQUIRE(!id.is_nil());

  sim::TargetBehavior behavior;
  behavior.work_duration = Duration::from_millis(5);
  fixture.execution.set_default_behavior(behavior);

  const CommandContext context = fixture.context();
  std::string detail;
  RF_REQUIRE(fixture.orchestrator->arm(id, context, &detail).ok());
  RF_REQUIRE(fixture.orchestrator->start(id, context, &detail).ok());
  // Let a few targets complete, then abort.
  // Abort as soon as one target has actually changed, which is the situation
  // compensation exists for.
  const std::uint32_t waited = fixture.run_until(
      [&]() { return fixture.orchestrator->find(id)->stages.front().succeeded_total > 0; },
      4000);
  RF_CHECK(waited < 4000);
  RF_REQUIRE(fixture.orchestrator->abort(id, fixture.context(), "operator abort", &detail).ok());
  const Rollout* rolling = fixture.orchestrator->find(id);
  RF_REQUIRE(rolling != nullptr);
  RF_CHECK(rolling->state == RolloutState::kRollingBack || rolling->state == RolloutState::kFailed);

  fixture.run(200);
  const Rollout* finished = fixture.orchestrator->find(id);
  RF_REQUIRE(finished != nullptr);
  RF_CHECK_EQ(finished->state, RolloutState::kFailed);
  if (finished->stages.front().rolled_back_total == 0) {
    RF_FAIL("no target was compensated; rollout reason: " + finished->state_reason);
  }
  RF_CHECK(finished->stages.front().rolled_back_total > 0);
  RF_CHECK_EQ(finished->stages.front().rollback_failed_total, std::uint32_t{0});
  RF_CHECK_EQ(finished->stages.front().state, StageState::kRolledBack);
}

RF_TEST(rollback_failure_is_recorded_not_hidden) {
  rf_test::Fixture fixture;
  StagePolicy policy = rf_test::quick_policy(4, 0);
  policy.canary_size = 0;
  policy.rollback_supported = true;
  const RolloutId id = [&]() {
    fixture.build_orchestrator(nullptr, true);
    rf_test::PlanBuilder builder;
    builder.add_stage("wave", 0, rf_test::all_switches(), policy)
        .action("wave", "apply")
        .action("wave", "undo", ActionKind::kCompensate, RollbackSupport::kSupported)
        .compensation("wave", "undo");
    const ChangePlan plan = builder.build();
    std::string detail;
    auto created = fixture.orchestrator->create_rollout(plan, fixture.context(), &detail);
    return created.ok() ? created.value() : RolloutId{};
  }();
  RF_REQUIRE(!id.is_nil());

  sim::TargetBehavior behavior;
  behavior.work_duration = Duration::from_millis(5);
  fixture.execution.set_default_behavior(behavior);

  const CommandContext context = fixture.context();
  std::string detail;
  RF_REQUIRE(fixture.orchestrator->arm(id, context, &detail).ok());
  RF_REQUIRE(fixture.orchestrator->start(id, context, &detail).ok());
  // Abort as soon as one target has actually changed, which is the situation
  // compensation exists for.
  const std::uint32_t waited = fixture.run_until(
      [&]() { return fixture.orchestrator->find(id)->stages.front().succeeded_total > 0; },
      4000);
  RF_CHECK(waited < 4000);

  // Compensation fails for every target.
  sim::TargetBehavior failing;
  failing.work_duration = Duration::from_millis(5);
  failing.fail = true;
  fixture.execution.set_default_behavior(failing);
  RF_REQUIRE(fixture.orchestrator->abort(id, fixture.context(), "operator abort", &detail).ok());
  fixture.run(200);
  const Rollout* finished = fixture.orchestrator->find(id);
  RF_REQUIRE(finished != nullptr);
  RF_CHECK_EQ(finished->state, RolloutState::kFailed);
  if (finished->stages.front().rollback_failed_total == 0) {
    RF_FAIL("no compensation failure was recorded; rollout reason: " +
             finished->state_reason);
  }
  RF_CHECK(finished->stages.front().rollback_failed_total > 0);
  RF_CHECK_EQ(finished->stages.front().state, StageState::kRolledBack);
}

RF_TEST(canary_limits_the_first_batch) {
  rf_test::Fixture fixture;
  StagePolicy policy = rf_test::quick_policy(4, 2);
  policy.canary_size = 2;
  policy.canary_requires_health_gate = true;
  const RolloutId id = create_single_stage(fixture, policy);
  const CommandContext context = fixture.context();
  std::string detail;
  RF_REQUIRE(fixture.orchestrator->arm(id, context, &detail).ok());
  RF_REQUIRE(fixture.orchestrator->start(id, context, &detail).ok());

  // After the first dispatch tick exactly the canary batch is in flight.
  bool observed = false;
  for (int tick = 0; tick < 40; ++tick) {
    fixture.run(1);
    const Rollout* rollout = fixture.orchestrator->find(id);
    RF_REQUIRE(rollout != nullptr);
    const StageRuntime& stage = rollout->stages.front();
    if (stage.dispatched_total > 0 && !stage.canary_cleared) {
      // The canary bounds how many distinct targets are admitted before the
      // batch clears. Retries of a target already in the batch are attempts,
      // not new admissions, so the bound is on admitted targets, not on
      // dispatched attempts.
      std::uint32_t admitted_targets = 0;
      for (const TargetRuntime& entry : stage.targets) {
        if (entry.attempts_created > 0) {
          ++admitted_targets;
        }
      }
      RF_CHECK(admitted_targets <= 2);
      observed = true;
    }
  }
  RF_CHECK(observed);
  RF_CHECK(ticks_to_complete(fixture, id) < 20000);
  RF_CHECK_EQ(fixture.orchestrator->find(id)->stages.front().succeeded_total, std::uint32_t{6});
}

RF_TEST(failure_budget_fails_the_stage) {
  rf_test::Fixture fixture;
  StagePolicy policy = rf_test::quick_policy(4, 0);
  policy.canary_size = 0;
  policy.failure_budget_targets = 1;
  policy.failure_budget_fraction = ratio(0, 1);
  policy.max_attempts_per_target = 1;
  const RolloutId id = create_single_stage(fixture, policy, rf_test::all_switches(), true,
                                           TargetEvidenceRule{});
  sim::TargetBehavior behavior;
  behavior.work_duration = Duration::from_millis(5);
  behavior.fail = true;
  fixture.execution.set_default_behavior(behavior);

  const CommandContext context = fixture.context();
  std::string detail;
  RF_REQUIRE(fixture.orchestrator->arm(id, context, &detail).ok());
  RF_REQUIRE(fixture.orchestrator->start(id, context, &detail).ok());
  RF_CHECK(ticks_to_complete(fixture, id) < 20000);
  const Rollout* finished = fixture.orchestrator->find(id);
  RF_REQUIRE(finished != nullptr);
  RF_CHECK_EQ(finished->state, RolloutState::kFailed);
  RF_CHECK_EQ(finished->stages.front().state, StageState::kFailed);
}

RF_TEST(soak_delays_the_evidence_gate) {
  rf_test::Fixture fixture;
  StagePolicy policy = rf_test::quick_policy(6, 0);
  policy.canary_size = 0;
  policy.evidence_gate.soak = Duration::from_seconds(5);
  const RolloutId id = create_single_stage(fixture, policy);
  const CommandContext context = fixture.context();
  std::string detail;
  RF_REQUIRE(fixture.orchestrator->arm(id, context, &detail).ok());
  RF_REQUIRE(fixture.orchestrator->start(id, context, &detail).ok());

  bool saw_soak = false;
  for (int tick = 0; tick < 4000; ++tick) {
    fixture.run(1, 10);
    const Rollout* rollout = fixture.orchestrator->find(id);
    RF_REQUIRE(rollout != nullptr);
    if (rollout->state == RolloutState::kSoaking) {
      saw_soak = true;
      RF_CHECK(rollout->stages.front().succeeded_total == 6);
      break;
    }
    if (is_terminal(rollout->state)) {
      break;
    }
  }
  RF_CHECK(saw_soak);
  RF_CHECK(ticks_to_complete(fixture, id, 40000) < 40000);
  const Rollout* soaked = fixture.orchestrator->find(id);
  if (soaked == nullptr || soaked->state != RolloutState::kCompleted) {
    RF_FAIL(soaked == nullptr ? std::string("the rollout disappeared")
                              : ("state " + std::string(to_string(soaked->state)) +
                                 " because: " + soaked->state_reason));
  }
  RF_CHECK(soaked != nullptr && soaked->state == RolloutState::kCompleted);
}

RF_TEST(explanations_are_deterministic_and_complete) {
  rf_test::Fixture fixture;
  StagePolicy policy = rf_test::quick_policy(2, 0);
  policy.canary_size = 0;
  const RolloutId id = create_single_stage(fixture, policy);
  const CommandContext context = fixture.context();
  std::string detail;
  RF_REQUIRE(fixture.orchestrator->arm(id, context, &detail).ok());
  RF_REQUIRE(fixture.orchestrator->start(id, context, &detail).ok());
  RF_CHECK(ticks_to_complete(fixture, id) < 20000);

  auto first = fixture.orchestrator->explain(id, std::nullopt);
  auto second = fixture.orchestrator->explain(id, std::nullopt);
  RF_REQUIRE(first.ok());
  RF_REQUIRE(second.ok());
  RF_CHECK_EQ(first.value(), second.value());
  RF_CHECK(first.value().find("rationale") != std::string::npos);
  RF_CHECK(first.value().find("generation") != std::string::npos);

  const Rollout* rollout = fixture.orchestrator->find(id);
  RF_REQUIRE(rollout != nullptr);
  RF_REQUIRE(!rollout->decisions.empty());
  for (const Decision& decision : rollout->decisions) {
    RF_CHECK(!decision.selected.empty());
    RF_CHECK(!decision.rationale.empty());
    RF_CHECK(!decision.incarnation.is_nil());
    RF_CHECK(!is_nil(decision.inputs_digest));
  }
  auto specific = fixture.orchestrator->explain(id, rollout->decisions.front().id);
  RF_REQUIRE(specific.ok());
  RF_CHECK(specific.value().find("decision") != std::string::npos);
}

RF_TEST(restart_restores_and_reconciles_without_redispatch) {
  rf_test::Fixture fixture;
  StagePolicy policy = rf_test::quick_policy(2, 0);
  policy.canary_size = 0;
  policy.attempt_deadline = Duration::from_seconds(3600);
  const RolloutId id = create_single_stage(fixture, policy);
  sim::TargetBehavior behavior;
  behavior.work_duration = Duration::from_seconds(30);
  fixture.execution.set_default_behavior(behavior);

  const CommandContext context = fixture.context();
  std::string detail;
  RF_REQUIRE(fixture.orchestrator->arm(id, context, &detail).ok());
  RF_REQUIRE(fixture.orchestrator->start(id, context, &detail).ok());
  fixture.run(6);
  const std::uint32_t dispatched_before =
      fixture.orchestrator->find(id)->stages.front().dispatched_total;
  RF_CHECK(dispatched_before > 0);

  const Rollout snapshot = *fixture.orchestrator->find(id);
  const ChangePlan plan = *fixture.orchestrator->plan_for(id);
  const std::size_t effects_before = fixture.execution.known_effects();
  RF_CHECK(effects_before > 0);

  // A new incarnation adopts the durable state. The executor still holds the
  // effects, so reconciliation must resolve them instead of redispatching.
  rf_test::Fixture restarted;
  restarted.build_inventory(6, 2, 2, 2, 2);
  restarted.build_orchestrator(nullptr, true);
  RF_REQUIRE(restarted.orchestrator->restore(snapshot, plan, RecoveryReport{}).ok());
  const Rollout* restored = restarted.orchestrator->find(id);
  RF_REQUIRE(restored != nullptr);
  RF_CHECK_EQ(restored->outstanding_attempts, dispatched_before);
  RF_CHECK_EQ(restarted.orchestrator->outstanding_attempts(id).size(),
              static_cast<std::size_t>(dispatched_before));
  // The restored rollout is owned by the new incarnation.
  RF_CHECK(restored->incarnation == restarted.orchestrator->incarnation());
}

RF_TEST(regenerate_creates_a_new_generation_and_new_membership) {
  rf_test::Fixture fixture;
  StagePolicy policy = rf_test::quick_policy(3, 0);
  policy.canary_size = 0;
  const RolloutId id = create_single_stage(fixture, policy);
  const CommandContext context = fixture.context();
  std::string detail;
  RF_REQUIRE(fixture.orchestrator->arm(id, context, &detail).ok());
  RF_REQUIRE(fixture.orchestrator->start(id, context, &detail).ok());

  const Rollout* armed = fixture.orchestrator->find(id);
  RF_REQUIRE(armed != nullptr);
  const GenerationId first_generation = armed->generation;
  const Digest256 first_membership = armed->stages.front().cohort.membership_digest;

  // Membership of an armed stage is immutable.
  RF_REQUIRE(fixture.orchestrator->abort(id, fixture.context(), "regenerate test", &detail).ok());
  fixture.run(20);
  RF_REQUIRE(fixture.orchestrator->find(id)->state == RolloutState::kFailed);
  RF_REQUIRE(fixture.orchestrator->regenerate(id, fixture.context(), &detail).ok());

  const Rollout* regenerated = fixture.orchestrator->find(id);
  RF_REQUIRE(regenerated != nullptr);
  RF_CHECK_EQ(regenerated->state, RolloutState::kValidated);
  RF_CHECK(!(regenerated->generation == first_generation));
  RF_CHECK(regenerated->stages.front().cohort.generation == regenerated->generation);
  RF_CHECK(!(regenerated->stages.front().cohort.generation == first_generation));
  RF_CHECK(!regenerated->stages.front().cohort.frozen);
  RF_REQUIRE(fixture.orchestrator->arm(id, fixture.context(), &detail).ok());
  RF_REQUIRE(fixture.orchestrator->start(id, fixture.context(), &detail).ok());
  RF_CHECK(ticks_to_complete(fixture, id) < 20000);
  RF_CHECK_EQ(fixture.orchestrator->find(id)->state, RolloutState::kCompleted);
}

RF_TEST(commands_are_fenced_by_generation_and_revision) {
  rf_test::Fixture fixture;
  StagePolicy policy = rf_test::quick_policy(2, 0);
  policy.canary_size = 0;
  const RolloutId id = create_single_stage(fixture, policy);
  const Rollout* rollout = fixture.orchestrator->find(id);
  RF_REQUIRE(rollout != nullptr);

  CommandContext stale_revision = fixture.context();
  stale_revision.expected_revision = Revision{value_of(rollout->revision) + 100};
  stale_revision.checked_revision = true;
  std::string detail;
  RF_CHECK_EQ(fixture.orchestrator->arm(id, stale_revision, &detail).code(),
              StatusCode::kStaleRevision);

  CommandContext stale_epoch = fixture.context();
  stale_epoch.controller_epoch = EpochCounter{value_of(fixture.orchestrator->controller_epoch()) + 1};
  RF_CHECK_EQ(fixture.orchestrator->arm(id, stale_epoch, &detail).code(), StatusCode::kStaleEpoch);

  CommandContext foreign_incarnation = fixture.context();
  foreign_incarnation.incarnation = fixture.ids.next<IncarnationId>(IdDomain::kIncarnation);
  RF_CHECK_EQ(fixture.orchestrator->arm(id, foreign_incarnation, &detail).code(),
              StatusCode::kStaleEpoch);

  RF_CHECK(fixture.orchestrator->arm(id, fixture.context(), &detail).ok());
}

RF_TEST(serialisation_round_trips_a_rollout_exactly) {
  rf_test::Fixture fixture;
  StagePolicy policy = rf_test::quick_policy(2, 0);
  policy.canary_size = 0;
  const RolloutId id = create_single_stage(fixture, policy);
  const CommandContext context = fixture.context();
  std::string detail;
  RF_REQUIRE(fixture.orchestrator->arm(id, context, &detail).ok());
  RF_REQUIRE(fixture.orchestrator->start(id, context, &detail).ok());
  fixture.run(10);

  const Rollout* rollout = fixture.orchestrator->find(id);
  RF_REQUIRE(rollout != nullptr);
  const std::vector<std::byte> payload =
      encode_rollout_payload(*rollout, fixture.limits);
  auto decoded = decode_rollout_payload(payload, fixture.limits);
  RF_REQUIRE(decoded.ok());
  RF_CHECK(digest_equal(rollout_digest(*rollout), rollout_digest(decoded.value())));
  RF_CHECK_EQ(decoded.value().stages.size(), rollout->stages.size());
  RF_CHECK_EQ(decoded.value().state, rollout->state);
  RF_CHECK_EQ(decoded.value().revision, rollout->revision);

  // A single flipped payload byte must be refused.
  std::vector<std::byte> corrupted = payload;
  corrupted[corrupted.size() / 2] ^= std::byte{0x01};
  auto rejected = decode_rollout_payload(corrupted, fixture.limits);
  RF_CHECK(!rejected.ok());
}

RF_TEST_MAIN()
