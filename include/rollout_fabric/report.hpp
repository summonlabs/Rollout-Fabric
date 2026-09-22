// Rollout Fabric - deterministic rendering of state, decisions and cohorts.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Rendering is pure: it reads a value and returns text. Two runs over the same
// state produce byte-identical output, which is what makes the golden-output
// determinism tests meaningful.
#pragma once

#include <string>
#include <vector>

#include "rollout_fabric/decision.hpp"
#include "rollout_fabric/evidence.hpp"
#include "rollout_fabric/json.hpp"
#include "rollout_fabric/limits.hpp"
#include "rollout_fabric/rollout.hpp"
#include "rollout_fabric/selector.hpp"
#include "rollout_fabric/target.hpp"

namespace rollout_fabric {

[[nodiscard]] std::string render_status(const Rollout& rollout,
                                        const TargetInventory& inventory,
                                        const RuntimeLimits& limits);

[[nodiscard]] std::string render_stage(const StageRuntime& stage, const TargetInventory& inventory);

[[nodiscard]] std::string render_cohort_membership(const CohortMembership& membership,
                                                   const TargetInventory& inventory);

[[nodiscard]] std::string render_decision(const Decision& decision);
[[nodiscard]] std::string render_decisions(const std::vector<Decision>& decisions);
[[nodiscard]] std::string render_gate_evaluation(const GateEvaluation& evaluation, std::string_view label);

// Machine readable rendering used by --json. Field order is fixed.
[[nodiscard]] JsonValue status_to_json(const Rollout& rollout, const TargetInventory& inventory);
[[nodiscard]] JsonValue decision_to_json(const Decision& decision);
[[nodiscard]] JsonValue cohort_to_json(const CohortMembership& membership, const TargetInventory& inventory);

[[nodiscard]] std::string render_target_brief(const TargetDescriptor& descriptor);

}  // namespace rollout_fabric
