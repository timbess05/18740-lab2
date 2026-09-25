// Report aggregation and output. Run from lab3-reduced/.

#include "report.hpp"
#include "test_helpers.hpp"

#include <sstream>
#include <string>

namespace {

SimConfig config_for(ProtocolKind protocol) {
  SimConfig config;
  config.protocol = protocol;
  return config;
}

Report report_for(ProtocolKind protocol, const std::string &trace_path) {
  const SimConfig config = config_for(protocol);
  Simulator sim(config, Trace(trace_path));
  sim.run();
  return Report::collect(sim, config, trace_path);
}

void test_totals_sum_the_per_core_stats() {
  const Report report = report_for(ProtocolKind::MSI, "tests/traces/sim_private_rw.trace");
  uint64_t reads = 0, writes = 0, stalls = 0;
  for (const CoreStats &core : report.cores) {
    reads += core.reads;
    writes += core.writes;
    stalls += core.stall_cycles;
  }
  EXPECT_EQ(report.totals.reads, reads);
  EXPECT_EQ(report.totals.writes, writes);
  EXPECT_EQ(report.totals.stall_cycles, stalls);
  EXPECT_EQ(report.accesses(), reads + writes);
  EXPECT_EQ(report.cores.size(), std::size_t{4});
}

void test_config_is_echoed() {
  const Report report = report_for(ProtocolKind::MOESI, "tests/traces/sim_dirty_share.trace");
  EXPECT_EQ(report.protocol, std::string("MOESI"));
  EXPECT_EQ(report.trace_path, std::string("tests/traces/sim_dirty_share.trace"));
  EXPECT_EQ(report.num_cores, std::size_t{4});
  EXPECT_TRUE(!report.ideal_bus);
}

void test_utilisation_is_zero_under_the_perfect_model() {
  SimConfig config = config_for(ProtocolKind::MSI);
  config.set_perfect(true);
  Simulator sim(config, Trace("tests/traces/sim_dirty_share.trace"));
  sim.run();
  const Report report = Report::collect(sim, config, "perfect");

  // Neither resource spends a cycle serving, so both ratios are zero however
  // long the run takes.
  EXPECT_EQ(report.bus.busy_cycles, std::uint64_t{0});
  EXPECT_EQ(report.memory.service_cycles, std::uint64_t{0});
  EXPECT_TRUE(report.bus_utilisation() == 0.0);
  EXPECT_TRUE(report.memory_utilisation() == 0.0);
  // Cores still advance, one cache probe per access -- without that they would
  // never yield to each other and no sharing would occur.
  EXPECT_TRUE(report.makespan > std::uint64_t{0});
  EXPECT_TRUE(report.stall_per_access() == 1.0);
  // Traffic is still counted, which is the whole point of the ideal model.
  EXPECT_TRUE(report.bus.transactions > 0);
}

void test_ratios_survive_a_zero_length_run() {
  // Degenerate on purpose: a free cache probe makes makespan 0 under the ideal
  // model, and every ratio must return 0 rather than dividing by zero.
  SimConfig config = config_for(ProtocolKind::MSI);
  config.set_perfect(true);
  config.cache_hit_cycles = 0;
  Simulator sim(config, Trace("tests/traces/sim_dirty_share.trace"));
  sim.run();
  const Report report = Report::collect(sim, config, "degenerate");

  EXPECT_EQ(report.makespan, std::uint64_t{0});
  EXPECT_TRUE(report.bus_utilisation() == 0.0);
  EXPECT_TRUE(report.memory_utilisation() == 0.0);
  EXPECT_TRUE(report.stall_per_access() == 0.0);
}

void test_hit_rate_matches_the_counters() {
  const Report report = report_for(ProtocolKind::MSI, "tests/traces/sim_read_share.trace");
  // Four accesses: two cold misses, two hits.
  EXPECT_EQ(report.accesses(), std::uint64_t{4});
  EXPECT_EQ(report.totals.hits, std::uint64_t{2});
  EXPECT_TRUE(report.hit_rate() == 0.5);
}

void test_utilisation_reflects_a_saturated_bus() {
  // Pins its own bus geometry rather than relying on the calibrated defaults:
  // the point is that utilisation tracks the configuration, so the test has to
  // state the configuration it expects.
  SimConfig config = config_for(ProtocolKind::MOESI);
  config.bus.addr_cycles = 4;
  config.bus.width_bytes = 16; // 64 B line -> 4 data cycles, a narrow bus
  Simulator sim(config, Trace("tests/traces/sim_dirty_share.trace"));
  sim.run();
  const Report report = Report::collect(sim, config, "narrow-bus");

  // Nearly every cycle of this run has a transaction on the bus, so the bus is
  // the binding constraint and memory is not.
  EXPECT_TRUE(report.bus_utilisation() > 0.9);
  EXPECT_TRUE(report.memory_utilisation() < 0.5);
  EXPECT_TRUE(report.bus_utilisation() > report.memory_utilisation());
}

// Uses the full 16-core archetype rather than the small fixtures: the small
// ones are latency-bound, so no bandwidth knob moves them at all.
void test_widening_the_bus_relieves_the_interconnect() {
  const auto measure = [](uint32_t width_bytes) {
    SimConfig config = config_for(ProtocolKind::MSI);
    config.bus.width_bytes = width_bytes;
    Simulator sim(config, Trace("traces/dirty_share.trace"));
    sim.run();
    return Report::collect(sim, config, "width");
  };

  const Report narrow = measure(16); // 64 B line -> 4 data cycles
  const Report wide = measure(64);   // 64 B line -> 1 data cycle

  // The calibration in one assertion: widening the bus moves the binding
  // resource from the interconnect to memory. That is what makes room for
  // memory contention -- and therefore the O state -- to show up in runtime.
  EXPECT_TRUE(narrow.bus_utilisation() > narrow.memory_utilisation());
  EXPECT_TRUE(wide.memory_utilisation() > wide.bus_utilisation());
  EXPECT_TRUE(narrow.bus_utilisation() > wide.bus_utilisation());
}

void test_text_output_mentions_the_headline_numbers() {
  const Report report = report_for(ProtocolKind::MOSI, "tests/traces/sim_dirty_share.trace");
  std::ostringstream text;
  report.write_text(text);
  const std::string output = text.str();

  EXPECT_TRUE(output.find("MOSI") != std::string::npos);
  EXPECT_TRUE(output.find("makespan") != std::string::npos);
  EXPECT_TRUE(output.find("bus utilisation") != std::string::npos);
  EXPECT_TRUE(output.find("Per-core") != std::string::npos);
  EXPECT_TRUE(output.find(std::to_string(report.makespan)) != std::string::npos);
}

void test_json_output_is_balanced_and_complete() {
  const Report report = report_for(ProtocolKind::MESI, "tests/traces/sim_private_rw.trace");
  std::ostringstream json;
  report.write_json(json);
  const std::string output = json.str();

  int depth = 0;
  bool ever_negative = false;
  for (char character : output) {
    if (character == '{' || character == '[')
      ++depth;
    if (character == '}' || character == ']')
      --depth;
    ever_negative = ever_negative || depth < 0;
  }
  EXPECT_TRUE(!ever_negative); // never closes more than it opens
  EXPECT_EQ(depth, 0);         // and closes everything it opened

  for (const char *key : {"\"config\"", "\"runtime\"", "\"accesses\"", "\"bus\"", "\"memory\"",
                          "\"cores\"", "\"bus_utilisation\"", "\"makespan\""}) {
    EXPECT_TRUE(output.find(key) != std::string::npos);
  }
  // No trailing comma before a closing brace or bracket.
  EXPECT_TRUE(output.find(",\n  }") == std::string::npos);
  EXPECT_TRUE(output.find(",\n  ]") == std::string::npos);
}

void test_json_reports_one_entry_per_core() {
  const Report report = report_for(ProtocolKind::MSI, "tests/traces/sim_private_rw.trace");
  std::ostringstream json;
  report.write_json(json);
  const std::string output = json.str();

  size_t count = 0;
  for (size_t at = output.find("\"core\":"); at != std::string::npos;
       at = output.find("\"core\":", at + 1)) {
    ++count;
  }
  EXPECT_EQ(count, report.cores.size());
}

} // namespace

int main() {
  test_totals_sum_the_per_core_stats();
  test_config_is_echoed();
  test_utilisation_is_zero_under_the_perfect_model();
  test_ratios_survive_a_zero_length_run();
  test_hit_rate_matches_the_counters();
  test_utilisation_reflects_a_saturated_bus();
  test_widening_the_bus_relieves_the_interconnect();
  test_text_output_mentions_the_headline_numbers();
  test_json_output_is_balanced_and_complete();
  test_json_reports_one_entry_per_core();
  TEST_REPORT_AND_EXIT();
}
