// End-of-run statistics.
//
// Gathers the counters scattered across Core, SnoopBus and MainMemory into one
// aggregate.

#pragma once

#include "core.hpp"
#include "main_memory.hpp"
#include "simulator.hpp"
#include "snoop_bus.hpp"

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

struct Report {

  std::string protocol;
  std::string trace_path;
  size_t num_cores = 0;
  size_t cache_bytes = 0;
  size_t cache_assoc = 0;
  size_t block_bytes = 0;
  bool ideal_bus = false;
  bool ideal_memory = false;
  uint32_t bus_addr_cycles = 0;
  uint32_t bus_width_bytes = 0;
  uint32_t memory_banks = 0;

  uint64_t makespan = 0;

  uint64_t memory_drained_at = 0;

  // Summed over cores.
  CoreStats totals;
  std::vector<CoreStats> cores;

  BusStats bus;
  MemoryStats memory;

  uint64_t accesses() const { return totals.reads + totals.writes; }

  // Fraction of accesses that hit, 0..1. Zero if there were no accesses.
  double hit_rate() const;

  // Fraction of the run the bus was occupied, 0..1. This is the number that
  // says whether bus transactions are scarce, and therefore whether the E
  // state can pay off in cycles rather than only in traffic.
  double bus_utilisation() const;

  // Fraction of available bank-cycles memory spent serving, 0..1. The same
  // question for the O state. Measured over max(makespan, memory_drained_at).
  double memory_utilisation() const;

  // Cycles memory kept working after the last core retired. Non-zero means the
  // write stream is oversubscribed: the cores finished but memory had not.
  uint64_t memory_drain_tail() const {
    return memory_drained_at > makespan ? memory_drained_at - makespan : 0;
  }

  // Mean cycles a core spent blocked per access.
  double stall_per_access() const;

  static Report collect(const Simulator &sim, const SimConfig &config,
                        const std::string &trace_path);

  void write_text(std::ostream &out) const;
  void write_json(std::ostream &out) const;

  // One fixed-width line for sweep tables; `header_row` labels its columns.
  void write_row(std::ostream &out) const;
  static void write_row_header(std::ostream &out);
};
