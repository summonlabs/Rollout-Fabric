// Rollout Fabric - a strict, bounded JSON reader for upstream plan documents.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The approved plan arrives as a document this runtime did not author, so the
// reader treats every byte as hostile: nesting depth, string length, array
// length, object member count and total document size are all bounded, numeric
// conversion is range checked, and duplicate object keys are rejected rather
// than silently resolved. There are no exceptions and no partial results.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "rollout_fabric/codec.hpp"
#include "rollout_fabric/digest.hpp"
#include "rollout_fabric/ids.hpp"
#include "rollout_fabric/limits.hpp"
#include "rollout_fabric/plan.hpp"
#include "rollout_fabric/status.hpp"
#include "rollout_fabric/target.hpp"

namespace rollout_fabric {

class JsonValue {
 public:
  enum class Type : std::uint8_t { kNull = 0, kBool = 1, kNumber = 2, kString = 3, kArray = 4, kObject = 5 };

  JsonValue() = default;
  static JsonValue make_null();
  static JsonValue make_bool(bool value);
  static JsonValue make_number(std::int64_t value);
  static JsonValue make_string(std::string value);
  static JsonValue make_array();
  static JsonValue make_object();

  [[nodiscard]] Type type() const noexcept { return type_; }
  [[nodiscard]] bool is_null() const noexcept { return type_ == Type::kNull; }
  [[nodiscard]] bool is_bool() const noexcept { return type_ == Type::kBool; }
  [[nodiscard]] bool is_number() const noexcept { return type_ == Type::kNumber; }
  [[nodiscard]] bool is_string() const noexcept { return type_ == Type::kString; }
  [[nodiscard]] bool is_array() const noexcept { return type_ == Type::kArray; }
  [[nodiscard]] bool is_object() const noexcept { return type_ == Type::kObject; }

  [[nodiscard]] bool as_bool() const noexcept { return boolean_; }
  [[nodiscard]] std::int64_t as_number() const noexcept { return number_; }
  [[nodiscard]] const std::string& as_string() const noexcept { return string_; }
  [[nodiscard]] const std::vector<JsonValue>& items() const noexcept { return items_; }
  [[nodiscard]] const std::vector<std::pair<std::string, JsonValue>>& members() const noexcept {
    return members_;
  }

  [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept;

  void push_back(JsonValue value);
  void set(std::string key, JsonValue value);

  // Typed accessors that produce a diagnostic naming the offending path.
  [[nodiscard]] Result<std::string> require_string(std::string_view path) const;
  [[nodiscard]] Result<std::int64_t> require_integer(std::string_view path) const;
  [[nodiscard]] Result<bool> require_bool(std::string_view path) const;
  [[nodiscard]] Result<const JsonValue*> require_object(std::string_view path) const;
  [[nodiscard]] Result<const JsonValue*> require_array(std::string_view path) const;
  [[nodiscard]] Result<const JsonValue*> optional_object(std::string_view path) const;
  [[nodiscard]] Result<std::string> optional_string(std::string_view path,
                                                    std::string fallback) const;

 private:
  Type type_ = Type::kNull;
  bool boolean_ = false;
  std::int64_t number_ = 0;
  std::string string_;
  std::vector<JsonValue> items_;
  std::vector<std::pair<std::string, JsonValue>> members_;
};

inline constexpr std::uint32_t kMaxJsonDepth = 32;
inline constexpr std::uint32_t kMaxJsonMembers = 4096;
inline constexpr std::uint32_t kMaxJsonElements = 65536;
inline constexpr std::uint32_t kMaxJsonStringBytes = 8192;

[[nodiscard]] Result<JsonValue> parse_json(std::string_view text, const RuntimeLimits& limits);

// Canonical, deterministic serialisation used by the CLI's --json output.
[[nodiscard]] std::string to_json(const JsonValue& value, bool pretty);

// --- Document loaders ------------------------------------------------------

// Parses an approved Change Planner plan document and validates it. Stage and
// action identifiers are derived from the plan's source digest and the declared
// names, so the same document always yields the same identities.
[[nodiscard]] Result<ChangePlan> parse_change_plan(std::string_view text, const RuntimeLimits& limits);
[[nodiscard]] Result<ChangePlan> change_plan_from_json(const JsonValue& value, const RuntimeLimits& limits);

// Parses a target inventory document.
[[nodiscard]] Result<TargetInventory> parse_inventory(std::string_view text, const RuntimeLimits& limits);
[[nodiscard]] Result<TargetInventory> inventory_from_json(const JsonValue& value, const RuntimeLimits& limits);

struct PlanParseIssue {
  std::string path;
  std::string message;
};

}  // namespace rollout_fabric
