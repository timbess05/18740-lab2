// Bus arbitration, snoop orchestration, and transaction timing.
// See snoop_bus.hpp for the serialization model.

#include "snoop_bus.hpp"

#include "fsm_check.hpp"
#include "main_memory.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

SnoopBus::SnoopBus(BusConfig config, const CoherenceProtocol &protocol, MainMemory &memory,
                   size_t block_bytes)
    : config_(config), protocol_(protocol), memory_(memory), block_bytes_(block_bytes) {
  if (block_bytes_ == 0) {
    throw std::runtime_error("SnoopBus: block_bytes must be > 0");
  }
  if (config_.width_bytes == 0) {
    throw std::runtime_error("SnoopBus: width_bytes must be > 0");
  }
  data_cycles_ =
      static_cast<uint32_t>((block_bytes_ + config_.width_bytes - 1) / config_.width_bytes);
  fsm_checked_ = config_.check_fsm && has_fsm_rules(protocol_.name());
}

void SnoopBus::register_core(SnoopTarget &target) {
  const uint32_t id = target.core_id();
  if (id >= targets_.size()) {
    targets_.resize(id + 1, nullptr);
    peer_states_.resize(id + 1, CacheState::I);
  }
  targets_[id] = &target;
}

void SnoopBus::count_transaction(BusReqType type) {
  ++stats_.transactions;
  switch (type) {
  case BusReqType::Read:
    ++stats_.reads;
    break;
  case BusReqType::ReadX:
    ++stats_.reads_exclusive;
    break;
  case BusReqType::Upgrade:
    ++stats_.upgrades;
    break;
  case BusReqType::Writeback:
    ++stats_.writebacks;
    break;
  }
}

// Every registered cache reports what it holds. The requestor's own entry is
// filled in too; the protocol ignores it.
void SnoopBus::collect_peer_states(uint64_t addr) {
  for (size_t id = 0; id < targets_.size(); ++id) {
    peer_states_[id] = targets_[id] ? targets_[id]->snoop(addr) : CacheState::I;
  }
}

void SnoopBus::validate(const BusRequest &request, const ProtocolDecision &decision) const {
  const std::string who = protocol_.name();
  for (const PeerTransition &change : decision.peer_transitions) {
    if (change.core_id >= targets_.size()) {
      throw std::runtime_error(who + ": peer transition names core " +
                               std::to_string(change.core_id) + ", but only " +
                               std::to_string(targets_.size()) + " cores exist");
    }
    if (change.core_id == request.core_id) {
      throw std::runtime_error(who + ": peer transition names core " +
                               std::to_string(change.core_id) +
                               ", which is the requestor; a protocol reports the requestor's own"
                               " state through requestor_state, and must skip its own entry in"
                               " peer_states");
    }
  }
  // Dirty data must survive the transaction. If any cache held the line
  // modified beforehand, then afterwards either some cache still holds it
  // modified, or memory was updated. Without this a protocol can silently drop
  // the only copy of a written line: the resulting states are perfectly legal
  // under single-writer/multiple-reader, so the invariant checker sees nothing
  // wrong, and the run reports plausible numbers for a machine that has lost
  // the program's data.
  bool dirty_before = false;
  bool dirty_after = false;
  for (uint32_t core_id = 0; core_id < peer_states_.size(); ++core_id) {
    dirty_before = dirty_before || is_dirty(peer_states_[core_id]);

    CacheState after = peer_states_[core_id];
    if (core_id == request.core_id) {
      after = decision.requestor_state;
    } else {
      for (const PeerTransition &change : decision.peer_transitions) {
        if (change.core_id == core_id) {
          after = change.new_state;
          break;
        }
      }
    }
    dirty_after = dirty_after || is_dirty(after);
  }
  if (dirty_before && !dirty_after && !decision.writeback_needed) {
    throw std::runtime_error(who + ": " + to_string(request.type) +
                             " leaves no cache holding the line modified and asks for no"
                             " writeback, so the only up-to-date copy is lost; a dirty line"
                             " must either stay dirty somewhere or go to memory");
  }

  if (decision.data_from_peer) {
    if (decision.supplier_id >= peer_states_.size()) {
      throw std::runtime_error(who + ": supplier_id " + std::to_string(decision.supplier_id) +
                               " is out of range");
    }
    if (decision.supplier_id == request.core_id) {
      throw std::runtime_error(who + ": names the requestor as its own data supplier");
    }
    if (peer_states_[decision.supplier_id] == CacheState::I) {
      throw std::runtime_error(who + ": names core " + std::to_string(decision.supplier_id) +
                               " as data supplier, but that core does not hold the line");
    }
  }
}

BusResponse SnoopBus::issue(const BusRequest &request) {
  count_transaction(request.type);

  collect_peer_states(request.addr);
  const ProtocolDecision decision =
      protocol_.on_request(request.type, request.core_id, peer_states_);
  validate(request, decision);

  // Live FSM check: the same rules the offline battery uses, applied to the
  // states this workload actually produced. Capped so a systematically wrong
  // arc reports once per kind rather than once per transaction.
  if (fsm_checked_ && fsm_failures_.size() < 25) {
    for (std::string &problem :
         check_decision(fsm_rules_for(protocol_.name()), request.type, request.core_id,
                        peer_states_, decision)) {
      if (fsm_failures_.size() < 25) {
        fsm_failures_.push_back(std::move(problem));
      }
    }
  }

  for (const PeerTransition &change : decision.peer_transitions) {
    if (SnoopTarget *peer = targets_[change.core_id]) {
      peer->apply_transition(request.addr, change.new_state);
    }
    if (change.new_state == CacheState::I) {
      ++stats_.invalidations;
    }
  }
  if (decision.data_from_peer) {
    ++stats_.peer_supplied;
  }

  BusResponse response;
  response.new_state = decision.requestor_state;

  if (config_.infinite_bandwidth) {
    // Transactions still serialize logically; they just cost nothing. Memory is
    // still consulted so its own stats and bandwidth limit stay meaningful.
    const uint64_t completion =
        start_memory_access(request, decision, request.cycle, &response.backpressure);
    response.latency = completion - request.cycle;
    return response;
  }

  // Reserve the bus. A transaction that arrives while the bus is busy waits.
  const uint64_t granted = std::max(request.cycle, free_at_);
  stats_.arbitration_cycles += granted - request.cycle;

  const uint64_t occupancy = config_.addr_cycles + (moves_data(request.type) ? data_cycles_ : 0);
  free_at_ = granted + occupancy;
  stats_.busy_cycles += occupancy;

  // Memory is reached after the address phase and runs in parallel with the
  // rest of the bus timeline, so the transaction ends at whichever finishes
  // last. The bus itself is free again at free_at_ regardless.
  const uint64_t memory_done =
      start_memory_access(request, decision, granted + config_.addr_cycles, &response.backpressure);
  response.latency = std::max(free_at_, memory_done) - request.cycle;
  return response;
}

// Issues whatever memory traffic the decision implies and returns the cycle it
// completes, or `at` if none is needed.
uint64_t SnoopBus::start_memory_access(const BusRequest &request, const ProtocolDecision &decision,
                                       uint64_t at, uint64_t *backpressure) {
  uint64_t completion = at;

  // Only a fill blocks the requestor, and only when no cache could supply the
  // line. This is the sole memory access on the transaction's critical path.
  const bool fetches_line =
      (request.type == BusReqType::Read || request.type == BusReqType::ReadX) &&
      !decision.data_from_peer;
  if (fetches_line) {
    completion = std::max(completion, memory_.access(request.addr, /*is_write=*/false, at));
  }

  // Writes are posted, never awaited. Covers both an explicit Writeback and the
  // flush a protocol without the O state owes when an M holder gives up sole
  // ownership. In both cases the data reaching memory is not what anyone is
  // waiting for: an eviction's data is already gone from the core's viewpoint,
  // and a flush hands the line to the requestor over the bus while the memory
  // controller writes it in the background.
  //
  // The access still runs, so it occupies its bank and delays every later
  // memory access -- which is the real cost, and exactly the cost the O state
  // removes. Charging its latency to the requesting core instead would make O
  // look like a latency optimisation, and would show a benefit even with memory
  // almost idle.
  if (decision.writeback_needed) {
    const uint64_t write_done = memory_.access(request.addr, /*is_write=*/true, at);
    const uint64_t backlog = write_done > at ? write_done - at : 0;
    const uint64_t buffer = memory_.write_buffer_cycles();
    if (backlog > buffer) {
      // Write buffer full: the overflow is what the core actually waits for.
      *backpressure = backlog - buffer;
      completion = std::max(completion, at + *backpressure);
    }
  }

  return completion;
}
