// FSM arc checking for the three protocols students implement.
//
// This file states each protocol's FSM declaratively and
// checks an implementation against it two ways:
//
//   run_fsm_scenarios()   Exhaustive and offline. Enumerates every legal
//                         combination of peer states over a small cluster,
//                         crosses it with every bus transaction, and compares
//                         the whole ProtocolDecision against the FSM. Covers
//                         arcs no trace happens to exercise.
//
//   check_decision()      Live. Called on every real transaction, so an arc is
//                         also checked in the context it actually fires in,
//                         against the states the workload really produced.
//
// Both are gated by --check. The always-on validation in SnoopBus stays where
// it is: that one guards the framework against a decision it cannot apply,
// which is a different question from whether the FSM is right.

#pragma once

#include "cache.hpp"
#include "protocol.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// One legal edge of a peer's state machine: holding a line in `from`, this
// cache observes `observed` on the bus and must move to `to`. An arc whose
// `from` and `to` are equal means the transaction leaves that peer alone.
struct PeerArc {
  CacheState from;
  BusReqType observed;
  CacheState to;
};

// A protocol's FSM, stated as data.
//
// The two booleans are the entire difference between the three protocols, and
// name the optimisation each one has:
//
//   exclusive_on_uncontended_read   a read miss that finds no other holder
//                                   lands in E rather than S, so the write
//                                   that usually follows needs no bus at all.
//                                   MESI and MOESI have it; MOSI does not.
//
//   flushes_when_supplying_dirty    a modified holder handing the line to a
//                                   reader must also write it back, because
//                                   there is nowhere to keep a line dirty and
//                                   shared. MESI must; MOSI and MOESI need
//                                   not, because they have O.
struct FsmRules {
  const char *name;
  std::vector<CacheState> alphabet; // states this protocol can produce
  std::vector<PeerArc> peer_arcs;
  bool exclusive_on_uncontended_read;
  bool flushes_when_supplying_dirty;
};

// The FSM for one of the three implemented protocols. Throws for MI and MSI,
// which ship as worked references and are not checked.
const FsmRules &fsm_rules_for(const std::string &protocol_name);
bool has_fsm_rules(const std::string &protocol_name);

// Checks one decision against the FSM. Returns every problem found, so a single
// wrong arc does not hide the rest. `peers_before` is indexed by core id and
// includes the requestor's own entry, which is read from `requestor_id`.
std::vector<std::string> check_decision(const FsmRules &rules, BusReqType request,
                                        uint32_t requestor_id,
                                        const std::vector<CacheState> &peers_before,
                                        const ProtocolDecision &decision);

struct ScenarioReport {
  size_t scenarios = 0;
  // Distinct FSM arcs the battery actually exercised, against the number the
  // protocol declares. Equal means every documented edge was reached; fewer
  // means some edge is unreachable with this many peers and is going
  // unchecked.
  size_t arcs_exercised = 0;
  size_t arcs_declared = 0;
  std::vector<std::string> failures; // capped; see the implementation

  bool complete() const { return arcs_exercised == arcs_declared; }
};

// Drives `protocol` through every legal peer configuration over `peers` peer
// caches, crossed with every bus transaction it can issue.
ScenarioReport run_fsm_scenarios(const CoherenceProtocol &protocol, uint32_t peers = 3);
