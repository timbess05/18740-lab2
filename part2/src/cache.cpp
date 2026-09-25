// Set-associative cache storage. See cache.hpp for the interface contract.

#include "cache.hpp"

#include <stdexcept>
#include <string>

namespace {

bool is_power_of_two(size_t value) { return value != 0 && (value & (value - 1)) == 0; }

} // namespace

const char *to_string(CacheState state) {
  switch (state) {
  case CacheState::I:
    return "I";
  case CacheState::S:
    return "S";
  case CacheState::E:
    return "E";
  case CacheState::M:
    return "M";
  case CacheState::O:
    return "O";
  }
  return "?";
}

Cache::Cache(size_t size_bytes, size_t assoc, size_t block_size)
    : block_size_(block_size), assoc_(assoc) {
  if (size_bytes == 0 || assoc == 0 || block_size == 0) {
    throw std::runtime_error("Cache: size_bytes, assoc, and block_size must all be > 0");
  }
  if (!is_power_of_two(block_size)) {
    throw std::runtime_error("Cache: block_size (" + std::to_string(block_size) +
                             ") must be a power of two");
  }
  if (size_bytes % (assoc * block_size) != 0) {
    throw std::runtime_error("Cache: size_bytes (" + std::to_string(size_bytes) +
                             ") must be divisible by assoc * block_size (" +
                             std::to_string(assoc * block_size) + ")");
  }
  num_sets_ = size_bytes / (assoc * block_size);
  lines_.assign(num_sets_ * assoc_, CacheLine{});
}

size_t Cache::set_index(uint64_t addr) const {
  return static_cast<size_t>((addr / block_size_) % num_sets_);
}

uint64_t Cache::tag_of(uint64_t addr) const { return (addr / block_size_) / num_sets_; }

uint64_t Cache::block_addr(size_t set, uint64_t tag) const {
  return (tag * num_sets_ + set) * block_size_;
}

std::optional<size_t> Cache::find_resident_way(size_t set, uint64_t tag) const {
  const size_t base = set * assoc_;
  for (size_t way = 0; way < assoc_; ++way) {
    const CacheLine &line = lines_[base + way];
    if (line.state != CacheState::I && line.tag == tag) {
      return base + way;
    }
  }
  return std::nullopt;
}

std::optional<size_t> Cache::find_free_way(size_t set) const {
  const size_t base = set * assoc_;
  for (size_t way = 0; way < assoc_; ++way) {
    if (lines_[base + way].state == CacheState::I) {
      return base + way;
    }
  }
  return std::nullopt;
}

size_t Cache::pick_lru_victim(size_t set) const {
  const size_t base = set * assoc_;
  size_t victim = base;
  uint64_t oldest_stamp = lines_[base].lru_stamp;
  for (size_t way = 1; way < assoc_; ++way) {
    // Strict less-than keeps the lowest way index when stamps tie, which
    // happens for the cold ways of a set at startup.
    if (lines_[base + way].lru_stamp < oldest_stamp) {
      oldest_stamp = lines_[base + way].lru_stamp;
      victim = base + way;
    }
  }
  return victim;
}

CacheResult Cache::lookup(uint64_t addr) {
  CacheResult result;
  const size_t set = set_index(addr);
  const uint64_t tag = tag_of(addr);

  if (const auto hit_way = find_resident_way(set, tag)) {
    lines_[*hit_way].lru_stamp = ++lru_clock_;
    result.hit = true;
    result.state_before = lines_[*hit_way].state;
    return result;
  }

  // Miss. An eviction is only needed once the set has no invalid way left.
  if (find_free_way(set)) {
    return result;
  }

  const CacheLine &victim = lines_[pick_lru_victim(set)];
  result.eviction_needed = true;
  result.victim_addr = block_addr(set, victim.tag);
  result.victim_state = victim.state;
  return result;
}

void Cache::install(uint64_t addr, CacheState state) {
  const size_t set = set_index(addr);
  const auto resident = find_resident_way(set, tag_of(addr));
  const auto free_way = resident ? resident : find_free_way(set);
  const size_t slot = free_way ? *free_way : pick_lru_victim(set);

  CacheLine &line = lines_[slot];
  line.tag = tag_of(addr);
  line.state = state;
  line.lru_stamp = ++lru_clock_;
}

void Cache::update_state(uint64_t addr, CacheState new_state) {
  if (const auto way = find_resident_way(set_index(addr), tag_of(addr))) {
    lines_[*way].state = new_state;
  }
}

std::optional<CacheState> Cache::state(uint64_t addr) const {
  if (const auto way = find_resident_way(set_index(addr), tag_of(addr))) {
    return lines_[*way].state;
  }
  return std::nullopt;
}
