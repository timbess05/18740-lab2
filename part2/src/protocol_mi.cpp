// MI: Modified and Invalid only.
//
// A line can only be held exclusively, so there is no way to read without
// taking ownership. Every miss is a ReadX that invalidates every other copy,
// and two cores reading the same line trade it back and forth indefinitely.
//
//   request_for   read/write miss  -> ReadX        (reads need ownership)
//                 read/write hit   -> nullopt      (hit implies M)
//
//   on_request    ReadX   every holder -> I; an M holder flushes (supplies the
//                         line and writes it back); requestor -> M
//                 Writeback  requestor -> I, memory owes a write

#include "protocol.hpp"

std::optional<BusReqType> MIProtocol::request_for(AccessType access, CacheState state) const {
  switch (state) {
  case CacheState::I:
    // The defining property of MI: even a read must take the line exclusively.
    return BusReqType::ReadX;
  case CacheState::M:
    // Sole owner already, so reads and writes are both silent hits.
    return std::nullopt;
  default:
    protocol_detail::reject_state(name(), access, state, "M and I");
  }
}

ProtocolDecision MIProtocol::on_request(BusReqType type, uint32_t requestor_id,
                                        const std::vector<CacheState> &peer_states) const {
  ProtocolDecision decision;

  switch (type) {
  case BusReqType::ReadX: {
    for (uint32_t holder : find_holders(peer_states, requestor_id)) {
      decision.peer_transitions.push_back({holder, CacheState::I});
    }
    // At most one peer can hold the line, and only ever in M.
    if (const auto owner = find_supplier(peer_states, requestor_id, {CacheState::M})) {
      decision.data_from_peer = true;
      decision.supplier_id = *owner;
      // No O state to park the dirty line in, so memory must be updated even
      // though the requestor is taking the data directly.
      decision.writeback_needed = true;
    }
    decision.requestor_state = CacheState::M;
    break;
  }

  case BusReqType::Writeback:
    decision.requestor_state = CacheState::I;
    decision.writeback_needed = true;
    break;

  case BusReqType::Read:
    protocol_detail::reject_request(name(), type, "reads take ownership via ReadX");
  case BusReqType::Upgrade:
    protocol_detail::reject_request(name(), type, "a writable line is already M");
  }

  return decision;
}
