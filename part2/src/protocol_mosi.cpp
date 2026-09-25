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
    // TODO:
    break;

  case CacheState::S:
    // TODO:
    break;

  case CacheState::O:
    // TODO:
    break;

  case CacheState::M:
    // TODO:
    break;

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
  // Remove these two lines once you use the parameters.
  (void)requestor_id;
  (void)peer_states;

  switch (type) {
  case BusReqType::Read:
    // TODO: somebody wants a readable copy.
    //   - Several caches might be able to supply it. If one of them holds the
    //     only up-to-date copy, it is the one you want.
    //   - This is where O is created. A modified holder is about to stop being
    //     the only holder: MSI would write it back here, and MOSI's whole
    //     point is that it does not have to. What should that holder become,
    //     and what does it stay responsible for?
    //   - Where does the requestor end up? MOSI has no E state.
    break;

  case BusReqType::ReadX:
    // TODO: somebody wants to write a line it does not hold.
    //   - No other cache may keep a copy afterwards, including the owner.
    //   - The requestor is about to become the new holder of this data. If a
    //     peer's copy was dirty, does memory need to hear about it?
    break;

  case BusReqType::Upgrade:
    // TODO: the requestor already holds a readable copy -- possibly as the
    // owner of a dirty line -- and now wants to write it.
    //   - Which peers must lose their copy?
    //   - If one of them was the owner, its dirty value is about to be
    //     overwritten by the requestor. Does memory need to see it first?
    break;

  case BusReqType::Writeback:
    // TODO: the requestor is evicting a dirty line. Note that under MOSI a
    // line can be dirty in two different states. No peer is involved.
    break;
  }
  not_implemented("on_request");
}
