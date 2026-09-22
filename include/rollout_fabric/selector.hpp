// Rollout Fabric - cohort selection and deterministic membership snapshots.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Selection is a pure function of (inventory, selector). Materialising a
// cohort freezes the resulting member list together with the selector text, the
// inventory digest and an inventory epoch. Once a stage is armed that list is
// immutable: the only way to obtain different membership is to create a new
// rollout generation, which is an explicitly recorded event.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rollout_fabric/codec.hpp"
#include "rollout_fabric/digest.hpp"
#include "rollout_fabric/ids.hpp"
#include "rollout_fabric/limits.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/target.hpp"
#include "rollout_fabric/time.hpp"

namespace rollout_fabric {

enum class SelectorOp : std::uint8_t {
  kAll = 0,
  kByIds = 1,
  kByKind = 2,
  kBySite = 3,
  kByPod = 4,
  kByRack = 5,
  kByFailureDomain = 6,
  kByLabel = 7,
  kAnd = 8,
  kOr = 9,
  kNot = 10,
};

[[nodiscard]] std::string_view to_string(SelectorOp op) noexcept;
[[nodiscard]] std::optional<SelectorOp> parse_selector_op(std::string_view text) noexcept;

inline constexpr std::uint32_t kMaxSelectorDepth = 8;

struct SelectorTerm {
  SelectorOp op = SelectorOp::kAll;

  std::vector<TargetId> ids;                  // kByIds
  std::vector<TargetKind> kinds;              // kByKind
  std::vector<std::string> sites;             // kBySite
  std::vector<std::string> pods;              // kByPod
  std::vector<std::string> racks;             // kByRack
  std::vector<FailureDomainId> failure_domains;  // kByFailureDomain
  std::string label_key;                      // kByLabel
  std::string label_value;                    // kByLabel (empty means "key present")
  bool label_value_is_set = false;
  std::vector<SelectorTerm> children;         // kAnd / kOr / kNot

  [[nodiscard]] Digest256 selector_digest() const;

  // Validates operator arity, field sizes and nesting depth.
  [[nodiscard]] Status validate(const RuntimeLimits& limits) const;

  // Renders a stable, human readable form used in explanations and in
  // "why is this target in the cohort" answers.
  [[nodiscard]] std::string describe() const;
};

void encode(ByteWriter& writer, const SelectorTerm& value);
[[nodiscard]] Result<SelectorTerm> decode_selector_term(ByteReader& reader, const RuntimeLimits& limits);

// Evaluates a selector against one descriptor.
[[nodiscard]] bool selector_matches(const SelectorTerm& term, const TargetDescriptor& target) noexcept;

struct CohortMembership {
  CohortId id{};
  StageId stage{};
  GenerationId generation{};
  SelectorTerm selector{};
  Digest256 selector_digest{};
  std::vector<TargetId> members{};  // canonical: sorted by identifier bytes, unique
  Digest256 membership_digest{};
  Digest256 inventory_digest{};
  EpochCounter inventory_epoch{0};
  Timestamp frozen_at{};
  // Set when the stage owning this cohort is armed. A frozen cohort cannot be
  // re-materialised in place.
  bool frozen = false;

  [[nodiscard]] std::size_t size() const noexcept { return members.size(); }
  [[nodiscard]] bool contains(const TargetId& id) const noexcept;
  [[nodiscard]] std::vector<TargetId> domain_members(const FailureDomainId& domain,
                                                     const TargetInventory& inventory) const;
};

[[nodiscard]] Digest256 compute_membership_digest(const SelectorTerm& selector,
                                                  const std::vector<TargetId>& members,
                                                  const Digest256& inventory_digest) noexcept;

struct CohortMaterialization {
  CohortMembership membership{};
  // Targets that matched the selector but were discarded because the cohort
  // would have exceeded max_targets_per_cohort. Non-empty means the plan is
  // truncated and the caller must decide whether that is acceptable.
  std::vector<TargetId> truncated{};
};

// Deterministic: members are emitted in identifier order regardless of the
// inventory's internal ordering.
[[nodiscard]] Result<CohortMaterialization> materialize_cohort(
    const SelectorTerm& selector,
    const TargetInventory& inventory,
    const StageId& stage,
    const GenerationId& generation,
    const RuntimeLimits& limits,
    Timestamp now);

// Rendering helper used by the CLI and by explanations.
[[nodiscard]] std::string describe_cohort(const CohortMembership& membership,
                                          const TargetInventory& inventory);

}  // namespace rollout_fabric
