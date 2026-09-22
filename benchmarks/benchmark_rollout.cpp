// Rollout Fabric - benchmarks.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every benchmark here reports COMPLETED work: rollouts that finished their
// stages, targets that were carried through their gates, evidence records that
// were admitted and applied, journal records that were written and flushed, and
// frames that were decoded from a byte stream. Submission rate is not reported
// because it would measure the queue rather than the runtime.
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "rollout_fabric/journal.hpp"
#include "rollout_fabric/orchestrator.hpp"
#include "rollout_fabric/plan.hpp"
#include "rollout_fabric/sim_adapters.hpp"
#include "rollout_fabric/target.hpp"
#include "rollout_fabric/transport.hpp"
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

class Stopwatch {
 public:
  Stopwatch() : start_(std::chrono::steady_clock::now()) {}
  [[nodiscard]] double seconds() const {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - start_).count();
  }

 private:
  std::chrono::steady_clock::time_point start_;
};

[[nodiscard]] TargetInventory build_inventory(std::uint32_t targets, const RuntimeLimits& limits) {
  TargetInventory inventory;
  for (std::uint32_t index = 0; index < targets; ++index) {
    TargetDescriptor descriptor;
    descriptor.name = "leaf-" + std::to_string(index);
    descriptor.id = derive_target(descriptor.name);
    descriptor.kind = TargetKind::kSwitch;
    descriptor.placement.site = "site-" + std::to_string(index % 4);
    descriptor.placement.pod = "pod-" + std::to_string(index % 8);
    descriptor.placement.rack = "rack-" + std::to_string(index % 16);
    descriptor.placement.failure_domain = derive_domain("fd-" + std::to_string(index % 32));
    const Status status = inventory.add(std::move(descriptor), limits);
    (void)status;
  }
  return inventory;
}

[[nodiscard]] ChangePlan build_plan(std::uint32_t stages, const RuntimeLimits& limits) {
  const Digest256 source = sha256(std::string("benchmark-plan"));
  ChangePlan plan;
  plan.id = derive_plan_id(source);
  plan.source_digest = source;
  plan.change_id = "CHG-BENCH";
  plan.approved_by = "benchmark";
  plan.approved_at = Timestamp::from_unix_seconds(1767225600);
  for (std::uint32_t ordinal = 0; ordinal < stages; ++ordinal) {
    StageSpec stage;
    stage.name = "wave-" + std::to_string(ordinal);
    stage.id = derive_stage_id(plan.id, stage.name);
    stage.ordinal = ordinal;
    if (ordinal > 0) {
      stage.predecessors.push_back(plan.stages.back().id);
    }
    stage.selector.op = SelectorOp::kByKind;
    stage.selector.kinds.push_back(TargetKind::kSwitch);
    stage.policy.max_concurrency = 64;
    stage.policy.canary_size = 0;
    stage.policy.min_healthy_fraction = ratio(1, 1);
    stage.policy.failure_budget_targets = 0;
    stage.policy.health_gate.min_consecutive_healthy = 1;
    stage.policy.blast_radius.max_targets_changing_global = 64;
    stage.policy.blast_radius.max_targets_changing_per_stage = 64;
    stage.policy.blast_radius.max_targets_changing_per_failure_domain = 64;
    stage.policy.blast_radius.max_targets_changing_per_rack = 64;
    stage.policy.blast_radius.max_targets_changing_per_pod = 64;
    stage.policy.blast_radius.max_targets_changing_per_site = 64;
    stage.policy.blast_radius.max_failure_domains_changing = 64;
    ActionSpec action;
    action.name = "apply";
    action.id = derive_action_id(stage.id, action.name);
    action.kind = ActionKind::kApplyConfiguration;
    action.artifact_digest = sha256(stage.name + "/artifact");
    stage.actions.push_back(action);
    plan.stages.push_back(stage);
  }
  const Status status = plan.validate(limits);
  (void)status;
  return plan;
}

void benchmark_rollouts(std::uint32_t targets, std::uint32_t stages, std::uint32_t repetitions) {
  const RuntimeLimits limits;
  ManualClock clock;
  IdFactory ids = IdFactory::from_entropy();
  const TargetInventory inventory = build_inventory(targets, limits);
  const ChangePlan plan = build_plan(stages, limits);

  sim::SimExecutionAdapter execution(clock, limits);
  sim::TargetBehavior behavior;
  behavior.work_duration = Duration::from_millis(1);
  execution.set_default_behavior(behavior);
  const HealthSourceId source = ids.next<HealthSourceId>(IdDomain::kHealthSource);
  sim::SimHealthAdapter health(clock, limits, source);

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

  std::uint64_t completed = 0;
  std::uint64_t targets_changed = 0;
  std::uint64_t ticks = 0;
  const Stopwatch watch;
  for (std::uint32_t repetition = 0; repetition < repetitions; ++repetition) {
    CommandContext context;
    context.operator_name = "benchmark";
    context.operator_id = ids.next<OperatorId>(IdDomain::kOperator);
    context.authority = ids.next<AuthorityId>(IdDomain::kAuthority);
    context.incarnation = orchestrator.incarnation();
    context.controller_epoch = orchestrator.controller_epoch();
    std::string detail;
    auto created = orchestrator.create_rollout(plan, context, &detail);
    if (!created.ok()) {
      std::printf("benchmark: create failed: %s\n", created.status().to_string().c_str());
      return;
    }
    const RolloutId id = created.value();
    if (!orchestrator.arm(id, context, &detail).ok() ||
        !orchestrator.start(id, context, &detail).ok()) {
      return;
    }
    for (std::uint32_t tick = 0; tick < 600000; ++tick) {
      clock.advance(Duration::from_millis(1));
      (void)orchestrator.tick();
      ++ticks;
      const Rollout* rollout = orchestrator.find(id);
      if (rollout != nullptr && is_terminal(rollout->state)) {
        if (rollout->state == RolloutState::kCompleted) {
          ++completed;
          for (const StageRuntime& stage : rollout->stages) {
            targets_changed += stage.succeeded_total;
          }
        }
        break;
      }
    }
  }
  const double seconds = watch.seconds();
  std::printf(
      "rollouts          targets=%-6u stages=%-3u completed=%-4llu targets_changed=%-8llu "
      "ticks=%-9llu  %8.1f ms/rollout  %10.0f targets/s\n",
      targets, stages, static_cast<unsigned long long>(completed),
      static_cast<unsigned long long>(targets_changed), static_cast<unsigned long long>(ticks),
      seconds * 1000.0 / static_cast<double>(completed),
      static_cast<double>(targets_changed) / (seconds > 0 ? seconds : 1.0));
  (void)targets;
}

void benchmark_journal(std::uint32_t records) {
  const RuntimeLimits limits;
  const std::string path = "benchmark-journal.tmp";
  JournalOpenOptions options;
  options.path = path;
  options.limits = limits;
  auto writer = JournalWriter::open(options);
  if (!writer.ok()) {
    std::printf("journal benchmark: %s\n", writer.status().to_string().c_str());
    return;
  }
  const std::vector<std::byte> payload(256, std::byte{0x5A});
  const IncarnationId incarnation = IdFactory::from_entropy().next<IncarnationId>(IdDomain::kIncarnation);
  const Stopwatch watch;
  std::uint64_t written = 0;
  for (std::uint32_t index = 0; index < records; ++index) {
    const Status status =
        writer.value()->append(JournalRecordType::kEvidenceRecorded, EpochCounter{1}, incarnation,
                               Timestamp::from_unix_seconds(1767225600), payload);
    if (!status.ok()) {
      break;
    }
    ++written;
  }
  const double seconds = watch.seconds();
  std::printf("journal           records=%-8llu flushed+fsynced  %8.1f us/record  %10.0f records/s\n",
              static_cast<unsigned long long>(written), seconds * 1e6 / static_cast<double>(written),
              static_cast<double>(written) / (seconds > 0 ? seconds : 1.0));

  // Recovery throughput: how fast the durable state can be proved and replayed.
  std::vector<JournalRecord> recovered;
  const Stopwatch recovery_watch;
  auto report = scan_journal(path, limits, &recovered, false);
  const double recovery_seconds = recovery_watch.seconds();
  if (report.ok()) {
    std::printf("journal-recovery  records=%-8llu verified     %8.1f us/record  %10.0f records/s\n",
                static_cast<unsigned long long>(recovered.size()),
                recovery_seconds * 1e6 / static_cast<double>(recovered.size() ? recovered.size() : 1),
                static_cast<double>(recovered.size()) / (recovery_seconds > 0 ? recovery_seconds : 1.0));
  }
  writer.value().reset();
  (void)std::remove(path.c_str());
  (void)std::remove((path + ".lock").c_str());
}

void benchmark_framing(std::uint32_t frames, std::uint32_t body_bytes) {
  const RuntimeLimits limits;
  Frame frame;
  frame.type = MessageType::kEvidence;
  frame.sequence = Sequence{1};
  frame.body.assign(body_bytes, std::byte{0x11});
  std::vector<std::byte> encoded;
  const Status status = encode_frame(frame, limits, encoded);
  if (!status.ok()) {
    std::printf("framing benchmark: %s\n", status.to_string().c_str());
    return;
  }
  const Stopwatch watch;
  std::uint64_t decoded = 0;
  FrameDecoder decoder(limits);
  std::vector<Frame> out;
  for (std::uint32_t index = 0; index < frames; ++index) {
    out.clear();
    const Status feed = decoder.feed(std::span<const std::byte>(encoded.data(), encoded.size()), out);
    if (!feed.ok()) {
      break;
    }
    decoded += out.size();
  }
  const double seconds = watch.seconds();
  std::printf("framing           decoded=%-8llu body=%-6u %8.2f us/frame   %10.0f frames/s   "
              "%8.1f MiB/s\n",
              static_cast<unsigned long long>(decoded), body_bytes,
              seconds * 1e6 / static_cast<double>(decoded ? decoded : 1),
              static_cast<double>(decoded) / (seconds > 0 ? seconds : 1.0),
              static_cast<double>(decoded) * static_cast<double>(body_bytes) /
                  (seconds > 0 ? seconds : 1.0) / (1024.0 * 1024.0));
}

}  // namespace

int main() {
  std::printf("%s\n", version_banner().c_str());
  std::printf("all figures measure completed work\n\n");
  benchmark_rollouts(64, 1, 20);
  benchmark_rollouts(256, 3, 5);
  benchmark_rollouts(1024, 2, 2);
  benchmark_journal(20000);
  benchmark_framing(200000, 256);
  benchmark_framing(20000, 16384);
  return 0;
}
