// Coherence protocols.
//
// A protocol is a pure decision function with no storage of its own. It never
// touches a Cache; it reports what should happen and Core/SnoopBus apply it.
// All five protocols therefore share identical cache storage, so a protocol
// comparison differs only in the state machine.
//
// Each protocol answers two questions:
//
//   request_for()  Core side. Given an access and the line's current state,
//                  which bus transaction (if any) does it need? This is where
//                  the E-state optimization lives: MESI returns nullopt for a
//                  write hit in E, MSI has no E state to return it for.
//
//   on_request()   Bus side. Given a transaction and every peer's state, what
//                  does the requestor end up in, what do the peers move to,
//                  who supplies the data, and is a writeback owed to memory?
//
// This splits the original lab's four gem5 callbacks in two: the bus-grant and
// memory-response halves were plumbing, and the framework handles them now.

#pragma once

#include "cache.hpp"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <vector>

enum class BusReqType : uint8_t {
  Read,     // read miss: want a readable copy
  ReadX,    // write miss: want an exclusive copy ("read for ownership")
  Upgrade,  // already readable, want to write; no data needed
  Writeback // evicting a dirty line
};

const char *to_string(BusReqType type);

// True if the transaction moves a cache line over the bus. Upgrade does not --
// that is exactly why the E state is worth having.
constexpr bool moves_data(BusReqType type) { return type != BusReqType::Upgrade; }

struct PeerTransition {
  uint32_t core_id = 0;
  CacheState new_state = CacheState::I;
};

struct ProtocolDecision {
  // State the requestor's line ends up in.
  CacheState requestor_state = CacheState::I;

  // Peers whose state changes. A transition to I is an invalidation; the bus
  // counts those separately. Peers that keep their state are omitted.
  std::vector<PeerTransition> peer_transitions;

  // A peer supplied the line, so no memory read is needed.
  bool data_from_peer = false;
  uint32_t supplier_id = 0;

  // Memory owes a write. Independent of data_from_peer: a protocol without the
  // O state sets both on a "flush", where the owner hands the line to the
  // requestor and updates memory in the same transaction. Avoiding this write
  // is the entire benefit of the O state.
  bool writeback_needed = false;
};

class CoherenceProtocol {
public:
  virtual ~CoherenceProtocol() = default;

  // Bus transaction needed for `access` on a line currently in `state`, or
  // nullopt for a silent hit that needs no bus at all.
  //
  // Never returns Writeback -- evictions are driven by the cache, not by an
  // access. Throws std::runtime_error if `state` is one this protocol cannot
  // produce (e.g. E under MSI), which indicates a bug rather than a workload.
  virtual std::optional<BusReqType> request_for(AccessType access, CacheState state) const = 0;

  // Outcome of `type` issued by `requestor_id`. `peer_states` is indexed by
  // core id and covers every core; the requestor's own entry must be ignored.
  // Cores that do not hold the line appear as I.
  virtual ProtocolDecision on_request(BusReqType type, uint32_t requestor_id,
                                      const std::vector<CacheState> &peer_states) const = 0;

  virtual const char *name() const = 0;
};

// First peer holding the line in one of `priority` states, tried in order.
// Shared by every protocol's supplier selection.
std::optional<uint32_t> find_supplier(const std::vector<CacheState> &peer_states,
                                      uint32_t requestor_id,
                                      std::initializer_list<CacheState> priority);

// Every peer other than the requestor that holds the line in any valid state.
std::vector<uint32_t> find_holders(const std::vector<CacheState> &peer_states,
                                   uint32_t requestor_id);

namespace protocol_detail {
// Reports an access in a state the protocol cannot produce. `states` names the
// states it does have, for the error message.
[[noreturn]] void reject_state(const char *protocol, AccessType access, CacheState state,
                               const char *states);

// Reports a bus transaction this protocol never issues. `reason` explains why.
[[noreturn]] void reject_request(const char *protocol, BusReqType type, const char *reason);
} // namespace protocol_detail

// MI: only M and I. There is no way to hold a line for reading without holding
// it exclusively, so every miss -- read or write -- is a ReadX that invalidates
// every other copy. Concurrent readers therefore ping-pong the line
// continuously, which is the cost the S state exists to remove.
class MIProtocol : public CoherenceProtocol {
public:
  std::optional<BusReqType> request_for(AccessType access, CacheState state) const override;
  ProtocolDecision on_request(BusReqType type, uint32_t requestor_id,
                              const std::vector<CacheState> &peer_states) const override;
  const char *name() const override { return "MI"; }
};

// MSI: adds S, so multiple cores can hold a readable copy at once. A write to a
// line held in S needs an Upgrade to invalidate the other sharers.
//
// Because there is no E state, a read miss with no sharers still lands in S,
// and the following write must pay for an Upgrade that MESI would not need.
// Because there is no O state, an M peer supplying a line must also write it
// back to memory, which MOSI would not need.
class MSIProtocol : public CoherenceProtocol {
public:
  std::optional<BusReqType> request_for(AccessType access, CacheState state) const override;
  ProtocolDecision on_request(BusReqType type, uint32_t requestor_id,
                              const std::vector<CacheState> &peer_states) const override;
  const char *name() const override { return "MSI"; }
};

// MESI: MSI plus E, a clean line known to be the only copy.
//
// A read miss that finds no other holder lands in E rather than S, so the write
// that usually follows needs no bus transaction at all -- the line moves E->M
// silently. That removes one Upgrade per private read-then-write sequence,
// which is an address-phase bus slot and nothing else. E therefore pays off
// exactly when bus transactions are the scarce resource.
//
// E does not help with dirty sharing: like MSI, an M holder supplying a line
// must also write it back, because there is still no O state.
class MESIProtocol : public CoherenceProtocol {
public:
  std::optional<BusReqType> request_for(AccessType access, CacheState state) const override;
  ProtocolDecision on_request(BusReqType type, uint32_t requestor_id,
                              const std::vector<CacheState> &peer_states) const override;
  const char *name() const override { return "MESI"; }
};

// MOSI: MSI plus O, a dirty line that other caches may also hold as S.
//
// When an M holder supplies a line to a reader it moves to O and keeps the
// dirty data, instead of flushing to memory. Memory is written only when the
// owner finally evicts. That removes one memory write per M-to-shared
// transition, so O pays off exactly when memory bandwidth is the scarce
// resource -- a different resource from the one E saves.
//
// Without E, an uncontended read miss still lands in S, so MOSI keeps paying
// MSI's Upgrade on private read-then-write.
class MOSIProtocol : public CoherenceProtocol {
public:
  std::optional<BusReqType> request_for(AccessType access, CacheState state) const override;
  ProtocolDecision on_request(BusReqType type, uint32_t requestor_id,
                              const std::vector<CacheState> &peer_states) const override;
  const char *name() const override { return "MOSI"; }
};

// MOESI: both optimisations at once. E removes the Upgrade on private
// read-then-write; O removes the writeback on dirty sharing. Because the two
// target different resources, MOESI only beats both MESI and MOSI on a workload
// that exercises both patterns and constrains both resources.
class MOESIProtocol : public CoherenceProtocol {
public:
  std::optional<BusReqType> request_for(AccessType access, CacheState state) const override;
  ProtocolDecision on_request(BusReqType type, uint32_t requestor_id,
                              const std::vector<CacheState> &peer_states) const override;
  const char *name() const override { return "MOESI"; }
};
