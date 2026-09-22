// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "rollout_fabric/target.hpp"

#include <algorithm>

#include "decode_util.hpp"

namespace rollout_fabric {
namespace {

using detail::dc_bool;
using detail::dc_digest;
using detail::dc_id;
using detail::dc_string;
using detail::dc_u32;
using detail::dc_u8;
using detail::wr_digest;
using detail::wr_id;

[[nodiscard]] Status validate_text(const std::string& text, std::size_t max_bytes, const char* field) {
  if (text.size() > max_bytes) {
    return make_status(StatusCode::kLimitExceeded,
                       std::string(field) + " is " + std::to_string(text.size()) +
                           " bytes, above the maximum of " + std::to_string(max_bytes));
  }
  if (!is_valid_utf8(text)) {
    return make_status(StatusCode::kInvalidArgument, std::string(field) + " is not valid UTF-8");
  }
  return Status::success();
}

}  // namespace

std::string_view to_string(TargetKind kind) noexcept {
  switch (kind) {
    case TargetKind::kSwitch: return "switch";
    case TargetKind::kDevice: return "device";
    case TargetKind::kRack: return "rack";
    case TargetKind::kPod: return "pod";
    case TargetKind::kSite: return "site";
    case TargetKind::kFailureDomain: return "failure_domain";
  }
  return "unknown";
}

std::optional<TargetKind> parse_target_kind(std::string_view text) noexcept {
  if (text == "switch") return TargetKind::kSwitch;
  if (text == "device") return TargetKind::kDevice;
  if (text == "rack") return TargetKind::kRack;
  if (text == "pod") return TargetKind::kPod;
  if (text == "site") return TargetKind::kSite;
  if (text == "failure_domain" || text == "failure-domain" || text == "domain") {
    return TargetKind::kFailureDomain;
  }
  return std::nullopt;
}

std::optional<std::string_view> TargetDescriptor::label_value(std::string_view key) const noexcept {
  const auto found = std::lower_bound(labels.begin(), labels.end(), key,
                                      [](const Label& label, std::string_view probe) {
                                        return label.key < probe;
                                      });
  if (found == labels.end() || found->key != key) {
    return std::nullopt;
  }
  return found->value;
}

Digest256 TargetDescriptor::descriptor_digest() const {
  ByteWriter writer;
  writer.string("rollout-fabric/target-descriptor/v1");
  wr_id(writer, id);
  writer.string(name);
  writer.u8(static_cast<std::uint8_t>(kind));
  writer.string(placement.site);
  writer.string(placement.pod);
  writer.string(placement.rack);
  wr_id(writer, placement.failure_domain);
  writer.u32(static_cast<std::uint32_t>(labels.size()));
  for (const Label& label : labels) {
    writer.string(label.key);
    writer.string(label.value);
  }
  return sha256(writer.span());
}

Status TargetDescriptor::validate(const RuntimeLimits& limits) const {
  if (id.is_nil()) {
    return make_status(StatusCode::kInvalidArgument, "target identifier is nil");
  }
  Status status = validate_text(name, limits.max_name_bytes, "target name");
  if (!status.ok()) {
    return status;
  }
  if (name.empty()) {
    return make_status(StatusCode::kInvalidArgument, "target name must not be empty");
  }
  status = validate_text(placement.site, limits.max_label_value_bytes, "placement.site");
  if (!status.ok()) {
    return status;
  }
  status = validate_text(placement.pod, limits.max_label_value_bytes, "placement.pod");
  if (!status.ok()) {
    return status;
  }
  status = validate_text(placement.rack, limits.max_label_value_bytes, "placement.rack");
  if (!status.ok()) {
    return status;
  }
  if (placement.failure_domain.is_nil()) {
    return make_status(StatusCode::kInvalidArgument,
                       "placement.failure_domain must name a domain; an unnamed failure domain "
                       "would make correlated-change accounting meaningless");
  }
  if (labels.size() > limits.max_labels_per_target) {
    return make_status(StatusCode::kLimitExceeded,
                       "target declares " + std::to_string(labels.size()) +
                           " labels, above max_labels_per_target");
  }
  for (std::size_t index = 0; index < labels.size(); ++index) {
    const Label& label = labels[index];
    status = validate_text(label.key, limits.max_label_key_bytes, "label key");
    if (!status.ok()) {
      return status;
    }
    status = validate_text(label.value, limits.max_label_value_bytes, "label value");
    if (!status.ok()) {
      return status;
    }
    if (label.key.empty()) {
      return make_status(StatusCode::kInvalidArgument, "label key must not be empty");
    }
    if (index > 0 && !(labels[index - 1] < label)) {
      return make_status(StatusCode::kInvalidArgument,
                         "labels must be sorted by key with at most one entry per key; '" +
                             label.key + "' breaks that invariant");
    }
  }
  return Status::success();
}

void encode(ByteWriter& writer, const TargetDescriptor& value) {
  wr_id(writer, value.id);
  writer.string(value.name);
  writer.u8(static_cast<std::uint8_t>(value.kind));
  writer.string(value.placement.site);
  writer.string(value.placement.pod);
  writer.string(value.placement.rack);
  wr_id(writer, value.placement.failure_domain);
  writer.u32(static_cast<std::uint32_t>(value.labels.size()));
  for (const Label& label : value.labels) {
    writer.string(label.key);
    writer.string(label.value);
  }
}

Result<TargetDescriptor> decode_target_descriptor(ByteReader& reader, const RuntimeLimits& limits) {
  TargetDescriptor value;
  RF_TRY(dc_id(reader, "target.id", value.id));
  RF_TRY(dc_string(reader, "target.name", limits.max_name_bytes, value.name));
  std::uint8_t kind = 0;
  RF_TRY(dc_u8(reader, "target.kind", kind));
  if (kind > static_cast<std::uint8_t>(TargetKind::kFailureDomain)) {
    return make_status(StatusCode::kCorrupt, "unknown target kind " + std::to_string(kind));
  }
  value.kind = static_cast<TargetKind>(kind);
  RF_TRY(dc_string(reader, "target.placement.site", limits.max_label_value_bytes, value.placement.site));
  RF_TRY(dc_string(reader, "target.placement.pod", limits.max_label_value_bytes, value.placement.pod));
  RF_TRY(dc_string(reader, "target.placement.rack", limits.max_label_value_bytes, value.placement.rack));
  RF_TRY(dc_id(reader, "target.placement.failure_domain", value.placement.failure_domain));
  std::uint32_t label_count = 0;
  RF_TRY(dc_u32(reader, "target.labels", label_count));
  if (label_count > limits.max_labels_per_target) {
    return make_status(StatusCode::kLimitExceeded, "target label count exceeds max_labels_per_target");
  }
  value.labels.reserve(label_count);
  for (std::uint32_t index = 0; index < label_count; ++index) {
    Label label;
    RF_TRY(dc_string(reader, "label.key", limits.max_label_key_bytes, label.key));
    RF_TRY(dc_string(reader, "label.value", limits.max_label_value_bytes, label.value));
    value.labels.push_back(std::move(label));
  }
  const Status status = value.validate(limits);
  if (!status.ok()) {
    return status;
  }
  return value;
}

Status TargetInventory::add(TargetDescriptor descriptor, const RuntimeLimits& limits) {
  const Status status = descriptor.validate(limits);
  if (!status.ok()) {
    return status;
  }
  const auto found = std::lower_bound(targets_.begin(), targets_.end(), descriptor.id,
                                      [](const TargetDescriptor& entry, const TargetId& probe) {
                                        return entry.id < probe;
                                      });
  if (found != targets_.end() && found->id == descriptor.id) {
    return make_status(StatusCode::kAlreadyExists,
                       "target " + descriptor.id.to_hex() + " is already registered");
  }
  if (targets_.size() >= limits.max_total_targets) {
    return make_status(StatusCode::kLimitExceeded,
                       "inventory holds " + std::to_string(targets_.size()) +
                           " targets, at max_total_targets");
  }
  targets_.insert(found, std::move(descriptor));
  reindex();
  bump_epoch();
  return Status::success();
}

Status TargetInventory::remove(const TargetId& id) {
  const auto found = std::lower_bound(targets_.begin(), targets_.end(), id,
                                      [](const TargetDescriptor& entry, const TargetId& probe) {
                                        return entry.id < probe;
                                      });
  if (found == targets_.end() || !(found->id == id)) {
    return make_status(StatusCode::kNotFound, "target " + id.to_hex() + " is not registered");
  }
  targets_.erase(found);
  reindex();
  bump_epoch();
  return Status::success();
}

const TargetDescriptor* TargetInventory::find(const TargetId& id) const noexcept {
  const auto found = index_.find(id);
  if (found == index_.end()) {
    return nullptr;
  }
  return &targets_[found->second];
}

void TargetInventory::reindex() {
  index_.clear();
  index_.reserve(targets_.size());
  for (std::size_t position = 0; position < targets_.size(); ++position) {
    index_.emplace(targets_[position].id, position);
  }
}

void TargetInventory::bump_epoch() noexcept { epoch_ = EpochCounter{value_of(epoch_) + 1}; }

Digest256 TargetInventory::inventory_digest() const {
  ByteWriter writer;
  writer.string("rollout-fabric/inventory/v1");
  writer.u32(static_cast<std::uint32_t>(targets_.size()));
  for (const TargetDescriptor& descriptor : targets_) {
    wr_digest(writer, descriptor.descriptor_digest());
  }
  return sha256(writer.span());
}

std::vector<TargetId> TargetInventory::failure_domain_members(const FailureDomainId& domain) const {
  std::vector<TargetId> members;
  for (const TargetDescriptor& descriptor : targets_) {
    if (descriptor.placement.failure_domain == domain) {
      members.push_back(descriptor.id);
    }
  }
  return members;
}

}  // namespace rollout_fabric
