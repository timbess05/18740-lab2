// Coherence invariant checking. Run from lab3-reduced/.

#include "invariants.hpp"
#include "simulator.hpp"
#include "test_helpers.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

bool legal(std::vector<CacheState> states) {
  return !check_coherence_invariant(states).has_value();
}

std::string why(std::vector<CacheState> states) {
  const auto problem = check_coherence_invariant(states);
  return problem ? *problem : std::string();
}

void test_legal_combinations() {
  EXPECT_TRUE(legal({CacheState::I, CacheState::I}));                // nobody holds it
  EXPECT_TRUE(legal({CacheState::M, CacheState::I}));                // one writer
  EXPECT_TRUE(legal({CacheState::E, CacheState::I}));                // one exclusive reader
  EXPECT_TRUE(legal({CacheState::S, CacheState::S, CacheState::S})); // many readers
  // The combination O exists for: one dirty owner alongside clean sharers.
  EXPECT_TRUE(legal({CacheState::O, CacheState::S, CacheState::S}));
  EXPECT_TRUE(legal({CacheState::O, CacheState::I}));
}

void test_two_owners_is_illegal() {
  // The classic bug: a transfer that forgets to invalidate the previous owner.
  EXPECT_TRUE(!legal({CacheState::M, CacheState::M}));
  EXPECT_TRUE(!legal({CacheState::M, CacheState::O}));
  EXPECT_TRUE(!legal({CacheState::O, CacheState::O}));
  EXPECT_TRUE(!legal({CacheState::E, CacheState::E}));
  EXPECT_TRUE(!legal({CacheState::M, CacheState::E}));
}

void test_exclusive_holder_cannot_coexist_with_sharers() {
  // The other classic bug: promoting to M without invalidating the sharers, so
  // a write would go unseen.
  EXPECT_TRUE(!legal({CacheState::M, CacheState::S}));
  EXPECT_TRUE(!legal({CacheState::E, CacheState::S}));
  EXPECT_TRUE(!legal({CacheState::S, CacheState::S, CacheState::M}));
}

void test_messages_name_the_offending_cores() {
  const std::string two_owners = why({CacheState::M, CacheState::I, CacheState::M});
  EXPECT_TRUE(two_owners.find("core 0 (M)") != std::string::npos);
  EXPECT_TRUE(two_owners.find("core 2 (M)") != std::string::npos);
  EXPECT_TRUE(two_owners.find("own the line") != std::string::npos);

  const std::string exclusive_with_sharer = why({CacheState::M, CacheState::S});
  EXPECT_TRUE(exclusive_with_sharer.find("core 1 (S)") != std::string::npos);
  EXPECT_TRUE(exclusive_with_sharer.find("sharers") != std::string::npos);
}

void test_violation_description_is_locatable() {
  const InvariantViolation violation{42, 3, 0x1000, "two or more caches own the line"};
  const std::string text = violation.describe();
  EXPECT_TRUE(text.find("access 42") != std::string::npos);
  EXPECT_TRUE(text.find("core 3") != std::string::npos);
  EXPECT_TRUE(text.find("0x1000") != std::string::npos);
}

void test_every_protocol_stays_coherent() {
  const char *traces[] = {
      "tests/traces/sim_read_share.trace",
      "tests/traces/sim_private_rw.trace",
      "tests/traces/sim_dirty_share.trace",
      "tests/traces/sim_two_core_race.trace",
  };
  for (ProtocolKind kind : {ProtocolKind::MI, ProtocolKind::MSI, ProtocolKind::MESI,
                            ProtocolKind::MOSI, ProtocolKind::MOESI}) {
    for (const char *trace : traces) {
      SimConfig config;
      config.protocol = kind;
      config.check_invariants = true;
      Simulator sim(config, Trace(trace));
      sim.run();
      EXPECT_TRUE(sim.violations().empty());
    }
  }
}

// A student's protocol is untrusted input: these are the mistakes that used to
// crash or, worse, produce plausible wrong numbers. Each must now name itself.
namespace hostile {

struct Base : CoherenceProtocol {
  std::optional<BusReqType> request_for(AccessType access, CacheState state) const override {
    if (state == CacheState::I)
      return access == AccessType::Read ? BusReqType::Read : BusReqType::ReadX;
    return std::nullopt;
  }
  ProtocolDecision on_request(BusReqType, uint32_t,
                              const std::vector<CacheState> &) const override {
    return {};
  }
  const char *name() const override { return "hostile"; }
};

struct NoRequestOnMiss : Base {
  std::optional<BusReqType> request_for(AccessType, CacheState) const override {
    return std::nullopt;
  }
};

struct OutOfRangePeer : Base {
  ProtocolDecision on_request(BusReqType, uint32_t,
                              const std::vector<CacheState> &peers) const override {
    ProtocolDecision decision;
    decision.peer_transitions.push_back({static_cast<uint32_t>(peers.size()), CacheState::I});
    return decision;
  }
};

struct TransitionsRequestor : Base {
  ProtocolDecision on_request(BusReqType, uint32_t self,
                              const std::vector<CacheState> &) const override {
    ProtocolDecision decision;
    decision.peer_transitions.push_back({self, CacheState::I});
    return decision;
  }
};

// Legal states, destroyed data: invalidates the modified holder without writing
// it back and without handing the line to anyone.
struct LosesDirtyData : Base {
  ProtocolDecision on_request(BusReqType type, uint32_t self,
                              const std::vector<CacheState> &peers) const override {
    ProtocolDecision decision;
    for (uint32_t id = 0; id < peers.size(); ++id) {
      if (id != self && peers[id] != CacheState::I) {
        decision.peer_transitions.push_back({id, CacheState::I});
      }
    }
    decision.requestor_state = type == BusReqType::Read ? CacheState::S : CacheState::M;
    return decision;
  }
};

struct PhantomSupplier : Base {
  ProtocolDecision on_request(BusReqType, uint32_t,
                              const std::vector<CacheState> &) const override {
    ProtocolDecision decision;
    decision.data_from_peer = true;
    decision.supplier_id = 1; // core 1 holds nothing
    return decision;
  }
};

// Runs `accesses` under `protocol`; true if the framework rejected any of them.
// A second access from another core is what exposes peer-handling bugs.
bool rejected(const CoherenceProtocol &protocol, bool two_cores = false) {
  try {
    MemoryConfig memory_config;
    MainMemory memory(memory_config);
    BusConfig bus_config;
    SnoopBus bus(bus_config, protocol, memory, 64);
    Core core0(0, 1024, 2, 64, 1, protocol, bus);
    Core core1(1, 1024, 2, 64, 1, protocol, bus);
    bus.register_core(core0);
    bus.register_core(core1);

    MemOp op;
    op.core_id = 0;
    op.addr = 0x1000;
    if (two_cores) {
      op.op = MemOp::Op::Write; // leaves core 0 holding the line dirty
    }
    core0.access(op, 0);

    if (two_cores) {
      MemOp peer_read;
      peer_read.core_id = 1;
      peer_read.addr = 0x1000;
      core1.access(peer_read, 10);
    }
    return false;
  } catch (const std::runtime_error &) {
    return true;
  }
}

} // namespace hostile

void test_buggy_protocols_are_rejected_not_tolerated() {
  // Undefined behaviour previously: the empty optional was dereferenced.
  EXPECT_TRUE(hostile::rejected(hostile::NoRequestOnMiss{}));
  // Heap buffer overflow previously.
  EXPECT_TRUE(hostile::rejected(hostile::OutOfRangePeer{}));
  // Silently corrupted the requestor's own line previously.
  EXPECT_TRUE(hostile::rejected(hostile::TransitionsRequestor{}));
  // Silently skipped the memory read previously, so timing was wrong but
  // nothing looked amiss.
  EXPECT_TRUE(hostile::rejected(hostile::PhantomSupplier{}));
}

void test_silently_losing_dirty_data_is_rejected() {
  // The states this protocol produces are legal under single-writer/
  // multiple-reader, so the invariant checker alone reports nothing wrong --
  // yet the only up-to-date copy of a written line has been destroyed.
  const std::vector<CacheState> after_the_fact = {CacheState::I, CacheState::S};
  EXPECT_TRUE(!check_coherence_invariant(after_the_fact).has_value());
  EXPECT_TRUE(hostile::rejected(hostile::LosesDirtyData{}, /*two_cores=*/true));
}

void test_shipped_protocols_pass_the_same_validation() {
  for (ProtocolKind kind : {ProtocolKind::MI, ProtocolKind::MSI, ProtocolKind::MESI,
                            ProtocolKind::MOSI, ProtocolKind::MOESI}) {
    const std::unique_ptr<CoherenceProtocol> protocol = make_protocol(kind);
    EXPECT_TRUE(!hostile::rejected(*protocol));
    EXPECT_TRUE(!hostile::rejected(*protocol, /*two_cores=*/true));
  }
}

void test_core_count_is_bounded() {
  SimConfig config;
  config.num_cores = kMaxCores + 1;
  bool threw = false;
  try {
    Simulator sim(config, Trace("tests/traces/sim_read_share.trace"));
  } catch (const std::runtime_error &) {
    threw = true;
  }
  EXPECT_TRUE(threw);
}

void test_checking_is_off_by_default() {
  SimConfig config;
  config.protocol = ProtocolKind::MOESI;
  Simulator sim(config, Trace("tests/traces/sim_dirty_share.trace"));
  sim.run();
  EXPECT_TRUE(sim.violations().empty()); // not run, so nothing collected
  EXPECT_TRUE(!config.check_invariants);
}

} // namespace

int main() {
  test_legal_combinations();
  test_two_owners_is_illegal();
  test_exclusive_holder_cannot_coexist_with_sharers();
  test_messages_name_the_offending_cores();
  test_violation_description_is_locatable();
  test_every_protocol_stays_coherent();
  test_buggy_protocols_are_rejected_not_tolerated();
  test_silently_losing_dirty_data_is_rejected();
  test_shipped_protocols_pass_the_same_validation();
  test_core_count_is_bounded();
  test_checking_is_off_by_default();
  TEST_REPORT_AND_EXIT();
}
