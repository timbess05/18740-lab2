// MOSI -- Modified, Owner, Shared, Invalid.            [ YOU IMPLEMENT THIS ]
//
// MOSI is MSI plus one state. O is a line that is dirty AND shared: this 
// cache has written it, other caches now hold readable copies of it, and memory 
// is still stale.
//
// WHERE TO LOOK
//   include/protocol.hpp      the contract: ProtocolDecision and what each
//                             field means. Read this first.
//   src/protocol_msi.cpp      the worked example closest to this file. MOSI
//                             differs from it in a small number of places.
//   src/protocol_mi.cpp       the degenerate protocol, useful for seeing what
//                             the S state buys.
//
// HELPERS AVAILABLE (declared in include/protocol.hpp)
//   find_supplier(peer_states, requestor_id, {priority...})
//       First peer holding the line in one of the listed states, tried in
//       order. Returns nullopt if nobody holds it.
//   find_holders(peer_states, requestor_id)
//       Every peer holding the line in any valid state.
//   protocol_detail::reject_state(name(), access, state, "...")
//       Report an access in a state MOSI cannot produce.

#include "protocol.hpp"

#include <stdexcept>
#include <string>

namespace {

[[noreturn]] void not_implemented(const char *function) {
  throw std::runtime_error(std::string("not implemented: MOSIProtocol::") + function +
                           " -- see src/protocol_mosi.cpp");
}

} // namespace

// Core side. Given an access and the state this cache holds the line in, which
// bus transaction does the access need? Return nullopt for a silent hit: one
// that needs no bus transaction at all.
std::optional<BusReqType> MOSIProtocol::request_for(AccessType access, CacheState state) const {
  switch (state) {
  case CacheState::I:
    return access == AccessType::Read ? BusReqType::Read : BusReqType::ReadX;

  case CacheState::S:
    return access == AccessType::Read ? std::optional<BusReqType>{}
                                      : std::optional<BusReqType>{BusReqType::Upgrade};

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
ProtocolDecision MOSIProtocol::on_request(BusReqType type, uint32_t requestor_id,
                                          const std::vector<CacheState> &peer_states) const {
  ProtocolDecision decision;

  switch (type) {
  case BusReqType::Read: {
    // Prefer the M holder: it has the only up-to-date copy.
    const auto supplier = find_supplier(peer_states, requestor_id, {CacheState::M, CacheState::O, CacheState::S});
    if (supplier) {
      decision.data_from_peer = true;
      decision.supplier_id = *supplier;
      if (peer_states[*supplier] == CacheState::M) {
        decision.peer_transitions.push_back({*supplier, CacheState::O});
      }
      // An S supplier keeps its state, so it needs no transition entry.
    }
    // No E state, so even an uncontended read miss lands in S.
    decision.requestor_state = CacheState::S;
    break;
  }

  case BusReqType::ReadX: {
    const auto supplier = find_supplier(peer_states, requestor_id, {CacheState::M, CacheState::O, CacheState::S});
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
