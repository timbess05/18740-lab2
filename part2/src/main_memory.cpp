// Banked DRAM timing. See main_memory.hpp for the bandwidth dial.

#include "main_memory.hpp"

#include <algorithm>
#include <stdexcept>

MainMemory::MainMemory(MemoryConfig config) : config_(config) {
  if (config_.num_banks == 0) {
    throw std::runtime_error("MainMemory: num_banks must be > 0");
  }
  if (config_.row_bytes == 0) {
    throw std::runtime_error("MainMemory: row_bytes must be > 0");
  }
  open_row_.assign(config_.num_banks, -1);
  busy_until_.assign(config_.num_banks, 0);
}

// Rows interleave across banks before advancing, so consecutive rows land in
// different banks and a sequential sweep spreads across all of them.
uint32_t MainMemory::bank_of(uint64_t addr) const {
  return static_cast<uint32_t>((addr / config_.row_bytes) % config_.num_banks);
}

uint64_t MainMemory::row_of(uint64_t addr) const {
  return addr / (static_cast<uint64_t>(config_.row_bytes) * config_.num_banks);
}

uint64_t MainMemory::busy_until() const {
  uint64_t last = 0;
  for (uint64_t bank_free : busy_until_) {
    last = std::max(last, bank_free);
  }
  return last;
}

uint64_t MainMemory::access(uint64_t addr, bool is_write, uint64_t arrival) {
  if (is_write) {
    ++stats_.writes;
  } else {
    ++stats_.reads;
  }

  if (config_.infinite_bandwidth) {
    return arrival;
  }

  const uint32_t bank = bank_of(addr);
  const int64_t row = static_cast<int64_t>(row_of(addr));

  // Wait for the bank to finish whatever it was doing.
  uint64_t queued = 0;
  if (busy_until_[bank] > arrival) {
    queued = busy_until_[bank] - arrival;
    ++stats_.bank_conflicts;
  }

  uint64_t service = config_.row_access;
  if (open_row_[bank] == row) {
    ++stats_.row_hits;
  } else {
    ++stats_.row_misses;
    service += config_.row_open;
    if (open_row_[bank] >= 0) {
      // A different row is open and must be closed first.
      service += config_.row_close;
    }
  }
  if (is_write) {
    service += config_.write_recover;
  }

  const uint64_t completion = arrival + queued + service;
  open_row_[bank] = row;
  busy_until_[bank] = completion;

  stats_.queue_cycles += queued;
  stats_.service_cycles += service;
  return completion;
}
