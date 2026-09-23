#pragma once

// Queue Observatory test harness.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The harness is deliberately tiny and dependency free: a registry, a set of
// assertion macros that record file and line, and a runner. There is no
// timeout anywhere in the harness or in any test. A test either reaches its
// conclusion or the process is killed by the operating system, which is what
// makes a hanging test a real defect rather than a flake.

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "qobs/core/Result.hpp"
#include "qobs/model/Evidence.hpp"
#include "qobs/model/Identity.hpp"
#include "qobs/model/Sample.hpp"

namespace qobs::test {

struct TestCase {
  std::string suite{};
  std::string name{};
  void (*fn)() = nullptr;
  bool slow{false};
};

[[nodiscard]] std::vector<TestCase>& registry();

struct Registrar {
  Registrar(const char* suite, const char* name, void (*fn)(), bool slow);
};

/// Render a value for an assertion message. Arithmetic types and strings have
/// readable forms; everything else falls back to a placeholder.
template <class T>
[[nodiscard]] std::string display(const T& value) {
  if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (std::is_same_v<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_integral_v<T>) {
    return std::to_string(value);
  } else if constexpr (std::is_floating_point_v<T>) {
    return std::to_string(value);
  } else if constexpr (std::is_same_v<T, std::string>) {
    return "\"" + value + "\"";
  } else if constexpr (std::is_same_v<T, std::string_view>) {
    return "\"" + std::string(value) + "\"";
  } else {
    return "<value>";
  }
}

/// Record a failed assertion for the currently running test.
void report_failure(const char* file, int line, const std::string& message);

/// Assertion entry points.
///
/// Assertions are functions rather than inline statements so that every
/// temporary produced by an operand expression stays alive for the whole
/// comparison, and so that a condition that happens to be a compile-time
/// constant does not trip a constant-condition warning.
bool check(bool passed, const char* expression, const char* file, int line);

template <class A, class B>
bool check_eq(const A& actual, const B& expected, const char* actual_expression,
              const char* expected_expression, const char* file, int line) {
  if (actual == expected) {
    return true;
  }
  report_failure(file, line,
                 std::string("expected ") + actual_expression + " == " + expected_expression +
                     " but saw " + display(actual) + " vs " + display(expected));
  return false;
}

template <class A, class B>
bool check_ne(const A& actual, const B& unexpected, const char* actual_expression,
              const char* unexpected_expression, const char* file, int line) {
  if (!(actual == unexpected)) {
    return true;
  }
  report_failure(file, line,
                 std::string("expected ") + actual_expression + " != " + unexpected_expression);
  return false;
}

bool check_status(const Status& status, const char* expression, const char* file, int line);
bool check_fails(const Status& status, const char* expression, const char* file, int line);

/// Bounded element access.
///
/// Dereferencing an empty result set is a defect in the test, not in the
/// runtime, and turning it into an access violation would hide the real
/// failure. These helpers report it and return a default-constructed value so
/// the assertion that follows produces a readable message.
template <class Container>
[[nodiscard]] const typename Container::value_type& front_of(const Container& container,
                                                             const char* file, int line) {
  if (!container.empty()) {
    return container.front();
  }
  report_failure(file, line, "the result set is empty but its first element was used");
  static const typename Container::value_type kDefault{};
  return kDefault;
}

template <class Container>
[[nodiscard]] const typename Container::value_type& back_of(const Container& container,
                                                            const char* file, int line) {
  if (!container.empty()) {
    return container.back();
  }
  report_failure(file, line, "the result set is empty but its last element was used");
  static const typename Container::value_type kDefault{};
  return kDefault;
}

/// Run every registered test. Returns the process exit code.
int run_all(int argc, char** argv);

}  // namespace qobs::test

#define QOBS_TEST(suite_name, test_name)                                          \
  static void qobs_test_##suite_name##_##test_name();                             \
  static const ::qobs::test::Registrar qobs_registrar_##suite_name##_##test_name( \
      #suite_name, #test_name, &qobs_test_##suite_name##_##test_name, false);     \
  static void qobs_test_##suite_name##_##test_name()

#define QOBS_TEST_SLOW(suite_name, test_name)                                     \
  static void qobs_test_##suite_name##_##test_name();                             \
  static const ::qobs::test::Registrar qobs_registrar_##suite_name##_##test_name( \
      #suite_name, #test_name, &qobs_test_##suite_name##_##test_name, true);      \
  static void qobs_test_##suite_name##_##test_name()

#define QOBS_CHECK(condition) \
  (void)::qobs::test::check(static_cast<bool>(condition), #condition, __FILE__, __LINE__)

#define QOBS_CHECK_EQ(actual, expected) \
  (void)::qobs::test::check_eq((actual), (expected), #actual, #expected, __FILE__, __LINE__)

#define QOBS_CHECK_NE(actual, unexpected) \
  (void)::qobs::test::check_ne((actual), (unexpected), #actual, #unexpected, __FILE__, __LINE__)

#define QOBS_CHECK_STATUS(expr) \
  (void)::qobs::test::check_status((expr), #expr, __FILE__, __LINE__)

#define QOBS_CHECK_FAILS(expr) \
  (void)::qobs::test::check_fails((expr), #expr, __FILE__, __LINE__)

#define QOBS_FRONT(container) ::qobs::test::front_of((container), __FILE__, __LINE__)
#define QOBS_BACK(container) ::qobs::test::back_of((container), __FILE__, __LINE__)

#define QOBS_REQUIRE(condition)                                                      \
  do {                                                                               \
    if (!::qobs::test::check(static_cast<bool>(condition), #condition, __FILE__,      \
                             __LINE__)) {                                            \
      return;                                                                        \
    }                                                                                \
  } while (false)

namespace qobs::test {

/// Create a unique temporary directory for the running test and return its
/// path. The directory is removed when the process exits.
[[nodiscard]] std::string make_temp_directory(const std::string& label);

/// Build a representative sample with explicit values. Kept in the harness so
/// that every suite constructs evidence the same way.
struct SampleBuilder {
  QueueSample sample{};

  SampleBuilder(const char* device, const char* port, std::uint32_t queue_index,
                const char* source, const char* incarnation);
  SampleBuilder& sequence(std::uint64_t value);
  SampleBuilder& generation(std::uint64_t value);
  SampleBuilder& authority(SourceAuthority value);
  SampleBuilder& observed_at(Nanos ns);
  SampleBuilder& received_at(Nanos ns);
  SampleBuilder& set(SampleField field, std::uint64_t value);
  SampleBuilder& classes(std::optional<std::uint16_t> traffic_class,
                         std::optional<const char*> scheduling_class);
  [[nodiscard]] QueueSample build() const { return sample; }
};

}  // namespace qobs::test
