// Core access handling. See core.hpp for the blocking model.

#include "core.hpp"

#include <stdexcept>
#include <string>

namespace {

AccessType access_type_of(const MemOp &op) {
  return op.op == MemOp::Op::Read ? AccessType::Read : AccessType::Write;
}

} // namespace

Core::Core(uint32_t core_id, size_t cache_bytes, size_t cache_assoc, size_t block_bytes,
           uint32_t hit_cycles, const CoherenceProtocol &protocol, SnoopBus &bus)
    : core_id_(core_id), cache_(cache_bytes, cache_assoc, block_bytes), hit_cycles_(hit_cycles),
      protocol_(protocol), bus_(bus) {}

CacheState Core::snoop(uint64_t addr) const {
  return cache_.state(addr).value_or(CacheState::I);
}

void Core::apply_transition(uint64_t addr, CacheState new_state) {
  // A transition to I is an invalidation; update_state handles both, and is a
  // no-op if this cache does not hold the line.
  cache_.update_state(addr, new_state);
}

uint64_t Core::write_back_victim(const CacheResult &result, uint64_t at_cycle) {
  const BusResponse response =
      bus_.issue({core_id_, BusReqType::Writeback, result.victim_addr, at_cycle});
  // The bus and memory time is not this core's to wait for -- the line is
  // already gone from its viewpoint. Only a full write buffer stalls it.
  return response.backpressure;
}

uint64_t Core::access(const MemOp &op, uint64_t at_cycle) {
  const AccessType type = access_type_of(op);
  if (type == AccessType::Read) {
    ++stats_.reads;
  } else {
    ++stats_.writes;
  }

  const uint64_t block = cache_.block_align(op.addr);
  const CacheResult result = cache_.lookup(block);
  const CacheState current = result.hit ? result.state_before : CacheState::I;

  const std::optional<BusReqType> request = protocol_.request_for(type, current);

  if (result.hit) {
    ++stats_.hits;
    if (!request) {
      // Readable and, for a write, already writable. Nothing reaches the bus.
      ++stats_.silent_hits;
      stats_.stall_cycles += hit_cycles_;
      if (type == AccessType::Write) {
        
        cache_.update_state(block, CacheState::M);
      }
      return hit_cycles_;
    }
    // A write to a line held non-exclusively: invalidate the other holders.
    ++stats_.upgrade_hits;
    const BusResponse response = bus_.issue({core_id_, *request, block, at_cycle});
    cache_.update_state(block, response.new_state);
    stats_.stall_cycles += hit_cycles_ + response.latency;
    return hit_cycles_ + response.latency;
  }

  ++stats_.misses;
  if (!request) {
    // Dereferencing the empty optional below would be undefined behaviour, and
    // in practice silently proceeds with a garbage transaction type.
    throw std::runtime_error(std::string(protocol_.name()) +
                             ": request_for() returned no bus transaction for a " +
                             (type == AccessType::Read ? "read" : "write") +
                             " miss; a line in I must be fetched over the bus");
  }
  if (result.eviction_needed) {
    ++stats_.evictions;
  }

  // Fetch before writeback, deliberately. Issuing the writeback first would put
  // its bus occupancy ahead of the fill and stall this core behind data it is
  // not waiting for.
  const BusResponse response = bus_.issue({core_id_, *request, block, at_cycle});

  uint64_t writeback_stall = 0;
  if (result.eviction_needed && is_dirty(result.victim_state)) {
    ++stats_.dirty_evictions;
    writeback_stall = write_back_victim(result, at_cycle);
  }

  // install() displaces exactly the victim lookup() reported, so the writeback
  // above named the right address.
  cache_.install(block, response.new_state);

  const uint64_t stall = hit_cycles_ + response.latency + writeback_stall;
  stats_.stall_cycles += stall;
  return stall;
}
