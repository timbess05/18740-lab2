// Trace parser: field parsing, comment/blank handling, per-core bucketing.
// Run from lab3-reduced/ (the Makefile does this).

#include "test_helpers.hpp"
#include "trace.hpp"

#include <stdexcept>
#include <string>

namespace {

// Returns true if constructing a Trace from `path` threw. Malformed cases.
bool load_throws(const std::string &path) {
  try {
    Trace t(path);
  } catch (const std::runtime_error &) {
    return true;
  }
  return false;
}

void test_basic_fields() {
  const Trace t("tests/traces/basic.trace");
  EXPECT_EQ(t.total_ops(), std::size_t{5});
  EXPECT_EQ(t.num_cores(), std::size_t{1});
  const auto &ops = t.per_core[0];
  EXPECT_EQ(ops[0].delta, std::uint64_t{0});
  EXPECT_EQ(ops[0].core_id, std::uint32_t{0});
  EXPECT_TRUE(ops[0].op == MemOp::Op::Read);
  EXPECT_EQ(ops[0].addr, std::uint64_t{0x1000});
  EXPECT_TRUE(ops[2].op == MemOp::Op::Write);
  EXPECT_EQ(ops[3].delta, std::uint64_t{5});
  // delta 0 means "issue as soon as the previous access completes".
  EXPECT_EQ(ops[4].delta, std::uint64_t{0});
}

void test_deltas_need_not_be_monotonic() {
  // 0, 1, 2, 5, 0 -- a delta may be smaller than the one before it. Nothing to
  // validate beyond non-negativity, which the field parser enforces.
  const Trace t("tests/traces/basic.trace");
  EXPECT_TRUE(t.per_core[0][3].delta > t.per_core[0][4].delta);
}

void test_addresses_not_block_aligned() {
  // The parser must hand back the raw byte address; block alignment is the
  // cache's job so traces can express false sharing.
  const Trace t("tests/traces/basic.trace");
  EXPECT_EQ(t.per_core[0][1].addr, std::uint64_t{0x1040});
}

void test_comments_and_blanks() {
  const Trace t("tests/traces/comments_blanks.trace");
  EXPECT_EQ(t.total_ops(), std::size_t{2});
  EXPECT_EQ(t.per_core[0][0].addr, std::uint64_t{0x1000});
  EXPECT_TRUE(t.per_core[0][1].op == MemOp::Op::Write);
}

void test_empty_trace() {
  const Trace t("tests/traces/empty.trace");
  EXPECT_EQ(t.num_cores(), std::size_t{0});
  EXPECT_EQ(t.total_ops(), std::size_t{0});
}

void test_bucketing_by_core() {
  const Trace t("tests/traces/two_core.trace");
  EXPECT_EQ(t.num_cores(), std::size_t{2});
  EXPECT_EQ(t.total_ops(), std::size_t{4});
  EXPECT_EQ(t.per_core[0].size(), std::size_t{2});
  EXPECT_EQ(t.per_core[1].size(), std::size_t{2});
  EXPECT_TRUE(t.per_core[0][0].op == MemOp::Op::Write);
  EXPECT_TRUE(t.per_core[1][0].op == MemOp::Op::Read);
}

void test_per_core_order_is_file_order() {
  const Trace t("tests/traces/unsorted_cores.trace");
  EXPECT_EQ(t.per_core[0][0].delta, std::uint64_t{0});
  EXPECT_EQ(t.per_core[0][1].delta, std::uint64_t{10});
  EXPECT_EQ(t.per_core[0][2].delta, std::uint64_t{20});
}

void test_concatenated_per_core_streams() {
  // Core 2's ops appear after all of core 0's. There is no cross-core
  // ordering constraint, so this is a valid trace.
  const Trace t("tests/traces/unsorted_cores.trace");
  EXPECT_EQ(t.total_ops(), std::size_t{5});
  EXPECT_EQ(t.per_core[0].size(), std::size_t{3});
  EXPECT_EQ(t.per_core[2].size(), std::size_t{2});
}

void test_absent_core_gets_empty_slot() {
  // The trace names cores 0 and 2, so core 1 exists but is idle.
  const Trace t("tests/traces/unsorted_cores.trace");
  EXPECT_EQ(t.num_cores(), std::size_t{3});
  EXPECT_TRUE(t.per_core[1].empty());
}

void test_malformed_inputs_throw() {
  EXPECT_TRUE(load_throws("tests/traces/bad_op.trace"));          // FLUSH is gone
  EXPECT_TRUE(load_throws("tests/traces/bad_no_0x.trace"));       // bare decimal address
  EXPECT_TRUE(load_throws("tests/traces/bad_extra_field.trace")); // stale `size` column
  EXPECT_TRUE(load_throws("tests/traces/does_not_exist.trace"));
}

void test_error_message_names_file_and_line() {
  try {
    Trace t("tests/traces/bad_op.trace");
    EXPECT_TRUE(false); // should have thrown
  } catch (const std::runtime_error &e) {
    const std::string msg = e.what();
    EXPECT_TRUE(msg.find(":2:") != std::string::npos);
    EXPECT_TRUE(msg.find("FLUSH") != std::string::npos);
  }
}

} // namespace

int main() {
  test_basic_fields();
  test_deltas_need_not_be_monotonic();
  test_addresses_not_block_aligned();
  test_comments_and_blanks();
  test_empty_trace();
  test_bucketing_by_core();
  test_per_core_order_is_file_order();
  test_concatenated_per_core_streams();
  test_absent_core_gets_empty_slot();
  test_malformed_inputs_throw();
  test_error_message_names_file_and_line();
  TEST_REPORT_AND_EXIT();
}
