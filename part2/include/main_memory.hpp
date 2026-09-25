// Main memory: banked DRAM with row-buffer locality.
//
//
// Bandwidth dial, from most to least constrained:
//   num_banks = 1                  maximum queueing
//   num_banks = 8 (default)        realistic bank-level parallelism
//   num_banks large                latency without queueing
//   infinite_bandwidth = true      no latency at all (the idealized model)

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct MemoryConfig {
  // Zero-cost memory. Every access completes in the cycle it is issued and no
  // bank state is touched.
  bool infinite_bandwidth = false;

  uint32_t num_banks = 8;
  uint32_t row_bytes = 8192;

  // Timings in memory cycles. Defaults are LPDDR5X-derived and preserve the
  // real timing ratios; absolute values are chosen to be readable rather than
  // to model a specific part.
  uint32_t row_access = 17;  // tCL:  read out of an already-open row
  uint32_t row_open = 17;    // tRCD: open a row in an idle bank
  uint32_t row_close = 17;   // tRP:  close the currently-open row first

  uint32_t write_recover = 120;

  // Depth of the write buffer, in cycles of queued write service.
  uint32_t write_buffer_cycles = 512;
};

struct MemoryStats {
  uint64_t reads = 0;
  uint64_t writes = 0;
  uint64_t row_hits = 0;       // access found its row already open
  uint64_t row_misses = 0;     // bank had to open a row (idle or wrong row)
  uint64_t bank_conflicts = 0; // access arrived while its bank was still busy
  uint64_t queue_cycles = 0;   // total time spent waiting on a busy bank
  uint64_t service_cycles = 0; // total time spent actually being served
};

class MainMemory {
public:
  // Throws std::runtime_error if num_banks or row_bytes is zero.
  explicit MainMemory(MemoryConfig config);

  // Serves one access arriving at `arrival`. Returns the cycle it completes.
  uint64_t access(uint64_t addr, bool is_write, uint64_t arrival);

  const MemoryStats &stats() const { return stats_; }

  // Which bank an address maps to. Exposed so tests and traces can reason
  // about bank conflicts deliberately.
  uint32_t bank_of(uint64_t addr) const;

  // Cycle the last bank finishes. Because writes are posted rather than
  // awaited, memory can still be draining after every core has retired its
  // final access, so this can exceed the simulation's makespan.
  uint64_t busy_until() const;

  uint64_t write_buffer_cycles() const { return config_.write_buffer_cycles; }

private:
  MemoryConfig config_;
  MemoryStats stats_;

  // Per bank. open_row_ is -1 when the bank has no row open.
  std::vector<int64_t> open_row_;
  std::vector<uint64_t> busy_until_;

  uint64_t row_of(uint64_t addr) const;
};
