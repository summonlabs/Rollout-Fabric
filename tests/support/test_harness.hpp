// Rollout Fabric - minimal test harness.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Deliberately small and deliberately ohne timeouts. A test that hangs is a
// defect to be diagnosed, not something to be hidden behind a watchdog: the
// suite runs plainly and is allowed to finish on its own.
#pragma once

#include <cstdint>
#include <functional>
#include <ostream>
#include <sstream>
#include <type_traits>
#include <utility>
#include <string>
#include <vector>

namespace rf_test {

struct TestCase {
  std::string name;
  std::string file;
  int line = 0;
  std::function<void()> body;
};

class Registry {
 public:
  [[nodiscard]] static Registry& instance();

  void add(TestCase test);
  [[nodiscard]] const std::vector<TestCase>& tests() const noexcept { return tests_; }

  // Failure recording. A test may record many failures; all of them are shown.
  void fail(const std::string& file, int line, const std::string& message);
  [[nodiscard]] bool current_failed() const noexcept { return current_failures_ > 0; }
  [[nodiscard]] std::size_t current_failures() const noexcept { return current_failures_; }

  void begin_test();
  void end_test();

  // Checks performed outside any test (in a fixture) are attributed to the
  // running test all the same.
  void check(bool condition, const std::string& expression, const std::string& file, int line);

  [[nodiscard]] std::size_t total_checks() const noexcept { return total_checks_; }
  [[nodiscard]] std::size_t failed_checks() const noexcept { return failed_checks_; }
  [[nodiscard]] std::size_t failed_tests() const noexcept { return failed_tests_; }
  [[nodiscard]] const std::vector<std::string>& messages() const noexcept { return messages_; }

  void set_filter(std::string filter) { filter_ = std::move(filter); }
  [[nodiscard]] const std::string& filter() const noexcept { return filter_; }

  int run_all();

 private:
  std::vector<TestCase> tests_;
  std::vector<std::string> messages_;
  std::string filter_;
  std::size_t current_failures_ = 0;
  std::size_t total_checks_ = 0;
  std::size_t failed_checks_ = 0;
  std::size_t failed_tests_ = 0;
};

struct Registrar {
  Registrar(const char* name, const char* file, int line, std::function<void()> body);
};

template <class T, class = void>
struct is_streamable : std::false_type {};

template <class T>
struct is_streamable<T, std::void_t<decltype(std::declval<std::ostream&>()
                                             << std::declval<const T&>())>>
    : std::true_type {};

// Renders a value for a failure message. Falls back to a placeholder for types
// that are not streamable, so the harness never fails to compile because of a
// diagnostic.
template <class T>
[[nodiscard]] std::string describe(const T& value) {
  if constexpr (is_streamable<T>::value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else {
    return "<value>";
  }
}

[[nodiscard]] std::string describe(bool value);
[[nodiscard]] std::string describe(const std::string& value);

}  // namespace rf_test

#define RF_TEST(name)                                                              \
  static void rf_test_body_##name();                                               \
  static const ::rf_test::Registrar rf_test_registrar_##name(#name, __FILE__,      \
                                                             __LINE__,             \
                                                             &rf_test_body_##name); \
  static void rf_test_body_##name()

#define RF_CHECK(expression)                                                       \
  ::rf_test::Registry::instance().check(static_cast<bool>(expression), #expression, \
                                        __FILE__, __LINE__)

#define RF_CHECK_EQ(actual, expected)                                              \
  do {                                                                             \
    const auto& rf_actual_ = (actual);                                             \
    const auto& rf_expected_ = (expected);                                         \
    const bool rf_equal_ = (rf_actual_) == (rf_expected_);                         \
    ::rf_test::Registry::instance().check(                                         \
        rf_equal_,                                                                 \
        std::string(#actual) + " == " + #expected + " (actual: " +                 \
            ::rf_test::describe(rf_actual_) + ", expected: " +                     \
            ::rf_test::describe(rf_expected_) + ")",                               \
        __FILE__, __LINE__);                                                       \
  } while (false)

#define RF_CHECK_NE(actual, unexpected)                                            \
  do {                                                                             \
    const auto& rf_actual_ = (actual);                                             \
    const auto& rf_unexpected_ = (unexpected);                                     \
    ::rf_test::Registry::instance().check(                                         \
        !((rf_actual_) == (rf_unexpected_)),                                       \
        std::string(#actual) + " != " + #unexpected, __FILE__, __LINE__);          \
  } while (false)

#define RF_REQUIRE(expression)                                                     \
  do {                                                                             \
    /* The expression is evaluated exactly once: a command call or any other */    \
    /* expression with side effects must not run twice. */                         \
    const bool rf_require_ok_ = static_cast<bool>(expression);                     \
    ::rf_test::Registry::instance().check(rf_require_ok_, #expression, __FILE__,    \
                                          __LINE__);                               \
    if (!rf_require_ok_) {                                                         \
      return;                                                                      \
    }                                                                              \
  } while (false)

#define RF_FAIL(message)                                                           \
  ::rf_test::Registry::instance().fail(__FILE__, __LINE__, (message))

#define RF_TEST_MAIN()                                                             \
  int main(int argc, char** argv) {                                                \
    if (argc > 1) {                                                                \
      ::rf_test::Registry::instance().set_filter(argv[1]);                         \
    }                                                                              \
    return ::rf_test::Registry::instance().run_all();                              \
  }