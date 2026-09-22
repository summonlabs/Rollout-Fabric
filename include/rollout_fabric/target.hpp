// Rollout Fabric - targets, placement and inventory.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A target is anything the execution adapter can act on: a switch, a device, a
// rack, a pod, a site or a failure domain. Placement is the axis along which
// correlated change is measured, so it is part of the target's identity-bearing
// description and part of every membership digest.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rollout_fabric/codec.hpp"
#include "rollout_fabric/digest.hpp"
#include "rollout_fabric/ids.hpp"
#include "rollout_fabric/limits.hpp"
#include "rollout_fabric/status.hpp"

namespace rollout_fabric {

enum class TargetKind : std::uint8_t {
  kSwitch = 0,
  kDevice = 1,
  kRack = 2,
  kPod = 3,
  kSite = 4,
  kFailureDomain = 5,
};

[[nodiscard]] std::string_view to_string(TargetKind kind) noexcept;
[[nodiscard]] std::optional<TargetKind> parse_target_kind(std::string_view text) noexcept;

struct Label {
  std::string key;
  std::string value;

  friend bool operator==(const Label&, const Label&) = default;
  friend auto operator<=>(const Label& lhs, const Label& rhs) noexcept {
    if (const auto cmp = lhs.key <=> rhs.key; cmp != 0) {
      return cmp;
    }
    return lhs.value <=> rhs.value;
  }
};

// Where a target physically lives. Empty components are permitted and simply
// mean "not declared"; they never silently alias another location, because
// blast-radius accounting keys on the (site, pod, rack, failure domain) tuple
// and treats the empty string as its own bucket.
struct Placement {
  std::string site;
  std::string pod;
  std::string rack;
  FailureDomainId failure_domain{};

  friend bool operator==(const Placement&, const Placement&) = default;
};

struct TargetDescriptor {
  TargetId id{};
  std::string name;
  TargetKind kind = TargetKind::kDevice;
  Placement placement{};
  std::vector<Label> labels;  // canonical: sorted by key, at most one per key

  // Canonical encoding of every field that can influence a decision. Two
  // descriptors with equal digests are interchangeable for orchestration.
  [[nodiscard]] Digest256 descriptor_digest() const;

  [[nodiscard]] std::optional<std::string_view> label_value(std::string_view key) const noexcept;

  // Validates field sizes and the label canonicalisation invariant.
  [[nodiscard]] Status validate(const RuntimeLimits& limits) const;
};

void encode(ByteWriter& writer, const TargetDescriptor& value);
[[nodiscard]] Result<TargetDescriptor> decode_target_descriptor(ByteReader& reader, const RuntimeLimits& limits);

// A bounded, monotonically versioned collection of descriptors. The version is
// an epoch: a cohort snapshot records the epoch it was taken against, so a
// membership can always be explained in terms of the inventory that produced
// it, and a stale inventory can never silently rewrite a frozen cohort.
class TargetInventory {
 public:
  TargetInventory() = default;

  [[nodiscard]] Status add(TargetDescriptor descriptor, const RuntimeLimits& limits);
  [[nodiscard]] Status remove(const TargetId& id);

  [[nodiscard]] const TargetDescriptor* find(const TargetId& id) const noexcept;
  [[nodiscard]] const std::vector<TargetDescriptor>& all() const noexcept { return targets_; }
  [[nodiscard]] std::size_t size() const noexcept { return targets_.size(); }
  [[nodiscard]] bool empty() const noexcept { return targets_.empty(); }

  // Increments on every mutation. Callers that cache a derived view must
  // compare epochs before reusing it.
  [[nodiscard]] EpochCounter epoch() const noexcept { return epoch_; }

  // Deterministic digest over every descriptor, in canonical id order.
  [[nodiscard]] Digest256 inventory_digest() const;

  [[nodiscard]] std::vector<TargetId> failure_domain_members(const FailureDomainId& domain) const;

 private:
  void reindex();
  void bump_epoch() noexcept;

  std::vector<TargetDescriptor> targets_;  // sorted by id
  std::unordered_map<TargetId, std::size_t, StrongIdHash<TargetIdTag>> index_;
  EpochCounter epoch_{0};
};

}  // namespace rollout_fabric
