// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Strict, bounded JSON reading and canonical writing.
//
// The reader is the first thing an upstream document touches, so it assumes
// nothing: depth, member count, element count, string length and total document
// size are all bounded before anything is allocated, numbers are range checked
// against a 64-bit integer, and a duplicate object key is an error rather than a
// silent last-one-wins. The writer is the inverse and is deterministic: members
// are emitted in stored order, so the same value always renders to the same
// bytes.
#include "rollout_fabric/json.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace rollout_fabric {
namespace {

[[nodiscard]] bool is_digit(char character) noexcept {
  return character >= '0' && character <= '9';
}

[[nodiscard]] int hex_value(char character) noexcept {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return 10 + (character - 'a');
  }
  if (character >= 'A' && character <= 'F') {
    return 10 + (character - 'A');
  }
  return -1;
}

[[nodiscard]] char hex_digit(unsigned value) noexcept {
  return static_cast<char>(value < 10u ? ('0' + value) : ('a' + (value - 10u)));
}

// Encodes one code point as UTF-8. Surrogate halves never reach this function:
// the reader rejects them before conversion.
void append_utf8(std::string& out, std::uint32_t codepoint) {
  if (codepoint <= 0x7Fu) {
    out.push_back(static_cast<char>(codepoint));
    return;
  }
  if (codepoint <= 0x7FFu) {
    out.push_back(static_cast<char>(0xC0u | (codepoint >> 6)));
    out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    return;
  }
  if (codepoint <= 0xFFFFu) {
    out.push_back(static_cast<char>(0xE0u | (codepoint >> 12)));
    out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    return;
  }
  out.push_back(static_cast<char>(0xF0u | (codepoint >> 18)));
  out.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu)));
  out.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
  out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
}

// "stages[2].policy.max_concurrency: expected an integer".
[[nodiscard]] std::string type_error(std::string_view path, const char* expected) {
  std::string message;
  if (!path.empty()) {
    message.assign(path);
    message.append(": ");
  }
  message.append("expected ");
  message.append(expected);
  return message;
}

[[nodiscard]] std::string quoted(std::string_view text) {
  return "'" + std::string(text) + "'";
}

// Recursive descent reader over a borrowed view. One instance per document; it
// keeps no state a caller can observe.
class JsonReader {
 public:
  JsonReader(std::string_view text, const RuntimeLimits& limits) noexcept
      : text_(text), limits_(limits) {}

  [[nodiscard]] Result<JsonValue> parse() {
    if (text_.size() > limits_.max_document_bytes) {
      return make_status(StatusCode::kLimitExceeded,
                         "the document is " + std::to_string(text_.size()) +
                             " bytes, above the configured maximum of " +
                             std::to_string(limits_.max_document_bytes));
    }
    skip_whitespace();
    // The top-level value sits at depth zero, so a document nested inside
    // kMaxJsonDepth arrays still parses and one more level does not.
    auto document = parse_value(0);
    if (!document.ok()) {
      return document.status();
    }
    skip_whitespace();
    if (!at_end()) {
      return syntax_error("trailing content after the top-level value");
    }
    return std::move(document).value();
  }

 private:
  [[nodiscard]] bool at_end() const noexcept { return position_ >= text_.size(); }

  [[nodiscard]] char peek() const noexcept { return text_[position_]; }

  void skip_whitespace() noexcept {
    while (position_ < text_.size()) {
      const char character = text_[position_];
      if (character != ' ' && character != '\t' && character != '\n' && character != '\r') {
        break;
      }
      ++position_;
    }
  }

  [[nodiscard]] Status syntax_error(const std::string& message) const {
    return make_status(StatusCode::kInvalidArgument,
                       message + " at byte " + std::to_string(position_));
  }

  [[nodiscard]] Status limit_error(const std::string& message) const {
    return make_status(StatusCode::kLimitExceeded, message);
  }

  [[nodiscard]] Result<JsonValue> parse_value(std::uint32_t depth) {
    if (depth > kMaxJsonDepth) {
      return limit_error("nesting exceeds the maximum depth of " + std::to_string(kMaxJsonDepth));
    }
    if (at_end()) {
      return syntax_error("unexpected end of input where a value was expected");
    }
    switch (peek()) {
      case '{':
        return parse_object(depth);
      case '[':
        return parse_array(depth);
      case '"': {
        auto text = parse_string();
        if (!text.ok()) {
          return text.status();
        }
        return JsonValue::make_string(std::move(text).value());
      }
      case 't':
        return parse_literal("true", JsonValue::make_bool(true));
      case 'f':
        return parse_literal("false", JsonValue::make_bool(false));
      case 'n':
        return parse_literal("null", JsonValue::make_null());
      case 'N':
        return syntax_error("NaN is not a JSON number");
      case 'I':
        return syntax_error("Infinity is not a JSON number");
      case '+':
        return syntax_error("a leading '+' is not valid JSON; a number carries a leading '-' only");
      case '.':
        return syntax_error("a bare '.' is not a JSON number");
      default:
        break;
    }
    if (peek() == '-' || is_digit(peek())) {
      return parse_number();
    }
    return syntax_error("unexpected character " + quoted(std::string(1, peek())) +
                        " where a value was expected");
  }

  [[nodiscard]] Result<JsonValue> parse_literal(std::string_view literal, JsonValue value) {
    if (text_.size() - position_ < literal.size() ||
        text_.compare(position_, literal.size(), literal) != 0) {
      return syntax_error("expected " + quoted(literal));
    }
    position_ += literal.size();
    return value;
  }

  [[nodiscard]] Result<JsonValue> parse_number() {
    const bool negative = peek() == '-';
    if (negative) {
      ++position_;
      if (at_end() || !is_digit(peek())) {
        return syntax_error("a '-' sign must be followed by a digit");
      }
    }
    const std::size_t first_digit = position_;
    std::uint64_t magnitude = 0;
    while (!at_end() && is_digit(peek())) {
      const std::uint64_t digit = static_cast<std::uint64_t>(peek() - '0');
      if (magnitude > (UINT64_MAX - digit) / 10u) {
        return syntax_error("number does not fit in a 64-bit integer");
      }
      magnitude = magnitude * 10u + digit;
      ++position_;
    }
    if (position_ - first_digit > 1 && text_[first_digit] == '0') {
      return syntax_error("leading zeros are not allowed in a JSON number");
    }
    if (!at_end() && (peek() == '.' || peek() == 'e' || peek() == 'E')) {
      return syntax_error("a number must be an integer; fractions and exponents are not accepted");
    }
    constexpr std::uint64_t kPositiveLimit = 9223372036854775807ull;
    constexpr std::uint64_t kNegativeLimit = 9223372036854775808ull;
    const std::uint64_t limit = negative ? kNegativeLimit : kPositiveLimit;
    if (magnitude > limit) {
      return syntax_error("number does not fit in a signed 64-bit integer");
    }
    if (!negative) {
      return JsonValue::make_number(static_cast<std::int64_t>(magnitude));
    }
    if (magnitude == kNegativeLimit) {
      return JsonValue::make_number(INT64_MIN);
    }
    return JsonValue::make_number(-static_cast<std::int64_t>(magnitude));
  }

  [[nodiscard]] Result<std::uint32_t> parse_hex4() {
    if (text_.size() - position_ < 4) {
      return syntax_error("a \\u escape requires four hexadecimal digits");
    }
    std::uint32_t value = 0;
    for (std::size_t offset = 0; offset < 4; ++offset) {
      const int digit = hex_value(text_[position_ + offset]);
      if (digit < 0) {
        return syntax_error("a \\u escape requires four hexadecimal digits");
      }
      value = (value << 4) | static_cast<std::uint32_t>(digit);
    }
    position_ += 4;
    return value;
  }

  // The caller guarantees the current character is the opening quote.
  [[nodiscard]] Result<std::string> parse_string() {
    ++position_;
    std::string out;
    for (;;) {
      if (at_end()) {
        return syntax_error("unterminated string");
      }
      const char character = text_[position_];
      if (character == '"') {
        ++position_;
        break;
      }
      if (static_cast<unsigned char>(character) < 0x20u) {
        return syntax_error("a string contains an unescaped control character");
      }
      if (character != '\\') {
        out.push_back(character);
        ++position_;
      } else {
        ++position_;
        if (at_end()) {
          return syntax_error("unterminated escape sequence");
        }
        const char escape = text_[position_];
        ++position_;
        switch (escape) {
          case '"':
            out.push_back('"');
            break;
          case '\\':
            out.push_back('\\');
            break;
          case '/':
            out.push_back('/');
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'u': {
            auto unit = parse_hex4();
            if (!unit.ok()) {
              return unit.status();
            }
            std::uint32_t codepoint = unit.value();
            if (codepoint >= 0xD800u && codepoint <= 0xDBFFu) {
              if (text_.size() - position_ < 2 || text_[position_] != '\\' ||
                  text_[position_ + 1] != 'u') {
                return syntax_error(
                    "a high surrogate escape must be followed by a low surrogate escape");
              }
              position_ += 2;
              auto low = parse_hex4();
              if (!low.ok()) {
                return low.status();
              }
              if (low.value() < 0xDC00u || low.value() > 0xDFFFu) {
                return syntax_error(
                    "a high surrogate escape is not followed by a low surrogate escape");
              }
              codepoint = 0x10000u + ((codepoint - 0xD800u) << 10) + (low.value() - 0xDC00u);
            } else if (codepoint >= 0xDC00u && codepoint <= 0xDFFFu) {
              return syntax_error("a lone low surrogate escape does not encode a character");
            }
            append_utf8(out, codepoint);
            break;
          }
          default:
            return syntax_error("invalid escape sequence " +
                                quoted(std::string("\\") + escape));
        }
      }
      if (out.size() > kMaxJsonStringBytes) {
        return limit_error("a string exceeds the maximum of " +
                           std::to_string(kMaxJsonStringBytes) + " bytes");
      }
    }
    return out;
  }

  [[nodiscard]] Result<JsonValue> parse_object(std::uint32_t depth) {
    ++position_;
    JsonValue object = JsonValue::make_object();
    skip_whitespace();
    if (!at_end() && peek() == '}') {
      ++position_;
      return object;
    }
    for (;;) {
      skip_whitespace();
      if (at_end()) {
        return syntax_error("unterminated object");
      }
      if (peek() != '"') {
        return syntax_error("expected a string key inside an object");
      }
      auto key = parse_string();
      if (!key.ok()) {
        return key.status();
      }
      skip_whitespace();
      if (at_end() || peek() != ':') {
        return syntax_error("expected ':' after the object key " + quoted(key.value()));
      }
      ++position_;
      for (const auto& member : object.members()) {
        if (std::string_view(member.first) == key.value()) {
          return make_status(StatusCode::kInvalidArgument,
                             "duplicate object key " + quoted(key.value()) + " at byte " +
                                 std::to_string(position_));
        }
      }
      if (object.members().size() >= kMaxJsonMembers) {
        return limit_error("an object exceeds the maximum of " + std::to_string(kMaxJsonMembers) +
                           " members");
      }
      skip_whitespace();
      auto value = parse_value(depth + 1);
      if (!value.ok()) {
        return value.status();
      }
      object.set(std::move(key).value(), std::move(value).value());
      skip_whitespace();
      if (at_end()) {
        return syntax_error("unterminated object");
      }
      if (peek() == ',') {
        ++position_;
        continue;
      }
      if (peek() == '}') {
        ++position_;
        break;
      }
      return syntax_error("expected ',' or '}' inside an object");
    }
    return object;
  }

  [[nodiscard]] Result<JsonValue> parse_array(std::uint32_t depth) {
    ++position_;
    JsonValue array = JsonValue::make_array();
    skip_whitespace();
    if (!at_end() && peek() == ']') {
      ++position_;
      return array;
    }
    for (;;) {
      skip_whitespace();
      if (array.items().size() >= kMaxJsonElements) {
        return limit_error("an array exceeds the maximum of " + std::to_string(kMaxJsonElements) +
                           " elements");
      }
      auto value = parse_value(depth + 1);
      if (!value.ok()) {
        return value.status();
      }
      array.push_back(std::move(value).value());
      skip_whitespace();
      if (at_end()) {
        return syntax_error("unterminated array");
      }
      if (peek() == ',') {
        ++position_;
        continue;
      }
      if (peek() == ']') {
        ++position_;
        break;
      }
      return syntax_error("expected ',' or ']' inside an array");
    }
    return array;
  }

  std::string_view text_;
  const RuntimeLimits& limits_;
  std::size_t position_ = 0;
};

void write_escaped_string(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const char character : text) {
    switch (character) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default: {
        const auto value = static_cast<unsigned char>(character);
        if (value < 0x20u) {
          out.append("\\u00");
          out.push_back(hex_digit(value >> 4));
          out.push_back(hex_digit(value & 0x0Fu));
        } else {
          // UTF-8 passes through untouched: the writer never re-encodes text it
          // did not itself escape.
          out.push_back(character);
        }
        break;
      }
    }
  }
  out.push_back('"');
}

void append_indent(std::string& out, std::size_t depth) { out.append(depth * 2, ' '); }

void write_value(const JsonValue& value, bool pretty, std::size_t depth, std::string& out) {
  switch (value.type()) {
    case JsonValue::Type::kNull:
      out.append("null");
      return;
    case JsonValue::Type::kBool:
      out.append(value.as_bool() ? "true" : "false");
      return;
    case JsonValue::Type::kNumber:
      out.append(std::to_string(value.as_number()));
      return;
    case JsonValue::Type::kString:
      write_escaped_string(out, value.as_string());
      return;
    case JsonValue::Type::kArray: {
      const std::vector<JsonValue>& items = value.items();
      if (items.empty()) {
        out.append("[]");
        return;
      }
      out.push_back('[');
      for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) {
          out.push_back(',');
        }
        if (pretty) {
          out.push_back('\n');
          append_indent(out, depth + 1);
        }
        write_value(items[index], pretty, depth + 1, out);
      }
      if (pretty) {
        out.push_back('\n');
        append_indent(out, depth);
      }
      out.push_back(']');
      return;
    }
    case JsonValue::Type::kObject: {
      const std::vector<std::pair<std::string, JsonValue>>& members = value.members();
      if (members.empty()) {
        out.append("{}");
        return;
      }
      out.push_back('{');
      for (std::size_t index = 0; index < members.size(); ++index) {
        if (index != 0) {
          out.push_back(',');
        }
        if (pretty) {
          out.push_back('\n');
          append_indent(out, depth + 1);
        }
        write_escaped_string(out, members[index].first);
        out.push_back(':');
        if (pretty) {
          out.push_back(' ');
        }
        write_value(members[index].second, pretty, depth + 1, out);
      }
      if (pretty) {
        out.push_back('\n');
        append_indent(out, depth);
      }
      out.push_back('}');
      return;
    }
  }
}

}  // namespace

JsonValue JsonValue::make_null() { return JsonValue(); }

JsonValue JsonValue::make_bool(bool value) {
  JsonValue out;
  out.type_ = Type::kBool;
  out.boolean_ = value;
  return out;
}

JsonValue JsonValue::make_number(std::int64_t value) {
  JsonValue out;
  out.type_ = Type::kNumber;
  out.number_ = value;
  return out;
}

JsonValue JsonValue::make_string(std::string value) {
  JsonValue out;
  out.type_ = Type::kString;
  out.string_ = std::move(value);
  return out;
}

JsonValue JsonValue::make_array() {
  JsonValue out;
  out.type_ = Type::kArray;
  return out;
}

JsonValue JsonValue::make_object() {
  JsonValue out;
  out.type_ = Type::kObject;
  return out;
}

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
  for (const auto& member : members_) {
    if (std::string_view(member.first) == key) {
      return &member.second;
    }
  }
  return nullptr;
}

void JsonValue::push_back(JsonValue value) {
  if (type_ != Type::kArray) {
    // A value that was not an array becomes one; nothing else about it is
    // observable after this, because every other field is cleared.
    items_.clear();
    members_.clear();
    string_.clear();
    boolean_ = false;
    number_ = 0;
    type_ = Type::kArray;
  }
  items_.push_back(std::move(value));
}

void JsonValue::set(std::string key, JsonValue value) {
  if (type_ != Type::kObject) {
    items_.clear();
    members_.clear();
    string_.clear();
    boolean_ = false;
    number_ = 0;
    type_ = Type::kObject;
  }
  for (auto& member : members_) {
    if (member.first == key) {
      member.second = std::move(value);
      return;
    }
  }
  members_.emplace_back(std::move(key), std::move(value));
}

Result<std::string> JsonValue::require_string(std::string_view path) const {
  if (type_ != Type::kString) {
    return make_status(StatusCode::kInvalidArgument, type_error(path, "a string"));
  }
  return string_;
}

Result<std::int64_t> JsonValue::require_integer(std::string_view path) const {
  if (type_ != Type::kNumber) {
    return make_status(StatusCode::kInvalidArgument, type_error(path, "an integer"));
  }
  return number_;
}

Result<bool> JsonValue::require_bool(std::string_view path) const {
  if (type_ != Type::kBool) {
    return make_status(StatusCode::kInvalidArgument, type_error(path, "a boolean"));
  }
  return boolean_;
}

Result<const JsonValue*> JsonValue::require_object(std::string_view path) const {
  if (type_ != Type::kObject) {
    return make_status(StatusCode::kInvalidArgument, type_error(path, "an object"));
  }
  return this;
}

Result<const JsonValue*> JsonValue::require_array(std::string_view path) const {
  if (type_ != Type::kArray) {
    return make_status(StatusCode::kInvalidArgument, type_error(path, "an array"));
  }
  return this;
}

Result<const JsonValue*> JsonValue::optional_object(std::string_view path) const {
  if (is_null()) {
    return static_cast<const JsonValue*>(nullptr);
  }
  return require_object(path);
}

Result<std::string> JsonValue::optional_string(std::string_view path, std::string fallback) const {
  if (is_null()) {
    return fallback;
  }
  auto text = require_string(path);
  if (!text.ok()) {
    return text.status();
  }
  return text;
}

Result<JsonValue> parse_json(std::string_view text, const RuntimeLimits& limits) {
  JsonReader reader(text, limits);
  return reader.parse();
}

std::string to_json(const JsonValue& value, bool pretty) {
  std::string out;
  write_value(value, pretty, 0, out);
  return out;
}

}  // namespace rollout_fabric
