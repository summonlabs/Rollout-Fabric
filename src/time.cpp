// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "rollout_fabric/time.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace rollout_fabric {
namespace {

// Days from 1970-01-01 to y-m-d, Hinnant's civil calendar algorithm.
[[nodiscard]] constexpr std::int64_t days_from_civil(std::int64_t year, unsigned month,
                                                     unsigned day) noexcept {
  year -= month <= 2 ? 1 : 0;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned month_prime = month > 2u ? month - 3u : month + 9u;
  const unsigned day_of_year = (153u * month_prime + 2u) / 5u + day - 1u;
  const unsigned day_of_era =
      year_of_era * 365u + year_of_era / 4u - year_of_era / 100u + day_of_year;
  return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

struct CivilDate {
  std::int64_t year = 1970;
  unsigned month = 1;
  unsigned day = 1;
};

[[nodiscard]] constexpr CivilDate civil_from_days(std::int64_t days) noexcept {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned day_of_era = static_cast<unsigned>(days - era * 146097);
  const unsigned year_of_era =
      (day_of_era - day_of_era / 1460u + day_of_era / 36524u - day_of_era / 146096u) / 365u;
  const std::int64_t year = static_cast<std::int64_t>(year_of_era) + era * 400;
  const unsigned day_of_year =
      day_of_era - (365u * year_of_era + year_of_era / 4u - year_of_era / 100u);
  const unsigned mp = (5u * day_of_year + 2u) / 153u;
  const unsigned day = day_of_year - (153u * mp + 2u) / 5u + 1u;
  const unsigned month = mp < 10u ? mp + 3u : mp - 9u;
  return CivilDate{year + (month <= 2 ? 1 : 0), month, day};
}

[[nodiscard]] bool parse_fixed(std::string_view text, std::size_t offset, std::size_t count,
                               int& out) noexcept {
  if (offset + count > text.size()) {
    return false;
  }
  int value = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const char character = text[offset + index];
    if (character < '0' || character > '9') {
      return false;
    }
    value = (value * 10) + (character - '0');
  }
  out = value;
  return true;
}

}  // namespace

Clock::~Clock() = default;

Timestamp SystemClock::wall_now() const {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return Timestamp::from_unix_nanos(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

Timestamp SystemClock::monotonic_now() const {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return Timestamp::from_unix_nanos(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

ManualClock::ManualClock() noexcept
    : wall_(Timestamp::from_unix_seconds(1767225600)),   // 2026-01-01T00:00:00Z
      monotonic_(Timestamp::from_unix_seconds(1767225600)) {}

std::string Timestamp::to_rfc3339() const {
  // Floor division, so that an instant before the epoch is split into a whole
  // number of seconds and a non-negative sub-second remainder. Truncating
  // division would place 1969-12-31T23:59:59.750Z one second into 1970 and
  // print a negative fractional part.
  const std::int64_t total_seconds = nanos_ / 1000000000;
  std::int64_t subsecond_nanos = nanos_ % 1000000000;
  std::int64_t floored_seconds = total_seconds;
  if (subsecond_nanos < 0) {
    subsecond_nanos += 1000000000;
    floored_seconds -= 1;
  }
  std::int64_t seconds_of_day = floored_seconds % 86400;
  std::int64_t days = floored_seconds / 86400;
  if (seconds_of_day < 0) {
    seconds_of_day += 86400;
    --days;
  }
  const CivilDate date = civil_from_days(days);
  const unsigned hour = static_cast<unsigned>(seconds_of_day / 3600);
  const unsigned minute = static_cast<unsigned>((seconds_of_day % 3600) / 60);
  const unsigned second = static_cast<unsigned>(seconds_of_day % 60);
  const std::int64_t millis = subsecond_nanos / 1000000;

  char buffer[40];
  const int written = std::snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02uT%02u:%02u:%02u.%03lldZ",
                                    static_cast<long long>(date.year), date.month, date.day, hour,
                                    minute, second, static_cast<long long>(millis));
  if (written <= 0) {
    return "1970-01-01T00:00:00.000Z";
  }
  return std::string(buffer, static_cast<std::size_t>(written));
}

Result<Timestamp> Timestamp::parse_rfc3339(std::string_view text) {
  if (text.size() < 20) {
    return make_status(StatusCode::kInvalidArgument, "timestamp is shorter than YYYY-MM-DDTHH:MM:SSZ");
  }
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!parse_fixed(text, 0, 4, year) || text[4] != '-' || !parse_fixed(text, 5, 2, month) ||
      text[7] != '-' || !parse_fixed(text, 8, 2, day) ||
      (text[10] != 'T' && text[10] != 't') || !parse_fixed(text, 11, 2, hour) ||
      text[13] != ':' || !parse_fixed(text, 14, 2, minute) || text[16] != ':' ||
      !parse_fixed(text, 17, 2, second)) {
    return make_status(StatusCode::kInvalidArgument, "timestamp '" + std::string(text) +
                                                         "' is not an RFC 3339 instant");
  }
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
    return make_status(StatusCode::kInvalidArgument, "timestamp field out of range");
  }

  std::size_t cursor = 19;
  std::int64_t fractional_nanos = 0;
  if (cursor < text.size() && text[cursor] == '.') {
    ++cursor;
    std::int64_t scale = 100000000;
    std::size_t digits = 0;
    while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
      if (digits < 9) {
        fractional_nanos += static_cast<std::int64_t>(text[cursor] - '0') * scale;
        scale /= 10;
      }
      ++digits;
      ++cursor;
    }
    if (digits == 0) {
      return make_status(StatusCode::kInvalidArgument, "timestamp has an empty fractional part");
    }
  }

  std::int64_t offset_seconds = 0;
  if (cursor < text.size() && (text[cursor] == 'Z' || text[cursor] == 'z')) {
    ++cursor;
  } else if (cursor < text.size() && (text[cursor] == '+' || text[cursor] == '-')) {
    const bool negative = text[cursor] == '-';
    ++cursor;
    int offset_hour = 0;
    int offset_minute = 0;
    if (!parse_fixed(text, cursor, 2, offset_hour)) {
      return make_status(StatusCode::kInvalidArgument, "timestamp offset is malformed");
    }
    cursor += 2;
    if (cursor < text.size() && text[cursor] == ':') {
      ++cursor;
    }
    if (!parse_fixed(text, cursor, 2, offset_minute)) {
      return make_status(StatusCode::kInvalidArgument, "timestamp offset is malformed");
    }
    cursor += 2;
    if (offset_hour > 23 || offset_minute > 59) {
      return make_status(StatusCode::kInvalidArgument, "timestamp offset out of range");
    }
    offset_seconds = static_cast<std::int64_t>(offset_hour) * 3600 +
                     static_cast<std::int64_t>(offset_minute) * 60;
    if (negative) {
      offset_seconds = -offset_seconds;
    }
  } else {
    return make_status(StatusCode::kInvalidArgument,
                       "timestamp must end with Z or a numeric UTC offset");
  }

  if (cursor != text.size()) {
    return make_status(StatusCode::kInvalidArgument,
                       "timestamp has trailing characters: '" + std::string(text.substr(cursor)) + "'");
  }

  const std::int64_t days = days_from_civil(year, static_cast<unsigned>(month),
                                            static_cast<unsigned>(day));
  const std::int64_t seconds =
      days * 86400 + static_cast<std::int64_t>(hour) * 3600 +
      static_cast<std::int64_t>(minute) * 60 + static_cast<std::int64_t>(second) - offset_seconds;
  return Timestamp::from_unix_nanos(seconds * 1000000000 + fractional_nanos);
}

Freshness evaluate_freshness(Timestamp observed_at, Timestamp now, Duration max_age,
                             Duration max_future_skew) {
  Freshness result;
  if (observed_at.is_nil()) {
    result.reason = "no observation time was recorded";
    return result;
  }
  const Duration age = now - observed_at;
  result.age = age;
  if (age.is_negative()) {
    const Duration ahead = Duration::from_nanos(age.nanos() == INT64_MIN ? INT64_MAX : -age.nanos());
    if (ahead > max_future_skew) {
      result.reason = "observation is " + std::to_string(ahead.millis()) +
                      " ms in the future, beyond the permitted clock skew of " +
                      std::to_string(max_future_skew.millis()) + " ms";
      return result;
    }
    result.fresh = true;
    result.reason = "observation is within the permitted clock skew";
    return result;
  }
  if (age > max_age) {
    result.reason = "observation is " + std::to_string(age.millis()) +
                    " ms old, older than the permitted maximum of " +
                    std::to_string(max_age.millis()) + " ms";
    return result;
  }
  result.fresh = true;
  result.reason = "observation is " + std::to_string(age.millis()) + " ms old";
  return result;
}

}  // namespace rollout_fabric
