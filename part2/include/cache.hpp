// Private per-core cache: set-associative, LRU, write-back, write-allocate.
//
// Protocol-agnostic. The cache stores a coherence state per line but never
// decides what that state should be -- CoherenceProtocol does, and Core
// applies the result. All five protocols share this storage unchanged, so
// protocol comparisons differ only in the state machine.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// I/S/M are used by every protocol. E is MESI/MOESI only; O is MOSI/MOESI
// only. A protocol simply never produces the states it does not have.
enum class CacheState : uint8_t {
  I, // Invalid
  S, // Shared        (clean, may be present elsewhere)
  E, // Exclusive     (clean, known to be the only copy)
  M, // Modified      (dirty, only copy)
  O  // Owner         (dirty, may be present elsewhere as S)
};

enum class AccessType : uint8_t { Read, Write };

const char *to_string(CacheState state);

// A line holds dirty data exactly in M and O. There is no separate dirty bit:
// it would be a second source of truth that could drift from the state, and
// every protocol already encodes dirtiness in the state itself.
constexpr bool is_dirty(CacheState state) {
  return state == CacheState::M || state == CacheState::O;
}

struct CacheLine {
  uint64_t tag = 0;
  CacheState state = CacheState::I;
  uint64_t lru_stamp = 0;
};

struct CacheResult {
  bool hit = false;
  CacheState state_before = CacheState::I; // I on a miss

  // Set only on a miss into a full set. The eviction has NOT happened yet --
  // these describe the victim that the next install() will displace, so the
  // caller can issue a writeback for it first. See the install() contract.
  bool eviction_needed = false;
  uint64_t victim_addr = 0; // block-aligned
  CacheState victim_state = CacheState::I;
};

class Cache {
public:
  // Throws std::runtime_error if the geometry is invalid: any argument zero,
  // block_size not a power of two, or size_bytes not divisible by
  // assoc * block_size.
  Cache(size_t size_bytes, size_t assoc, size_t block_size);

  // Looks up addr, updating LRU on a hit. On a miss into a full set, reports
  // the prospective victim without evicting it.
  CacheResult lookup(uint64_t addr);

  // Allocates a line for addr in `state`, displacing the LRU victim if the set
  // is full. Any writeback the victim needs is the caller's responsibility.
  //
  // Contract: if a preceding lookup(addr) reported eviction_needed, install()
  // displaces exactly that victim -- provided nothing else touched this set in
  // between. Callers must not interleave other accesses to the same cache.
  void install(uint64_t addr, CacheState state);

  // Forces a state change on a resident line. No-op if addr is not resident.
  void update_state(uint64_t addr, CacheState new_state);

  // Current state of addr, or nullopt if not resident. Used to answer snoops.
  std::optional<CacheState> state(uint64_t addr) const;

  size_t block_size() const { return block_size_; }
  size_t num_sets() const { return num_sets_; }
  size_t assoc() const { return assoc_; }

  uint64_t block_align(uint64_t addr) const { return addr & ~(block_size_ - 1); }

private:
  size_t block_size_;
  size_t assoc_;
  size_t num_sets_;
  std::vector<CacheLine> lines_; // num_sets_ * assoc_, set-major
  uint64_t lru_clock_ = 0;

  size_t set_index(uint64_t addr) const;
  uint64_t tag_of(uint64_t addr) const;
  uint64_t block_addr(size_t set, uint64_t tag) const;

  // Index into lines_ of the way holding `tag` in `set`, or nullopt. No LRU
  // update.
  std::optional<size_t> find_resident_way(size_t set, uint64_t tag) const;
  // Index into lines_ of an invalid way in `set`, or nullopt if the set is
  // full.
  std::optional<size_t> find_free_way(size_t set) const;
  // Index into lines_ of the LRU victim. Ties go to the lowest way index, so
  // eviction order does not depend on the standard library.
  size_t pick_lru_victim(size_t set) const;
};
