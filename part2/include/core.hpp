// A CPU core and its private cache.
//
// Blocking: one outstanding request at a time. access() runs a memory access to
// completion and returns the number of cycles the core stalled, which is what
// makes coherence savings show up as runtime rather than only as traffic.
//
// The core owns no coherence policy. It asks the protocol which bus transaction
// an access needs, hands that to the bus, and applies whatever state comes
// back. Snoops arrive through the SnoopTarget interface and are likewise just
// applied.

#pragma once

#include "cache.hpp"
#include "protocol.hpp"
#include "snoop_bus.hpp"
#include "trace.hpp"

#include <cstddef>
#include <cstdint>

struct CoreStats {
  uint64_t reads = 0;
  uint64_t writes = 0;
  uint64_t hits = 0;
  uint64_t misses = 0;

  // Hits that needed no bus transaction at all. The E state exists to move
  // write hits from `upgrade_hits` into this bucket.
  uint64_t silent_hits = 0;
  // Write hits on a line held non-exclusively, each costing an Upgrade.
  uint64_t upgrade_hits = 0;

  uint64_t evictions = 0;
  uint64_t dirty_evictions = 0; // each one owes memory a write

  uint64_t stall_cycles = 0; // total time blocked on the memory system
};

class Core : public SnoopTarget {
public:
  Core(uint32_t core_id, size_t cache_bytes, size_t cache_assoc, size_t block_bytes,
       uint32_t hit_cycles, const CoherenceProtocol &protocol, SnoopBus &bus);

  // Runs one access issued at `at_cycle`. Returns the stall in cycles: the
  // cache probe for a hit that needs no bus, otherwise the probe plus the bus
  // transaction's latency.
  uint64_t access(const MemOp &op, uint64_t at_cycle);

  // SnoopTarget
  uint32_t core_id() const override { return core_id_; }
  CacheState snoop(uint64_t addr) const override;
  void apply_transition(uint64_t addr, CacheState new_state) override;

  const CoreStats &stats() const { return stats_; }

private:
  uint32_t core_id_;
  Cache cache_;
  uint32_t hit_cycles_;
  const CoherenceProtocol &protocol_;
  SnoopBus &bus_;
  CoreStats stats_;

  // Issues the writeback a dirty victim owes and returns only the cycles this
  // core must actually wait: a real core drops the victim in a writeback buffer
  // and moves on, so the bus and memory time is not charged to it. The
  // transaction still occupies both, so it still delays other cores -- which is
  // the cost the O state actually saves. A full write buffer is the one case
  // that does stall the evicting core.
  uint64_t write_back_victim(const CacheResult &result, uint64_t at_cycle);
};
