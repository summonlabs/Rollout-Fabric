// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Deterministic dispatch planning and blast-radius accounting.
//
// Blast radius is enforced here, at the only place that can create work. The
// planner walks the stage's cohort in canonical identifier order and admits a
// target only when every configured dimension still has room. The walk order
// never depends on adapter timing, map iteration or arrival order, so the same
// state always produces the same batch.
#include <algorithm>
#include <string>

#include "rollout_fabric/orchestrator.hpp"

namespace rollout_fabric {
namespace {

struct DimensionUsage {
  std::uint32_t per_failure_domain = 0;
  std::uint32_t per_rack = 0;
  std::uint32_t per_pod = 0;
  std::uint32_t per_site = 0;
  std::uint32_t distinct_domains = 0;
  bool domain_present = false;
};

// Counts how much of each correlated population is changing right now, both
// globally and around one candidate target.
[[nodiscard]] DimensionUsage usage_around(const Rollout& rollout, const TargetInventory& inventory,
                                          const TargetDescriptor& candidate) {
  DimensionUsage usage;
  std::vector<FailureDomainId> domains;
  for (const StageRuntime& stage : rollout.stages) {
    for (const TargetRuntime& target : stage.targets) {
      if (!target.counts_as_changing()) {
        continue;
      }
      const TargetDescriptor* descriptor = inventory.find(target.target);
      if (descriptor == nullptr) {
        continue;
      }
      if (descriptor->placement.failure_domain == candidate.placement.failure_domain) {
        ++usage.per_failure_domain;
        usage.domain_present = true;
      }
      if (!candidate.placement.rack.empty() &&
          descriptor->placement.rack == candidate.placement.rack) {
        ++usage.per_rack;
      }
      if (!candidate.placement.pod.empty() && descriptor->placement.pod == candidate.placement.pod) {
        ++usage.per_pod;
      }
      if (!candidate.placement.site.empty() &&
          descriptor->placement.site == candidate.placement.site) {
        ++usage.per_site;
      }
      if (std::find(domains.begin(), domains.end(), descriptor->placement.failure_domain) ==
          domains.end()) {
        domains.push_back(descriptor->placement.failure_domain);
      }
    }
  }
  usage.distinct_domains = static_cast<std::uint32_t>(domains.size());
  return usage;
}

[[nodiscard]] std::string target_label(const TargetDescriptor& descriptor) {
  if (descriptor.name.empty()) {
    return descriptor.id.to_hex().substr(0, 12);
  }
  return descriptor.name;
}

}  // namespace

const ActionSpec* primary_action(const StageRuntime& stage) noexcept {
  for (const ActionSpec& action : stage.actions) {
    switch (action.kind) {
      case ActionKind::kApplyConfiguration:
      case ActionKind::kRestart:
      case ActionKind::kDrain:
      case ActionKind::kUpgrade:
      case ActionKind::kCustom:
        return &action;
      case ActionKind::kVerify:
      case ActionKind::kRollback:
      case ActionKind::kCompensate:
        break;
    }
  }
  return stage.actions.empty() ? nullptr : &stage.actions.front();
}

const ActionSpec* compensation_action(const StageRuntime& stage, std::uint32_t step) noexcept {
  if (step >= stage.policy.compensation_actions.size()) {
    return nullptr;
  }
  const ActionId& id = stage.policy.compensation_actions[step];
  for (const ActionSpec& action : stage.actions) {
    if (action.id == id) {
      return &action;
    }
  }
  return nullptr;
}

Status blast_radius_admits(const Rollout& rollout, const StageRuntime& stage,
                           const TargetInventory& inventory, const TargetId& candidate,
                           std::string* reason) {
  const TargetDescriptor* descriptor = inventory.find(candidate);
  if (descriptor == nullptr) {
    if (reason != nullptr) {
      *reason = "target " + candidate.to_hex().substr(0, 12) + " is not in the inventory";
    }
    return make_status(StatusCode::kNotFound, "target is not in the inventory");
  }
  const BlastRadiusPolicy& policy = stage.policy.blast_radius;
  const std::uint32_t global_changing = rollout.changing_count();
  if (global_changing >= policy.max_targets_changing_global) {
    if (reason != nullptr) {
      *reason = "global blast radius: " + std::to_string(global_changing) + " of " +
                std::to_string(policy.max_targets_changing_global) + " targets are already changing";
    }
    return make_status(StatusCode::kPolicyViolation, "global blast radius exhausted");
  }
  const std::uint32_t stage_changing = stage.changing_count();
  if (stage_changing >= policy.max_targets_changing_per_stage) {
    if (reason != nullptr) {
      *reason = "stage blast radius: " + std::to_string(stage_changing) + " of " +
                std::to_string(policy.max_targets_changing_per_stage) +
                " targets are already changing in this stage";
    }
    return make_status(StatusCode::kPolicyViolation, "stage blast radius exhausted");
  }

  const DimensionUsage usage = usage_around(rollout, inventory, *descriptor);
  if (usage.per_failure_domain >= policy.max_targets_changing_per_failure_domain) {
    if (reason != nullptr) {
      *reason = "failure domain " + descriptor->placement.failure_domain.to_hex().substr(0, 12) +
                " already has " + std::to_string(usage.per_failure_domain) + " of " +
                std::to_string(policy.max_targets_changing_per_failure_domain) +
                " targets changing";
    }
    return make_status(StatusCode::kPolicyViolation, "failure-domain blast radius exhausted");
  }
  if (!usage.domain_present && usage.distinct_domains >= policy.max_failure_domains_changing) {
    if (reason != nullptr) {
      *reason = "already " + std::to_string(usage.distinct_domains) + " failure domain(s) changing, "
                "the limit is " + std::to_string(policy.max_failure_domains_changing);
    }
    return make_status(StatusCode::kPolicyViolation, "failure-domain count limit reached");
  }
  if (!descriptor->placement.rack.empty() &&
      usage.per_rack >= policy.max_targets_changing_per_rack) {
    if (reason != nullptr) {
      *reason = "rack " + descriptor->placement.rack + " already has " +
                std::to_string(usage.per_rack) + " of " +
                std::to_string(policy.max_targets_changing_per_rack) + " targets changing";
    }
    return make_status(StatusCode::kPolicyViolation, "rack blast radius exhausted");
  }
  if (!descriptor->placement.pod.empty() && usage.per_pod >= policy.max_targets_changing_per_pod) {
    if (reason != nullptr) {
      *reason = "pod " + descriptor->placement.pod + " already has " +
                std::to_string(usage.per_pod) + " of " +
                std::to_string(policy.max_targets_changing_per_pod) + " targets changing";
    }
    return make_status(StatusCode::kPolicyViolation, "pod blast radius exhausted");
  }
  if (!descriptor->placement.site.empty() && usage.per_site >= policy.max_targets_changing_per_site) {
    if (reason != nullptr) {
      *reason = "site " + descriptor->placement.site + " already has " +
                std::to_string(usage.per_site) + " of " +
                std::to_string(policy.max_targets_changing_per_site) + " targets changing";
    }
    return make_status(StatusCode::kPolicyViolation, "site blast radius exhausted");
  }
  return Status::success();
}

DispatchPlan plan_dispatch(const Rollout& rollout, const StageRuntime& stage,
                           const TargetInventory& inventory, const RuntimeLimits& limits) {
  DispatchPlan plan;
  plan.changing_before = rollout.changing_count();

  const BlastRadiusPolicy& radius = stage.policy.blast_radius;
  if (rollout.changing_count() >= radius.max_targets_changing_global) {
    plan.rejected.push_back(RejectedAlternative{
        "dispatch",
        "global blast radius: " + std::to_string(rollout.changing_count()) + " of " +
            std::to_string(radius.max_targets_changing_global) + " targets are already changing"});
    return plan;
  }
  if (stage.changing_count() >= radius.max_targets_changing_per_stage) {
    plan.rejected.push_back(RejectedAlternative{
        "dispatch",
        "stage blast radius: " + std::to_string(stage.changing_count()) + " of " +
            std::to_string(radius.max_targets_changing_per_stage) +
            " targets are already changing in this stage"});
    return plan;
  }

  // max_concurrency bounds how many attempts may be in flight at once, so the
  // budget for this tick is what is left of it, not the whole limit again.
  const std::uint64_t in_flight = stage.changing_count();
  const std::uint64_t concurrency_room =
      in_flight >= stage.policy.max_concurrency ? 0 : stage.policy.max_concurrency - in_flight;
  std::uint64_t capacity = std::min<std::uint64_t>(concurrency_room, limits.max_dispatch_batch);
  const std::uint64_t stage_room =
      radius.max_targets_changing_per_stage - stage.changing_count();
  capacity = std::min<std::uint64_t>(capacity, stage_room);
  const std::uint64_t global_room = radius.max_targets_changing_global - rollout.changing_count();
  capacity = std::min<std::uint64_t>(capacity, global_room);

  // The canary bounds how many *new* targets may be admitted before the batch
  // clears. A retry of a target that is already part of the batch is not a new
  // admission, so it is still dispatched; holding it back would stall the
  // rollout behind a batch that can never make progress.
  std::uint32_t canary_admitted = 0;
  for (const TargetRuntime& target : stage.targets) {
    if (target.attempts_created > 0) {
      ++canary_admitted;
    }
  }
  const bool canary_active = !stage.canary_cleared && stage.policy.canary_size > 0;
  // Counted as the batch is built: the bound applies to the admissions made in
  // this tick as well as to the targets admitted by earlier ticks.
  std::uint32_t new_admissions = 0;
  if (capacity == 0) {
    plan.rejected.push_back(RejectedAlternative{
        "dispatch", "no dispatch capacity is available under the current concurrency and blast "
                    "radius limits"});
    return plan;
  }

  const ActionSpec* action = primary_action(stage);
  if (action == nullptr) {
    plan.rejected.push_back(RejectedAlternative{"dispatch", "the stage declares no action to run"});
    return plan;
  }

  for (const TargetRuntime& target : stage.targets) {
    if (plan.admitted.size() >= capacity) {
      plan.rejected.push_back(RejectedAlternative{
          target.target.to_hex().substr(0, 12),
          "the dispatch batch for this tick is full (" + std::to_string(capacity) + ")"});
      continue;
    }
    if (target.state != TargetState::kPending) {
      continue;
    }
    const bool canary_full_now =
        canary_active &&
        (canary_admitted + new_admissions) >= stage.policy.canary_size;
    if (canary_full_now && target.attempts_created == 0) {
      plan.rejected.push_back(RejectedAlternative{
          target.target.to_hex().substr(0, 12),
          "the canary batch of " + std::to_string(stage.policy.canary_size) +
              " target(s) has been dispatched and has not yet cleared its health gate"});
      continue;
    }
    std::string reason;
    const Status status = blast_radius_admits(rollout, stage, inventory, target.target, &reason);
    if (!status.ok()) {
      const TargetDescriptor* descriptor = inventory.find(target.target);
      plan.rejected.push_back(
          RejectedAlternative{descriptor != nullptr ? target_label(*descriptor)
                                                    : target.target.to_hex().substr(0, 12),
                              reason});
      continue;
    }
    DispatchPlanEntry entry;
    entry.target = target.target;
    entry.epoch = AttemptEpoch{value_of(target.epoch) + 1};
    entry.action = action->id;
    entry.attempt_number = target.attempts_created + 1;
    if (target.attempts_created == 0) {
      ++new_admissions;
    }
    plan.admitted.push_back(entry);
  }
  return plan;
}

}  // namespace rollout_fabric
