// Coherence safety checking.
//
// Every protocol here, however many states it has, must maintain
// single-writer/multiple-reader: a line is either held for writing by exactly
// one cache, or held for reading by any number of caches, never both.
// Expressed over the five states:
//
//   at most one cache in M, E or O   -- only one cache may own a line
//   if any cache is in M or E        -- an exclusive holder tolerates no others
//       no other cache holds it at all
//   O may coexist with any number of S  -- that is what O is for
//
// This is protocol-independent, so it checks a student's MESI/MOSI/MOESI
// without a reference implementation to diff against, and it names the
// specific fault rather than reporting a number that differs. It catches the
// usual bugs directly: failing to invalidate sharers on a write, promoting to
// M without an Upgrade, or leaving two owners after a transfer.
//

#pragma once

#include "cache.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Describes a violation of the single-writer/multiple-reader invariant, or
// nullopt if `states` is a legal combination. `states` is indexed by core id.
std::optional<std::string> check_coherence_invariant(const std::vector<CacheState> &states);

struct InvariantViolation {
  uint64_t access_index = 0; // which access in the run exposed it
  uint32_t core_id = 0;      // core whose access exposed it
  uint64_t addr = 0;         // block-aligned line
  std::string message;

  std::string describe() const;
};
