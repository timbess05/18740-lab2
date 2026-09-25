// The FSM checker itself. Run from lab3-reduced/.
//
// Two things to establish: that the battery is exhaustive over each protocol's
// declared arcs, and that each class of FSM error is actually detected. A
// checker that passes everything is worse than none, so most of this file is
// deliberately-wrong decisions that must be rejected.

#include "fsm_check.hpp"
#include "simulator.hpp"
#include "test_helpers.hpp"

#include <memory>
#include <string>

namespace {

const ProtocolKind kImplemented[] = {ProtocolKind::MESI, ProtocolKind::MOSI, ProtocolKind::MOESI};

void test_only_the_implemented_protocols_have_rules() {
  // MI and MSI ship as worked references, so there is nothing to check.
  EXPECT_TRUE(has_fsm_rules("MESI"));
  EXPECT_TRUE(has_fsm_rules("MOSI"));
  EXPECT_TRUE(has_fsm_rules("MOESI"));
  EXPECT_TRUE(!has_fsm_rules("MI"));
  EXPECT_TRUE(!has_fsm_rules("MSI"));
}

void test_the_two_flags_are_what_distinguishes_the_protocols() {
  // Every difference between the three FSMs reduces to these two booleans.
  EXPECT_TRUE(fsm_rules_for("MESI").exclusive_on_uncontended_read);
  EXPECT_TRUE(fsm_rules_for("MESI").flushes_when_supplying_dirty);

  EXPECT_TRUE(!fsm_rules_for("MOSI").exclusive_on_uncontended_read);
  EXPECT_TRUE(!fsm_rules_for("MOSI").flushes_when_supplying_dirty);

  EXPECT_TRUE(fsm_rules_for("MOESI").exclusive_on_uncontended_read);
  EXPECT_TRUE(!fsm_rules_for("MOESI").flushes_when_supplying_dirty);
}

void test_alphabets_exclude_states_the_protocol_cannot_produce() {
  const auto has = [](const FsmRules &rules, CacheState state) {
    for (CacheState allowed : rules.alphabet) {
      if (allowed == state)
        return true;
    }
    return false;
  };
  EXPECT_TRUE(!has(fsm_rules_for("MESI"), CacheState::O));
  EXPECT_TRUE(has(fsm_rules_for("MESI"), CacheState::E));
  EXPECT_TRUE(!has(fsm_rules_for("MOSI"), CacheState::E));
  EXPECT_TRUE(has(fsm_rules_for("MOSI"), CacheState::O));
  EXPECT_TRUE(has(fsm_rules_for("MOESI"), CacheState::E));
  EXPECT_TRUE(has(fsm_rules_for("MOESI"), CacheState::O));
}

void test_the_battery_passes_the_shipped_protocols() {
  for (ProtocolKind kind : kImplemented) {
    const std::unique_ptr<CoherenceProtocol> protocol = make_protocol(kind);
    const ScenarioReport report = run_fsm_scenarios(*protocol);
    if (!report.failures.empty()) {
      std::fprintf(stderr, "  %s: %s\n", protocol->name(), report.failures.front().c_str());
    }
    EXPECT_TRUE(report.failures.empty());
    EXPECT_TRUE(report.scenarios > 0);
  }
}

void test_the_battery_reaches_every_declared_arc() {
  // The coverage claim. If an arc is declared but never exercised it is going
  // unchecked, and either the battery or the arc table is wrong.
  for (ProtocolKind kind : kImplemented) {
    const std::unique_ptr<CoherenceProtocol> protocol = make_protocol(kind);
    const ScenarioReport report = run_fsm_scenarios(*protocol);
    if (!report.complete()) {
      std::fprintf(stderr, "  %s: only %zu of %zu arcs exercised\n", protocol->name(),
                   report.arcs_exercised, report.arcs_declared);
    }
    EXPECT_TRUE(report.complete());
    EXPECT_EQ(report.arcs_exercised, report.arcs_declared);
  }
}

// --- each class of FSM error, injected directly into a decision ---

// Peers: core 0 is the requestor (I), core 1 holds the line in `peer`.
std::vector<CacheState> peers_with(CacheState peer) {
  return {CacheState::I, peer, CacheState::I};
}

bool rejects(const FsmRules &rules, BusReqType request, const std::vector<CacheState> &peers,
             const ProtocolDecision &decision) {
  return !check_decision(rules, request, /*requestor_id=*/0, peers, decision).empty();
}

void test_a_correct_decision_is_accepted() {
  // MOESI, read with a modified peer: peer becomes Owner, supplies, no writeback.
  ProtocolDecision decision;
  decision.requestor_state = CacheState::S;
  decision.peer_transitions.push_back({1, CacheState::O});
  decision.data_from_peer = true;
  decision.supplier_id = 1;
  EXPECT_TRUE(!rejects(fsm_rules_for("MOESI"), BusReqType::Read, peers_with(CacheState::M),
                       decision));
}

void test_an_arc_landing_in_the_wrong_state_is_rejected() {
  // MOESI must move an M peer to O on a read, not to S.
  ProtocolDecision decision;
  decision.requestor_state = CacheState::S;
  decision.peer_transitions.push_back({1, CacheState::S});
  decision.data_from_peer = true;
  decision.supplier_id = 1;
  decision.writeback_needed = true;
  EXPECT_TRUE(rejects(fsm_rules_for("MOESI"), BusReqType::Read, peers_with(CacheState::M),
                      decision));
}

void test_a_peer_that_fails_to_move_is_rejected() {
  // A ReadX must invalidate every holder; leaving one alone is the classic bug.
  ProtocolDecision decision;
  decision.requestor_state = CacheState::M;
  decision.data_from_peer = true;
  decision.supplier_id = 1;
  // No peer_transitions at all: core 1 stays in S.
  EXPECT_TRUE(rejects(fsm_rules_for("MESI"), BusReqType::ReadX, peers_with(CacheState::S),
                      decision));
}

void test_a_peer_that_moves_when_it_should_not_is_rejected() {
  // A plain read must leave sharers alone.
  ProtocolDecision decision;
  decision.requestor_state = CacheState::S;
  decision.peer_transitions.push_back({1, CacheState::I});
  decision.data_from_peer = true;
  decision.supplier_id = 1;
  EXPECT_TRUE(rejects(fsm_rules_for("MESI"), BusReqType::Read, peers_with(CacheState::S),
                      decision));
}

void test_the_wrong_requestor_state_is_rejected() {
  // MESI must take an uncontended read exclusively.
  ProtocolDecision shared;
  shared.requestor_state = CacheState::S;
  EXPECT_TRUE(rejects(fsm_rules_for("MESI"), BusReqType::Read, peers_with(CacheState::I), shared));

  // MOSI must not: it has no E state.
  ProtocolDecision exclusive;
  exclusive.requestor_state = CacheState::E;
  EXPECT_TRUE(rejects(fsm_rules_for("MOSI"), BusReqType::Read, peers_with(CacheState::I),
                      exclusive));
}

void test_a_missing_writeback_is_rejected() {
  // MESI has no O state, so an M peer supplying a reader must also flush.
  ProtocolDecision decision;
  decision.requestor_state = CacheState::S;
  decision.peer_transitions.push_back({1, CacheState::S});
  decision.data_from_peer = true;
  decision.supplier_id = 1;
  // writeback_needed left false
  EXPECT_TRUE(rejects(fsm_rules_for("MESI"), BusReqType::Read, peers_with(CacheState::M),
                      decision));
}

void test_a_needless_writeback_is_rejected() {
  // MOSI keeps the dirty line in the Owner, so writing back here wastes the
  // bandwidth the O state exists to save.
  ProtocolDecision decision;
  decision.requestor_state = CacheState::S;
  decision.peer_transitions.push_back({1, CacheState::O});
  decision.data_from_peer = true;
  decision.supplier_id = 1;
  decision.writeback_needed = true;
  EXPECT_TRUE(rejects(fsm_rules_for("MOSI"), BusReqType::Read, peers_with(CacheState::M),
                      decision));
}

void test_failing_to_supply_from_a_holder_is_rejected() {
  // Going to memory for a line another cache already holds.
  ProtocolDecision decision;
  decision.requestor_state = CacheState::S;
  // data_from_peer left false even though core 1 holds the line
  EXPECT_TRUE(rejects(fsm_rules_for("MOESI"), BusReqType::Read, peers_with(CacheState::S),
                      decision));
}

void test_supplying_when_nobody_holds_the_line_is_rejected() {
  ProtocolDecision decision;
  decision.requestor_state = CacheState::E;
  decision.data_from_peer = true;
  decision.supplier_id = 1;
  EXPECT_TRUE(rejects(fsm_rules_for("MOESI"), BusReqType::Read, peers_with(CacheState::I),
                      decision));
}

void test_an_upgrade_that_moves_data_is_rejected() {
  // An Upgrade is address-phase only; the requestor already has the line.
  ProtocolDecision decision;
  decision.requestor_state = CacheState::M;
  decision.peer_transitions.push_back({1, CacheState::I});
  decision.data_from_peer = true;
  decision.supplier_id = 1;
  std::vector<CacheState> peers = {CacheState::S, CacheState::S, CacheState::I};
  EXPECT_TRUE(rejects(fsm_rules_for("MESI"), BusReqType::Upgrade, peers, decision));
}

void test_a_state_the_protocol_cannot_observe_is_rejected() {
  // An O peer under MESI: the protocol has no arc for it, and saying so is
  // more useful than silently accepting whatever it does.
  ProtocolDecision decision;
  decision.requestor_state = CacheState::S;
  decision.data_from_peer = true;
  decision.supplier_id = 1;
  EXPECT_TRUE(rejects(fsm_rules_for("MESI"), BusReqType::Read, peers_with(CacheState::O),
                      decision));
}

void test_failures_name_the_arc_and_the_configuration() {
  ProtocolDecision decision;
  decision.requestor_state = CacheState::S;
  decision.peer_transitions.push_back({1, CacheState::S});
  decision.data_from_peer = true;
  decision.supplier_id = 1;
  decision.writeback_needed = true;
  const std::vector<std::string> problems = check_decision(
      fsm_rules_for("MOESI"), BusReqType::Read, 0, peers_with(CacheState::M), decision);
  EXPECT_TRUE(!problems.empty());
  const std::string &first = problems.front();
  EXPECT_TRUE(first.find("MOESI") != std::string::npos);
  EXPECT_TRUE(first.find("M -> O") != std::string::npos);
  EXPECT_TRUE(first.find("core 1") != std::string::npos);
}

// --- the gating ---

void test_checking_is_off_unless_requested() {
  SimConfig config;
  config.protocol = ProtocolKind::MOESI;
  Simulator sim(config, Trace("tests/traces/sim_dirty_share.trace"));
  sim.run();
  EXPECT_EQ(sim.fsm_report().scenarios, std::size_t{0});
  EXPECT_TRUE(sim.fsm_failures().empty());
}

void test_checking_runs_the_battery_and_the_live_check() {
  SimConfig config;
  config.protocol = ProtocolKind::MOESI;
  config.check_invariants = true;
  Simulator sim(config, Trace("tests/traces/sim_dirty_share.trace"));
  sim.run();
  EXPECT_TRUE(sim.fsm_report().scenarios > 0);
  EXPECT_TRUE(sim.fsm_report().complete());
  EXPECT_TRUE(sim.fsm_report().failures.empty());
  EXPECT_TRUE(sim.fsm_failures().empty());
}

void test_reference_protocols_are_not_checked() {
  // MI and MSI have no declared FSM, so enabling checking must not fabricate
  // failures for them.
  for (ProtocolKind kind : {ProtocolKind::MI, ProtocolKind::MSI}) {
    SimConfig config;
    config.protocol = kind;
    config.check_invariants = true;
    Simulator sim(config, Trace("tests/traces/sim_dirty_share.trace"));
    sim.run();
    EXPECT_EQ(sim.fsm_report().scenarios, std::size_t{0});
    EXPECT_TRUE(sim.fsm_failures().empty());
  }
}

void test_every_implemented_protocol_is_clean_on_every_trace() {
  const char *traces[] = {"traces/read_share.trace", "traces/private_rw.trace",
                          "traces/dirty_share.trace", "traces/migratory.trace",
                          "traces/mixed.trace"};
  for (ProtocolKind kind : kImplemented) {
    for (const char *trace : traces) {
      SimConfig config;
      config.protocol = kind;
      config.check_invariants = true;
      Simulator sim(config, Trace(trace));
      sim.run();
      EXPECT_TRUE(sim.fsm_failures().empty());
      EXPECT_TRUE(sim.violations().empty());
    }
  }
}

} // namespace

int main() {
  RUN_TEST(test_only_the_implemented_protocols_have_rules);
  RUN_TEST(test_the_two_flags_are_what_distinguishes_the_protocols);
  RUN_TEST(test_alphabets_exclude_states_the_protocol_cannot_produce);
  RUN_TEST(test_the_battery_passes_the_shipped_protocols);
  RUN_TEST(test_the_battery_reaches_every_declared_arc);

  RUN_TEST(test_a_correct_decision_is_accepted);
  RUN_TEST(test_an_arc_landing_in_the_wrong_state_is_rejected);
  RUN_TEST(test_a_peer_that_fails_to_move_is_rejected);
  RUN_TEST(test_a_peer_that_moves_when_it_should_not_is_rejected);
  RUN_TEST(test_the_wrong_requestor_state_is_rejected);
  RUN_TEST(test_a_missing_writeback_is_rejected);
  RUN_TEST(test_a_needless_writeback_is_rejected);
  RUN_TEST(test_failing_to_supply_from_a_holder_is_rejected);
  RUN_TEST(test_supplying_when_nobody_holds_the_line_is_rejected);
  RUN_TEST(test_an_upgrade_that_moves_data_is_rejected);
  RUN_TEST(test_a_state_the_protocol_cannot_observe_is_rejected);
  RUN_TEST(test_failures_name_the_arc_and_the_configuration);

  RUN_TEST(test_checking_is_off_unless_requested);
  RUN_TEST(test_checking_runs_the_battery_and_the_live_check);
  RUN_TEST(test_reference_protocols_are_not_checked);
  RUN_TEST(test_every_implemented_protocol_is_clean_on_every_trace);
  TEST_REPORT_AND_EXIT();
}
