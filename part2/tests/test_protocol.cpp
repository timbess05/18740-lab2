// Protocol state machines, exercised directly: no Cache, no bus, no timing.
// Run from lab3-reduced/ (the Makefile does this).
//
// Convention in these tests: core 0 is the requestor, and its own entry in
// peer_states is deliberately set to a non-I state to confirm the protocol
// ignores it.

#include "protocol.hpp"
#include "test_helpers.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kRequestor = 0;

// peer_states with the requestor's own slot poisoned, so a protocol that fails
// to skip it produces a visibly wrong answer.
std::vector<CacheState> peers(std::initializer_list<CacheState> others) {
  std::vector<CacheState> states{CacheState::M}; // slot 0 = requestor, ignored
  states.insert(states.end(), others);
  return states;
}

// New state of `core_id` in the decision, or nullopt if it was left alone.
std::optional<CacheState> transition_of(const ProtocolDecision &decision, uint32_t core_id) {
  for (const PeerTransition &change : decision.peer_transitions) {
    if (change.core_id == core_id)
      return change.new_state;
  }
  return std::nullopt;
}

bool request_for_throws(const CoherenceProtocol &protocol, AccessType access, CacheState state) {
  try {
    protocol.request_for(access, state);
  } catch (const std::runtime_error &) {
    return true;
  }
  return false;
}

// ---------------------------------------------------------------- MI

void test_mi_reads_need_ownership() {
  const MIProtocol mi;
  // The defining property: a read miss issues ReadX, not Read.
  EXPECT_TRUE(mi.request_for(AccessType::Read, CacheState::I) == BusReqType::ReadX);
  EXPECT_TRUE(mi.request_for(AccessType::Write, CacheState::I) == BusReqType::ReadX);
}

void test_mi_hits_are_silent() {
  const MIProtocol mi;
  // A hit implies M, which is already exclusive, so neither access needs the bus.
  EXPECT_TRUE(!mi.request_for(AccessType::Read, CacheState::M).has_value());
  EXPECT_TRUE(!mi.request_for(AccessType::Write, CacheState::M).has_value());
}

void test_mi_rejects_states_it_cannot_produce() {
  const MIProtocol mi;
  EXPECT_TRUE(request_for_throws(mi, AccessType::Read, CacheState::S));
  EXPECT_TRUE(request_for_throws(mi, AccessType::Read, CacheState::E));
  EXPECT_TRUE(request_for_throws(mi, AccessType::Write, CacheState::O));
}

void test_mi_readx_invalidates_the_owner_and_flushes() {
  const MIProtocol mi;
  const ProtocolDecision decision =
      mi.on_request(BusReqType::ReadX, kRequestor, peers({CacheState::M, CacheState::I}));

  EXPECT_TRUE(decision.requestor_state == CacheState::M);
  EXPECT_EQ(decision.peer_transitions.size(), std::size_t{1});
  EXPECT_TRUE(transition_of(decision, 1) == CacheState::I);
  EXPECT_TRUE(decision.data_from_peer);
  EXPECT_EQ(decision.supplier_id, std::uint32_t{1});
  // No O state, so memory must be updated even though the data went
  // cache-to-cache.
  EXPECT_TRUE(decision.writeback_needed);
}

void test_mi_readx_with_no_holder_reads_memory() {
  const MIProtocol mi;
  const ProtocolDecision decision =
      mi.on_request(BusReqType::ReadX, kRequestor, peers({CacheState::I, CacheState::I}));

  EXPECT_TRUE(decision.requestor_state == CacheState::M);
  EXPECT_TRUE(decision.peer_transitions.empty());
  EXPECT_TRUE(!decision.data_from_peer);
  EXPECT_TRUE(!decision.writeback_needed);
}

void test_mi_never_issues_read_or_upgrade() {
  const MIProtocol mi;
  const std::vector<CacheState> states = peers({CacheState::I});
  bool read_threw = false, upgrade_threw = false;
  try {
    mi.on_request(BusReqType::Read, kRequestor, states);
  } catch (const std::runtime_error &) {
    read_threw = true;
  }
  try {
    mi.on_request(BusReqType::Upgrade, kRequestor, states);
  } catch (const std::runtime_error &) {
    upgrade_threw = true;
  }
  EXPECT_TRUE(read_threw);
  EXPECT_TRUE(upgrade_threw);
}

// ---------------------------------------------------------------- MSI

void test_msi_read_miss_does_not_take_ownership() {
  const MSIProtocol msi;
  // The S state is exactly what MI lacks.
  EXPECT_TRUE(msi.request_for(AccessType::Read, CacheState::I) == BusReqType::Read);
  EXPECT_TRUE(msi.request_for(AccessType::Write, CacheState::I) == BusReqType::ReadX);
}

void test_msi_write_to_shared_needs_upgrade() {
  const MSIProtocol msi;
  EXPECT_TRUE(!msi.request_for(AccessType::Read, CacheState::S).has_value());
  EXPECT_TRUE(msi.request_for(AccessType::Write, CacheState::S) == BusReqType::Upgrade);
  EXPECT_TRUE(!msi.request_for(AccessType::Write, CacheState::M).has_value());
}

void test_msi_rejects_states_it_cannot_produce() {
  const MSIProtocol msi;
  EXPECT_TRUE(request_for_throws(msi, AccessType::Write, CacheState::E));
  EXPECT_TRUE(request_for_throws(msi, AccessType::Read, CacheState::O));
}

void test_msi_read_demotes_modified_peer_and_writes_back() {
  const MSIProtocol msi;
  const ProtocolDecision decision =
      msi.on_request(BusReqType::Read, kRequestor, peers({CacheState::M, CacheState::I}));

  EXPECT_TRUE(decision.requestor_state == CacheState::S);
  EXPECT_TRUE(transition_of(decision, 1) == CacheState::S);
  EXPECT_TRUE(decision.data_from_peer);
  EXPECT_EQ(decision.supplier_id, std::uint32_t{1});
  // The cost the O state removes: dirty data cannot stay dirty in a cache that
  // is no longer the sole owner, so memory is updated here.
  EXPECT_TRUE(decision.writeback_needed);
}

void test_msi_read_from_shared_peer_is_free_of_memory_traffic() {
  const MSIProtocol msi;
  const ProtocolDecision decision =
      msi.on_request(BusReqType::Read, kRequestor, peers({CacheState::S, CacheState::S}));

  EXPECT_TRUE(decision.requestor_state == CacheState::S);
  EXPECT_TRUE(decision.data_from_peer);
  EXPECT_TRUE(!decision.writeback_needed);
  // Sharers keep their state, so nothing is reported.
  EXPECT_TRUE(decision.peer_transitions.empty());
}

void test_msi_uncontended_read_miss_lands_in_shared_not_exclusive() {
  const MSIProtocol msi;
  const ProtocolDecision decision =
      msi.on_request(BusReqType::Read, kRequestor, peers({CacheState::I, CacheState::I}));

  // With no sharers this could safely be E, but MSI has no E. The write that
  // follows will therefore pay for an Upgrade -- the cost MESI removes.
  EXPECT_TRUE(decision.requestor_state == CacheState::S);
  EXPECT_TRUE(!decision.data_from_peer);
  EXPECT_TRUE(!decision.writeback_needed);
}

void test_msi_readx_invalidates_all_sharers() {
  const MSIProtocol msi;
  const ProtocolDecision decision = msi.on_request(
      BusReqType::ReadX, kRequestor, peers({CacheState::S, CacheState::S, CacheState::I}));

  EXPECT_TRUE(decision.requestor_state == CacheState::M);
  EXPECT_EQ(decision.peer_transitions.size(), std::size_t{2});
  EXPECT_TRUE(transition_of(decision, 1) == CacheState::I);
  EXPECT_TRUE(transition_of(decision, 2) == CacheState::I);
  EXPECT_TRUE(!transition_of(decision, 3).has_value());
  // A clean sharer can still supply the line, so no memory read is needed.
  EXPECT_TRUE(decision.data_from_peer);
  EXPECT_TRUE(!decision.writeback_needed);
}

void test_msi_readx_on_modified_peer_writes_back() {
  const MSIProtocol msi;
  const ProtocolDecision decision =
      msi.on_request(BusReqType::ReadX, kRequestor, peers({CacheState::M}));

  EXPECT_TRUE(transition_of(decision, 1) == CacheState::I);
  EXPECT_TRUE(decision.data_from_peer);
  EXPECT_TRUE(decision.writeback_needed);
}

void test_msi_upgrade_moves_no_data() {
  const MSIProtocol msi;
  const ProtocolDecision decision =
      msi.on_request(BusReqType::Upgrade, kRequestor, peers({CacheState::S, CacheState::S}));

  EXPECT_TRUE(decision.requestor_state == CacheState::M);
  EXPECT_EQ(decision.peer_transitions.size(), std::size_t{2});
  // The requestor already has the line, so no supplier and no memory access.
  EXPECT_TRUE(!decision.data_from_peer);
  EXPECT_TRUE(!decision.writeback_needed);
  EXPECT_TRUE(!moves_data(BusReqType::Upgrade));
}

void test_writeback_is_uniform_across_protocols() {
  const MIProtocol mi;
  const MSIProtocol msi;
  for (const CoherenceProtocol *protocol : {static_cast<const CoherenceProtocol *>(&mi),
                                            static_cast<const CoherenceProtocol *>(&msi)}) {
    const ProtocolDecision decision =
        protocol->on_request(BusReqType::Writeback, kRequestor, peers({CacheState::I}));
    EXPECT_TRUE(decision.requestor_state == CacheState::I);
    EXPECT_TRUE(decision.writeback_needed);
    EXPECT_TRUE(decision.peer_transitions.empty());
  }
}

// ------------------------------------------------- MI vs MSI, side by side

void test_read_sharing_is_where_mi_and_msi_diverge() {
  const MIProtocol mi;
  const MSIProtocol msi;

  // Same situation -- a core reading a line another core holds -- and the two
  // protocols answer differently. MI must take the line away; MSI shares it.
  const auto mi_request = mi.request_for(AccessType::Read, CacheState::I);
  const auto msi_request = msi.request_for(AccessType::Read, CacheState::I);
  EXPECT_TRUE(mi_request == BusReqType::ReadX);
  EXPECT_TRUE(msi_request == BusReqType::Read);

  const ProtocolDecision mi_result = mi.on_request(*mi_request, kRequestor, peers({CacheState::M}));
  const ProtocolDecision msi_result =
      msi.on_request(*msi_request, kRequestor, peers({CacheState::M}));

  // MI leaves the previous holder with nothing, so its next read misses again.
  EXPECT_TRUE(transition_of(mi_result, 1) == CacheState::I);
  // MSI leaves it readable, so its next read hits.
  EXPECT_TRUE(transition_of(msi_result, 1) == CacheState::S);
}


// ------------------------------------------------- MESI, MOSI, MOESI

void test_mesi_uncontended_read_takes_the_line_exclusively() {
  const MESIProtocol mesi;
  const ProtocolDecision decision =
      mesi.on_request(BusReqType::Read, kRequestor, peers({CacheState::I, CacheState::I}));
  // E, where MSI would have said S.
  EXPECT_TRUE(decision.requestor_state == CacheState::E);
  EXPECT_TRUE(!decision.data_from_peer);
}

void test_mesi_write_to_exclusive_needs_no_bus() {
  const MESIProtocol mesi;
  // The whole point of E: no Upgrade, no transaction, no invalidations.
  EXPECT_TRUE(!mesi.request_for(AccessType::Write, CacheState::E).has_value());
  // MSI, lacking E, would have been in S here and paid for one.
  EXPECT_TRUE(MSIProtocol().request_for(AccessType::Write, CacheState::S) == BusReqType::Upgrade);
}

void test_mesi_read_with_a_sharer_still_lands_in_shared() {
  const MESIProtocol mesi;
  const ProtocolDecision decision =
      mesi.on_request(BusReqType::Read, kRequestor, peers({CacheState::S}));
  EXPECT_TRUE(decision.requestor_state == CacheState::S);
}

void test_mesi_exclusive_peer_demotes_to_shared_without_writeback() {
  const MESIProtocol mesi;
  const ProtocolDecision decision =
      mesi.on_request(BusReqType::Read, kRequestor, peers({CacheState::E}));
  EXPECT_TRUE(transition_of(decision, 1) == CacheState::S);
  EXPECT_TRUE(decision.data_from_peer);
  EXPECT_TRUE(!decision.writeback_needed); // E is clean
}

void test_mesi_still_flushes_dirty_lines() {
  const MESIProtocol mesi;
  const ProtocolDecision decision =
      mesi.on_request(BusReqType::Read, kRequestor, peers({CacheState::M}));
  // E does nothing for dirty sharing: this is the cost O removes.
  EXPECT_TRUE(transition_of(decision, 1) == CacheState::S);
  EXPECT_TRUE(decision.writeback_needed);
}

void test_mosi_read_makes_the_writer_an_owner_instead_of_flushing() {
  const MOSIProtocol mosi;
  const ProtocolDecision decision =
      mosi.on_request(BusReqType::Read, kRequestor, peers({CacheState::M}));
  // The O-state saving: dirty data stays in cache, memory is untouched.
  EXPECT_TRUE(transition_of(decision, 1) == CacheState::O);
  EXPECT_TRUE(decision.data_from_peer);
  EXPECT_TRUE(!decision.writeback_needed);
}

void test_mosi_owner_supplies_later_readers_and_stays_owner() {
  const MOSIProtocol mosi;
  const ProtocolDecision decision =
      mosi.on_request(BusReqType::Read, kRequestor, peers({CacheState::O, CacheState::S}));
  EXPECT_TRUE(decision.data_from_peer);
  EXPECT_EQ(decision.supplier_id, std::uint32_t{1}); // owner preferred over sharer
  EXPECT_TRUE(decision.peer_transitions.empty());    // owner keeps its state
  EXPECT_TRUE(!decision.writeback_needed);
}

void test_mosi_has_no_exclusive_state() {
  const MOSIProtocol mosi;
  const ProtocolDecision decision =
      mosi.on_request(BusReqType::Read, kRequestor, peers({CacheState::I}));
  // No E, so an uncontended read miss still lands in S and the write that
  // follows still pays for an Upgrade.
  EXPECT_TRUE(decision.requestor_state == CacheState::S);
  EXPECT_TRUE(mosi.request_for(AccessType::Write, CacheState::S) == BusReqType::Upgrade);
  EXPECT_TRUE(request_for_throws(mosi, AccessType::Read, CacheState::E));
}

void test_mosi_write_to_owned_line_needs_an_upgrade() {
  const MOSIProtocol mosi;
  // O is readable and dirty, but others may hold S copies.
  EXPECT_TRUE(!mosi.request_for(AccessType::Read, CacheState::O).has_value());
  EXPECT_TRUE(mosi.request_for(AccessType::Write, CacheState::O) == BusReqType::Upgrade);
}

void test_mosi_readx_moves_dirty_data_without_memory() {
  const MOSIProtocol mosi;
  const ProtocolDecision decision =
      mosi.on_request(BusReqType::ReadX, kRequestor, peers({CacheState::O, CacheState::S}));
  EXPECT_TRUE(decision.requestor_state == CacheState::M);
  EXPECT_EQ(decision.peer_transitions.size(), std::size_t{2});
  EXPECT_TRUE(decision.data_from_peer);
  EXPECT_TRUE(!decision.writeback_needed);
}

void test_moesi_has_both_savings() {
  const MOESIProtocol moesi;
  // E: uncontended read miss is exclusive, and the write that follows is free.
  const ProtocolDecision uncontended =
      moesi.on_request(BusReqType::Read, kRequestor, peers({CacheState::I}));
  EXPECT_TRUE(uncontended.requestor_state == CacheState::E);
  EXPECT_TRUE(!moesi.request_for(AccessType::Write, CacheState::E).has_value());

  // O: an M holder becomes the owner rather than flushing.
  const ProtocolDecision dirty =
      moesi.on_request(BusReqType::Read, kRequestor, peers({CacheState::M}));
  EXPECT_TRUE(transition_of(dirty, 1) == CacheState::O);
  EXPECT_TRUE(!dirty.writeback_needed);
}

void test_moesi_never_writes_back_on_a_transfer() {
  const MOESIProtocol moesi;
  for (CacheState holder : {CacheState::M, CacheState::O, CacheState::E, CacheState::S}) {
    const ProtocolDecision decision =
        moesi.on_request(BusReqType::ReadX, kRequestor, peers({holder}));
    EXPECT_TRUE(decision.data_from_peer);
    EXPECT_TRUE(!decision.writeback_needed);
    EXPECT_TRUE(transition_of(decision, 1) == CacheState::I);
  }
}

void test_moesi_supplier_priority_prefers_the_owner() {
  const MOESIProtocol moesi;
  const ProtocolDecision decision = moesi.on_request(
      BusReqType::Read, kRequestor, peers({CacheState::S, CacheState::O, CacheState::S}));
  EXPECT_EQ(decision.supplier_id, std::uint32_t{2}); // the O peer, not core 1
}

// The two savings, isolated: which protocols have E, and which have O.
void test_protocol_matrix() {
  const MIProtocol mi;
  const MSIProtocol msi;
  const MESIProtocol mesi;
  const MOSIProtocol mosi;
  const MOESIProtocol moesi;

  // E: does an uncontended read miss land in a state a write can leave silently?
  const auto has_e = [](const CoherenceProtocol &p) {
    const CacheState after =
        p.on_request(p.request_for(AccessType::Read, CacheState::I) == BusReqType::ReadX
                         ? BusReqType::ReadX
                         : BusReqType::Read,
                     kRequestor, peers({CacheState::I}))
            .requestor_state;
    return !p.request_for(AccessType::Write, after).has_value();
  };
  EXPECT_TRUE(has_e(mi)); // trivially: MI's read miss already takes M
  EXPECT_TRUE(!has_e(msi));
  EXPECT_TRUE(has_e(mesi));
  EXPECT_TRUE(!has_e(mosi));
  EXPECT_TRUE(has_e(moesi));

  // O: can a modified peer hand a line to a reader without writing to memory?
  const auto has_o = [](const CoherenceProtocol &p) {
    const BusReqType request = *p.request_for(AccessType::Read, CacheState::I);
    return !p.on_request(request, kRequestor, peers({CacheState::M})).writeback_needed;
  };
  EXPECT_TRUE(!has_o(mi));
  EXPECT_TRUE(!has_o(msi));
  EXPECT_TRUE(!has_o(mesi));
  EXPECT_TRUE(has_o(mosi));
  EXPECT_TRUE(has_o(moesi));
}

void test_names() {
  EXPECT_EQ(std::string(MIProtocol().name()), std::string("MI"));
  EXPECT_EQ(std::string(MSIProtocol().name()), std::string("MSI"));
  EXPECT_EQ(std::string(MESIProtocol().name()), std::string("MESI"));
  EXPECT_EQ(std::string(MOSIProtocol().name()), std::string("MOSI"));
  EXPECT_EQ(std::string(MOESIProtocol().name()), std::string("MOESI"));
  EXPECT_EQ(std::string(to_string(BusReqType::ReadX)), std::string("ReadX"));
}

} // namespace

int main() {
  test_mi_reads_need_ownership();
  test_mi_hits_are_silent();
  test_mi_rejects_states_it_cannot_produce();
  test_mi_readx_invalidates_the_owner_and_flushes();
  test_mi_readx_with_no_holder_reads_memory();
  test_mi_never_issues_read_or_upgrade();

  test_msi_read_miss_does_not_take_ownership();
  test_msi_write_to_shared_needs_upgrade();
  test_msi_rejects_states_it_cannot_produce();
  test_msi_read_demotes_modified_peer_and_writes_back();
  test_msi_read_from_shared_peer_is_free_of_memory_traffic();
  test_msi_uncontended_read_miss_lands_in_shared_not_exclusive();
  test_msi_readx_invalidates_all_sharers();
  test_msi_readx_on_modified_peer_writes_back();
  test_msi_upgrade_moves_no_data();
  test_writeback_is_uniform_across_protocols();

  test_read_sharing_is_where_mi_and_msi_diverge();
  test_mesi_uncontended_read_takes_the_line_exclusively();
  test_mesi_write_to_exclusive_needs_no_bus();
  test_mesi_read_with_a_sharer_still_lands_in_shared();
  test_mesi_exclusive_peer_demotes_to_shared_without_writeback();
  test_mesi_still_flushes_dirty_lines();
  test_mosi_read_makes_the_writer_an_owner_instead_of_flushing();
  test_mosi_owner_supplies_later_readers_and_stays_owner();
  test_mosi_has_no_exclusive_state();
  test_mosi_write_to_owned_line_needs_an_upgrade();
  test_mosi_readx_moves_dirty_data_without_memory();
  test_moesi_has_both_savings();
  test_moesi_never_writes_back_on_a_transfer();
  test_moesi_supplier_priority_prefers_the_owner();
  test_protocol_matrix();

  test_names();
  TEST_REPORT_AND_EXIT();
}
