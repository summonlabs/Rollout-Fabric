// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "rollout_fabric/selector.hpp"

#include <algorithm>
#include <string>

#include "decode_util.hpp"

namespace rollout_fabric {
namespace {

using detail::dc_id;
using detail::dc_id_vector;
using detail::dc_string;
using detail::dc_string_vector;
using detail::dc_u32;
using detail::dc_u8;
using detail::wr_id;

[[nodiscard]] Status validate_selector(const SelectorTerm& term, const RuntimeLimits& limits,
                                       std::uint32_t depth, std::uint64_t& budget) {
  if (depth > kMaxSelectorDepth) {
    return make_status(StatusCode::kLimitExceeded,
                       "selector nesting depth " + std::to_string(depth) +
                           " exceeds the maximum of " + std::to_string(kMaxSelectorDepth));
  }
  const std::uint64_t terms = static_cast<std::uint64_t>(term.ids.size()) + term.kinds.size() +
                              term.sites.size() + term.pods.size() + term.racks.size() +
                              term.failure_domains.size() + term.children.size() + 1u;
  if (!checked_add(budget, terms, budget)) {
    return make_status(StatusCode::kLimitExceeded, "selector term count overflowed");
  }
  if (budget > limits.max_selector_terms) {
    return make_status(StatusCode::kLimitExceeded,
                       "selector uses " + std::to_string(budget) + " terms, above max_selector_terms");
  }
  const auto require_empty_children = [&](bool condition) -> Status {
    if (condition) {
      return Status::success();
    }
    return make_status(StatusCode::kInvalidArgument,
                       "selector operator " + std::string(to_string(term.op)) +
                           " does not take children");
  };

  switch (term.op) {
    case SelectorOp::kAll:
      return require_empty_children(term.children.empty());
    case SelectorOp::kByIds:
      if (term.ids.empty() || term.ids.size() > limits.max_targets_per_cohort) {
        return make_status(StatusCode::kInvalidArgument,
                           "by_ids requires between 1 and max_targets_per_cohort identifiers");
      }
      return require_empty_children(term.children.empty());
    case SelectorOp::kByKind:
      if (term.kinds.empty()) {
        return make_status(StatusCode::kInvalidArgument, "by_kind requires at least one kind");
      }
      return require_empty_children(term.children.empty());
    case SelectorOp::kBySite:
      if (term.sites.empty()) {
        return make_status(StatusCode::kInvalidArgument, "by_site requires at least one site");
      }
      return require_empty_children(term.children.empty());
    case SelectorOp::kByPod:
      if (term.pods.empty()) {
        return make_status(StatusCode::kInvalidArgument, "by_pod requires at least one pod");
      }
      return require_empty_children(term.children.empty());
    case SelectorOp::kByRack:
      if (term.racks.empty()) {
        return make_status(StatusCode::kInvalidArgument, "by_rack requires at least one rack");
      }
      return require_empty_children(term.children.empty());
    case SelectorOp::kByFailureDomain:
      if (term.failure_domains.empty()) {
        return make_status(StatusCode::kInvalidArgument,
                           "by_failure_domain requires at least one domain");
      }
      return require_empty_children(term.children.empty());
    case SelectorOp::kByLabel:
      if (term.label_key.empty()) {
        return make_status(StatusCode::kInvalidArgument, "by_label requires a label key");
      }
      if (term.label_key.size() > limits.max_label_key_bytes ||
          term.label_value.size() > limits.max_label_value_bytes) {
        return make_status(StatusCode::kLimitExceeded, "by_label key or value exceeds the size bound");
      }
      return require_empty_children(term.children.empty());
    case SelectorOp::kAnd:
    case SelectorOp::kOr:
      if (term.children.empty()) {
        return make_status(StatusCode::kInvalidArgument,
                           std::string(to_string(term.op)) + " requires at least one child");
      }
      break;
    case SelectorOp::kNot:
      if (term.children.size() != 1) {
        return make_status(StatusCode::kInvalidArgument, "not requires exactly one child");
      }
      break;
  }
  for (const SelectorTerm& child : term.children) {
    const Status status = validate_selector(child, limits, depth + 1, budget);
    if (!status.ok()) {
      return status;
    }
  }
  return Status::success();
}

void encode_selector(ByteWriter& writer, const SelectorTerm& term) {
  writer.u8(static_cast<std::uint8_t>(term.op));
  writer.u32(static_cast<std::uint32_t>(term.ids.size()));
  for (const TargetId& id : term.ids) {
    wr_id(writer, id);
  }
  writer.u32(static_cast<std::uint32_t>(term.kinds.size()));
  for (const TargetKind kind : term.kinds) {
    writer.u8(static_cast<std::uint8_t>(kind));
  }
  writer.u32(static_cast<std::uint32_t>(term.sites.size()));
  for (const std::string& site : term.sites) {
    writer.string(site);
  }
  writer.u32(static_cast<std::uint32_t>(term.pods.size()));
  for (const std::string& pod : term.pods) {
    writer.string(pod);
  }
  writer.u32(static_cast<std::uint32_t>(term.racks.size()));
  for (const std::string& rack : term.racks) {
    writer.string(rack);
  }
  writer.u32(static_cast<std::uint32_t>(term.failure_domains.size()));
  for (const FailureDomainId& domain : term.failure_domains) {
    wr_id(writer, domain);
  }
  writer.string(term.label_key);
  writer.string(term.label_value);
  writer.boolean(term.label_value_is_set);
  writer.u32(static_cast<std::uint32_t>(term.children.size()));
  for (const SelectorTerm& child : term.children) {
    encode_selector(writer, child);
  }
}

[[nodiscard]] Result<SelectorTerm> decode_selector(ByteReader& reader, const RuntimeLimits& limits,
                                                   std::uint32_t depth) {
  if (depth > kMaxSelectorDepth) {
    return make_status(StatusCode::kLimitExceeded, "selector nesting exceeds the maximum depth");
  }
  SelectorTerm term;
  std::uint8_t op = 0;
  RF_TRY(dc_u8(reader, "selector.op", op));
  if (op > static_cast<std::uint8_t>(SelectorOp::kNot)) {
    return make_status(StatusCode::kCorrupt, "unknown selector operator " + std::to_string(op));
  }
  term.op = static_cast<SelectorOp>(op);

  std::uint32_t count = 0;
  RF_TRY(dc_u32(reader, "selector.ids", count));
  if (count > limits.max_targets_per_cohort) {
    return make_status(StatusCode::kLimitExceeded, "selector id list exceeds max_targets_per_cohort");
  }
  term.ids.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    TargetId id{};
    RF_TRY(dc_id(reader, "selector.ids[]", id));
    term.ids.push_back(id);
  }

  RF_TRY(dc_u32(reader, "selector.kinds", count));
  if (count > 16u) {
    return make_status(StatusCode::kLimitExceeded, "selector kind list is too long");
  }
  term.kinds.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    std::uint8_t kind = 0;
    RF_TRY(dc_u8(reader, "selector.kinds[]", kind));
    if (kind > static_cast<std::uint8_t>(TargetKind::kFailureDomain)) {
      return make_status(StatusCode::kCorrupt, "unknown target kind in selector");
    }
    term.kinds.push_back(static_cast<TargetKind>(kind));
  }

  RF_TRY(dc_string_vector(reader, "selector.sites", limits.max_selector_terms,
                          limits.max_label_value_bytes, term.sites));
  RF_TRY(dc_string_vector(reader, "selector.pods", limits.max_selector_terms,
                          limits.max_label_value_bytes, term.pods));
  RF_TRY(dc_string_vector(reader, "selector.racks", limits.max_selector_terms,
                          limits.max_label_value_bytes, term.racks));
  RF_TRY(dc_id_vector(reader, "selector.failure_domains", limits.max_selector_terms,
                      term.failure_domains));
  RF_TRY(dc_string(reader, "selector.label_key", limits.max_label_key_bytes, term.label_key));
  RF_TRY(dc_string(reader, "selector.label_value", limits.max_label_value_bytes, term.label_value));
  RF_TRY(detail::dc_bool(reader, "selector.label_value_is_set", term.label_value_is_set));
  RF_TRY(dc_u32(reader, "selector.children", count));
  if (count > limits.max_selector_terms) {
    return make_status(StatusCode::kLimitExceeded, "selector child count exceeds max_selector_terms");
  }
  term.children.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    auto child = decode_selector(reader, limits, depth + 1);
    if (!child.ok()) {
      return child.status();
    }
    term.children.push_back(std::move(child).value());
  }
  return term;
}

[[nodiscard]] bool contains_string(const std::vector<std::string>& values, std::string_view probe) {
  return std::find(values.begin(), values.end(), probe) != values.end();
}

}  // namespace

std::string_view to_string(SelectorOp op) noexcept {
  switch (op) {
    case SelectorOp::kAll: return "all";
    case SelectorOp::kByIds: return "by_ids";
    case SelectorOp::kByKind: return "by_kind";
    case SelectorOp::kBySite: return "by_site";
    case SelectorOp::kByPod: return "by_pod";
    case SelectorOp::kByRack: return "by_rack";
    case SelectorOp::kByFailureDomain: return "by_failure_domain";
    case SelectorOp::kByLabel: return "by_label";
    case SelectorOp::kAnd: return "and";
    case SelectorOp::kOr: return "or";
    case SelectorOp::kNot: return "not";
  }
  return "unknown";
}

std::optional<SelectorOp> parse_selector_op(std::string_view text) noexcept {
  if (text == "all") return SelectorOp::kAll;
  if (text == "by_ids" || text == "by-ids") return SelectorOp::kByIds;
  if (text == "by_kind" || text == "by-kind") return SelectorOp::kByKind;
  if (text == "by_site" || text == "by-site") return SelectorOp::kBySite;
  if (text == "by_pod" || text == "by-pod") return SelectorOp::kByPod;
  if (text == "by_rack" || text == "by-rack") return SelectorOp::kByRack;
  if (text == "by_failure_domain" || text == "by-failure-domain") return SelectorOp::kByFailureDomain;
  if (text == "by_label" || text == "by-label") return SelectorOp::kByLabel;
  if (text == "and") return SelectorOp::kAnd;
  if (text == "or") return SelectorOp::kOr;
  if (text == "not") return SelectorOp::kNot;
  return std::nullopt;
}

Status SelectorTerm::validate(const RuntimeLimits& limits) const {
  std::uint64_t budget = 0;
  return validate_selector(*this, limits, 0, budget);
}

Digest256 SelectorTerm::selector_digest() const {
  ByteWriter writer;
  writer.string("rollout-fabric/selector/v1");
  encode_selector(writer, *this);
  return sha256(writer.span());
}

std::string SelectorTerm::describe() const {
  std::string text;
  switch (op) {
    case SelectorOp::kAll:
      return "all targets";
    case SelectorOp::kByIds: {
      text = "targets {";
      for (std::size_t index = 0; index < ids.size(); ++index) {
        if (index != 0) {
          text.append(", ");
        }
        text.append(ids[index].to_hex().substr(0, 12));
      }
      text.append("}");
      return text;
    }
    case SelectorOp::kByKind: {
      text = "kinds {";
      for (std::size_t index = 0; index < kinds.size(); ++index) {
        if (index != 0) {
          text.append(", ");
        }
        text.append(to_string(kinds[index]));
      }
      text.append("}");
      return text;
    }
    case SelectorOp::kBySite: {
      text = "sites {";
      for (std::size_t index = 0; index < sites.size(); ++index) {
        if (index != 0) {
          text.append(", ");
        }
        text.append(sites[index]);
      }
      text.append("}");
      return text;
    }
    case SelectorOp::kByPod: {
      text = "pods {";
      for (std::size_t index = 0; index < pods.size(); ++index) {
        if (index != 0) {
          text.append(", ");
        }
        text.append(pods[index]);
      }
      text.append("}");
      return text;
    }
    case SelectorOp::kByRack: {
      text = "racks {";
      for (std::size_t index = 0; index < racks.size(); ++index) {
        if (index != 0) {
          text.append(", ");
        }
        text.append(racks[index]);
      }
      text.append("}");
      return text;
    }
    case SelectorOp::kByFailureDomain: {
      text = "failure_domains {";
      for (std::size_t index = 0; index < failure_domains.size(); ++index) {
        if (index != 0) {
          text.append(", ");
        }
        text.append(failure_domains[index].to_hex().substr(0, 12));
      }
      text.append("}");
      return text;
    }
    case SelectorOp::kByLabel:
      text = "label " + label_key;
      if (label_value_is_set) {
        text.append("=").append(label_value);
      } else {
        text.append(" is present");
      }
      return text;
    case SelectorOp::kAnd:
    case SelectorOp::kOr:
    case SelectorOp::kNot: {
      text = to_string(op);
      text.append("(");
      for (std::size_t index = 0; index < children.size(); ++index) {
        if (index != 0) {
          text.append(", ");
        }
        text.append(children[index].describe());
      }
      text.append(")");
      return text;
    }
  }
  return "unknown selector";
}

void encode(ByteWriter& writer, const SelectorTerm& value) { encode_selector(writer, value); }

Result<SelectorTerm> decode_selector_term(ByteReader& reader, const RuntimeLimits& limits) {
  auto term = decode_selector(reader, limits, 0);
  if (!term.ok()) {
    return term.status();
  }
  const Status status = term.value().validate(limits);
  if (!status.ok()) {
    return status;
  }
  return term;
}

bool selector_matches(const SelectorTerm& term, const TargetDescriptor& target) noexcept {
  switch (term.op) {
    case SelectorOp::kAll:
      return true;
    case SelectorOp::kByIds:
      return std::find(term.ids.begin(), term.ids.end(), target.id) != term.ids.end();
    case SelectorOp::kByKind:
      return std::find(term.kinds.begin(), term.kinds.end(), target.kind) != term.kinds.end();
    case SelectorOp::kBySite:
      return contains_string(term.sites, target.placement.site);
    case SelectorOp::kByPod:
      return contains_string(term.pods, target.placement.pod);
    case SelectorOp::kByRack:
      return contains_string(term.racks, target.placement.rack);
    case SelectorOp::kByFailureDomain:
      return std::find(term.failure_domains.begin(), term.failure_domains.end(),
                       target.placement.failure_domain) != term.failure_domains.end();
    case SelectorOp::kByLabel: {
      const auto value = target.label_value(term.label_key);
      if (!value.has_value()) {
        return false;
      }
      if (!term.label_value_is_set) {
        return true;
      }
      return *value == term.label_value;
    }
    case SelectorOp::kAnd:
      for (const SelectorTerm& child : term.children) {
        if (!selector_matches(child, target)) {
          return false;
        }
      }
      return true;
    case SelectorOp::kOr:
      for (const SelectorTerm& child : term.children) {
        if (selector_matches(child, target)) {
          return true;
        }
      }
      return false;
    case SelectorOp::kNot:
      return !term.children.empty() && !selector_matches(term.children.front(), target);
  }
  return false;
}

Digest256 compute_membership_digest(const SelectorTerm& selector, const std::vector<TargetId>& members,
                                    const Digest256& inventory_digest) noexcept {
  ByteWriter writer;
  writer.string("rollout-fabric/cohort-membership/v1");
  writer.raw(std::span<const std::byte>(inventory_digest.data(), inventory_digest.size()));
  const Digest256 selector_hash = selector.selector_digest();
  writer.raw(std::span<const std::byte>(selector_hash.data(), selector_hash.size()));
  writer.u32(static_cast<std::uint32_t>(members.size()));
  for (const TargetId& member : members) {
    writer.raw(member.span());
  }
  return sha256(writer.span());
}

bool CohortMembership::contains(const TargetId& probe) const noexcept {
  return std::binary_search(members.begin(), members.end(), probe);
}

std::vector<TargetId> CohortMembership::domain_members(const FailureDomainId& domain,
                                                       const TargetInventory& inventory) const {
  std::vector<TargetId> result;
  for (const TargetId& member : members) {
    const TargetDescriptor* descriptor = inventory.find(member);
    if (descriptor != nullptr && descriptor->placement.failure_domain == domain) {
      result.push_back(member);
    }
  }
  return result;
}

Result<CohortMaterialization> materialize_cohort(const SelectorTerm& selector,
                                                 const TargetInventory& inventory,
                                                 const StageId& stage,
                                                 const GenerationId& generation,
                                                 const RuntimeLimits& limits,
                                                 Timestamp now) {
  const Status selector_status = selector.validate(limits);
  if (!selector_status.ok()) {
    return selector_status;
  }

  std::vector<TargetId> matched;
  matched.reserve(inventory.size());
  for (const TargetDescriptor& descriptor : inventory.all()) {
    if (selector_matches(selector, descriptor)) {
      matched.push_back(descriptor.id);
    }
  }
  std::sort(matched.begin(), matched.end());

  CohortMaterialization result;
  if (matched.size() > limits.max_targets_per_cohort) {
    result.truncated.assign(matched.begin() + static_cast<std::ptrdiff_t>(limits.max_targets_per_cohort),
                            matched.end());
    matched.resize(limits.max_targets_per_cohort);
  }

  const Digest256 inventory_digest = inventory.inventory_digest();
  CohortMembership& membership = result.membership;
  membership.stage = stage;
  membership.generation = generation;
  membership.selector = selector;
  membership.selector_digest = selector.selector_digest();
  membership.members = matched;
  membership.inventory_digest = inventory_digest;
  membership.inventory_epoch = inventory.epoch();
  membership.frozen_at = now;
  membership.frozen = false;
  membership.membership_digest = compute_membership_digest(selector, matched, inventory_digest);
  return result;
}

std::string describe_cohort(const CohortMembership& membership, const TargetInventory& inventory) {
  std::string text;
  text.append("cohort ");
  text.append(membership.id.to_hex().substr(0, 12));
  text.append(" stage=");
  text.append(membership.stage.to_hex().substr(0, 12));
  text.append(" generation=");
  text.append(membership.generation.to_hex().substr(0, 12));
  text.append(" members=");
  text.append(std::to_string(membership.size()));
  text.append(" frozen=");
  text.append(membership.frozen ? "yes" : "no");
  text.append(" selector=");
  text.append(membership.selector.describe());
  text.append("\n");
  for (const TargetId& member : membership.members) {
    const TargetDescriptor* descriptor = inventory.find(member);
    if (descriptor == nullptr) {
      text.append("  ").append(member.to_hex().substr(0, 12)).append(" (not in inventory)\n");
      continue;
    }
    text.append("  ").append(descriptor->name);
    text.append("  id=").append(member.to_hex().substr(0, 12));
    text.append("  kind=").append(to_string(descriptor->kind));
    text.append("  site=").append(descriptor->placement.site);
    text.append("  pod=").append(descriptor->placement.pod);
    text.append("  rack=").append(descriptor->placement.rack);
    text.append("  domain=").append(descriptor->placement.failure_domain.to_hex().substr(0, 12));
    text.append("\n");
  }
  return text;
}

}  // namespace rollout_fabric
