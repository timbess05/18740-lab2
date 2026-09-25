// Hand-rolled assertion macros for Phase 1 tests. Phase 2+ may swap in a real
// framework if the test suite grows. Kept tiny on purpose.

#pragma once

#include <cstdio>
#include <exception>

namespace test_helpers {

struct Counters {
  int passed = 0;
  int failed = 0;
};

inline Counters &counters() {
  static Counters c;
  return c;
}

} // namespace test_helpers

#define EXPECT_EQ(actual, expected)                                                                \
  do {                                                                                             \
    auto _a = (actual);                                                                            \
    auto _e = (expected);                                                                          \
    if (_a != _e) {                                                                                \
      std::fprintf(stderr, "%s:%d: EXPECT_EQ failed: %s != %s\n", __FILE__, __LINE__, #actual,     \
                   #expected);                                                                     \
      ++::test_helpers::counters().failed;                                                         \
    } else {                                                                                       \
      ++::test_helpers::counters().passed;                                                         \
    }                                                                                              \
  } while (0)

#define EXPECT_TRUE(expr)                                                                          \
  do {                                                                                             \
    if (!(expr)) {                                                                                 \
      std::fprintf(stderr, "%s:%d: EXPECT_TRUE failed: %s\n", __FILE__, __LINE__, #expr);          \
      ++::test_helpers::counters().failed;                                                         \
    } else {                                                                                       \
      ++::test_helpers::counters().passed;                                                         \
    }                                                                                              \
  } while (0)

// Runs one test function, turning an escaped exception into a reported failure
// instead of an abort. The simulator's always-on validation throws on a bad
// protocol decision, so without this a single fault kills the whole binary and
// says nothing about which case provoked it.
#define RUN_TEST(fn)                                                                               \
  do {                                                                                             \
    try {                                                                                          \
      fn();                                                                                        \
    } catch (const std::exception &_e) {                                                           \
      std::fprintf(stderr, "%s: threw: %s\n", #fn, _e.what());                                     \
      ++::test_helpers::counters().failed;                                                         \
    }                                                                                              \
  } while (0)

#define TEST_REPORT_AND_EXIT()                                                                     \
  do {                                                                                             \
    auto &_c = ::test_helpers::counters();                                                         \
    std::fprintf(stderr, "tests: %d passed, %d failed\n", _c.passed, _c.failed);                   \
    return _c.failed == 0 ? 0 : 1;                                                                 \
  } while (0)
