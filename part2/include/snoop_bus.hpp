// The coherence interconnect: a shared, snooped, serializing bus.
//
// Serialization is not a modelling shortcut. Snooping coherence is correct only
// because every cache observes coherence transactions in the same total order,
// so exactly one transaction occupies the bus at a time -- including
// transactions to unrelated cache lines. A single `free_at_` cycle counter is
// the whole arbitration model, and requests are granted first-come-first-served
// by arrival cycle.
//
// Memory is a separate resource and does not serialize with the bus. The bus is
// released as soon as its own occupancy expires, without waiting for a memory
// access it started; that access finishes later, in parallel with subsequent
// bus transactions.

#pragma once

#include "cache.hpp"
#include "protocol.hpp"

#include <cstdint>
#include <string>
#include <vector>

class MainMemory;

// What the bus needs from a cache in order to snoop it. Core implements this.
class SnoopTarget {
public:
  virtual ~SnoopTarget() = default;
  virtual uint32_t core_id() const = 0;

  // State this cache holds `addr` in, or I if it does not hold it.
  virtual CacheState snoop(uint64_t addr) const = 0;

  // Move `addr` to `new_state`. A transition to I is an invalidation.
  virtual void apply_transition(uint64_t addr, CacheState new_state) = 0;
};

struct BusConfig {
  // Zero-cost bus: no occupancy and no arbitration delay. Transactions still
  // happen and still serialize logically, they just take no time.
  bool infinite_bandwidth = false;

  // Arbitration, command, and snoop-response phase. Charged to every
  // transaction regardless of type -- this is what an Upgrade costs, and what
  // the E state avoids paying.
  uint32_t addr_cycles = 4;

  // Verify every decision against the protocol's documented FSM as it fires.
  // Off by default: it is a per-transaction cost, and it only applies to the
  // three protocols students implement.
  bool check_fsm = false;

  // Data phase width. A 64-byte line over a 64-byte bus is 1 data cycle.
  // Upgrade moves no data and skips this entirely.
  //
  // Calibrated, not arbitrary. The E state saves a whole transaction, so its
  // benefit grows as the data phase shrinks relative to the address phase: at
  // 16 B/cycle a Read costs 8 and an Upgrade 4, so MSI pays 1.5x MESI; at
  // 64 B/cycle it is 5 and 4, and MSI pays 1.8x. A wide bus also stops the
  // interconnect from saturating on every workload, which is what lets memory
  // contention become visible on the sharing archetype.
  uint32_t width_bytes = 64;
};

struct BusStats {
  uint64_t transactions = 0;
  uint64_t reads = 0;
  uint64_t reads_exclusive = 0;
  uint64_t upgrades = 0;
  uint64_t writebacks = 0;

  uint64_t invalidations = 0; // peers forced to I
  uint64_t peer_supplied = 0; // lines sourced cache-to-cache instead of memory

  uint64_t busy_cycles = 0;        // total bus occupancy
  uint64_t arbitration_cycles = 0; // total time transactions spent queued
};

struct BusRequest {
  uint32_t core_id = 0;
  BusReqType type = BusReqType::Read;
  uint64_t addr = 0;  // block-aligned by convention; the bus does not re-mask
  uint64_t cycle = 0; // when the requesting core issued it
};

struct BusResponse {
  CacheState new_state = CacheState::I; // state the requestor's line ends in
  uint64_t latency = 0;                 // cycles from issue to completion

  // Portion of the latency caused by a full write buffer. A requestor that
  // would otherwise not wait for its writeback -- an evicting core -- must
  // still wait for this.
  uint64_t backpressure = 0;
};

class SnoopBus {
public:
  // `block_bytes` must match the caches' block size; it sets the data-phase
  // length. Throws std::runtime_error if it or width_bytes is zero.
  SnoopBus(BusConfig config, const CoherenceProtocol &protocol, MainMemory &memory,
           size_t block_bytes);

  // Cores must be registered before the first transaction. Order does not
  // matter; targets are indexed by their own core_id.
  void register_core(SnoopTarget &target);

  // Runs one coherence transaction to completion: snoops every cache, asks the
  // protocol what happens, applies the result to the peers, reserves the bus,
  // and starts any memory access the transaction needs.
  BusResponse issue(const BusRequest &request);

  const BusStats &stats() const { return stats_; }

  // Empty unless BusConfig::check_fsm was set. Each entry is one way a
  // decision departed from the protocol's state machine.
  const std::vector<std::string> &fsm_failures() const { return fsm_failures_; }

  // Cycle the bus next becomes free. Useful for tests and for reporting.
  uint64_t free_at() const { return free_at_; }

private:
  BusConfig config_;
  const CoherenceProtocol &protocol_;
  MainMemory &memory_;
  size_t block_bytes_;
  uint32_t data_cycles_;

  std::vector<SnoopTarget *> targets_; // indexed by core_id, may contain gaps
  std::vector<CacheState> peer_states_; // reused across transactions
  BusStats stats_;
  std::vector<std::string> fsm_failures_;
  bool fsm_checked_ = false; // protocol has documented rules to check against

  uint64_t free_at_ = 0; // the serialization point

  void count_transaction(BusReqType type);
  void collect_peer_states(uint64_t addr);

  // Rejects decisions no protocol may legally produce. Student protocols are
  // untrusted input here: without these checks an out-of-range peer id is a
  // buffer overrun, and transitioning the requestor or naming a supplier that
  // holds nothing corrupts the run silently and yields plausible wrong numbers.
  void validate(const BusRequest &request, const ProtocolDecision &decision) const;

  // Issues whatever memory traffic `decision` implies, starting at `at`.
  // Returns the cycle it completes, or `at` if none is needed.
  uint64_t start_memory_access(const BusRequest &request, const ProtocolDecision &decision,
                               uint64_t at, uint64_t *backpressure);
};
