// Top-level wiring and the main loop.
//
// Scheduling model: each core is blocking and has exactly one pending access at
// any moment, so the whole event set is one ready time per core. That makes a
// general event queue unnecessary -- the loop repeatedly picks the core with
// the earliest ready time and runs its access to completion. It is a lazy
// N-way merge of sorted streams.
//
//     ready(core) = completion(previous access) + delta(next access)
//
// Committing to the minimum immediately is safe because a core's ready times
// are non-decreasing, so no core can later present an earlier one.
//
// Ties are broken round-robin, not by lowest core id. Fixed-priority
// arbitration starves high-numbered cores, and it degenerates badly when many
// cores are ready at the same cycle: under the ideal performance model nothing
// stalls, so every core stays ready at cycle 0 and core 0 would run its entire
// stream before core 1 issued anything. No sharing would ever occur, and the
// traffic-only sweep would report protocols as identical when they are not.
// Round-robin is just as deterministic and matches how real bus arbiters work.
//
// Because the loop runs one access to completion before choosing the next --
// peer state changes included -- the protocol always sees a consistent
// snapshot. There is no interleaving to guard against, and nothing to lock.

#pragma once

#include "core.hpp"
#include "fsm_check.hpp"
#include "invariants.hpp"
#include "main_memory.hpp"
#include "protocol.hpp"
#include "snoop_bus.hpp"
#include "trace.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class ProtocolKind : uint8_t { MI, MSI, MESI, MOSI, MOESI };

const char *to_string(ProtocolKind kind);

// Throws std::runtime_error if `name` is not a known protocol.
ProtocolKind parse_protocol(const std::string &name);

// Throws std::runtime_error for a protocol that is declared but not yet
// implemented.
std::unique_ptr<CoherenceProtocol> make_protocol(ProtocolKind kind);

struct SimConfig {
  ProtocolKind protocol = ProtocolKind::MSI;

  // 0 means "derive from the trace". A larger value adds idle cores; a smaller
  // one than the trace uses is an error.
  uint32_t num_cores = 0;

  // Cycles to probe the cache, charged to every access including hits. Must be
  // non-zero: a free hit does not advance the issuing core's clock, so under
  // the ideal performance model a core running a streak of hits would never
  // yield to another and no sharing would occur.
  uint32_t cache_hit_cycles = 1;

  size_t cache_bytes = 32 * 1024;
  size_t cache_assoc = 4;
  size_t block_bytes = 64;

  BusConfig bus;
  MemoryConfig memory;

  // Correctness checking, off by default because it costs time per access.
  // Enables all three of: the exhaustive FSM scenario battery, live FSM arc
  // checking on every transaction, and the single-writer/multiple-reader
  // invariant after every access.
  bool check_invariants = false;

  // Sets both interconnect and memory to zero cost: the idealised model, where
  // protocols still differ in traffic but not in runtime.
  void set_perfect(bool perfect) {
    bus.infinite_bandwidth = perfect;
    memory.infinite_bandwidth = perfect;
  }
};

class Simulator {
public:
  // Builds the whole system: protocol, memory, bus, and one core per modelled
  // core. Throws std::runtime_error if the config and trace disagree on core
  // count or the cache geometry is invalid.
  Simulator(const SimConfig &config, Trace trace);

  // Drains every core's op stream.
  void run();

  // Cycle the last core finishes. This is the headline number that differs
  // between protocols.
  uint64_t makespan() const;

  // Empty unless SimConfig::check_invariants was set. Any entry means the
  // protocol under test is unsafe, whatever its counters say.
  const std::vector<InvariantViolation> &violations() const { return violations_; }

  // Result of the offline FSM battery, run at construction when checking is
  // enabled and the protocol is one of the three students implement.
  const ScenarioReport &fsm_report() const { return fsm_report_; }

  // Departures from the FSM observed on real transactions during the run.
  const std::vector<std::string> &fsm_failures() const { return bus_->fsm_failures(); }

  size_t num_cores() const { return cores_.size(); }
  const Core &core(size_t index) const { return *cores_[index]; }
  const SnoopBus &bus() const { return *bus_; }
  const MainMemory &memory() const { return *memory_; }

private:
  // Per-core scheduling state: how far through its op stream, and when it next
  // becomes free.
  struct CoreSchedule {
    size_t cursor = 0;
    uint64_t ready_cycle = 0;
  };

  SimConfig config_;
  Trace trace_;

  // Declaration order is destruction order reversed: cores reference the bus,
  // which references memory and the protocol.
  std::unique_ptr<CoherenceProtocol> protocol_;
  std::unique_ptr<MainMemory> memory_;
  std::unique_ptr<SnoopBus> bus_;
  std::vector<std::unique_ptr<Core>> cores_;

  std::vector<CoreSchedule> schedule_;
  // Core that most recently issued, so ties rotate rather than always
  // resolving to the same core.
  uint32_t last_served_ = 0;
  std::vector<InvariantViolation> violations_;
  ScenarioReport fsm_report_;
  uint64_t access_count_ = 0;

  // Core with the earliest ready time that still has work, or nullopt when
  // every stream is drained. Ties resolve to the lowest core id.
  std::optional<uint32_t> next_core() const;

  // Collects a violation if `addr` is now held in an illegal combination of
  // states across the caches.
  void verify_line(uint64_t addr, uint32_t core_id);
};
