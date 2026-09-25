// MSI: Modified, Shared, Invalid.
//
// S lets several cores hold a readable copy at once, which removes MI's
// read-sharing ping-pong. Writing to a shared line then costs an Upgrade to
// invalidate the other sharers.
//
//   request_for   read miss        -> Read
//                 read hit (S/M)   -> nullopt
//                 write miss       -> ReadX
//                 write hit in S   -> Upgrade      (MESI avoids this via E)
//                 write hit in M   -> nullopt
//
//   on_request    Read     M holder -> S and flushes (supplies + writes back);
//                          S holder supplies, stays S; requestor -> S
//                 ReadX    every holder -> I; an M holder flushes; requestor -> M
//                 Upgrade  every holder -> I; no data, no memory; requestor -> M
//                 Writeback  requestor -> I, memory owes a write
//
// Two costs MSI cannot avoid, and which the richer protocols exist to remove:
//   - A read miss with no sharers lands in S, not E, so the write that follows
//     pays for an Upgrade. MESI's E state removes it.
//   - An M holder supplying a line must also write it back, because there is
//     no O state to keep dirty data in a cache that is no longer the sole
//     owner. MOSI's O state removes that write.

#include "protocol.hpp"

std::optional<BusReqType> MSIProtocol::request_for(AccessType access, CacheState state) const {
  switch (state) {
  case CacheState::I:
    return access == AccessType::Read ? BusReqType::Read : BusReqType::ReadX;
  case CacheState::S:
    // Readable but not writable: a write must invalidate the other sharers.
    return access == AccessType::Read ? std::optional<BusReqType>{}
                                      : std::optional<BusReqType>{BusReqType::Upgrade};
  case CacheState::M:
    return std::nullopt;
  default:
    protocol_detail::reject_state(name(), access, state, "M, S and I");
  }
}

ProtocolDecision MSIProtocol::on_request(BusReqType type, uint32_t requestor_id,
                                         const std::vector<CacheState> &peer_states) const {
  ProtocolDecision decision;

  switch (type) {
  case BusReqType::Read: {
    // Prefer the M holder: it has the only up-to-date copy.
    const auto supplier = find_supplier(peer_states, requestor_id, {CacheState::M, CacheState::S});
    if (supplier) {
      decision.data_from_peer = true;
      decision.supplier_id = *supplier;
      if (peer_states[*supplier] == CacheState::M) {
        // Flush: the line goes to the requestor and to memory in the same
        // transaction, because MSI has nowhere to keep it dirty and shared.
        decision.peer_transitions.push_back({*supplier, CacheState::S});
        decision.writeback_needed = true;
      }
      // An S supplier keeps its state, so it needs no transition entry.
    }
    // No E state, so even an uncontended read miss lands in S.
    decision.requestor_state = CacheState::S;
    break;
  }

  case BusReqType::ReadX: {
    const auto supplier = find_supplier(peer_states, requestor_id, {CacheState::M, CacheState::S});
    if (supplier) {
      decision.data_from_peer = true;
      decision.supplier_id = *supplier;
      if (peer_states[*supplier] == CacheState::M) {
        decision.writeback_needed = true;
      }
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

  case BusReqType::Writeback:
    decision.requestor_state = CacheState::I;
    decision.writeback_needed = true;
    break;
  }

  return decision;
}
