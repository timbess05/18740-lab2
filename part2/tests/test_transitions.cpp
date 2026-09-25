// Per-state transition tests.
//
// The other suites check that a protocol does the right thing. These check that
// it does ONLY the right thing: every case asserts the complete outcome,
// including what must not change. A silent E->M that also issued a bus
// transaction, or an Upgrade that quietly touched memory, passes every other
// test in the tree -- the states it produces are legal and the counters it
// moves are plausible.
//
// Two levels:
//
//   request_for   an exhaustive (protocol x access x state) table. Every cell
//                 is named: a bus request, a silent hit, or unsupported.
//
//   transitions   driven through a real Core/SnoopBus/MainMemory, snapshotting
//                 every counter and every cache's state before and after, so
//                 an assertion can say "this changed, and nothing else did".
//
// Run from lab3-reduced/.

#include "core.hpp"
#include "main_memory.hpp"
#include "simulator.hpp"
#include "snoop_bus.hpp"
#include "test_helpers.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr size_t kBlock = 64;
constexpr size_t kCacheBytes = 4096; // 16 sets x 4 ways
constexpr size_t kAssoc = 4;

const ProtocolKind kAllProtocols[] = {ProtocolKind::MI, ProtocolKind::MSI, ProtocolKind::MESI,
                                      ProtocolKind::MOSI, ProtocolKind::MOESI};

// ------------------------------------------------------ request_for table

// What a (protocol, access, state) cell is expected to yield.
enum class Expect { Read, ReadX, Upgrade, Silent, Unsupported };

const char *to_string(Expect expect) {
  switch (expect) {
  case Expect::Read:
    return "Read";
  case Expect::ReadX:
    return "ReadX";
  case Expect::Upgrade:
    return "Upgrade";
  case Expect::Silent:
    return "silent (no bus)";
  case Expect::Unsupported:
    return "unsupported state";
  }
  return "?";
}

bool matches(const CoherenceProtocol &protocol, AccessType access, CacheState state,
             Expect expect) {
  std::optional<BusReqType> request;
  try {
    request = protocol.request_for(access, state);
  } catch (const std::runtime_error &) {
    return expect == Expect::Unsupported;
  }
  switch (expect) {
  case Expect::Unsupported:
    return false; // should have thrown
  case Expect::Silent:
    return !request.has_value();
  case Expect::Read:
    return request == BusReqType::Read;
  case Expect::ReadX:
    return request == BusReqType::ReadX;
  case Expect::Upgrade:
    return request == BusReqType::Upgrade;
  }
  return false;
}

struct Cell {
  ProtocolKind kind;
  AccessType access;
  CacheState state;
  Expect expect;
};

// Every protocol x access x state combination, stated explicitly. Reading down
// a protocol's column is the fastest way to see what distinguishes it: MSI and
// MESI differ in exactly one cell, (Write, E), and that cell is the E state's
// entire reason for existing.
const Cell kRequestTable[] = {
    // MI: no way to hold a line for reading without owning it.
    {ProtocolKind::MI, AccessType::Read, CacheState::I, Expect::ReadX},
    {ProtocolKind::MI, AccessType::Write, CacheState::I, Expect::ReadX},
    {ProtocolKind::MI, AccessType::Read, CacheState::M, Expect::Silent},
    {ProtocolKind::MI, AccessType::Write, CacheState::M, Expect::Silent},
    {ProtocolKind::MI, AccessType::Read, CacheState::S, Expect::Unsupported},
    {ProtocolKind::MI, AccessType::Read, CacheState::E, Expect::Unsupported},
    {ProtocolKind::MI, AccessType::Read, CacheState::O, Expect::Unsupported},
    {ProtocolKind::MI, AccessType::Write, CacheState::S, Expect::Unsupported},
    {ProtocolKind::MI, AccessType::Write, CacheState::E, Expect::Unsupported},
    {ProtocolKind::MI, AccessType::Write, CacheState::O, Expect::Unsupported},

    // MSI: S makes reads shareable; a write to S must invalidate the sharers.
    {ProtocolKind::MSI, AccessType::Read, CacheState::I, Expect::Read},
    {ProtocolKind::MSI, AccessType::Write, CacheState::I, Expect::ReadX},
    {ProtocolKind::MSI, AccessType::Read, CacheState::S, Expect::Silent},
    {ProtocolKind::MSI, AccessType::Write, CacheState::S, Expect::Upgrade},
    {ProtocolKind::MSI, AccessType::Read, CacheState::M, Expect::Silent},
    {ProtocolKind::MSI, AccessType::Write, CacheState::M, Expect::Silent},
    {ProtocolKind::MSI, AccessType::Read, CacheState::E, Expect::Unsupported},
    {ProtocolKind::MSI, AccessType::Write, CacheState::E, Expect::Unsupported},
    {ProtocolKind::MSI, AccessType::Read, CacheState::O, Expect::Unsupported},
    {ProtocolKind::MSI, AccessType::Write, CacheState::O, Expect::Unsupported},

    // MESI: identical to MSI except (Write, E), which needs no bus at all.
    {ProtocolKind::MESI, AccessType::Read, CacheState::I, Expect::Read},
    {ProtocolKind::MESI, AccessType::Write, CacheState::I, Expect::ReadX},
    {ProtocolKind::MESI, AccessType::Read, CacheState::S, Expect::Silent},
    {ProtocolKind::MESI, AccessType::Write, CacheState::S, Expect::Upgrade},
    {ProtocolKind::MESI, AccessType::Read, CacheState::E, Expect::Silent},
    {ProtocolKind::MESI, AccessType::Write, CacheState::E, Expect::Silent}, // <- the E state
    {ProtocolKind::MESI, AccessType::Read, CacheState::M, Expect::Silent},
    {ProtocolKind::MESI, AccessType::Write, CacheState::M, Expect::Silent},
    {ProtocolKind::MESI, AccessType::Read, CacheState::O, Expect::Unsupported},
    {ProtocolKind::MESI, AccessType::Write, CacheState::O, Expect::Unsupported},

    // MOSI: O is readable and dirty, but shared, so a write still upgrades.
    {ProtocolKind::MOSI, AccessType::Read, CacheState::I, Expect::Read},
    {ProtocolKind::MOSI, AccessType::Write, CacheState::I, Expect::ReadX},
    {ProtocolKind::MOSI, AccessType::Read, CacheState::S, Expect::Silent},
    {ProtocolKind::MOSI, AccessType::Write, CacheState::S, Expect::Upgrade},
    {ProtocolKind::MOSI, AccessType::Read, CacheState::O, Expect::Silent},
    {ProtocolKind::MOSI, AccessType::Write, CacheState::O, Expect::Upgrade},
    {ProtocolKind::MOSI, AccessType::Read, CacheState::M, Expect::Silent},
    {ProtocolKind::MOSI, AccessType::Write, CacheState::M, Expect::Silent},
    {ProtocolKind::MOSI, AccessType::Read, CacheState::E, Expect::Unsupported},
    {ProtocolKind::MOSI, AccessType::Write, CacheState::E, Expect::Unsupported},

    // MOESI: every state is reachable; only S and O cost a bus transaction.
    {ProtocolKind::MOESI, AccessType::Read, CacheState::I, Expect::Read},
    {ProtocolKind::MOESI, AccessType::Write, CacheState::I, Expect::ReadX},
    {ProtocolKind::MOESI, AccessType::Read, CacheState::S, Expect::Silent},
    {ProtocolKind::MOESI, AccessType::Write, CacheState::S, Expect::Upgrade},
    {ProtocolKind::MOESI, AccessType::Read, CacheState::E, Expect::Silent},
    {ProtocolKind::MOESI, AccessType::Write, CacheState::E, Expect::Silent},
    {ProtocolKind::MOESI, AccessType::Read, CacheState::O, Expect::Silent},
    {ProtocolKind::MOESI, AccessType::Write, CacheState::O, Expect::Upgrade},
    {ProtocolKind::MOESI, AccessType::Read, CacheState::M, Expect::Silent},
    {ProtocolKind::MOESI, AccessType::Write, CacheState::M, Expect::Silent},
};

void test_request_for_table_is_exhaustive() {
  // Every protocol must have an entry for all 10 (access, state) pairs, so a
  // new state cannot be added without deciding what each protocol does with it.
  for (ProtocolKind kind : kAllProtocols) {
    size_t cells = 0;
    for (const Cell &cell : kRequestTable) {
      if (cell.kind == kind)
        ++cells;
    }
    EXPECT_EQ(cells, std::size_t{10});
  }
}

void test_request_for_table() {
  for (const Cell &cell : kRequestTable) {
    const std::unique_ptr<CoherenceProtocol> protocol = make_protocol(cell.kind);
    if (!matches(*protocol, cell.access, cell.state, cell.expect)) {
      std::fprintf(stderr, "  %s: %s in state %s should be %s\n", protocol->name(),
                   cell.access == AccessType::Read ? "read" : "write", to_string(cell.state),
                   to_string(cell.expect));
    }
    EXPECT_TRUE(matches(*protocol, cell.access, cell.state, cell.expect));
  }
}

// ------------------------------------------------------------- the system

// Everything a transition could touch, captured at one instant.
struct Snapshot {
  BusStats bus;
  MemoryStats memory;
  std::vector<CacheState> states; // per core, for one line
};

// A small real system: caches, bus, memory, protocol. Transitions are driven
// through actual accesses rather than by poking states, so the assertions cover
// Core and SnoopBus as well as the protocol.
class System {
public:
  System(ProtocolKind kind, uint32_t cores)
      : protocol_(make_protocol(kind)), memory_(MemoryConfig{}),
        bus_(BusConfig{}, *protocol_, memory_, kBlock) {
    for (uint32_t id = 0; id < cores; ++id) {
      cores_.push_back(
          std::make_unique<Core>(id, kCacheBytes, kAssoc, kBlock, 1, *protocol_, bus_));
      bus_.register_core(*cores_.back());
    }
  }

  uint64_t read(uint32_t core, uint64_t addr) { return access(core, addr, MemOp::Op::Read); }
  uint64_t write(uint32_t core, uint64_t addr) { return access(core, addr, MemOp::Op::Write); }

  CacheState state(uint32_t core, uint64_t addr) const { return cores_[core]->snoop(addr); }

  Snapshot snap(uint64_t addr) const {
    Snapshot shot;
    shot.bus = bus_.stats();
    shot.memory = memory_.stats();
    for (const auto &core : cores_) {
      shot.states.push_back(core->snoop(addr));
    }
    return shot;
  }

private:
  uint64_t access(uint32_t core, uint64_t addr, MemOp::Op op) {
    MemOp memop;
    memop.core_id = core;
    memop.addr = addr;
    memop.op = op;
    const uint64_t stall = cores_[core]->access(memop, clock_);
    clock_ += stall;
    return stall;
  }

  std::unique_ptr<CoherenceProtocol> protocol_;
  MainMemory memory_;
  SnoopBus bus_;
  std::vector<std::unique_ptr<Core>> cores_;
  uint64_t clock_ = 0;
};

// --- difference predicates, so a test can state exactly what moved ---

bool bus_untouched(const Snapshot &before, const Snapshot &after) {
  return before.bus.transactions == after.bus.transactions;
}

bool memory_untouched(const Snapshot &before, const Snapshot &after) {
  return before.memory.reads == after.memory.reads && before.memory.writes == after.memory.writes;
}

uint64_t new_transactions(const Snapshot &before, const Snapshot &after) {
  return after.bus.transactions - before.bus.transactions;
}

uint64_t new_memory_writes(const Snapshot &before, const Snapshot &after) {
  return after.memory.writes - before.memory.writes;
}

uint64_t new_memory_reads(const Snapshot &before, const Snapshot &after) {
  return after.memory.reads - before.memory.reads;
}

uint64_t new_invalidations(const Snapshot &before, const Snapshot &after) {
  return after.bus.invalidations - before.bus.invalidations;
}

// True if every core except `except` holds the line in the same state as before.
bool peers_unchanged(const Snapshot &before, const Snapshot &after, uint32_t except) {
  for (uint32_t core = 0; core < before.states.size(); ++core) {
    if (core != except && before.states[core] != after.states[core]) {
      return false;
    }
  }
  return true;
}

// --- setup helpers: leave `core` holding `addr` in a known state ---

// Reads a line no other core has touched. MI takes it as M, MSI/MOSI as S,
// MESI/MOESI as E.
void take_uncontended(System &system, uint32_t core, uint64_t addr) { system.read(core, addr); }

void take_modified(System &system, uint32_t core, uint64_t addr) { system.write(core, addr); }

// A second core reads the line, leaving both sharing it.
void take_shared(System &system, uint32_t core, uint32_t peer, uint64_t addr) {
  system.read(peer, addr);
  system.read(core, addr);
}

// `core` writes, then `peer` reads: under MOSI/MOESI that makes `core` the
// Owner of a dirty line the peer now shares.
void take_owned(System &system, uint32_t core, uint32_t peer, uint64_t addr) {
  system.write(core, addr);
  system.read(peer, addr);
}

// ------------------------------------------------ transitions out of I

void test_read_miss_lands_in_the_documented_state() {
  const struct {
    ProtocolKind kind;
    CacheState uncontended; // no other core holds the line
    CacheState contended;   // another core already holds it
  } cases[] = {
      {ProtocolKind::MI, CacheState::M, CacheState::M},
      {ProtocolKind::MSI, CacheState::S, CacheState::S},
      {ProtocolKind::MESI, CacheState::E, CacheState::S},
      {ProtocolKind::MOSI, CacheState::S, CacheState::S},
      {ProtocolKind::MOESI, CacheState::E, CacheState::S},
  };
  for (const auto &test : cases) {
    System uncontended(test.kind, 2);
    uncontended.read(0, 0x1000);
    EXPECT_TRUE(uncontended.state(0, 0x1000) == test.uncontended);

    System contended(test.kind, 2);
    contended.read(1, 0x1000);
    contended.read(0, 0x1000);
    EXPECT_TRUE(contended.state(0, 0x1000) == test.contended);
  }
}

void test_write_miss_always_lands_in_modified() {
  for (ProtocolKind kind : kAllProtocols) {
    System system(kind, 2);
    system.write(0, 0x1000);
    EXPECT_TRUE(system.state(0, 0x1000) == CacheState::M);
  }
}

void test_cold_miss_reads_memory_exactly_once() {
  for (ProtocolKind kind : kAllProtocols) {
    System system(kind, 2);
    const Snapshot before = system.snap(0x1000);
    system.read(0, 0x1000);
    const Snapshot after = system.snap(0x1000);
    EXPECT_EQ(new_transactions(before, after), std::uint64_t{1});
    EXPECT_EQ(new_memory_reads(before, after), std::uint64_t{1});
    EXPECT_EQ(new_memory_writes(before, after), std::uint64_t{0});
    // Nobody else held the line, so nobody else is disturbed.
    EXPECT_EQ(new_invalidations(before, after), std::uint64_t{0});
    EXPECT_TRUE(peers_unchanged(before, after, 0));
  }
}

// ------------------------------------------------ transitions out of E

void test_read_hit_in_exclusive_changes_nothing_at_all() {
  for (ProtocolKind kind : {ProtocolKind::MESI, ProtocolKind::MOESI}) {
    System system(kind, 2);
    take_uncontended(system, 0, 0x1000);
    EXPECT_TRUE(system.state(0, 0x1000) == CacheState::E);

    const Snapshot before = system.snap(0x1000);
    const uint64_t stall = system.read(0, 0x1000);
    const Snapshot after = system.snap(0x1000);

    EXPECT_TRUE(system.state(0, 0x1000) == CacheState::E); // still exclusive
    EXPECT_TRUE(bus_untouched(before, after));
    EXPECT_TRUE(memory_untouched(before, after));
    EXPECT_TRUE(peers_unchanged(before, after, 0));
    EXPECT_EQ(stall, std::uint64_t{1}); // the cache probe, nothing more
  }
}

void test_write_to_exclusive_is_a_silent_promotion_to_modified() {
  // The E state's entire purpose, and the case that is easiest to implement
  // wrongly in a way nothing else notices: the line must become M, and NOTHING
  // else may move -- no bus transaction, no memory access, no peer disturbed.
  for (ProtocolKind kind : {ProtocolKind::MESI, ProtocolKind::MOESI}) {
    System system(kind, 2);
    take_uncontended(system, 0, 0x1000);
    EXPECT_TRUE(system.state(0, 0x1000) == CacheState::E);

    const Snapshot before = system.snap(0x1000);
    const uint64_t stall = system.write(0, 0x1000);
    const Snapshot after = system.snap(0x1000);

    EXPECT_TRUE(system.state(0, 0x1000) == CacheState::M); // E -> M
    EXPECT_TRUE(bus_untouched(before, after));             // and nothing else
    EXPECT_TRUE(memory_untouched(before, after));
    EXPECT_TRUE(peers_unchanged(before, after, 0));
    EXPECT_EQ(new_invalidations(before, after), std::uint64_t{0});
    EXPECT_EQ(stall, std::uint64_t{1});
  }
}

void test_the_same_write_costs_an_upgrade_without_the_exclusive_state() {
  // The counterpart: MSI and MOSI reach S rather than E on the identical
  // access sequence, so the write that follows must go to the bus. This is the
  // measured cost that the E state removes.
  for (ProtocolKind kind : {ProtocolKind::MSI, ProtocolKind::MOSI}) {
    System system(kind, 2);
    take_uncontended(system, 0, 0x1000);
    EXPECT_TRUE(system.state(0, 0x1000) == CacheState::S);

    const Snapshot before = system.snap(0x1000);
    system.write(0, 0x1000);
    const Snapshot after = system.snap(0x1000);

    EXPECT_TRUE(system.state(0, 0x1000) == CacheState::M);
    EXPECT_EQ(new_transactions(before, after), std::uint64_t{1}); // the Upgrade
    EXPECT_EQ(after.bus.upgrades - before.bus.upgrades, std::uint64_t{1});
    // An Upgrade moves no data and needs no memory, even though it uses the bus.
    EXPECT_TRUE(memory_untouched(before, after));
  }
}

void test_exclusive_peer_demotes_to_shared_on_a_snooped_read() {
  for (ProtocolKind kind : {ProtocolKind::MESI, ProtocolKind::MOESI}) {
    System system(kind, 2);
    take_uncontended(system, 0, 0x1000);

    const Snapshot before = system.snap(0x1000);
    system.read(1, 0x1000);
    const Snapshot after = system.snap(0x1000);

    EXPECT_TRUE(system.state(0, 0x1000) == CacheState::S); // E -> S
    EXPECT_TRUE(system.state(1, 0x1000) == CacheState::S);
    // E is clean, so demoting it owes memory nothing, and the peer supplies
    // the line so no memory read is needed either.
    EXPECT_EQ(new_memory_writes(before, after), std::uint64_t{0});
    EXPECT_EQ(new_memory_reads(before, after), std::uint64_t{0});
    EXPECT_EQ(new_invalidations(before, after), std::uint64_t{0});
  }
}

void test_exclusive_peer_is_invalidated_by_a_snooped_write() {
  for (ProtocolKind kind : {ProtocolKind::MESI, ProtocolKind::MOESI}) {
    System system(kind, 2);
    take_uncontended(system, 0, 0x1000);

    const Snapshot before = system.snap(0x1000);
    system.write(1, 0x1000);
    const Snapshot after = system.snap(0x1000);

    EXPECT_TRUE(system.state(0, 0x1000) == CacheState::I); // E -> I
    EXPECT_TRUE(system.state(1, 0x1000) == CacheState::M);
    EXPECT_EQ(new_invalidations(before, after), std::uint64_t{1});
    EXPECT_EQ(new_memory_writes(before, after), std::uint64_t{0}); // E was clean
  }
}

// ------------------------------------------------ transitions out of S

void test_read_hit_in_shared_changes_nothing() {
  for (ProtocolKind kind :
       {ProtocolKind::MSI, ProtocolKind::MESI, ProtocolKind::MOSI, ProtocolKind::MOESI}) {
    System system(kind, 2);
    take_shared(system, 0, 1, 0x1000);
    EXPECT_TRUE(system.state(0, 0x1000) == CacheState::S);

    const Snapshot before = system.snap(0x1000);
    const uint64_t stall = system.read(0, 0x1000);
    const Snapshot after = system.snap(0x1000);

    EXPECT_TRUE(system.state(0, 0x1000) == CacheState::S);
    EXPECT_TRUE(bus_untouched(before, after));
    EXPECT_TRUE(memory_untouched(before, after));
    EXPECT_TRUE(peers_unchanged(before, after, 0));
    EXPECT_EQ(stall, std::uint64_t{1});
  }
}

void test_write_to_shared_invalidates_every_other_sharer() {
  for (ProtocolKind kind :
       {ProtocolKind::MSI, ProtocolKind::MESI, ProtocolKind::MOSI, ProtocolKind::MOESI}) {
    System system(kind, 4);
    // Three cores end up sharing the line.
    system.read(1, 0x1000);
    system.read(2, 0x1000);
    system.read(0, 0x1000);
    EXPECT_TRUE(system.state(0, 0x1000) == CacheState::S);

    const Snapshot before = system.snap(0x1000);
    system.write(0, 0x1000);
    const Snapshot after = system.snap(0x1000);

    EXPECT_TRUE(system.state(0, 0x1000) == CacheState::M);
    EXPECT_TRUE(system.state(1, 0x1000) == CacheState::I);
    EXPECT_TRUE(system.state(2, 0x1000) == CacheState::I);
    EXPECT_EQ(new_invalidations(before, after), std::uint64_t{2});
    // Core 3 never held the line and must not be reported as invalidated.
    EXPECT_TRUE(after.states[3] == CacheState::I);
    // No data moves and memory is untouched: an Upgrade is address-phase only.
    EXPECT_TRUE(memory_untouched(before, after));
  }
}

void test_shared_peer_survives_another_cores_read() {
  for (ProtocolKind kind :
       {ProtocolKind::MSI, ProtocolKind::MESI, ProtocolKind::MOSI, ProtocolKind::MOESI}) {
    System system(kind, 3);
    take_shared(system, 0, 1, 0x1000);

    const Snapshot before = system.snap(0x1000);
    system.read(2, 0x1000);
    const Snapshot after = system.snap(0x1000);

    // Sharers stay sharers; a third reader joins them.
    EXPECT_TRUE(system.state(0, 0x1000) == CacheState::S);
    EXPECT_TRUE(system.state(1, 0x1000) == CacheState::S);
    EXPECT_TRUE(system.state(2, 0x1000) == CacheState::S);
    EXPECT_EQ(new_invalidations(before, after), std::uint64_t{0});
    // A clean sharer can supply the line, so memory is not consulted.
    EXPECT_EQ(new_memory_reads(before, after), std::uint64_t{0});
  }
}

// ------------------------------------------------ transitions out of M

void test_hits_on_modified_change_nothing() {
  for (ProtocolKind kind : kAllProtocols) {
    for (bool writing : {false, true}) {
      System system(kind, 2);
      take_modified(system, 0, 0x1000);
      EXPECT_TRUE(system.state(0, 0x1000) == CacheState::M);

      const Snapshot before = system.snap(0x1000);
      const uint64_t stall = writing ? system.write(0, 0x1000) : system.read(0, 0x1000);
      const Snapshot after = system.snap(0x1000);

      EXPECT_TRUE(system.state(0, 0x1000) == CacheState::M); // M -> M
      EXPECT_TRUE(bus_untouched(before, after));
      EXPECT_TRUE(memory_untouched(before, after));
      EXPECT_TRUE(peers_unchanged(before, after, 0));
      EXPECT_EQ(stall, std::uint64_t{1});
    }
  }
}

void test_snooped_read_of_modified_splits_by_the_owner_state() {
  // The O state's entire purpose, isolated. The same access sequence: core 0
  // writes, core 1 reads. Protocols with O park the dirty line in the owner's
  // cache; protocols without it must flush to memory.
  const struct {
    ProtocolKind kind;
    CacheState owner_after;
    uint64_t memory_writes;
  } cases[] = {
      {ProtocolKind::MSI, CacheState::S, 1},
      {ProtocolKind::MESI, CacheState::S, 1},
      {ProtocolKind::MOSI, CacheState::O, 0},
      {ProtocolKind::MOESI, CacheState::O, 0},
  };
  for (const auto &test : cases) {
    System system(test.kind, 2);
    take_modified(system, 0, 0x1000);

    const Snapshot before = system.snap(0x1000);
    system.read(1, 0x1000);
    const Snapshot after = system.snap(0x1000);

    EXPECT_TRUE(system.state(0, 0x1000) == test.owner_after);
    EXPECT_TRUE(system.state(1, 0x1000) == CacheState::S);
    EXPECT_EQ(new_memory_writes(before, after), test.memory_writes);
    // Either way the reader takes the line from the cache, not from memory,
    // and the previous holder keeps a readable copy.
    EXPECT_EQ(new_memory_reads(before, after), std::uint64_t{0});
    EXPECT_EQ(new_invalidations(before, after), std::uint64_t{0});
    EXPECT_EQ(new_transactions(before, after), std::uint64_t{1});
  }
}

void test_snooped_write_of_modified_invalidates_and_may_flush() {
  const struct {
    ProtocolKind kind;
    uint64_t memory_writes;
  } cases[] = {
      {ProtocolKind::MI, 1},   {ProtocolKind::MSI, 1},   {ProtocolKind::MESI, 1},
      {ProtocolKind::MOSI, 0}, {ProtocolKind::MOESI, 0},
  };
  for (const auto &test : cases) {
    System system(test.kind, 2);
    take_modified(system, 0, 0x1000);

    const Snapshot before = system.snap(0x1000);
    system.write(1, 0x1000);
    const Snapshot after = system.snap(0x1000);

    EXPECT_TRUE(system.state(0, 0x1000) == CacheState::I); // M -> I
    EXPECT_TRUE(system.state(1, 0x1000) == CacheState::M);
    EXPECT_EQ(new_invalidations(before, after), std::uint64_t{1});
    // The dirty line goes cache-to-cache; only protocols without O also pay
    // memory for it.
    EXPECT_EQ(new_memory_writes(before, after), test.memory_writes);
    EXPECT_EQ(new_memory_reads(before, after), std::uint64_t{0});
  }
}

// ------------------------------------------------ transitions out of O

void test_read_hit_in_owner_changes_nothing() {
  for (ProtocolKind kind : {ProtocolKind::MOSI, ProtocolKind::MOESI}) {
    System system(kind, 2);
    take_owned(system, 0, 1, 0x1000);
    EXPECT_TRUE(system.state(0, 0x1000) == CacheState::O);

    const Snapshot before = system.snap(0x1000);
    const uint64_t stall = system.read(0, 0x1000);
    const Snapshot after = system.snap(0x1000);

    EXPECT_TRUE(system.state(0, 0x1000) == CacheState::O);
    EXPECT_TRUE(bus_untouched(before, after));
    EXPECT_TRUE(memory_untouched(before, after));
    EXPECT_TRUE(peers_unchanged(before, after, 0));
    EXPECT_EQ(stall, std::uint64_t{1});
  }
}

void test_write_to_owner_upgrades_without_touching_memory() {
  for (ProtocolKind kind : {ProtocolKind::MOSI, ProtocolKind::MOESI}) {
    System system(kind, 2);
    take_owned(system, 0, 1, 0x1000);

    const Snapshot before = system.snap(0x1000);
    system.write(0, 0x1000);
    const Snapshot after = system.snap(0x1000);

    EXPECT_TRUE(system.state(0, 0x1000) == CacheState::M); // O -> M
    EXPECT_TRUE(system.state(1, 0x1000) == CacheState::I); // the sharer goes
    EXPECT_EQ(after.bus.upgrades - before.bus.upgrades, std::uint64_t{1});
    EXPECT_EQ(new_invalidations(before, after), std::uint64_t{1});
    // The owner's dirty value is about to be overwritten by the requestor,
    // which already holds the line, so memory is never involved.
    EXPECT_TRUE(memory_untouched(before, after));
  }
}

void test_owner_supplies_later_readers_and_stays_owner() {
  for (ProtocolKind kind : {ProtocolKind::MOSI, ProtocolKind::MOESI}) {
    System system(kind, 3);
    take_owned(system, 0, 1, 0x1000);

    const Snapshot before = system.snap(0x1000);
    system.read(2, 0x1000);
    const Snapshot after = system.snap(0x1000);

    EXPECT_TRUE(system.state(0, 0x1000) == CacheState::O); // O -> O
    EXPECT_TRUE(system.state(2, 0x1000) == CacheState::S);
    EXPECT_EQ(after.bus.peer_supplied - before.bus.peer_supplied, std::uint64_t{1});
    // The owner answers the read itself: memory is untouched in both directions.
    EXPECT_TRUE(memory_untouched(before, after));
    EXPECT_EQ(new_invalidations(before, after), std::uint64_t{0});
  }
}

void test_owner_is_invalidated_by_a_snooped_write_without_flushing() {
  for (ProtocolKind kind : {ProtocolKind::MOSI, ProtocolKind::MOESI}) {
    System system(kind, 3);
    take_owned(system, 0, 1, 0x1000);

    const Snapshot before = system.snap(0x1000);
    system.write(2, 0x1000);
    const Snapshot after = system.snap(0x1000);

    EXPECT_TRUE(system.state(0, 0x1000) == CacheState::I); // O -> I
    EXPECT_TRUE(system.state(1, 0x1000) == CacheState::I); // the sharer too
    EXPECT_TRUE(system.state(2, 0x1000) == CacheState::M);
    EXPECT_EQ(new_invalidations(before, after), std::uint64_t{2});
    // Ownership of the dirty data passes to the writer, so memory stays out.
    EXPECT_TRUE(memory_untouched(before, after));
  }
}

// ------------------------------------------------------- eviction paths

// 256 bytes, direct-mapped: 4 sets, so these three addresses all collide.
constexpr uint64_t kConflictA = 0x0000;
constexpr uint64_t kConflictB = 0x0100;

class TinySystem {
public:
  explicit TinySystem(ProtocolKind kind)
      : protocol_(make_protocol(kind)), memory_(MemoryConfig{}),
        bus_(BusConfig{}, *protocol_, memory_, kBlock),
        core_(0, /*cache_bytes=*/256, /*assoc=*/1, kBlock, 1, *protocol_, bus_) {
    bus_.register_core(core_);
  }
  void read(uint64_t addr) { access(addr, MemOp::Op::Read); }
  void write(uint64_t addr) { access(addr, MemOp::Op::Write); }
  const BusStats &bus() const { return bus_.stats(); }
  const MemoryStats &memory() const { return memory_.stats(); }
  const CoreStats &core() const { return core_.stats(); }

private:
  void access(uint64_t addr, MemOp::Op op) {
    MemOp memop;
    memop.addr = addr;
    memop.op = op;
    clock_ += core_.access(memop, clock_);
  }
  std::unique_ptr<CoherenceProtocol> protocol_;
  MainMemory memory_;
  SnoopBus bus_;
  Core core_;
  uint64_t clock_ = 0;
};

// Two cores, each with a 4-set direct-mapped cache, so conflicts are easy to
// arrange while still allowing the sharing needed to reach O.
class SmallSystem {
public:
  explicit SmallSystem(ProtocolKind kind)
      : protocol_(make_protocol(kind)), memory_(MemoryConfig{}),
        bus_(BusConfig{}, *protocol_, memory_, kBlock) {
    for (uint32_t id = 0; id < 2; ++id) {
      cores_.push_back(std::make_unique<Core>(id, /*cache_bytes=*/256, /*assoc=*/1, kBlock, 1,
                                              *protocol_, bus_));
      bus_.register_core(*cores_.back());
    }
  }
  void read(uint32_t core, uint64_t addr) { access(core, addr, MemOp::Op::Read); }
  void write(uint32_t core, uint64_t addr) { access(core, addr, MemOp::Op::Write); }
  CacheState state(uint32_t core, uint64_t addr) const { return cores_[core]->snoop(addr); }
  uint64_t memory_writes() const { return memory_.stats().writes; }
  uint64_t bus_writebacks() const { return bus_.stats().writebacks; }
  uint64_t dirty_evictions(uint32_t core) const { return cores_[core]->stats().dirty_evictions; }

private:
  void access(uint32_t core, uint64_t addr, MemOp::Op op) {
    MemOp memop;
    memop.core_id = core;
    memop.addr = addr;
    memop.op = op;
    clock_ += cores_[core]->access(memop, clock_);
  }
  std::unique_ptr<CoherenceProtocol> protocol_;
  MainMemory memory_;
  SnoopBus bus_;
  std::vector<std::unique_ptr<Core>> cores_;
  uint64_t clock_ = 0;
};

void test_evicting_a_clean_line_writes_nothing_back() {
  for (ProtocolKind kind : kAllProtocols) {
    TinySystem system(kind);
    system.read(kConflictA); // clean under every protocol except MI
    if (kind == ProtocolKind::MI) {
      continue; // MI's read takes the line as M, so it is never clean
    }
    const uint64_t writes_before = system.memory().writes;
    system.read(kConflictB); // displaces it
    EXPECT_EQ(system.core().evictions, std::uint64_t{1});
    EXPECT_EQ(system.core().dirty_evictions, std::uint64_t{0});
    EXPECT_EQ(system.memory().writes, writes_before);
    EXPECT_EQ(system.bus().writebacks, std::uint64_t{0});
  }
}

void test_evicting_a_dirty_line_writes_it_back_exactly_once() {
  for (ProtocolKind kind : kAllProtocols) {
    TinySystem system(kind);
    system.write(kConflictA); // now M
    const uint64_t writes_before = system.memory().writes;

    system.read(kConflictB); // displaces the dirty line

    EXPECT_EQ(system.core().evictions, std::uint64_t{1});
    EXPECT_EQ(system.core().dirty_evictions, std::uint64_t{1});
    EXPECT_EQ(system.bus().writebacks, std::uint64_t{1});
    EXPECT_EQ(system.memory().writes, writes_before + 1);
  }
}

void test_evicting_an_owned_line_writes_it_back() {
  // O is dirty, so displacing it owes memory a write just as M does. The O
  // state DEFERS the write to eviction; it does not remove it. Without this
  // case a protocol could treat O as clean on eviction and lose the data --
  // and every earlier O test would still pass.
  for (ProtocolKind kind : {ProtocolKind::MOSI, ProtocolKind::MOESI}) {
    // Two cores, each direct-mapped with 4 sets, so kConflictA and kConflictB
    // collide and the second access displaces the first.
    SmallSystem system(kind);
    system.write(0, kConflictA); // core 0 holds it M
    system.read(1, kConflictA);  // core 0 becomes the Owner
    EXPECT_TRUE(system.state(0, kConflictA) == CacheState::O);

    const uint64_t writes_before = system.memory_writes();
    const uint64_t writebacks_before = system.bus_writebacks();

    system.read(0, kConflictB); // displaces the owned line

    EXPECT_EQ(system.dirty_evictions(0), std::uint64_t{1});
    EXPECT_EQ(system.bus_writebacks(), writebacks_before + 1);
    EXPECT_EQ(system.memory_writes(), writes_before + 1);
    EXPECT_TRUE(system.state(0, kConflictA) == CacheState::I);
  }
}

} // namespace

int main() {
  RUN_TEST(test_request_for_table_is_exhaustive);
  RUN_TEST(test_request_for_table);

  RUN_TEST(test_read_miss_lands_in_the_documented_state);
  RUN_TEST(test_write_miss_always_lands_in_modified);
  RUN_TEST(test_cold_miss_reads_memory_exactly_once);

  RUN_TEST(test_read_hit_in_exclusive_changes_nothing_at_all);
  RUN_TEST(test_write_to_exclusive_is_a_silent_promotion_to_modified);
  RUN_TEST(test_the_same_write_costs_an_upgrade_without_the_exclusive_state);
  RUN_TEST(test_exclusive_peer_demotes_to_shared_on_a_snooped_read);
  RUN_TEST(test_exclusive_peer_is_invalidated_by_a_snooped_write);

  RUN_TEST(test_read_hit_in_shared_changes_nothing);
  RUN_TEST(test_write_to_shared_invalidates_every_other_sharer);
  RUN_TEST(test_shared_peer_survives_another_cores_read);

  RUN_TEST(test_hits_on_modified_change_nothing);
  RUN_TEST(test_snooped_read_of_modified_splits_by_the_owner_state);
  RUN_TEST(test_snooped_write_of_modified_invalidates_and_may_flush);

  RUN_TEST(test_read_hit_in_owner_changes_nothing);
  RUN_TEST(test_write_to_owner_upgrades_without_touching_memory);
  RUN_TEST(test_owner_supplies_later_readers_and_stays_owner);
  RUN_TEST(test_owner_is_invalidated_by_a_snooped_write_without_flushing);

  RUN_TEST(test_evicting_a_clean_line_writes_nothing_back);
  RUN_TEST(test_evicting_a_dirty_line_writes_it_back_exactly_once);
  RUN_TEST(test_evicting_an_owned_line_writes_it_back);
  TEST_REPORT_AND_EXIT();
}
