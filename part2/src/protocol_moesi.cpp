// MOESI -- Modified, Owner, Exclusive, Shared, Invalid. [ YOU IMPLEMENT THIS ]
//
// MOESI has both of the states the previous two protocols added, and they are
// independent optimisations that save different things.
//
// WHERE TO LOOK
//   include/protocol.hpp      the contract: ProtocolDecision and what each
//                             field means.
//   src/protocol_mesi.cpp     your MESI, once it works -- E behaves the same
//                             way here.
//   src/protocol_mosi.cpp     your MOSI, once it works -- O behaves the same
//                             way here.
//   src/fsm_check.cpp         the specification: every arc of every protocol,
//                             stated as data. `build_moesi()` is this file's
//                             contract, and --check tests you against it.
//
// HELPERS AVAILABLE (declared in include/protocol.hpp)
//   find_supplier(peer_states, requestor_id, {priority...})
//       First peer holding the line in one of the listed states, tried in
//       order. With five states this is where the order starts to matter:
//       more than one cache may be able to supply, and they are not equally
//       good choices.
//   find_holders(peer_states, requestor_id)
//       Every peer holding the line in any valid state.

#include "protocol.hpp"

#include <stdexcept>
#include <string>

namespace {

[[noreturn]] void not_implemented(const char *function) {
  throw std::runtime_error(std::string("not implemented: MOESIProtocol::") + function +
                           " -- see src/protocol_moesi.cpp");
}

} // namespace

// Core side. Given an access and the state this cache holds the line in, which
// bus transaction does the access need? Return nullopt for a silent hit: one
// that needs no bus transaction at all.
//
// Every state is reachable here, so unlike MESI and MOSI there is no state to
// reject.
std::optional<BusReqType> MOESIProtocol::request_for(AccessType access, CacheState state) const {
  switch (state) {
  case CacheState::I:
    return access == AccessType::Read ? BusReqType::Read : BusReqType::ReadX;

  case CacheState::S:
    return access == AccessType::Read ? std::optional<BusReqType>{}
                                      : std::optional<BusReqType>{BusReqType::Upgrade};

  case CacheState::E:
    return std::nullopt;

  case CacheState::O:
    return access == AccessType::Read ? std::optional<BusReqType>{}
                                      : std::optional<BusReqType>{BusReqType::Upgrade};

  case CacheState::M:
    return std::nullopt;

  default:
    // E belongs to MESI and MOESI; MOSI can never produce it.
    protocol_detail::reject_state(name(), access, state, "M, O, S and I");
  }
  not_implemented("request_for");
}

// Bus side. `type` was issued by `requestor_id`; `peer_states` is indexed by
// core id and covers every core, so ignore the requestor's own entry. Return
// what happens to everyone.
//
// Fill in, for each transaction:
//   decision.requestor_state    the state the requesting cache ends in
//   decision.peer_transitions   every peer whose state changes; peers that
//                               keep their state are simply omitted
//   decision.data_from_peer     true if a cache supplies the line, so no
//                               memory read is needed
//   decision.supplier_id        which cache supplied it
//   decision.writeback_needed   true if memory must be written
ProtocolDecision MOESIProtocol::on_request(BusReqType type, uint32_t requestor_id,
                                           const std::vector<CacheState> &peer_states) const {
  ProtocolDecision decision;

  switch (type) {
  case BusReqType::Read: {
    // Prefer the M holder: it has the only up-to-date copy.
    const auto supplier = find_supplier(peer_states, requestor_id, {CacheState::E, CacheState::M, CacheState::O, CacheState::S});
    if (supplier) {
      decision.data_from_peer = true;
      decision.supplier_id = *supplier;
      if (peer_states[*supplier] == CacheState::E) {
        decision.peer_transitions.push_back({*supplier, CacheState::S});
      }
      if (peer_states[*supplier] == CacheState::M) {
        decision.peer_transitions.push_back({*supplier, CacheState::O});
      }
      // An S supplier keeps its state, so it needs no transition entry.
      decision.requestor_state = CacheState::S;
    }
    else {
      decision.requestor_state = CacheState::E;
    }
    break;
  }

  case BusReqType::ReadX: {
    const auto supplier = find_supplier(peer_states, requestor_id, {CacheState::E, CacheState::M, CacheState::O, CacheState::S});
    if (supplier) {
      decision.data_from_peer = true;
      decision.supplier_id = *supplier;
    }
    for (uint32_t holder : find_holders(peer_states, requestor_id)) {
      decision.peer_transitions.push_back({holder, CacheState::I});
    }
    decision.requestor_state = CacheState::M;
    break;
  }

  case BusReqType::Upgrade: {
    // The requestor already holds the line in S, so no data moves and memory
    // is untouched. All that is needed is to invalidate the other sharers.
    for (uint32_t holder : find_holders(peer_states, requestor_id)) {
      decision.peer_transitions.push_back({holder, CacheState::I});
    }
    decision.requestor_state = CacheState::M;
    break;
  }

  case BusReqType::Writeback: {
    decision.requestor_state = CacheState::I;
    decision.writeback_needed = true;
    break;
  }
  }
  return decision;
}
