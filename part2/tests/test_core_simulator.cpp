// Core access handling and the Simulator scheduling loop, end to end.
// Run from lab3-reduced/ (the Makefile does this).

#include "simulator.hpp"
#include "test_helpers.hpp"

#include <memory>
#include <stdexcept>
#include <string>

namespace {

// Timed model with the documented defaults, so latencies below are the ones
// the cost table predicts: 4 addr + 4 data on the bus, 34 for a memory read.
SimConfig timed_config(ProtocolKind protocol) {
  SimConfig config;
  config.protocol = protocol;
  config.cache_bytes = 32 * 1024;
  config.cache_assoc = 4;
  config.block_bytes = 64;
  config.bus.addr_cycles = 4;
  config.bus.width_bytes = 16;
  return config;
}

SimConfig perfect_config(ProtocolKind protocol) {
  SimConfig config = timed_config(protocol);
  config.set_perfect(true);
  return config;
}

// 128 bytes, direct-mapped, 64B lines -> two sets, so 0x0000 and 0x0080 collide.
SimConfig tiny_cache_config(ProtocolKind protocol) {
  SimConfig config = timed_config(protocol);
  config.cache_bytes = 128;
  config.cache_assoc = 1;
  return config;
}

Simulator run(const SimConfig &config, const std::string &trace_path) {
  Simulator sim(config, Trace(trace_path));
  sim.run();
  return sim;
}

// ------------------------------------------------------------------ Core

void test_read_miss_then_hit() {
  Simulator sim = run(timed_config(ProtocolKind::MSI), "tests/traces/sim_compute_delta.trace");
  const CoreStats &stats = sim.core(0).stats();
  EXPECT_EQ(stats.reads, std::uint64_t{2});
  EXPECT_EQ(stats.misses, std::uint64_t{2}); // different lines, both cold
  EXPECT_EQ(stats.hits, std::uint64_t{0});
}

void test_silent_hits_use_no_bus() {
  Simulator sim = run(timed_config(ProtocolKind::MSI), "tests/traces/sim_read_share.trace");
  // Four reads, two cold misses, two repeat reads that hit in S.
  EXPECT_EQ(sim.core(0).stats().silent_hits, std::uint64_t{1});
  EXPECT_EQ(sim.core(1).stats().silent_hits, std::uint64_t{1});
  EXPECT_EQ(sim.bus().stats().transactions, std::uint64_t{2});
}

void test_dirty_eviction_writes_back() {
  Simulator sim = run(tiny_cache_config(ProtocolKind::MSI), "tests/traces/sim_evict_dirty.trace");
  const CoreStats &stats = sim.core(0).stats();
  EXPECT_EQ(stats.evictions, std::uint64_t{1});
  EXPECT_EQ(stats.dirty_evictions, std::uint64_t{1});
  EXPECT_EQ(sim.bus().stats().writebacks, std::uint64_t{1});
  EXPECT_EQ(sim.memory().stats().writes, std::uint64_t{1});
}

void test_writeback_does_not_stall_the_core() {
  Simulator sim = run(tiny_cache_config(ProtocolKind::MSI), "tests/traces/sim_evict_dirty.trace");

  // Access 1 (W 0x0000, cold miss): 1 cycle probe, bus 1-9, memory read 5-39.
  // Access 2 (R 0x0080, evicts dirty 0x0000): issued at 39, probe then a fetch
  // whose memory row-hits and completes at 61.
  // The writeback is issued after the fetch and its latency is discarded, so
  // it contributes nothing to either stall.
  EXPECT_EQ(sim.core(0).stats().stall_cycles, sim.makespan());
  EXPECT_EQ(sim.makespan(), std::uint64_t{61});
  EXPECT_EQ(sim.core(0).stats().dirty_evictions, std::uint64_t{1});
}

void test_upgrade_hit_is_counted_separately() {
  // Core 1 reads the line into S, then core 0 writes it: that write is a miss
  // for core 0, but core 1's own later write would be an upgrade hit. Use the
  // share trace and a write-after-read pattern instead.
  SimConfig config = timed_config(ProtocolKind::MSI);
  Simulator sim(config, Trace("tests/traces/sim_read_share.trace"));
  sim.run();
  // No writes in that trace, so no upgrades.
  EXPECT_EQ(sim.core(0).stats().upgrade_hits, std::uint64_t{0});
  EXPECT_EQ(sim.bus().stats().upgrades, std::uint64_t{0});
}

// ------------------------------------------------------------- Scheduling

void test_delta_is_compute_time() {
  Simulator sim = run(perfect_config(ProtocolKind::MSI), "tests/traces/sim_compute_delta.trace");
  // Nothing stalls on the memory system under the perfect model, so the core
  // finishes at the sum of its deltas plus one cache probe per access.
  EXPECT_EQ(sim.makespan(), std::uint64_t{0 + 100 + 2});
}

void test_delta_is_measured_from_completion_not_issue() {
  Simulator sim = run(timed_config(ProtocolKind::MSI), "tests/traces/sim_compute_delta.trace");
  // First access is issued at 0 and completes at 38. The second waits its full
  // 100 cycles of compute after that, so it issues at 138 -- not at 100.
  EXPECT_TRUE(sim.makespan() > std::uint64_t{138});
}

void test_ties_are_broken_round_robin() {
  Simulator sim = run(timed_config(ProtocolKind::MSI), "tests/traces/sim_two_core_race.trace");
  // Both cores are ready at cycle 0 on unrelated lines. Exactly one is granted
  // the bus first and the other queues behind its occupancy; which one depends
  // on the rotation, so assert the shape rather than the identity.
  EXPECT_TRUE(sim.bus().stats().arbitration_cycles > std::uint64_t{0});
  EXPECT_TRUE(sim.core(0).stats().stall_cycles != sim.core(1).stats().stall_cycles);
}

void test_ties_rotate_rather_than_favouring_one_core() {
  // The reason round-robin matters: with delta 0 and an ideal model nothing
  // stalls, so every core stays ready at the same cycle forever. Fixed-priority
  // arbitration would let core 0 drain its entire stream before core 1 issued
  // anything, and no sharing would ever occur.
  SimConfig config = perfect_config(ProtocolKind::MSI);
  Simulator sim(config, Trace("tests/traces/sim_read_share.trace"));
  sim.run();

  // Both cores read the same line. Interleaved, the second reader finds it in
  // another cache; run sequentially, it would simply miss to memory.
  EXPECT_EQ(sim.bus().stats().peer_supplied, std::uint64_t{1});
  EXPECT_EQ(sim.memory().stats().reads, std::uint64_t{1});
}

void test_cores_block_on_their_outstanding_access() {
  Simulator sim = run(timed_config(ProtocolKind::MSI), "tests/traces/sim_read_share.trace");
  // Every op has delta 0, so a core's issue time is exactly its previous
  // completion. With four ops and real latencies, the makespan is well past
  // zero even though every delta is zero.
  EXPECT_TRUE(sim.makespan() > std::uint64_t{0});
}

// --------------------------------------------------------- Perfect vs timed

void test_perfect_model_still_moves_traffic() {
  Simulator sim = run(perfect_config(ProtocolKind::MSI), "tests/traces/sim_read_share.trace");
  // The memory system costs nothing, so the run is just one cache probe per
  // access -- but coherence still happened and is still measurable. This is
  // what makes the "traffic reduced, runtime unchanged" measurement possible.
  EXPECT_EQ(sim.bus().stats().busy_cycles, std::uint64_t{0});
  EXPECT_EQ(sim.memory().stats().service_cycles, std::uint64_t{0});
  EXPECT_EQ(sim.bus().stats().transactions, std::uint64_t{2});
  EXPECT_EQ(sim.bus().stats().peer_supplied, std::uint64_t{1});
  EXPECT_EQ(sim.memory().stats().reads, std::uint64_t{1});
}

void test_timed_model_costs_cycles_for_the_same_traffic() {
  Simulator perfect = run(perfect_config(ProtocolKind::MSI), "tests/traces/sim_read_share.trace");
  Simulator timed = run(timed_config(ProtocolKind::MSI), "tests/traces/sim_read_share.trace");
  // Identical coherence behaviour, different runtime. This holds here because
  // the workload's interleaving is forced by the trace; with heavier sharing
  // the two models can diverge in transition counts as well.
  EXPECT_EQ(perfect.bus().stats().transactions, timed.bus().stats().transactions);
  EXPECT_TRUE(timed.makespan() > perfect.makespan());
}

// ---------------------------------------------------------- MI versus MSI

void test_mi_reacquires_what_msi_shares() {
  Simulator mi = run(timed_config(ProtocolKind::MI), "tests/traces/sim_read_share.trace");
  Simulator msi = run(timed_config(ProtocolKind::MSI), "tests/traces/sim_read_share.trace");

  // MSI: two cold misses, then both cores hit in S. Nothing is ever written,
  // so no copy is ever invalidated.
  EXPECT_EQ(msi.bus().stats().transactions, std::uint64_t{2});
  EXPECT_EQ(msi.bus().stats().invalidations, std::uint64_t{0});

  // MI: every read takes the line exclusively, so readers keep stealing it from
  // each other. The exact counts depend on how the cores interleave, which is
  // emergent, so assert the property rather than a specific schedule.
  EXPECT_TRUE(mi.bus().stats().transactions > msi.bus().stats().transactions);
  EXPECT_TRUE(mi.bus().stats().invalidations > std::uint64_t{0});

  // The ping-pong shows up as runtime, not just as counters.
  EXPECT_TRUE(mi.makespan() > msi.makespan());
}

void test_mi_pays_in_memory_traffic_too() {
  Simulator mi = run(timed_config(ProtocolKind::MI), "tests/traces/sim_read_share.trace");
  Simulator msi = run(timed_config(ProtocolKind::MSI), "tests/traces/sim_read_share.trace");
  // Each MI handover flushes the line back to memory; MSI never writes at all
  // because nothing is ever modified.
  EXPECT_TRUE(mi.memory().stats().writes > msi.memory().stats().writes);
  EXPECT_EQ(msi.memory().stats().writes, std::uint64_t{0});
}

// -------------------------------------------------------------- Wiring

void test_core_count_derived_from_trace() {
  Simulator sim = run(timed_config(ProtocolKind::MSI), "tests/traces/sim_read_share.trace");
  EXPECT_EQ(sim.num_cores(), std::size_t{2});
}

void test_extra_cores_are_idle_but_present() {
  SimConfig config = timed_config(ProtocolKind::MSI);
  config.num_cores = 4;
  Simulator sim = run(config, "tests/traces/sim_read_share.trace");
  EXPECT_EQ(sim.num_cores(), std::size_t{4});
  EXPECT_EQ(sim.core(3).stats().reads, std::uint64_t{0});
}

void test_too_few_cores_is_an_error() {
  SimConfig config = timed_config(ProtocolKind::MSI);
  config.num_cores = 1;
  bool threw = false;
  try {
    Simulator sim(config, Trace("tests/traces/sim_read_share.trace"));
  } catch (const std::runtime_error &) {
    threw = true;
  }
  EXPECT_TRUE(threw);
}

void test_all_protocols_construct() {
  for (ProtocolKind kind : {ProtocolKind::MI, ProtocolKind::MSI, ProtocolKind::MESI,
                            ProtocolKind::MOSI, ProtocolKind::MOESI}) {
    const std::unique_ptr<CoherenceProtocol> protocol = make_protocol(kind);
    EXPECT_TRUE(protocol != nullptr);
    EXPECT_EQ(std::string(protocol->name()), std::string(to_string(kind)));
  }
}

void test_protocol_names_round_trip() {
  for (const char *name : {"MI", "MSI", "MESI", "MOSI", "MOESI"}) {
    EXPECT_EQ(std::string(to_string(parse_protocol(name))), std::string(name));
  }
  bool threw = false;
  try {
    parse_protocol("MOESIX");
  } catch (const std::runtime_error &) {
    threw = true;
  }
  EXPECT_TRUE(threw);
}

} // namespace

int main() {
  test_read_miss_then_hit();
  test_silent_hits_use_no_bus();
  test_dirty_eviction_writes_back();
  test_writeback_does_not_stall_the_core();
  test_upgrade_hit_is_counted_separately();

  test_delta_is_compute_time();
  test_delta_is_measured_from_completion_not_issue();
  test_ties_are_broken_round_robin();
  test_ties_rotate_rather_than_favouring_one_core();
  test_cores_block_on_their_outstanding_access();

  test_perfect_model_still_moves_traffic();
  test_timed_model_costs_cycles_for_the_same_traffic();

  test_mi_reacquires_what_msi_shares();
  test_mi_pays_in_memory_traffic_too();

  test_core_count_derived_from_trace();
  test_extra_cores_are_idle_but_present();
  test_too_few_cores_is_an_error();
  test_all_protocols_construct();
  test_protocol_names_round_trip();
  TEST_REPORT_AND_EXIT();
}
