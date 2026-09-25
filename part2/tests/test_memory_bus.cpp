// MainMemory bank timing and SnoopBus arbitration.
// Run from lab3-reduced/ (the Makefile does this).

#include "main_memory.hpp"
#include "protocol.hpp"
#include "snoop_bus.hpp"
#include "test_helpers.hpp"

#include <cstdint>
#include <map>
#include <stdexcept>

namespace {

constexpr size_t kBlock = 64;

// ------------------------------------------------------------ MainMemory

MemoryConfig timed_memory() {
  MemoryConfig config;
  config.num_banks = 8;
  config.row_bytes = 8192;
  config.row_access = 17;
  config.row_open = 17;
  config.row_close = 17;
  config.write_recover = 18;
  return config;
}

void test_first_access_opens_a_row() {
  MainMemory memory(timed_memory());
  // Idle bank: open the row, then read it out.
  EXPECT_EQ(memory.access(0x0000, false, 100), std::uint64_t{100 + 17 + 17});
  EXPECT_EQ(memory.stats().row_misses, std::uint64_t{1});
  EXPECT_EQ(memory.stats().row_hits, std::uint64_t{0});
}

void test_row_hit_is_cheaper() {
  MainMemory memory(timed_memory());
  memory.access(0x0000, false, 0);
  // Same row, and late enough that the bank is idle again.
  EXPECT_EQ(memory.access(0x0040, false, 1000), std::uint64_t{1000 + 17});
  EXPECT_EQ(memory.stats().row_hits, std::uint64_t{1});
}

void test_row_conflict_pays_to_close_first() {
  MainMemory memory(timed_memory());
  const uint64_t first = 0x0000;
  // Same bank, different row: one full stride of (row_bytes * num_banks).
  const uint64_t other_row = 8192ull * 8;
  EXPECT_EQ(memory.bank_of(first), memory.bank_of(other_row));

  memory.access(first, false, 0);
  EXPECT_EQ(memory.access(other_row, false, 1000), std::uint64_t{1000 + 17 + 17 + 17});
}

void test_writes_hold_the_bank_longer() {
  MainMemory memory(timed_memory());
  memory.access(0x0000, false, 0);
  const uint64_t read_done = memory.access(0x0000, false, 1000);
  const uint64_t write_done = memory.access(0x0000, true, 2000);
  EXPECT_EQ(read_done - 1000, std::uint64_t{17});
  EXPECT_EQ(write_done - 2000, std::uint64_t{17 + 18});
}

void test_same_bank_accesses_queue() {
  MainMemory memory(timed_memory());
  const uint64_t completion = memory.access(0x0000, false, 0); // busy until 34
  // Arrives while the bank is still busy.
  const uint64_t second = memory.access(0x0040, false, 10);
  EXPECT_EQ(completion, std::uint64_t{34});
  EXPECT_EQ(second, std::uint64_t{34 + 17}); // waits, then a row hit
  EXPECT_EQ(memory.stats().bank_conflicts, std::uint64_t{1});
  EXPECT_EQ(memory.stats().queue_cycles, std::uint64_t{24});
}

void test_different_banks_do_not_queue() {
  MainMemory memory(timed_memory());
  const uint64_t bank0 = 0x0000;
  const uint64_t bank1 = 8192; // next row interleaves into the next bank
  EXPECT_TRUE(memory.bank_of(bank0) != memory.bank_of(bank1));

  memory.access(bank0, false, 0);
  // Concurrent with the first access, and unaffected by it.
  EXPECT_EQ(memory.access(bank1, false, 0), std::uint64_t{34});
  EXPECT_EQ(memory.stats().bank_conflicts, std::uint64_t{0});
}

void test_single_bank_maximises_queueing() {
  MemoryConfig config = timed_memory();
  config.num_banks = 1;
  MainMemory memory(config);
  memory.access(0x0000, false, 0);
  memory.access(8192, false, 0);      // different row, same (only) bank
  memory.access(8192 * 2, false, 0);  // and again
  EXPECT_EQ(memory.stats().bank_conflicts, std::uint64_t{2});
}

void test_infinite_bandwidth_is_free() {
  MemoryConfig config = timed_memory();
  config.infinite_bandwidth = true;
  MainMemory memory(config);
  EXPECT_EQ(memory.access(0x0000, false, 500), std::uint64_t{500});
  EXPECT_EQ(memory.access(0x0000, true, 500), std::uint64_t{500});
  // Accesses are still counted, so traffic remains measurable.
  EXPECT_EQ(memory.stats().reads, std::uint64_t{1});
  EXPECT_EQ(memory.stats().writes, std::uint64_t{1});
}

void test_invalid_memory_config_throws() {
  bool threw = false;
  try {
    MemoryConfig config;
    config.num_banks = 0;
    MainMemory memory(config);
  } catch (const std::runtime_error &) {
    threw = true;
  }
  EXPECT_TRUE(threw);
}

// -------------------------------------------------------------- SnoopBus

// A cache stand-in: holds one state per address, records what the bus did.
class FakeCache : public SnoopTarget {
public:
  explicit FakeCache(uint32_t id) : id_(id) {}

  uint32_t core_id() const override { return id_; }

  CacheState snoop(uint64_t addr) const override {
    const auto found = lines_.find(addr);
    return found == lines_.end() ? CacheState::I : found->second;
  }

  void apply_transition(uint64_t addr, CacheState new_state) override {
    lines_[addr] = new_state;
    ++transitions_applied;
  }

  void set(uint64_t addr, CacheState state) { lines_[addr] = state; }

  uint64_t transitions_applied = 0;

private:
  uint32_t id_;
  std::map<uint64_t, CacheState> lines_;
};

BusConfig timed_bus() {
  BusConfig config;
  config.addr_cycles = 4;
  config.width_bytes = 16; // 64B line -> 4 data cycles
  return config;
}

void test_read_miss_to_memory_costs_bus_plus_memory() {
  MSIProtocol protocol;
  MainMemory memory(timed_memory());
  SnoopBus bus(timed_bus(), protocol, memory, kBlock);
  FakeCache core0(0), core1(1);
  bus.register_core(core0);
  bus.register_core(core1);

  const BusResponse response = bus.issue({0, BusReqType::Read, 0x0000, 0});
  // Bus: 4 addr + 4 data = 8. Memory starts at cycle 4, takes 34, ends at 38.
  EXPECT_EQ(bus.free_at(), std::uint64_t{8});
  EXPECT_EQ(response.latency, std::uint64_t{38});
  EXPECT_TRUE(response.new_state == CacheState::S);
}

void test_upgrade_skips_the_data_phase_and_memory() {
  MSIProtocol protocol;
  MainMemory memory(timed_memory());
  SnoopBus bus(timed_bus(), protocol, memory, kBlock);
  FakeCache core0(0), core1(1);
  bus.register_core(core0);
  bus.register_core(core1);
  core1.set(0x0000, CacheState::S);

  const BusResponse response = bus.issue({0, BusReqType::Upgrade, 0x0000, 0});
  // Address phase only: no data moves and memory is untouched. This 4-vs-8
  // gap, and the absent memory access, is what the E state saves.
  EXPECT_EQ(response.latency, std::uint64_t{4});
  EXPECT_EQ(bus.free_at(), std::uint64_t{4});
  EXPECT_EQ(memory.stats().reads, std::uint64_t{0});
  EXPECT_EQ(memory.stats().writes, std::uint64_t{0});
  EXPECT_EQ(bus.stats().invalidations, std::uint64_t{1});
}

void test_peer_supplied_line_skips_the_memory_read() {
  MSIProtocol protocol;
  MainMemory memory(timed_memory());
  SnoopBus bus(timed_bus(), protocol, memory, kBlock);
  FakeCache core0(0), core1(1);
  bus.register_core(core0);
  bus.register_core(core1);
  core1.set(0x0000, CacheState::S);

  const BusResponse response = bus.issue({0, BusReqType::Read, 0x0000, 0});
  EXPECT_EQ(memory.stats().reads, std::uint64_t{0});
  EXPECT_EQ(bus.stats().peer_supplied, std::uint64_t{1});
  EXPECT_EQ(response.latency, std::uint64_t{8}); // bus occupancy only
}

void test_msi_read_of_modified_line_writes_back() {
  MSIProtocol protocol;
  MainMemory memory(timed_memory());
  SnoopBus bus(timed_bus(), protocol, memory, kBlock);
  FakeCache core0(0), core1(1);
  bus.register_core(core0);
  bus.register_core(core1);
  core1.set(0x0000, CacheState::M);

  bus.issue({0, BusReqType::Read, 0x0000, 0});
  // The flush MSI owes because it has no O state: data goes cache-to-cache
  // AND memory is updated.
  EXPECT_EQ(bus.stats().peer_supplied, std::uint64_t{1});
  EXPECT_EQ(memory.stats().reads, std::uint64_t{0});
  EXPECT_EQ(memory.stats().writes, std::uint64_t{1});
  EXPECT_TRUE(core1.snoop(0x0000) == CacheState::S);
}

void test_transactions_serialize_even_on_different_lines() {
  MSIProtocol protocol;
  MainMemory memory(timed_memory());
  SnoopBus bus(timed_bus(), protocol, memory, kBlock);
  FakeCache core0(0), core1(1);
  bus.register_core(core0);
  bus.register_core(core1);
  core1.set(0x0000, CacheState::S);
  core1.set(0x8000, CacheState::S);

  // Two unrelated addresses issued in the same cycle. The bus is a total
  // order, so the second waits for the first.
  bus.issue({0, BusReqType::Read, 0x0000, 0});
  const BusResponse second = bus.issue({0, BusReqType::Read, 0x8000, 0});

  EXPECT_EQ(bus.stats().arbitration_cycles, std::uint64_t{8});
  EXPECT_EQ(second.latency, std::uint64_t{16}); // waited 8, then took 8
  EXPECT_EQ(bus.free_at(), std::uint64_t{16});
}

void test_bus_is_released_before_memory_finishes() {
  MSIProtocol protocol;
  MainMemory memory(timed_memory());
  SnoopBus bus(timed_bus(), protocol, memory, kBlock);
  FakeCache core0(0), core1(1);
  bus.register_core(core0);
  bus.register_core(core1);

  const BusResponse first = bus.issue({0, BusReqType::Read, 0x0000, 0});
  // The memory access runs to cycle 38, but the bus frees at 8, so a second
  // transaction overlaps it rather than queueing behind it.
  EXPECT_EQ(first.latency, std::uint64_t{38});
  EXPECT_EQ(bus.free_at(), std::uint64_t{8});

  const BusResponse second = bus.issue({1, BusReqType::Read, 0x8000, 10});
  EXPECT_EQ(bus.stats().arbitration_cycles, std::uint64_t{0}); // bus was free
  // Its own memory access ran in a different bank, concurrently with the first.
  // Granted at 10, memory starts at 14 and runs 34 cycles, ending at 48.
  EXPECT_EQ(second.latency, std::uint64_t{38});
  EXPECT_EQ(memory.stats().bank_conflicts, std::uint64_t{0});
}

void test_infinite_bandwidth_bus_is_free() {
  MSIProtocol protocol;
  MemoryConfig ideal_memory = timed_memory();
  ideal_memory.infinite_bandwidth = true;
  MainMemory memory(ideal_memory);

  BusConfig config = timed_bus();
  config.infinite_bandwidth = true;
  SnoopBus bus(config, protocol, memory, kBlock);
  FakeCache core0(0), core1(1);
  bus.register_core(core0);
  bus.register_core(core1);

  const BusResponse response = bus.issue({0, BusReqType::Read, 0x0000, 500});
  EXPECT_EQ(response.latency, std::uint64_t{0});
  EXPECT_EQ(bus.stats().arbitration_cycles, std::uint64_t{0});
  // Coherence still happened, and traffic is still counted.
  EXPECT_TRUE(response.new_state == CacheState::S);
  EXPECT_EQ(bus.stats().transactions, std::uint64_t{1});
}

void test_stats_count_by_type() {
  MSIProtocol protocol;
  MainMemory memory(timed_memory());
  SnoopBus bus(timed_bus(), protocol, memory, kBlock);
  FakeCache core0(0), core1(1);
  bus.register_core(core0);
  bus.register_core(core1);
  core1.set(0x0000, CacheState::S);

  bus.issue({0, BusReqType::Read, 0x0000, 0});
  bus.issue({0, BusReqType::Upgrade, 0x0000, 100});
  bus.issue({0, BusReqType::Writeback, 0x0000, 200});

  EXPECT_EQ(bus.stats().transactions, std::uint64_t{3});
  EXPECT_EQ(bus.stats().reads, std::uint64_t{1});
  EXPECT_EQ(bus.stats().upgrades, std::uint64_t{1});
  EXPECT_EQ(bus.stats().writebacks, std::uint64_t{1});
  EXPECT_EQ(memory.stats().writes, std::uint64_t{1}); // the writeback
}

void test_mi_read_sharing_costs_more_than_msi() {
  // The same access pattern under both protocols: core 1 holds the line, core 0
  // reads it. MI must take ownership; MSI shares.
  MainMemory mi_memory(timed_memory());
  MIProtocol mi;
  SnoopBus mi_bus(timed_bus(), mi, mi_memory, kBlock);
  FakeCache mi_core0(0), mi_core1(1);
  mi_bus.register_core(mi_core0);
  mi_bus.register_core(mi_core1);
  mi_core1.set(0x0000, CacheState::M);
  mi_bus.issue({0, BusReqType::ReadX, 0x0000, 0});

  MainMemory msi_memory(timed_memory());
  MSIProtocol msi;
  SnoopBus msi_bus(timed_bus(), msi, msi_memory, kBlock);
  FakeCache msi_core0(0), msi_core1(1);
  msi_bus.register_core(msi_core0);
  msi_bus.register_core(msi_core1);
  msi_core1.set(0x0000, CacheState::M);
  msi_bus.issue({0, BusReqType::Read, 0x0000, 0});

  // Both write back, but MI leaves the previous holder invalid, so its next
  // read is another full transaction. MSI leaves it shared.
  EXPECT_TRUE(mi_core1.snoop(0x0000) == CacheState::I);
  EXPECT_TRUE(msi_core1.snoop(0x0000) == CacheState::S);
  EXPECT_EQ(mi_bus.stats().invalidations, std::uint64_t{1});
  EXPECT_EQ(msi_bus.stats().invalidations, std::uint64_t{0});
}

} // namespace

int main() {
  test_first_access_opens_a_row();
  test_row_hit_is_cheaper();
  test_row_conflict_pays_to_close_first();
  test_writes_hold_the_bank_longer();
  test_same_bank_accesses_queue();
  test_different_banks_do_not_queue();
  test_single_bank_maximises_queueing();
  test_infinite_bandwidth_is_free();
  test_invalid_memory_config_throws();

  test_read_miss_to_memory_costs_bus_plus_memory();
  test_upgrade_skips_the_data_phase_and_memory();
  test_peer_supplied_line_skips_the_memory_read();
  test_msi_read_of_modified_line_writes_back();
  test_transactions_serialize_even_on_different_lines();
  test_bus_is_released_before_memory_finishes();
  test_infinite_bandwidth_bus_is_free();
  test_stats_count_by_type();
  test_mi_read_sharing_costs_more_than_msi();
  TEST_REPORT_AND_EXIT();
}
