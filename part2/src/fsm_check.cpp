// The three FSMs, and the two ways of checking an implementation against them.
// See fsm_check.hpp for what each is for.

#include "fsm_check.hpp"
#include "invariants.hpp"

#include <algorithm>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {

constexpr size_t kMaxFailures = 25;

std::string describe(const std::vector<CacheState> &peers, uint32_t requestor_id) {
  std::ostringstream out;
  for (uint32_t core = 0; core < peers.size(); ++core) {
    if (core != 0)
      out << " ";
    out << (core == requestor_id ? "[" : "") << to_string(peers[core])
        << (core == requestor_id ? "]" : "");
  }
  return out.str();
}

// Peers that hold the line, ignoring the requestor's own entry.
std::vector<uint32_t> holders_of(const std::vector<CacheState> &peers, uint32_t requestor_id) {
  std::vector<uint32_t> holders;
  for (uint32_t core = 0; core < peers.size(); ++core) {
    if (core != requestor_id && peers[core] != CacheState::I) {
      holders.push_back(core);
    }
  }
  return holders;
}

// -------------------------------------------------------------- the FSMs

// A Writeback is between the evicting cache and memory; no peer observes it,
// so every state a peer can legally be in stays put.
//
// Only two such states exist, and which ones depends on the protocol. The
// evicting cache is a dirty sole owner, so no peer may hold the line at all --
// except that a protocol with O tolerates sharers alongside the owner, so S is
// reachable there. Declaring an arc for every state would inflate the arc
// count with edges no execution can reach, and make the coverage figure
// meaningless.
void add_writeback_arcs(std::vector<PeerArc> &arcs, bool has_owner_state) {
  arcs.push_back({CacheState::I, BusReqType::Writeback, CacheState::I});
  if (has_owner_state) {
    arcs.push_back({CacheState::S, BusReqType::Writeback, CacheState::S});
  }
}

FsmRules build_mesi() {
  FsmRules rules;
  rules.name = "MESI";
  rules.alphabet = {CacheState::I, CacheState::S, CacheState::E, CacheState::M};
  rules.exclusive_on_uncontended_read = true;
  rules.flushes_when_supplying_dirty = true; // no O state to park dirty data in
  rules.peer_arcs = {
      // A cache that does not hold the line is unaffected by anything.
      {CacheState::I, BusReqType::Read, CacheState::I},
      {CacheState::I, BusReqType::ReadX, CacheState::I},
      {CacheState::I, BusReqType::Upgrade, CacheState::I},
      // Sharers survive a read and are invalidated by any write.
      {CacheState::S, BusReqType::Read, CacheState::S},
      {CacheState::S, BusReqType::ReadX, CacheState::I},
      {CacheState::S, BusReqType::Upgrade, CacheState::I},
      // E is clean and sole: it demotes to S for a reader, dies for a writer.
      {CacheState::E, BusReqType::Read, CacheState::S},
      {CacheState::E, BusReqType::ReadX, CacheState::I},
      // M is dirty and sole. Demoting it to S is what forces the flush.
      {CacheState::M, BusReqType::Read, CacheState::S},
      {CacheState::M, BusReqType::ReadX, CacheState::I},
  };
  add_writeback_arcs(rules.peer_arcs, /*has_owner_state=*/false);
  return rules;
}

FsmRules build_mosi() {
  FsmRules rules;
  rules.name = "MOSI";
  rules.alphabet = {CacheState::I, CacheState::S, CacheState::O, CacheState::M};
  rules.exclusive_on_uncontended_read = false; // no E: an uncontended read is S
  rules.flushes_when_supplying_dirty = false;  // O keeps the dirty line instead
  rules.peer_arcs = {
      {CacheState::I, BusReqType::Read, CacheState::I},
      {CacheState::I, BusReqType::ReadX, CacheState::I},
      {CacheState::I, BusReqType::Upgrade, CacheState::I},
      {CacheState::S, BusReqType::Read, CacheState::S},
      {CacheState::S, BusReqType::ReadX, CacheState::I},
      {CacheState::S, BusReqType::Upgrade, CacheState::I},
      // The Owner answers reads itself and stays the Owner.
      {CacheState::O, BusReqType::Read, CacheState::O},
      {CacheState::O, BusReqType::ReadX, CacheState::I},
      {CacheState::O, BusReqType::Upgrade, CacheState::I},
      // M becomes the Owner rather than flushing. This arc is the O state.
      {CacheState::M, BusReqType::Read, CacheState::O},
      {CacheState::M, BusReqType::ReadX, CacheState::I},
  };
  add_writeback_arcs(rules.peer_arcs, /*has_owner_state=*/true);
  return rules;
}

FsmRules build_moesi() {
  FsmRules rules;
  rules.name = "MOESI";
  rules.alphabet = {CacheState::I, CacheState::S, CacheState::E, CacheState::O, CacheState::M};
  rules.exclusive_on_uncontended_read = true; // both optimisations at once
  rules.flushes_when_supplying_dirty = false;
  rules.peer_arcs = {
      {CacheState::I, BusReqType::Read, CacheState::I},
      {CacheState::I, BusReqType::ReadX, CacheState::I},
      {CacheState::I, BusReqType::Upgrade, CacheState::I},
      {CacheState::S, BusReqType::Read, CacheState::S},
      {CacheState::S, BusReqType::ReadX, CacheState::I},
      {CacheState::S, BusReqType::Upgrade, CacheState::I},
      {CacheState::E, BusReqType::Read, CacheState::S},
      {CacheState::E, BusReqType::ReadX, CacheState::I},
      {CacheState::O, BusReqType::Read, CacheState::O},
      {CacheState::O, BusReqType::ReadX, CacheState::I},
      {CacheState::O, BusReqType::Upgrade, CacheState::I},
      {CacheState::M, BusReqType::Read, CacheState::O},
      {CacheState::M, BusReqType::ReadX, CacheState::I},
  };
  add_writeback_arcs(rules.peer_arcs, /*has_owner_state=*/true);
  return rules;
}

const FsmRules &mesi() {
  static const FsmRules rules = build_mesi();
  return rules;
}
const FsmRules &mosi() {
  static const FsmRules rules = build_mosi();
  return rules;
}
const FsmRules &moesi() {
  static const FsmRules rules = build_moesi();
  return rules;
}

// The arc this protocol requires from `from` on observing `observed`, or
// nullopt if the protocol documents no such arc (the state is unreachable for
// that transaction).
std::optional<CacheState> required_arc(const FsmRules &rules, CacheState from,
                                       BusReqType observed) {
  for (const PeerArc &arc : rules.peer_arcs) {
    if (arc.from == from && arc.observed == observed) {
      return arc.to;
    }
  }
  return std::nullopt;
}

} // namespace

bool has_fsm_rules(const std::string &protocol_name) {
  return protocol_name == "MESI" || protocol_name == "MOSI" || protocol_name == "MOESI";
}

const FsmRules &fsm_rules_for(const std::string &protocol_name) {
  if (protocol_name == "MESI")
    return mesi();
  if (protocol_name == "MOSI")
    return mosi();
  if (protocol_name == "MOESI")
    return moesi();
  throw std::runtime_error("no FSM rules for protocol '" + protocol_name +
                           "' (only MESI, MOSI and MOESI are checked)");
}

std::vector<std::string> check_decision(const FsmRules &rules, BusReqType request,
                                        uint32_t requestor_id,
                                        const std::vector<CacheState> &peers_before,
                                        const ProtocolDecision &decision) {
  std::vector<std::string> problems;
  const std::string context =
      std::string(rules.name) + " " + to_string(request) + " by core " +
      std::to_string(requestor_id) + " with peers " + describe(peers_before, requestor_id) + ": ";

  // What the decision says each peer ends in: the transition if one is listed,
  // otherwise its state is unchanged.
  std::vector<CacheState> after = peers_before;
  for (const PeerTransition &change : decision.peer_transitions) {
    if (change.core_id < after.size()) {
      after[change.core_id] = change.new_state;
    }
  }

  // 1. Every peer must follow the arc the FSM documents -- including the peers
  //    that must NOT move, which a transition list can only get wrong by
  //    omission and so is easy to leave untested.
  for (uint32_t core = 0; core < peers_before.size(); ++core) {
    if (core == requestor_id) {
      continue;
    }
    const std::optional<CacheState> expected = required_arc(rules, peers_before[core], request);
    if (!expected) {
      problems.push_back(context + "core " + std::to_string(core) + " holds the line in " +
                         to_string(peers_before[core]) + ", which " + rules.name +
                         " can never observe during this transaction");
      continue;
    }
    if (after[core] != *expected) {
      problems.push_back(context + "core " + std::to_string(core) + " must go " +
                         to_string(peers_before[core]) + " -> " + to_string(*expected) +
                         ", but the decision leaves it in " + to_string(after[core]));
    }
  }

  // 2. The requestor must land where the protocol says.
  const std::vector<uint32_t> holders = holders_of(peers_before, requestor_id);
  std::optional<CacheState> expected_requestor;
  switch (request) {
  case BusReqType::Read:
    expected_requestor = (holders.empty() && rules.exclusive_on_uncontended_read) ? CacheState::E
                                                                                  : CacheState::S;
    break;
  case BusReqType::ReadX:
  case BusReqType::Upgrade:
    expected_requestor = CacheState::M;
    break;
  case BusReqType::Writeback:
    expected_requestor = CacheState::I;
    break;
  }
  if (expected_requestor && decision.requestor_state != *expected_requestor) {
    problems.push_back(context + "the requestor must end in " + to_string(*expected_requestor) +
                       ", not " + to_string(decision.requestor_state));
  }

  // 3. A cache may supply the line exactly when one holds it. Supplying when
  //    nobody does fabricates data; failing to supply when someone does sends
  //    the requestor to memory for a line already on chip.
  const bool moves_line = request == BusReqType::Read || request == BusReqType::ReadX;
  if (moves_line && !holders.empty() && !decision.data_from_peer) {
    problems.push_back(context +
                       "a peer holds the line, so it must be supplied cache-to-cache rather"
                       " than read from memory");
  }
  if (decision.data_from_peer && holders.empty()) {
    problems.push_back(context + "claims a peer supplied the line, but no peer holds it");
  }
  if (!moves_line && decision.data_from_peer) {
    problems.push_back(context + to_string(request) + " moves no data, so it cannot be supplied");
  }

  // 4. Memory is written exactly when the protocol has no way to keep the
  //    dirty line in a cache. This is the whole of the O state.
  const bool dirty_peer =
      std::any_of(holders.begin(), holders.end(),
                  [&](uint32_t core) { return is_dirty(peers_before[core]); });
  bool expected_writeback = false;
  if (request == BusReqType::Writeback) {
    expected_writeback = true;
  } else if (moves_line && dirty_peer) {
    expected_writeback = rules.flushes_when_supplying_dirty;
  }
  if (decision.writeback_needed != expected_writeback) {
    problems.push_back(context + std::string("must") + (expected_writeback ? " " : " not ") +
                       "write the line back to memory here" +
                       (expected_writeback
                            ? "; a dirty line cannot stay dirty in a cache this protocol shares"
                            : "; the dirty line stays in a cache, which is what the O state is for"));
  }

  return problems;
}

ScenarioReport run_fsm_scenarios(const CoherenceProtocol &protocol, uint32_t peers) {
  ScenarioReport report;
  const FsmRules &rules = fsm_rules_for(protocol.name());
  report.arcs_declared = rules.peer_arcs.size();

  // Every (from, observed, to) the battery actually drove a peer through,
  // including the arcs where the peer correctly stays put.
  std::set<std::tuple<int, int, int>> exercised;

  // Every combination of this protocol's states across `peers` caches, plus
  // the requestor. Combinations that violate single-writer/multiple-reader are
  // unreachable and would be unfair to test against.
  const size_t alphabet = rules.alphabet.size();
  size_t combinations = 1;
  for (uint32_t i = 0; i < peers; ++i) {
    combinations *= alphabet;
  }

  const BusReqType requests[] = {BusReqType::Read, BusReqType::ReadX, BusReqType::Upgrade,
                                 BusReqType::Writeback};

  for (size_t encoded = 0; encoded < combinations; ++encoded) {
    // Core 0 is the requestor; cores 1..peers are the peers.
    std::vector<CacheState> states(peers + 1, CacheState::I);
    size_t rest = encoded;
    for (uint32_t core = 1; core <= peers; ++core) {
      states[core] = rules.alphabet[rest % alphabet];
      rest /= alphabet;
    }
    if (check_coherence_invariant(states)) {
      continue; // not a state the machine can reach
    }

    for (BusReqType request : requests) {
      // Only exercise transactions a cache in the requestor's position could
      // actually issue: a Read or ReadX comes from I, an Upgrade from a
      // readable-but-shared state, a Writeback from a dirty one.
      std::vector<CacheState> from_states;
      switch (request) {
      case BusReqType::Read:
      case BusReqType::ReadX:
        from_states = {CacheState::I};
        break;
      case BusReqType::Upgrade:
        from_states = {CacheState::S};
        if (std::count(rules.alphabet.begin(), rules.alphabet.end(), CacheState::O)) {
          from_states.push_back(CacheState::O);
        }
        break;
      case BusReqType::Writeback:
        from_states = {CacheState::M};
        if (std::count(rules.alphabet.begin(), rules.alphabet.end(), CacheState::O)) {
          from_states.push_back(CacheState::O);
        }
        break;
      }

      for (CacheState requestor_state : from_states) {
        std::vector<CacheState> scenario = states;
        scenario[0] = requestor_state;
        if (check_coherence_invariant(scenario)) {
          continue;
        }

        ++report.scenarios;
        ProtocolDecision decision;
        try {
          decision = protocol.on_request(request, /*requestor_id=*/0, scenario);
        } catch (const std::exception &error) {
          if (report.failures.size() < kMaxFailures) {
            report.failures.push_back(std::string(rules.name) + " " + to_string(request) +
                                      " with peers " + describe(scenario, 0) + ": threw: " +
                                      error.what());
          }
          continue;
        }
        // Record the arc each peer took, whether or not it moved.
        for (uint32_t core = 1; core <= peers; ++core) {
          CacheState after = scenario[core];
          for (const PeerTransition &change : decision.peer_transitions) {
            if (change.core_id == core) {
              after = change.new_state;
              break;
            }
          }
          exercised.insert({static_cast<int>(scenario[core]), static_cast<int>(request),
                            static_cast<int>(after)});
        }
        for (std::string &problem : check_decision(rules, request, 0, scenario, decision)) {
          if (report.failures.size() < kMaxFailures) {
            report.failures.push_back(std::move(problem));
          }
        }
      }
    }
  }

  // Only count arcs the protocol declares: a wrong arc is a failure above, not
  // coverage.
  for (const PeerArc &arc : rules.peer_arcs) {
    if (exercised.count({static_cast<int>(arc.from), static_cast<int>(arc.observed),
                         static_cast<int>(arc.to)})) {
      ++report.arcs_exercised;
    }
  }
  return report;
}
