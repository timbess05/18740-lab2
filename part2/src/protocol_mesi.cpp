// MESI -- Modified, Exclusive, Shared, Invalid.        [ YOU IMPLEMENT THIS ]
//
// MESI is MSI plus one state. E is a clean line that no other cache holds. It
// is worth having because of what it lets you skip: a core that already knows
// it holds the only copy can write without telling anyone, where MSI has to
// broadcast an Upgrade to invalidate sharers that do not exist.
//
// WHERE TO LOOK
//   include/protocol.hpp      the contract: ProtocolDecision and what each
//                             field means. Read this first.
//   src/protocol_msi.cpp      the worked example closest to this file. MESI
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
//       Report an access in a state MESI cannot produce.

#include "protocol.hpp"

#include <stdexcept>
#include <string>

namespace {

[[noreturn]] void not_implemented(const char *function) {
  throw std::runtime_error(std::string("not implemented: MESIProtocol::") + function +
                           " -- see src/protocol_mesi.cpp");
}

} // namespace

// Core side. Given an access and the state this cache holds the line in, which
// bus transaction does the access need? Return nullopt for a silent hit: one
// that needs no bus transaction at all.
std::optional<BusReqType> MESIProtocol::request_for(AccessType access, CacheState state) const {
  switch (state) {
  case CacheState::I:
    // TODO:
    break;

  case CacheState::S:
    // TODO:
    break;

  case CacheState::E:
    // TODO:
    break;

  case CacheState::M:
    // TODO:
    break;

  default:
    // O belongs to MOSI and MOESI; MESI can never produce it.
    protocol_detail::reject_state(name(), access, state, "M, E, S and I");
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
ProtocolDecision MESIProtocol::on_request(BusReqType type, uint32_t requestor_id,
                                          const std::vector<CacheState> &peer_states) const {
  // Remove these two lines once you use the parameters.
  (void)requestor_id;
  (void)peer_states;

  switch (type) {
  case BusReqType::Read:
    // TODO: somebody wants a readable copy.
    //   - Can any cache supply it, and if several could, which should?
    //   - What happens to a peer that was the only holder?
    //   - Where does the requestor end up, and does that depend on whether
    //     anyone else held the line? This is where E is reached.
    //   - MESI has no O state. If the supplier's copy was dirty, and it is
    //     about to stop being the only holder, where does that data go?
    break;

  case BusReqType::ReadX:
    // TODO: somebody wants to write a line it does not hold.
    //   - No other cache may keep a copy afterwards.
    //   - Can a peer still supply the data, saving a memory read?
    //   - Same dirty-data question as above.
    break;

  case BusReqType::Upgrade:
    // TODO: the requestor already holds a readable copy and now wants to
    // write it.
    //   - Which peers must lose their copy?
    //   - Does any data move? Does memory need to be touched at all?
    break;

  case BusReqType::Writeback:
    // TODO: the requestor is evicting a dirty line. No peer is involved.
    break;
  }
  not_implemented("on_request");
}
