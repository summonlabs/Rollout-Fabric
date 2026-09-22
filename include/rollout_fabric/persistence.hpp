// Rollout Fabric - durable payload encoding.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The journal moves opaque bytes; this header is what gives those bytes
// meaning. Every payload is wrapped in a versioned envelope so that a record
// written by a different schema is rejected rather than misread, and every
// payload carries the digest of the value it holds so that a record which
// decodes but describes different content is still refused.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "rollout_fabric/limits.hpp"
#include "rollout_fabric/plan.hpp"
#include "rollout_fabric/rollout.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/target.hpp"

namespace rollout_fabric {

inline constexpr std::uint32_t kPayloadSchemaVersion = 1;
inline constexpr std::string_view kPayloadTag = "rollout-fabric/payload/v1";

[[nodiscard]] std::vector<std::byte> encode_inventory_payload(const TargetInventory& inventory,
                                                             const RuntimeLimits& limits);
[[nodiscard]] Result<TargetInventory> decode_inventory_payload(std::span<const std::byte> bytes,
                                                               const RuntimeLimits& limits);

[[nodiscard]] std::vector<std::byte> encode_plan_payload(const ChangePlan& plan,
                                                         const RuntimeLimits& limits);
[[nodiscard]] Result<ChangePlan> decode_plan_payload(std::span<const std::byte> bytes,
                                                     const RuntimeLimits& limits);

[[nodiscard]] std::vector<std::byte> encode_rollout_payload(const Rollout& rollout,
                                                            const RuntimeLimits& limits);
[[nodiscard]] Result<Rollout> decode_rollout_payload(std::span<const std::byte> bytes,
                                                     const RuntimeLimits& limits);

// Records the operator command that caused a mutation, so an auditor can see
// who asked for what without trusting the process that is currently running.
struct OperatorCommandRecord {
  RolloutId rollout{};
  GenerationId generation{};
  Revision revision_observed{};
  OperatorId operator_id{};
  AuthorityId authority{};
  IncarnationId incarnation{};
  EpochCounter controller_epoch{0};
  std::string command;
  std::string reason;
  Timestamp issued_at{};
};

[[nodiscard]] std::vector<std::byte> encode_operator_command(const OperatorCommandRecord& record,
                                                             const RuntimeLimits& limits);
[[nodiscard]] Result<OperatorCommandRecord> decode_operator_command(std::span<const std::byte> bytes,
                                                                    const RuntimeLimits& limits);

[[nodiscard]] std::vector<std::byte> encode_incarnation_claim(const IncarnationId& incarnation,
                                                              EpochCounter epoch,
                                                              Timestamp claimed_at);

}  // namespace rollout_fabric
