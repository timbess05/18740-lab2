// Cache storage: geometry, indexing, LRU, the lookup/install victim contract,
// and state bookkeeping. Run from lab3-reduced/ (the Makefile does this).

#include "cache.hpp"
#include "test_helpers.hpp"

#include <stdexcept>
#include <string>

namespace {

// 4 sets x 2 ways x 64B = 512B. Addresses 64*4=256 apart collide in a set.
constexpr size_t kBlock = 64;
constexpr size_t kAssoc = 2;
constexpr size_t kSets = 4;
constexpr size_t kSize = kSets * kAssoc * kBlock;
constexpr uint64_t kSetStride = kSets * kBlock; // 256

Cache make_cache() { return Cache(kSize, kAssoc, kBlock); }

bool construction_throws(size_t size_bytes, size_t assoc, size_t block_size) {
  try {
    Cache cache(size_bytes, assoc, block_size);
  } catch (const std::runtime_error &) {
    return true;
  }
  return false;
}

void test_geometry() {
  Cache cache = make_cache();
  EXPECT_EQ(cache.num_sets(), kSets);
  EXPECT_EQ(cache.assoc(), kAssoc);
  EXPECT_EQ(cache.block_size(), kBlock);
}

void test_invalid_geometry_throws() {
  EXPECT_TRUE(construction_throws(0, kAssoc, kBlock));
  EXPECT_TRUE(construction_throws(kSize, 0, kBlock));
  EXPECT_TRUE(construction_throws(kSize, kAssoc, 0));
  EXPECT_TRUE(construction_throws(kSize, kAssoc, 100)); // not a power of two
  EXPECT_TRUE(construction_throws(300, kAssoc, kBlock)); // not divisible
  EXPECT_TRUE(!construction_throws(kSize, kAssoc, kBlock));
}

void test_block_align() {
  Cache cache = make_cache();
  EXPECT_EQ(cache.block_align(0x1000), std::uint64_t{0x1000});
  EXPECT_EQ(cache.block_align(0x103F), std::uint64_t{0x1000});
  EXPECT_EQ(cache.block_align(0x1040), std::uint64_t{0x1040});
}

void test_cold_miss_then_hit() {
  Cache cache = make_cache();
  CacheResult miss = cache.lookup(0x1000);
  EXPECT_TRUE(!miss.hit);
  EXPECT_TRUE(miss.state_before == CacheState::I);
  EXPECT_TRUE(!miss.eviction_needed);

  cache.install(0x1000, CacheState::E);
  CacheResult hit = cache.lookup(0x1000);
  EXPECT_TRUE(hit.hit);
  EXPECT_TRUE(hit.state_before == CacheState::E);
}

void test_offsets_within_block_are_one_line() {
  Cache cache = make_cache();
  cache.install(0x1000, CacheState::S);
  // Every byte in [0x1000, 0x1040) maps to the same line.
  EXPECT_TRUE(cache.lookup(0x1001).hit);
  EXPECT_TRUE(cache.lookup(0x103F).hit);
  EXPECT_TRUE(!cache.lookup(0x1040).hit);
}

void test_set_index_wraps() {
  Cache cache = make_cache();
  // Two blocks kSetStride apart share a set; the set has 2 ways, so both fit.
  cache.install(0x0000, CacheState::S);
  cache.install(0x0000 + kSetStride, CacheState::S);
  EXPECT_TRUE(cache.lookup(0x0000).hit);
  EXPECT_TRUE(cache.lookup(0x0000 + kSetStride).hit);
}

void test_eviction_reports_lru_victim() {
  Cache cache = make_cache();
  cache.install(0x0000, CacheState::M);              // way 0
  cache.install(0x0000 + kSetStride, CacheState::S); // way 1
  cache.lookup(0x0000 + kSetStride);                 // makes way 1 most-recent

  // Third block in the same set: the set is full, way 0 is LRU.
  CacheResult miss = cache.lookup(0x0000 + 2 * kSetStride);
  EXPECT_TRUE(!miss.hit);
  EXPECT_TRUE(miss.eviction_needed);
  EXPECT_EQ(miss.victim_addr, std::uint64_t{0x0000});
  EXPECT_TRUE(miss.victim_state == CacheState::M);
  EXPECT_TRUE(is_dirty(miss.victim_state));
}

void test_lookup_does_not_evict() {
  Cache cache = make_cache();
  cache.install(0x0000, CacheState::M);
  cache.install(0x0000 + kSetStride, CacheState::S);

  cache.lookup(0x0000 + 2 * kSetStride); // reports a victim, evicts nothing
  EXPECT_TRUE(cache.state(0x0000).has_value());
  EXPECT_TRUE(cache.state(0x0000 + kSetStride).has_value());
}

void test_install_displaces_the_reported_victim() {
  // The contract Core depends on: the victim named by lookup() is the one
  // install() removes, so the writeback is issued for the right address.
  Cache cache = make_cache();
  cache.install(0x0000, CacheState::M);
  cache.install(0x0000 + kSetStride, CacheState::S);
  cache.lookup(0x0000 + kSetStride); // way 0 is now LRU

  const uint64_t fill = 0x0000 + 2 * kSetStride;
  CacheResult miss = cache.lookup(fill);
  cache.install(fill, CacheState::E);

  EXPECT_TRUE(!cache.state(miss.victim_addr).has_value()); // victim gone
  EXPECT_TRUE(cache.state(0x0000 + kSetStride).has_value()); // survivor kept
  EXPECT_TRUE(cache.state(fill).has_value());
}

void test_install_prefers_free_way_over_eviction() {
  Cache cache = make_cache();
  cache.install(0x0000, CacheState::M);
  cache.install(0x0000 + kSetStride, CacheState::S);
  cache.update_state(0x0000, CacheState::I); // frees way 0

  cache.install(0x0000 + 2 * kSetStride, CacheState::E);
  // The surviving S line must still be there; the free way was used.
  EXPECT_TRUE(cache.state(0x0000 + kSetStride).has_value());
  EXPECT_TRUE(cache.state(0x0000 + 2 * kSetStride).has_value());
}

void test_state_accessors() {
  Cache cache = make_cache();
  EXPECT_TRUE(!cache.state(0x1000).has_value());

  cache.install(0x1000, CacheState::E);
  EXPECT_TRUE(cache.state(0x1000) == CacheState::E);

  cache.update_state(0x1000, CacheState::M);
  EXPECT_TRUE(cache.state(0x1000) == CacheState::M);

  cache.update_state(0x1000, CacheState::I);
  EXPECT_TRUE(!cache.state(0x1000).has_value());
}

void test_updates_to_absent_lines_are_noops() {
  Cache cache = make_cache();
  cache.update_state(0x9999, CacheState::M); // must not create a line
  cache.update_state(0x9999, CacheState::I);
  EXPECT_TRUE(!cache.state(0x9999).has_value());
}

void test_dirty_is_derived_from_state() {
  EXPECT_TRUE(is_dirty(CacheState::M));
  EXPECT_TRUE(is_dirty(CacheState::O));
  EXPECT_TRUE(!is_dirty(CacheState::I));
  EXPECT_TRUE(!is_dirty(CacheState::S));
  EXPECT_TRUE(!is_dirty(CacheState::E));
}

void test_state_names() {
  EXPECT_EQ(std::string(to_string(CacheState::I)), std::string("I"));
  EXPECT_EQ(std::string(to_string(CacheState::O)), std::string("O"));
}

} // namespace

int main() {
  test_geometry();
  test_invalid_geometry_throws();
  test_block_align();
  test_cold_miss_then_hit();
  test_offsets_within_block_are_one_line();
  test_set_index_wraps();
  test_eviction_reports_lru_victim();
  test_lookup_does_not_evict();
  test_install_displaces_the_reported_victim();
  test_install_prefers_free_way_over_eviction();
  test_state_accessors();
  test_updates_to_absent_lines_are_noops();
  test_dirty_is_derived_from_state();
  test_state_names();
  TEST_REPORT_AND_EXIT();
}
