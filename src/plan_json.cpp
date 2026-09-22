// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Document loaders for the two JSON documents this runtime consumes: the
// approved Change Planner plan and the target inventory. Both arrive from
// outside the runtime, so every value is range checked, every collection is
// bounded against RuntimeLimits before it is built, and every identifier is
// derived here rather than trusted from the document. A document that fails
// validation is never returned half-built.
#include "rollout_fabric/json.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "decode_util.hpp"
#include "rollout_fabric/evidence_types.hpp"
#include "rollout_fabric/policy.hpp"
#include "rollout_fabric/selector.hpp"
#include "rollout_fabric/target.hpp"
#include "rollout_fabric/time.hpp"

namespace rollout_fabric {
namespace {

// A document that does not name its own kind and schema version is not a
// document this loader was written for.
constexpr std::string_view kChangePlanKind = "rollout_fabric.change_plan";
constexpr std::string_view kInventoryKind = "rollout_fabric.inventory";
constexpr std::int64_t kSchemaVersion = 1;

// --- Identifier derivation --------------------------------------------------
//
// Identities are derived from the declaring name, never carried in the
// document, so two processes reading the same bytes agree on every identifier.

[[nodiscard]] TargetId derive_target_id(std::string_view name) noexcept {
  Sha256 hasher;
  hasher.update(std::string_view("rollout-fabric/target/v1"));
  hasher.update(name);
  const Digest256 digest = hasher.finish();
  return TargetId::from_bytes(
      std::span<const std::byte, TargetId::byte_size>(digest.data(), TargetId::byte_size));
}

[[nodiscard]] FailureDomainId derive_failure_domain_id(std::string_view name) noexcept {
  Sha256 hasher;
  hasher.update(std::string_view("rollout-fabric/failure-domain/v1"));
  hasher.update(name);
  const Digest256 digest = hasher.finish();
  return FailureDomainId::from_bytes(std::span<const std::byte, FailureDomainId::byte_size>(
      digest.data(), FailureDomainId::byte_size));
}

// --- Path rendering ---------------------------------------------------------

[[nodiscard]] std::string join_path(std::string_view parent, std::string_view key) {
  if (parent.empty()) {
    return std::string(key);
  }
  std::string out(parent);
  out.push_back('.');
  out.append(key);
  return out;
}

[[nodiscard]] std::string index_path(std::string_view parent, std::size_t index) {
  std::string out(parent);
  out.push_back('[');
  out.append(std::to_string(index));
  out.push_back(']');
  return out;
}

[[nodiscard]] std::string quoted(std::string_view text) { return "'" + std::string(text) + "'"; }

// --- Bounded reads ----------------------------------------------------------

[[nodiscard]] Status check_count(std::size_t count, std::size_t max_count, std::string_view path,
                                 const char* what) {
  if (count > max_count) {
    return make_status(StatusCode::kLimitExceeded,
                       std::string(path) + " declares " + std::to_string(count) + " " + what +
                           ", above the maximum of " + std::to_string(max_count));
  }
  return Status::success();
}

[[nodiscard]] Result<const JsonValue*> require_member(const JsonValue& object, std::string_view key,
                                                      std::string_view parent) {
  const JsonValue* member = object.find(key);
  if (member == nullptr) {
    return make_status(StatusCode::kInvalidArgument, join_path(parent, key) + " is required");
  }
  return member;
}

[[nodiscard]] Result<std::string> read_required_string(const JsonValue& object, std::string_view key,
                                                       std::string_view parent) {
  auto member = require_member(object, key, parent);
  if (!member.ok()) {
    return member.status();
  }
  return member.value()->require_string(join_path(parent, key));
}

[[nodiscard]] Result<std::int64_t> read_required_integer(const JsonValue& object,
                                                         std::string_view key,
                                                         std::string_view parent) {
  auto member = require_member(object, key, parent);
  if (!member.ok()) {
    return member.status();
  }
  return member.value()->require_integer(join_path(parent, key));
}

[[nodiscard]] Result<std::string> read_optional_string(const JsonValue& object, std::string_view key,
                                                       std::string_view parent,
                                                       std::string fallback) {
  const JsonValue* member = object.find(key);
  if (member == nullptr) {
    return fallback;
  }
  return member->optional_string(join_path(parent, key), std::move(fallback));
}

[[nodiscard]] Status read_optional_bool(const JsonValue& object, std::string_view key,
                                       std::string_view parent, bool& out) {
  const JsonValue* member = object.find(key);
  if (member == nullptr) {
    return Status::success();
  }
  auto value = member->require_bool(join_path(parent, key));
  if (!value.ok()) {
    return value.status();
  }
  out = value.value();
  return Status::success();
}

[[nodiscard]] Status read_required_object(const JsonValue& object, std::string_view key,
                                          std::string_view parent, const JsonValue*& out) {
  auto member = require_member(object, key, parent);
  if (!member.ok()) {
    return member.status();
  }
  auto found = member.value()->require_object(join_path(parent, key));
  if (!found.ok()) {
    return found.status();
  }
  out = found.value();
  return Status::success();
}

[[nodiscard]] Status read_required_array(const JsonValue& object, std::string_view key,
                                         std::string_view parent, const JsonValue*& out) {
  auto member = require_member(object, key, parent);
  if (!member.ok()) {
    return member.status();
  }
  auto found = member.value()->require_array(join_path(parent, key));
  if (!found.ok()) {
    return found.status();
  }
  out = found.value();
  return Status::success();
}

[[nodiscard]] Status read_optional_array(const JsonValue& object, std::string_view key,
                                         std::string_view parent, const JsonValue*& out) {
  out = nullptr;
  const JsonValue* member = object.find(key);
  if (member == nullptr) {
    return Status::success();
  }
  auto array = member->require_array(join_path(parent, key));
  if (!array.ok()) {
    return array.status();
  }
  out = array.value();
  return Status::success();
}

[[nodiscard]] Status assign_u32(const JsonValue& value, std::string_view path, std::uint32_t& out) {
  auto number = value.require_integer(path);
  if (!number.ok()) {
    return number.status();
  }
  const std::int64_t maximum =
      static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max());
  if (number.value() < 0 || number.value() > maximum) {
    return make_status(StatusCode::kInvalidArgument,
                       std::string(path) + ": value " + std::to_string(number.value()) +
                           " is outside the range of an unsigned 32-bit integer");
  }
  out = static_cast<std::uint32_t>(number.value());
  return Status::success();
}

[[nodiscard]] Status read_required_u32(const JsonValue& object, std::string_view key,
                                       std::string_view parent, std::uint32_t& out) {
  auto member = require_member(object, key, parent);
  if (!member.ok()) {
    return member.status();
  }
  return assign_u32(*member.value(), join_path(parent, key), out);
}

[[nodiscard]] Status read_optional_u32(const JsonValue& object, std::string_view key,
                                       std::string_view parent, std::uint32_t& out) {
  const JsonValue* member = object.find(key);
  if (member == nullptr) {
    return Status::success();
  }
  return assign_u32(*member, join_path(parent, key), out);
}

[[nodiscard]] Status assign_millis(const JsonValue& value, std::string_view path, Duration& out) {
  auto number = value.require_integer(path);
  if (!number.ok()) {
    return number.status();
  }
  constexpr std::int64_t kMaxMillis = std::numeric_limits<std::int64_t>::max() / 1000000;
  if (number.value() > kMaxMillis || number.value() < -kMaxMillis) {
    return make_status(StatusCode::kInvalidArgument,
                       std::string(path) + ": duration is out of range at millisecond precision");
  }
  out = Duration::from_millis(number.value());
  return Status::success();
}

[[nodiscard]] Status read_optional_millis(const JsonValue& object, std::string_view key,
                                          std::string_view parent, Duration& out) {
  const JsonValue* member = object.find(key);
  if (member == nullptr) {
    return Status::success();
  }
  return assign_millis(*member, join_path(parent, key), out);
}

template <class Id>
[[nodiscard]] Status assign_id(const JsonValue& value, std::string_view path, Id& out) {
  auto text = value.require_string(path);
  if (!text.ok()) {
    return text.status();
  }
  const auto parsed = Id::parse(text.value());
  if (!parsed.has_value()) {
    return make_status(StatusCode::kInvalidArgument,
                       std::string(path) + ": expected " + std::to_string(Id::hex_size) +
                           " hexadecimal characters");
  }
  out = parsed.value();
  return Status::success();
}

template <class Id>
[[nodiscard]] Status read_optional_id(const JsonValue& object, std::string_view key,
                                      std::string_view parent, Id& out) {
  const JsonValue* member = object.find(key);
  if (member == nullptr) {
    return Status::success();
  }
  return assign_id(*member, join_path(parent, key), out);
}

[[nodiscard]] Status assign_digest(const JsonValue& value, std::string_view path, Digest256& out) {
  auto text = value.require_string(path);
  if (!text.ok()) {
    return text.status();
  }
  if (!parse_hex256(text.value(), out)) {
    return make_status(StatusCode::kInvalidArgument,
                       std::string(path) +
                           ": expected 64 hexadecimal characters naming a SHA-256 digest");
  }
  return Status::success();
}

[[nodiscard]] Status read_required_digest(const JsonValue& object, std::string_view key,
                                          std::string_view parent, Digest256& out) {
  auto member = require_member(object, key, parent);
  if (!member.ok()) {
    return member.status();
  }
  return assign_digest(*member.value(), join_path(parent, key), out);
}

template <class Enum>
[[nodiscard]] Status assign_enum(const JsonValue& value, std::string_view path, const char* expected,
                                 std::optional<Enum> (*parse)(std::string_view) noexcept, Enum& out) {
  auto text = value.require_string(path);
  if (!text.ok()) {
    return text.status();
  }
  const auto parsed = parse(text.value());
  if (!parsed.has_value()) {
    return make_status(StatusCode::kInvalidArgument, std::string(path) + ": expected " + expected +
                                                         ", found " + quoted(text.value()));
  }
  out = parsed.value();
  return Status::success();
}

template <class Enum>
[[nodiscard]] Status read_required_enum(const JsonValue& object, std::string_view key,
                                        std::string_view parent, const char* expected,
                                        std::optional<Enum> (*parse)(std::string_view) noexcept,
                                        Enum& out) {
  auto member = require_member(object, key, parent);
  if (!member.ok()) {
    return member.status();
  }
  return assign_enum(*member.value(), join_path(parent, key), expected, parse, out);
}

template <class Enum>
[[nodiscard]] Status read_optional_enum(const JsonValue& object, std::string_view key,
                                        std::string_view parent, const char* expected,
                                        std::optional<Enum> (*parse)(std::string_view) noexcept,
                                        Enum& out) {
  const JsonValue* member = object.find(key);
  if (member == nullptr) {
    return Status::success();
  }
  return assign_enum(*member, join_path(parent, key), expected, parse, out);
}

// --- Ratios -----------------------------------------------------------------

[[nodiscard]] Status parse_u32_text(std::string_view text, std::string_view path,
                                    std::uint32_t& out) {
  if (text.empty()) {
    return make_status(StatusCode::kInvalidArgument,
                       std::string(path) + ": expected a decimal integer");
  }
  std::uint64_t value = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      return make_status(StatusCode::kInvalidArgument,
                         std::string(path) + ": expected a decimal integer, found " +
                             quoted(text));
    }
    value = value * 10u + static_cast<std::uint64_t>(character - '0');
    if (value > std::numeric_limits<std::uint32_t>::max()) {
      return make_status(StatusCode::kInvalidArgument,
                         std::string(path) + ": value is outside the range of an unsigned 32-bit "
                                             "integer");
    }
  }
  out = static_cast<std::uint32_t>(value);
  return Status::success();
}

// Either "2/3" or {"numerator": 2, "denominator": 3}.
[[nodiscard]] Status parse_ratio(const JsonValue& value, std::string_view path, Ratio& out) {
  if (value.is_string()) {
    const std::string& text = value.as_string();
    const std::size_t slash = text.find('/');
    if (slash == std::string::npos || text.find('/', slash + 1) != std::string::npos) {
      return make_status(StatusCode::kInvalidArgument,
                         std::string(path) +
                             ": expected a ratio written as \"numerator/denominator\"");
    }
    std::uint32_t numerator = 0;
    std::uint32_t denominator = 0;
    const std::string_view view(text);
    RF_TRY(parse_u32_text(view.substr(0, slash), std::string(path) + ".numerator", numerator));
    RF_TRY(parse_u32_text(view.substr(slash + 1), std::string(path) + ".denominator", denominator));
    out = ratio(numerator, denominator);
    return Status::success();
  }
  if (value.is_object()) {
    std::uint32_t numerator = 0;
    std::uint32_t denominator = 0;
    RF_TRY(read_required_u32(value, "numerator", path, numerator));
    RF_TRY(read_required_u32(value, "denominator", path, denominator));
    out = ratio(numerator, denominator);
    return Status::success();
  }
  return make_status(StatusCode::kInvalidArgument,
                     std::string(path) + ": expected a ratio such as \"2/3\" or an object with "
                                         "numerator and denominator");
}

[[nodiscard]] Status read_optional_ratio(const JsonValue& object, std::string_view key,
                                         std::string_view parent, Ratio& out) {
  const JsonValue* member = object.find(key);
  if (member == nullptr) {
    return Status::success();
  }
  return parse_ratio(*member, join_path(parent, key), out);
}

// --- Bounded lists ----------------------------------------------------------

[[nodiscard]] Status read_optional_string_list(const JsonValue& object, std::string_view key,
                                               std::string_view parent, std::size_t max_count,
                                               std::size_t max_bytes_per_entry,
                                               std::vector<std::string>& out) {
  const JsonValue* array = nullptr;
  RF_TRY(read_optional_array(object, key, parent, array));
  if (array == nullptr) {
    return Status::success();
  }
  const std::string list_path = join_path(parent, key);
  RF_TRY(check_count(array->items().size(), max_count, list_path, "entries"));
  for (std::size_t index = 0; index < array->items().size(); ++index) {
    std::string entry;
    RF_TRY_ASSIGN(array->items()[index].require_string(index_path(list_path, index)), entry);
    if (entry.size() > max_bytes_per_entry) {
      return make_status(StatusCode::kLimitExceeded,
                         index_path(list_path, index) + " is " + std::to_string(entry.size()) +
                             " bytes, above the maximum of " + std::to_string(max_bytes_per_entry));
    }
    out.push_back(std::move(entry));
  }
  return Status::success();
}

template <class Id>
[[nodiscard]] Status read_optional_id_list(const JsonValue& object, std::string_view key,
                                           std::string_view parent, const RuntimeLimits& limits,
                                           std::vector<Id>& out) {
  const JsonValue* array = nullptr;
  RF_TRY(read_optional_array(object, key, parent, array));
  if (array == nullptr) {
    return Status::success();
  }
  const std::string list_path = join_path(parent, key);
  RF_TRY(check_count(array->items().size(), limits.max_selector_terms, list_path, "entries"));
  out.reserve(array->items().size());
  for (std::size_t index = 0; index < array->items().size(); ++index) {
    Id id{};
    RF_TRY(assign_id(array->items()[index], index_path(list_path, index), id));
    out.push_back(id);
  }
  return Status::success();
}

// --- Base64 -----------------------------------------------------------------

[[nodiscard]] int base64_value(char character) noexcept {
  if (character >= 'A' && character <= 'Z') {
    return character - 'A';
  }
  if (character >= 'a' && character <= 'z') {
    return 26 + (character - 'a');
  }
  if (character >= '0' && character <= '9') {
    return 52 + (character - '0');
  }
  if (character == '+') {
    return 62;
  }
  if (character == '/') {
    return 63;
  }
  return -1;
}

// Standard alphabet with canonical padding. The unrepresented low bits of a
// padded quantum must be zero, so a payload has exactly one spelling.
[[nodiscard]] Status decode_base64(std::string_view text, std::string_view path,
                                   std::size_t max_bytes, std::vector<std::byte>& out) {
  out.clear();
  if (text.size() % 4u != 0u) {
    return make_status(StatusCode::kInvalidArgument,
                       std::string(path) + ": base64 length must be a multiple of four");
  }
  const std::size_t decoded_max = (text.size() / 4u) * 3u;
  if (decoded_max > max_bytes) {
    return make_status(StatusCode::kLimitExceeded,
                       std::string(path) + ": decoded payload exceeds the maximum of " +
                           std::to_string(max_bytes) + " bytes");
  }
  out.reserve(decoded_max);
  for (std::size_t index = 0; index < text.size(); index += 4) {
    const bool last = index + 4 == text.size();
    const int first = base64_value(text[index]);
    const int second = base64_value(text[index + 1]);
    if (first < 0 || second < 0) {
      return make_status(StatusCode::kInvalidArgument,
                         std::string(path) + ": invalid base64 character");
    }
    out.push_back(static_cast<std::byte>(
        static_cast<std::uint8_t>((first << 2) | (second >> 4))));
    if (text[index + 2] == '=') {
      if (text[index + 3] != '=' || !last || (second & 0x0F) != 0) {
        return make_status(StatusCode::kInvalidArgument,
                           std::string(path) + ": malformed base64 padding");
      }
      continue;
    }
    const int third = base64_value(text[index + 2]);
    if (third < 0) {
      return make_status(StatusCode::kInvalidArgument,
                         std::string(path) + ": invalid base64 character");
    }
    out.push_back(static_cast<std::byte>(
        static_cast<std::uint8_t>(((second & 0x0F) << 4) | (third >> 2))));
    if (text[index + 3] == '=') {
      if (!last || (third & 0x03) != 0) {
        return make_status(StatusCode::kInvalidArgument,
                           std::string(path) + ": malformed base64 padding");
      }
      continue;
    }
    const int fourth = base64_value(text[index + 3]);
    if (fourth < 0) {
      return make_status(StatusCode::kInvalidArgument,
                         std::string(path) + ": invalid base64 character");
    }
    out.push_back(static_cast<std::byte>(
        static_cast<std::uint8_t>(((third & 0x03) << 6) | fourth)));
  }
  return Status::success();
}

// --- Selectors --------------------------------------------------------------

[[nodiscard]] Status parse_selector_term(const JsonValue& value, std::string_view path,
                                         std::uint32_t depth, const RuntimeLimits& limits,
                                         SelectorTerm& out) {
  if (depth > kMaxSelectorDepth) {
    return make_status(StatusCode::kLimitExceeded,
                       std::string(path) + ": selector nesting exceeds the maximum depth of " +
                           std::to_string(kMaxSelectorDepth));
  }
  const JsonValue* object = nullptr;
  RF_TRY_ASSIGN(value.require_object(path), object);
  RF_TRY(read_required_enum(*object, "op", path, "a selector operator", &parse_selector_op, out.op));
  RF_TRY(read_optional_id_list(*object, "ids", path, limits, out.ids));
  const JsonValue* kinds = nullptr;
  RF_TRY(read_optional_array(*object, "kinds", path, kinds));
  if (kinds != nullptr) {
    const std::string kinds_path = join_path(path, "kinds");
    RF_TRY(check_count(kinds->items().size(), limits.max_selector_terms, kinds_path, "entries"));
    out.kinds.reserve(kinds->items().size());
    for (std::size_t index = 0; index < kinds->items().size(); ++index) {
      TargetKind kind = TargetKind::kDevice;
      RF_TRY(assign_enum(kinds->items()[index], index_path(kinds_path, index), "a target kind",
                         &parse_target_kind, kind));
      out.kinds.push_back(kind);
    }
  }
  RF_TRY(read_optional_id_list(*object, "failure_domains", path, limits, out.failure_domains));
  RF_TRY(read_optional_string_list(*object, "sites", path, limits.max_selector_terms,
                                   limits.max_label_value_bytes, out.sites));
  RF_TRY(read_optional_string_list(*object, "pods", path, limits.max_selector_terms,
                                   limits.max_label_value_bytes, out.pods));
  RF_TRY(read_optional_string_list(*object, "racks", path, limits.max_selector_terms,
                                   limits.max_label_value_bytes, out.racks));
  RF_TRY_ASSIGN(read_optional_string(*object, "label_key", path, std::string()), out.label_key);
  const JsonValue* label_value = object->find("label_value");
  if (label_value != nullptr) {
    RF_TRY_ASSIGN(label_value->require_string(join_path(path, "label_value")), out.label_value);
    out.label_value_is_set = true;
  }
  const JsonValue* children = nullptr;
  RF_TRY(read_optional_array(*object, "children", path, children));
  if (children != nullptr) {
    const std::string children_path = join_path(path, "children");
    RF_TRY(check_count(children->items().size(), limits.max_selector_terms, children_path,
                       "children"));
    out.children.reserve(children->items().size());
    for (std::size_t index = 0; index < children->items().size(); ++index) {
      SelectorTerm child;
      RF_TRY(parse_selector_term(children->items()[index], index_path(children_path, index),
                                 depth + 1, limits, child));
      out.children.push_back(std::move(child));
    }
  }
  return Status::success();
}

// --- Policy -----------------------------------------------------------------

[[nodiscard]] Status parse_health_gate(const JsonValue& value, std::string_view path,
                                       HealthGatePolicy& out) {
  const JsonValue* object = nullptr;
  RF_TRY_ASSIGN(value.require_object(path), object);
  RF_TRY(read_optional_bool(*object, "enabled", path, out.enabled));
  RF_TRY(read_optional_millis(*object, "max_age_ms", path, out.max_age));
  RF_TRY(read_optional_id(*object, "required_source", path, out.required_source));
  RF_TRY(read_optional_u32(*object, "min_consecutive_healthy", path, out.min_consecutive_healthy));
  RF_TRY(read_optional_ratio(*object, "min_healthy_fraction", path, out.min_healthy_fraction));
  RF_TRY(read_optional_u32(*object, "failure_budget_targets", path, out.failure_budget_targets));
  RF_TRY(read_optional_ratio(*object, "failure_budget_fraction", path, out.failure_budget_fraction));
  return Status::success();
}

[[nodiscard]] Status parse_evidence_gate(const JsonValue& value, std::string_view path,
                                         EvidenceGatePolicy& out) {
  const JsonValue* object = nullptr;
  RF_TRY_ASSIGN(value.require_object(path), object);
  RF_TRY(read_optional_enum(*object, "mode", path, "a gate mode", &parse_gate_mode, out.mode));
  RF_TRY(read_optional_millis(*object, "soak_ms", path, out.soak));
  RF_TRY(read_optional_millis(*object, "max_age_ms", path, out.max_age));
  RF_TRY(read_optional_id(*object, "required_verifier", path, out.required_verifier));
  RF_TRY(read_optional_u32(*object, "min_passing_samples", path, out.min_passing_samples));
  RF_TRY(read_optional_ratio(*object, "min_passing_fraction", path, out.min_passing_fraction));
  RF_TRY(read_optional_bool(*object, "require_full_coverage", path, out.require_full_coverage));
  return Status::success();
}

[[nodiscard]] Status parse_blast_radius(const JsonValue& value, std::string_view path,
                                        BlastRadiusPolicy& out) {
  const JsonValue* object = nullptr;
  RF_TRY_ASSIGN(value.require_object(path), object);
  RF_TRY(read_optional_u32(*object, "max_targets_changing_global", path,
                           out.max_targets_changing_global));
  RF_TRY(read_optional_u32(*object, "max_targets_changing_per_stage", path,
                           out.max_targets_changing_per_stage));
  RF_TRY(read_optional_u32(*object, "max_targets_changing_per_failure_domain", path,
                           out.max_targets_changing_per_failure_domain));
  RF_TRY(read_optional_u32(*object, "max_targets_changing_per_rack", path,
                           out.max_targets_changing_per_rack));
  RF_TRY(read_optional_u32(*object, "max_targets_changing_per_pod", path,
                           out.max_targets_changing_per_pod));
  RF_TRY(read_optional_u32(*object, "max_targets_changing_per_site", path,
                           out.max_targets_changing_per_site));
  RF_TRY(read_optional_u32(*object, "max_failure_domains_changing", path,
                           out.max_failure_domains_changing));
  return Status::success();
}

// Compensation actions are declared by name and resolved once every stage is
// known, because a name can only be resolved against the stage that declares it.
[[nodiscard]] Status parse_stage_policy(const JsonValue& value, std::string_view path,
                                        const RuntimeLimits& limits, StagePolicy& out,
                                        std::vector<std::string>& compensation_names) {
  const JsonValue* object = nullptr;
  RF_TRY_ASSIGN(value.require_object(path), object);
  RF_TRY(read_optional_u32(*object, "max_concurrency", path, out.max_concurrency));
  RF_TRY(read_optional_ratio(*object, "min_healthy_fraction", path, out.min_healthy_fraction));
  RF_TRY(read_optional_u32(*object, "failure_budget_targets", path, out.failure_budget_targets));
  RF_TRY(read_optional_ratio(*object, "failure_budget_fraction", path, out.failure_budget_fraction));
  RF_TRY(read_optional_u32(*object, "canary_size", path, out.canary_size));
  RF_TRY(read_optional_bool(*object, "canary_requires_health_gate", path,
                            out.canary_requires_health_gate));
  RF_TRY(read_optional_enum(*object, "entry_gate", path, "a gate mode", &parse_gate_mode,
                            out.entry_gate));
  RF_TRY(read_optional_enum(*object, "advance_gate", path, "a gate mode", &parse_gate_mode,
                            out.advance_gate));
  RF_TRY(read_optional_millis(*object, "attempt_deadline_ms", path, out.attempt_deadline));
  RF_TRY(read_optional_u32(*object, "max_attempts_per_target", path, out.max_attempts_per_target));
  RF_TRY(read_optional_millis(*object, "evidence_max_age_ms", path, out.evidence_max_age));
  RF_TRY(read_optional_enum(*object, "pause_in_flight", path, "an in-flight policy",
                            &parse_in_flight_policy, out.pause_in_flight));
  RF_TRY(read_optional_bool(*object, "rollback_supported", path, out.rollback_supported));
  RF_TRY(read_optional_string_list(*object, "compensation_actions", path, limits.max_actions_per_stage,
                                   limits.max_name_bytes, compensation_names));
  RF_TRY(read_optional_bool(*object, "require_predecessors_succeeded", path,
                            out.require_predecessors_succeeded));
  RF_TRY(read_optional_u32(*object, "weight", path, out.weight));
  const JsonValue* health_gate = object->find("health_gate");
  if (health_gate != nullptr) {
    RF_TRY(parse_health_gate(*health_gate, join_path(path, "health_gate"), out.health_gate));
  }
  const JsonValue* evidence_gate = object->find("evidence_gate");
  if (evidence_gate != nullptr) {
    RF_TRY(parse_evidence_gate(*evidence_gate, join_path(path, "evidence_gate"), out.evidence_gate));
  }
  const JsonValue* blast_radius = object->find("blast_radius");
  if (blast_radius != nullptr) {
    RF_TRY(parse_blast_radius(*blast_radius, join_path(path, "blast_radius"), out.blast_radius));
  }
  return Status::success();
}

[[nodiscard]] Status parse_evidence_rule(const JsonValue& value, std::string_view path,
                                         TargetEvidenceRule& out) {
  const JsonValue* object = nullptr;
  RF_TRY_ASSIGN(value.require_object(path), object);
  RF_TRY(read_optional_enum(*object, "success_authority", path, "an evidence authority",
                            &parse_evidence_authority, out.success_authority));
  RF_TRY(read_optional_enum(*object, "failure_authority", path, "an evidence authority",
                            &parse_evidence_authority, out.failure_authority));
  RF_TRY(read_optional_bool(*object, "require_verification", path, out.require_verification));
  RF_TRY(read_optional_id(*object, "verifier", path, out.verifier));
  RF_TRY(read_optional_millis(*object, "health_settle_ms", path, out.health_settle));
  RF_TRY(read_optional_bool(*object, "require_health_after_success", path,
                            out.require_health_after_success));
  return Status::success();
}

// --- Actions and stages -----------------------------------------------------

[[nodiscard]] Status parse_action(const JsonValue& value, std::string_view path,
                                  const StageId& stage, const RuntimeLimits& limits,
                                  ActionSpec& out) {
  const JsonValue* object = nullptr;
  RF_TRY_ASSIGN(value.require_object(path), object);
  std::string name;
  RF_TRY_ASSIGN(read_required_string(*object, "name", path), name);
  if (name.empty()) {
    return make_status(StatusCode::kInvalidArgument,
                       join_path(path, "name") + " must not be empty");
  }
  if (name.size() > limits.max_name_bytes) {
    return make_status(StatusCode::kLimitExceeded,
                       join_path(path, "name") + " is " + std::to_string(name.size()) +
                           " bytes, above max_name_bytes");
  }
  out.name = std::move(name);
  out.id = derive_action_id(stage, out.name);
  RF_TRY(read_required_enum(*object, "kind", path, "an action kind", &parse_action_kind, out.kind));
  RF_TRY(read_required_digest(*object, "artifact_digest", path, out.artifact_digest));
  RF_TRY(read_optional_enum(*object, "rollback", path, "a rollback support value",
                            &parse_rollback_support, out.rollback));
  const JsonValue* parameters = object->find("parameters_base64");
  if (parameters != nullptr) {
    const std::string parameters_path = join_path(path, "parameters_base64");
    std::string text;
    RF_TRY_ASSIGN(parameters->require_string(parameters_path), text);
    RF_TRY(decode_base64(text, parameters_path, limits.max_message_bytes, out.parameters));
  }
  return Status::success();
}

struct ParsedStage {
  StageSpec spec;
  std::vector<std::string> predecessor_names;
  std::vector<std::string> compensation_names;
};

[[nodiscard]] Status parse_stage(const JsonValue& value, std::string_view path, const PlanId& plan,
                                 const RuntimeLimits& limits, ParsedStage& out) {
  const JsonValue* object = nullptr;
  RF_TRY_ASSIGN(value.require_object(path), object);
  std::string name;
  RF_TRY_ASSIGN(read_required_string(*object, "name", path), name);
  if (name.empty()) {
    return make_status(StatusCode::kInvalidArgument,
                       join_path(path, "name") + " must not be empty");
  }
  if (name.size() > limits.max_name_bytes) {
    return make_status(StatusCode::kLimitExceeded,
                       join_path(path, "name") + " is " + std::to_string(name.size()) +
                           " bytes, above max_name_bytes");
  }
  out.spec.name = std::move(name);
  out.spec.id = derive_stage_id(plan, out.spec.name);
  RF_TRY(read_required_u32(*object, "ordinal", path, out.spec.ordinal));
  RF_TRY(read_optional_string_list(*object, "predecessors", path, limits.max_stages_per_plan,
                                   limits.max_name_bytes, out.predecessor_names));
  const JsonValue* selector = nullptr;
  RF_TRY(read_required_object(*object, "selector", path, selector));
  RF_TRY(parse_selector_term(*selector, join_path(path, "selector"), 0, limits, out.spec.selector));
  const JsonValue* policy = object->find("policy");
  if (policy != nullptr) {
    RF_TRY(parse_stage_policy(*policy, join_path(path, "policy"), limits, out.spec.policy,
                              out.compensation_names));
  }
  const JsonValue* evidence_rule = object->find("evidence_rule");
  if (evidence_rule != nullptr) {
    RF_TRY(parse_evidence_rule(*evidence_rule, join_path(path, "evidence_rule"),
                               out.spec.evidence_rule));
  }
  const JsonValue* actions = nullptr;
  RF_TRY(read_required_array(*object, "actions", path, actions));
  const std::string actions_path = join_path(path, "actions");
  RF_TRY(check_count(actions->items().size(), limits.max_actions_per_stage, actions_path, "actions"));
  out.spec.actions.reserve(actions->items().size());
  for (std::size_t index = 0; index < actions->items().size(); ++index) {
    ActionSpec action;
    RF_TRY(parse_action(actions->items()[index], index_path(actions_path, index), out.spec.id,
                        limits, action));
    out.spec.actions.push_back(std::move(action));
  }
  return Status::success();
}

[[nodiscard]] Status require_document_kind(const JsonValue& document, std::string_view expected) {
  std::string kind;
  RF_TRY_ASSIGN(read_required_string(document, "kind", std::string_view()), kind);
  if (kind != expected) {
    return make_status(StatusCode::kInvalidArgument,
                       "kind: expected " + quoted(expected) + ", found " + quoted(kind));
  }
  return Status::success();
}

[[nodiscard]] Status require_schema_version(const JsonValue& document) {
  std::int64_t version = 0;
  RF_TRY_ASSIGN(read_required_integer(document, "schema_version", std::string_view()), version);
  if (version != kSchemaVersion) {
    return make_status(StatusCode::kInvalidArgument,
                       "schema_version: this build understands version " +
                           std::to_string(kSchemaVersion) + " only, not " +
                           std::to_string(version));
  }
  return Status::success();
}

// --- Inventory --------------------------------------------------------------

[[nodiscard]] Status parse_target(const JsonValue& value, std::string_view path,
                                  const RuntimeLimits& limits, TargetDescriptor& out) {
  const JsonValue* object = nullptr;
  RF_TRY_ASSIGN(value.require_object(path), object);
  std::string name;
  RF_TRY_ASSIGN(read_required_string(*object, "name", path), name);
  if (name.empty()) {
    return make_status(StatusCode::kInvalidArgument,
                       join_path(path, "name") + " must not be empty");
  }
  if (name.size() > limits.max_name_bytes) {
    return make_status(StatusCode::kLimitExceeded,
                       join_path(path, "name") + " is " + std::to_string(name.size()) +
                           " bytes, above max_name_bytes");
  }
  out.name = std::move(name);
  out.id = derive_target_id(out.name);
  RF_TRY(read_required_enum(*object, "kind", path, "a target kind", &parse_target_kind, out.kind));
  RF_TRY_ASSIGN(read_optional_string(*object, "site", path, std::string()), out.placement.site);
  RF_TRY_ASSIGN(read_optional_string(*object, "pod", path, std::string()), out.placement.pod);
  RF_TRY_ASSIGN(read_optional_string(*object, "rack", path, std::string()), out.placement.rack);
  std::string domain;
  RF_TRY_ASSIGN(read_required_string(*object, "failure_domain", path), domain);
  if (domain.empty()) {
    return make_status(StatusCode::kInvalidArgument,
                       join_path(path, "failure_domain") +
                           " must name the failure domain this target belongs to");
  }
  out.placement.failure_domain = derive_failure_domain_id(domain);
  const JsonValue* labels = object->find("labels");
  if (labels != nullptr) {
    const std::string labels_path = join_path(path, "labels");
    const JsonValue* label_object = nullptr;
    RF_TRY_ASSIGN(labels->require_object(labels_path), label_object);
    RF_TRY(check_count(label_object->members().size(), limits.max_labels_per_target, labels_path,
                       "labels"));
    out.labels.reserve(label_object->members().size());
    for (const auto& member : label_object->members()) {
      Label label;
      label.key = member.first;
      RF_TRY_ASSIGN(member.second.require_string(join_path(labels_path, member.first)), label.value);
      out.labels.push_back(std::move(label));
    }
    // Canonical form: sorted by key. The document's member order is not
    // observable, so it must not leak into the descriptor.
    std::sort(out.labels.begin(), out.labels.end(),
              [](const Label& lhs, const Label& rhs) { return lhs.key < rhs.key; });
  }
  return Status::success();
}

}  // namespace

Result<ChangePlan> parse_change_plan(std::string_view text, const RuntimeLimits& limits) {
  auto document = parse_json(text, limits);
  if (!document.ok()) {
    return document.status();
  }
  return change_plan_from_json(document.value(), limits);
}

Result<ChangePlan> change_plan_from_json(const JsonValue& value, const RuntimeLimits& limits) {
  const JsonValue* document = nullptr;
  RF_TRY_ASSIGN(value.require_object(std::string_view()), document);
  RF_TRY(require_document_kind(*document, kChangePlanKind));
  RF_TRY(require_schema_version(*document));

  ChangePlan plan;
  RF_TRY(read_required_digest(*document, "source_digest", std::string_view(), plan.source_digest));
  plan.id = derive_plan_id(plan.source_digest);

  std::string change_id;
  RF_TRY_ASSIGN(read_required_string(*document, "change_id", std::string_view()), change_id);
  if (change_id.empty()) {
    return make_status(StatusCode::kInvalidArgument, "change_id must not be empty");
  }
  if (change_id.size() > limits.max_name_bytes) {
    return make_status(StatusCode::kLimitExceeded,
                       "change_id is " + std::to_string(change_id.size()) +
                           " bytes, above max_name_bytes");
  }
  plan.change_id = std::move(change_id);

  std::string approved_by;
  RF_TRY_ASSIGN(read_required_string(*document, "approved_by", std::string_view()), approved_by);
  if (approved_by.empty()) {
    return make_status(StatusCode::kInvalidArgument, "approved_by must not be empty");
  }
  if (approved_by.size() > limits.max_name_bytes) {
    return make_status(StatusCode::kLimitExceeded,
                       "approved_by is " + std::to_string(approved_by.size()) +
                           " bytes, above max_name_bytes");
  }
  plan.approved_by = std::move(approved_by);

  std::string approved_at;
  RF_TRY_ASSIGN(read_required_string(*document, "approved_at", std::string_view()), approved_at);
  const auto timestamp = Timestamp::parse_rfc3339(approved_at);
  if (!timestamp.ok()) {
    return make_status(timestamp.status().code(),
                       "approved_at: " + timestamp.status().message());
  }
  plan.approved_at = timestamp.value();

  RF_TRY_ASSIGN(read_optional_string(*document, "summary", std::string_view(), std::string()),
                plan.summary);
  if (plan.summary.size() > limits.max_description_bytes) {
    return make_status(StatusCode::kLimitExceeded,
                       "summary is " + std::to_string(plan.summary.size()) +
                           " bytes, above max_description_bytes");
  }

  const JsonValue* stages = nullptr;
  RF_TRY(read_required_array(*document, "stages", std::string_view(), stages));
  RF_TRY(check_count(stages->items().size(), limits.max_stages_per_plan, "stages", "stages"));

  std::vector<ParsedStage> parsed;
  parsed.reserve(stages->items().size());
  // Stage names are resolved to identifiers here; a duplicate name derives the
  // same identifier twice and is rejected by ChangePlan::validate.
  std::map<std::string, StageId> stages_by_name;
  for (std::size_t index = 0; index < stages->items().size(); ++index) {
    ParsedStage stage;
    RF_TRY(parse_stage(stages->items()[index], index_path("stages", index), plan.id, limits, stage));
    stages_by_name.emplace(stage.spec.name, stage.spec.id);
    parsed.push_back(std::move(stage));
  }

  for (std::size_t index = 0; index < parsed.size(); ++index) {
    const std::string stage_path = index_path("stages", index);
    StageSpec& spec = parsed[index].spec;
    const std::string predecessors_path = join_path(stage_path, "predecessors");
    spec.predecessors.reserve(parsed[index].predecessor_names.size());
    for (std::size_t position = 0; position < parsed[index].predecessor_names.size(); ++position) {
      const std::string& name = parsed[index].predecessor_names[position];
      const auto found = stages_by_name.find(name);
      if (found == stages_by_name.end()) {
        return make_status(StatusCode::kInvalidArgument,
                           index_path(predecessors_path, position) + ": stage " +
                               quoted(spec.name) + " names the unknown predecessor " +
                               quoted(name));
      }
      spec.predecessors.push_back(found->second);
    }
    const std::string compensation_path = join_path(stage_path, "policy.compensation_actions");
    spec.policy.compensation_actions.reserve(parsed[index].compensation_names.size());
    for (std::size_t position = 0; position < parsed[index].compensation_names.size(); ++position) {
      const std::string& name = parsed[index].compensation_names[position];
      const ActionId id = derive_action_id(spec.id, name);
      bool declared = false;
      for (const ActionSpec& action : spec.actions) {
        if (action.id == id) {
          declared = true;
          break;
        }
      }
      if (!declared) {
        return make_status(StatusCode::kInvalidArgument,
                           index_path(compensation_path, position) + ": stage " +
                               quoted(spec.name) + " names the unknown compensation action " +
                               quoted(name) + "; a stage can only compensate with an action it "
                                             "declares itself");
      }
      spec.policy.compensation_actions.push_back(id);
    }
  }

  plan.stages.reserve(parsed.size());
  for (ParsedStage& stage : parsed) {
    plan.stages.push_back(std::move(stage.spec));
  }
  RF_TRY(plan.validate(limits));
  return plan;
}

Result<TargetInventory> parse_inventory(std::string_view text, const RuntimeLimits& limits) {
  auto document = parse_json(text, limits);
  if (!document.ok()) {
    return document.status();
  }
  return inventory_from_json(document.value(), limits);
}

Result<TargetInventory> inventory_from_json(const JsonValue& value, const RuntimeLimits& limits) {
  const JsonValue* document = nullptr;
  RF_TRY_ASSIGN(value.require_object(std::string_view()), document);
  RF_TRY(require_document_kind(*document, kInventoryKind));
  RF_TRY(require_schema_version(*document));

  const JsonValue* targets = nullptr;
  RF_TRY(read_required_array(*document, "targets", std::string_view(), targets));
  RF_TRY(check_count(targets->items().size(), limits.max_total_targets, "targets", "targets"));

  TargetInventory inventory;
  std::map<std::string, std::size_t> seen_names;
  for (std::size_t index = 0; index < targets->items().size(); ++index) {
    const std::string path = index_path("targets", index);
    TargetDescriptor descriptor;
    RF_TRY(parse_target(targets->items()[index], path, limits, descriptor));
    const auto existing = seen_names.find(descriptor.name);
    if (existing != seen_names.end()) {
      return make_status(StatusCode::kInvalidArgument,
                         join_path(path, "name") + ": duplicate target name " +
                             quoted(descriptor.name) + ", already declared at targets[" +
                             std::to_string(existing->second) + "]");
    }
    seen_names.emplace(descriptor.name, index);
    RF_TRY(inventory.add(std::move(descriptor), limits));
  }
  return inventory;
}

}  // namespace rollout_fabric
