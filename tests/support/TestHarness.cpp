#include "support/TestHarness.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace qobs::test {
namespace {

thread_local int g_test_failures = 0;
thread_local std::string g_current_test{};

std::string temporary_root() {
  static const std::string root = [] {
    std::error_code error;
    const auto base = std::filesystem::temp_directory_path(error);
    const auto stamp = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string path = (base / ("qobs-tests-" + std::to_string(stamp))).string();
    std::filesystem::create_directories(path, error);
    return path;
  }();
  return root;
}

}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

Registrar::Registrar(const char* suite, const char* name, void (*fn)(), bool slow) {
  TestCase test;
  test.suite = suite;
  test.name = name;
  test.fn = fn;
  test.slow = slow;
  registry().push_back(std::move(test));
}

void report_failure(const char* file, int line, const std::string& message) {
  ++g_test_failures;
  std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, message.c_str());
}

bool check(bool passed, const char* expression, const char* file, int line) {
  if (passed) {
    return true;
  }
  report_failure(file, line, std::string("check failed: ") + expression);
  return false;
}

bool check_status(const Status& status, const char* expression, const char* file, int line) {
  if (status.ok()) {
    return true;
  }
  report_failure(file, line, std::string("expected success from ") + expression + " but got " +
                                 status.to_string());
  return false;
}

bool check_fails(const Status& status, const char* expression, const char* file, int line) {
  if (!status.ok()) {
    return true;
  }
  report_failure(file, line,
                 std::string("expected failure from ") + expression + " but it succeeded");
  return false;
}

int run_all(int argc, char** argv) {
  std::string filter;
  bool list_only = false;
  for (int index = 1; index < argc; ++index) {
    const std::string token = argv[index];
    if (token == "--list") {
      list_only = true;
    } else if (token.rfind("--filter=", 0) == 0) {
      filter = token.substr(9);
    }
  }

  std::vector<TestCase>& tests = registry();
  std::stable_sort(tests.begin(), tests.end(), [](const TestCase& left, const TestCase& right) {
    if (left.suite != right.suite) {
      return left.suite < right.suite;
    }
    return left.name < right.name;
  });

  if (list_only) {
    for (const TestCase& test : tests) {
      std::printf("%s.%s\n", test.suite.c_str(), test.name.c_str());
    }
    return 0;
  }

  std::size_t executed = 0;
  std::size_t failed = 0;
  std::string last_suite;
  const auto started = std::chrono::steady_clock::now();
  for (const TestCase& test : tests) {
    const std::string full = test.suite + "." + test.name;
    if (!filter.empty() && full.find(filter) == std::string::npos) {
      continue;
    }
    if (test.suite != last_suite) {
      std::printf("[%s]\n", test.suite.c_str());
      last_suite = test.suite;
    }
    g_test_failures = 0;
    g_current_test = full;
    test.fn();
    ++executed;
    if (g_test_failures != 0) {
      ++failed;
      std::printf("  %-52s FAILED (%d)\n", test.name.c_str(), g_test_failures);
    } else {
      std::printf("  %-52s ok\n", test.name.c_str());
    }
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
  std::printf("\n%d test(s) executed, %d failed, %lld ms\n", static_cast<int>(executed),
              static_cast<int>(failed), static_cast<long long>(elapsed));

  std::error_code error;
  std::filesystem::remove_all(temporary_root(), error);
  return failed == 0 ? 0 : 1;
}

std::string make_temp_directory(const std::string& label) {
  static std::atomic<unsigned> counter{0};
  const std::string path =
      (std::filesystem::path(temporary_root()) /
       (label + "-" + std::to_string(counter.fetch_add(1))))
          .string();
  std::error_code error;
  std::filesystem::create_directories(path, error);
  return path;
}

SampleBuilder::SampleBuilder(const char* device, const char* port, std::uint32_t queue_index,
                             const char* source, const char* incarnation) {
  sample.queue.device = DeviceId(std::string(device));
  sample.queue.port = PortId(std::string(port));
  sample.queue.queue = QueueId::from_raw(queue_index);
  sample.source = SourceId(std::string(source));
  sample.incarnation = IncarnationId(std::string(incarnation));
  sample.generation = GenerationId::from_raw(1);
  sample.sequence = SourceSequence::from_raw(1);
  sample.authority = SourceAuthority::Primary;
  sample.observed.ns = 1000;
  sample.observed.domain = ClockDomainId(std::string("device-clock"));
  sample.received.steady = SteadyTime{1000};
  sample.received.wall = WallTime{1000};
}

SampleBuilder& SampleBuilder::sequence(std::uint64_t value) {
  sample.sequence = SourceSequence::from_raw(value);
  return *this;
}

SampleBuilder& SampleBuilder::generation(std::uint64_t value) {
  sample.generation = GenerationId::from_raw(value);
  return *this;
}

SampleBuilder& SampleBuilder::authority(SourceAuthority value) {
  sample.authority = value;
  return *this;
}

SampleBuilder& SampleBuilder::observed_at(Nanos ns) {
  sample.observed.ns = ns;
  return *this;
}

SampleBuilder& SampleBuilder::received_at(Nanos ns) {
  sample.received.steady = SteadyTime{ns};
  sample.received.wall = WallTime{ns};
  return *this;
}

SampleBuilder& SampleBuilder::set(SampleField field, std::uint64_t value) {
  sample.set_value(field, value);
  return *this;
}

SampleBuilder& SampleBuilder::classes(std::optional<std::uint16_t> traffic_class,
                                      std::optional<const char*> scheduling_class) {
  if (traffic_class.has_value()) {
    sample.classes.traffic_class = TrafficClassId::from_raw(*traffic_class);
    sample.classes.origin = AttributeOrigin::ObservedInSample;
  }
  if (scheduling_class.has_value()) {
    sample.classes.scheduling_class = SchedulingClassId(std::string(*scheduling_class));
    sample.classes.origin = AttributeOrigin::ObservedInSample;
  }
  return *this;
}

}  // namespace qobs::test

int main(int argc, char** argv) { return qobs::test::run_all(argc, argv); }
