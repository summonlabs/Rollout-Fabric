// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "test_harness.hpp"

#include <cstdio>
#include <exception>
#include <iostream>

namespace rf_test {

Registry& Registry::instance() {
  static Registry registry;
  return registry;
}

void Registry::add(TestCase test) { tests_.push_back(std::move(test)); }

void Registry::fail(const std::string& file, int line, const std::string& message) {
  ++current_failures_;
  ++failed_checks_;
  std::string text = file + ":" + std::to_string(line) + ": " + message;
  messages_.push_back(text);
  std::fprintf(stderr, "  FAIL %s\n", text.c_str());
}

void Registry::check(bool condition, const std::string& expression, const std::string& file,
                     int line) {
  ++total_checks_;
  if (!condition) {
    fail(file, line, expression);
  }
}

void Registry::begin_test() { current_failures_ = 0; }

void Registry::end_test() {
  if (current_failures_ != 0) {
    ++failed_tests_;
  }
}

int Registry::run_all() {
  std::size_t ran = 0;
  std::size_t skipped = 0;
  for (const TestCase& test : tests_) {
    if (!filter_.empty() && test.name.find(filter_) == std::string::npos) {
      ++skipped;
      continue;
    }
    ++ran;
    std::printf("[ RUN  ] %s\n", test.name.c_str());
    begin_test();
    try {
      test.body();
    } catch (const std::exception& error) {
      fail(test.file, test.line, std::string("uncaught exception: ") + error.what());
    } catch (...) {
      fail(test.file, test.line, "uncaught non-standard exception");
    }
    end_test();
    std::printf("[ %s ] %s\n", current_failures_ == 0 ? " OK " : "FAIL", test.name.c_str());
  }
  std::printf("\n%zu test(s) run, %zu skipped, %zu failed; %zu check(s), %zu failure(s)\n", ran,
              skipped, failed_tests_, total_checks_, failed_checks_);
  return failed_tests_ == 0 ? 0 : 1;
}

Registrar::Registrar(const char* name, const char* file, int line, std::function<void()> body) {
  TestCase test;
  test.name = name;
  test.file = file;
  test.line = line;
  test.body = std::move(body);
  Registry::instance().add(std::move(test));
}

std::string describe(bool value) { return value ? "true" : "false"; }

std::string describe(const std::string& value) { return "\"" + value + "\""; }

}  // namespace rf_test
