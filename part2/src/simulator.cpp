// System construction and the ready-time scheduling loop.
// See simulator.hpp for why there is no event queue.

#include "simulator.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

const char *to_string(ProtocolKind kind) {
  switch (kind) {
  case ProtocolKind::MI:
    return "MI";
  case ProtocolKind::MSI:
    return "MSI";
  case ProtocolKind::MESI:
    return "MESI";
  case ProtocolKind::MOSI:
    return "MOSI";
  case ProtocolKind::MOESI:
    return "MOESI";
  }
  return "?";
}

ProtocolKind parse_protocol(const std::string &name) {
  if (name == "MI")
    return ProtocolKind::MI;
  if (name == "MSI")
    return ProtocolKind::MSI;
  if (name == "MESI")
    return ProtocolKind::MESI;
  if (name == "MOSI")
    return ProtocolKind::MOSI;
  if (name == "MOESI")
    return ProtocolKind::MOESI;
  throw std::runtime_error("unknown protocol '" + name + "' (expected MI, MSI, MESI, MOSI, MOESI)");
}

std::unique_ptr<CoherenceProtocol> make_protocol(ProtocolKind kind) {
  switch (kind) {
  case ProtocolKind::MI:
    return std::make_unique<MIProtocol>();
  case ProtocolKind::MSI:
    return std::make_unique<MSIProtocol>();
  case ProtocolKind::MESI:
    return std::make_unique<MESIProtocol>();
  case ProtocolKind::MOSI:
    return std::make_unique<MOSIProtocol>();
  case ProtocolKind::MOESI:
    return std::make_unique<MOESIProtocol>();
  }
  throw std::runtime_error("unhandled protocol kind");
}

Simulator::Simulator(const SimConfig &config, Trace trace)
    : config_(config), trace_(std::move(trace)) {
  const size_t cores_in_trace = trace_.num_cores();
  size_t core_count = config_.num_cores;
  if (core_count == 0) {
    core_count = cores_in_trace;
  } else if (core_count < cores_in_trace) {
    throw std::runtime_error("config models " + std::to_string(core_count) + " cores but the " +
                             "trace uses " + std::to_string(cores_in_trace));
  }
  if (core_count == 0) {
    throw std::runtime_error("trace contains no memory accesses");
  }
  // The trace parser bounds core ids, but --cores is independent of the trace
  // and a large value silently allocates one full cache per core.
  if (core_count > kMaxCores) {
    throw std::runtime_error("config models " + std::to_string(core_count) +
                             " cores, above the maximum of " + std::to_string(kMaxCores));
  }

  // Cores beyond those the trace names stay idle, but still snoop the bus.
  trace_.per_core.resize(core_count);

  protocol_ = make_protocol(config_.protocol);
  memory_ = std::make_unique<MainMemory>(config_.memory);

  BusConfig bus_config = config_.bus;
  bus_config.check_fsm = config_.check_invariants;
  bus_ = std::make_unique<SnoopBus>(bus_config, *protocol_, *memory_, config_.block_bytes);

  // The offline battery covers arcs no trace may reach, so run it before the
  // trace rather than relying on the workload to exercise the state machine.
  if (config_.check_invariants && has_fsm_rules(protocol_->name())) {
    fsm_report_ = run_fsm_scenarios(*protocol_);
  }

  cores_.reserve(core_count);
  for (uint32_t id = 0; id < core_count; ++id) {
    cores_.push_back(std::make_unique<Core>(id, config_.cache_bytes, config_.cache_assoc,
                                            config_.block_bytes, config_.cache_hit_cycles,
                                            *protocol_, *bus_));
    bus_->register_core(*cores_.back());
  }

  schedule_.assign(core_count, CoreSchedule{});
}

std::optional<uint32_t> Simulator::next_core() const {
  std::optional<uint32_t> chosen;
  uint64_t earliest = 0;
  const uint32_t count = static_cast<uint32_t>(schedule_.size());

  for (uint32_t offset = 0; offset < count; ++offset) {
    // Start scanning just past whoever went last, so that a strict less-than
    // resolves ties to the next core in rotation rather than to core 0.
    const uint32_t id = (last_served_ + 1 + offset) % count;
    const std::vector<MemOp> &ops = trace_.per_core[id];
    if (schedule_[id].cursor >= ops.size()) {
      continue; // drained
    }
    const uint64_t ready = schedule_[id].ready_cycle + ops[schedule_[id].cursor].delta;
    if (!chosen || ready < earliest) {
      earliest = ready;
      chosen = id;
    }
  }
  return chosen;
}

void Simulator::run() {
  while (const std::optional<uint32_t> id = next_core()) {
    CoreSchedule &state = schedule_[*id];
    const MemOp &op = trace_.per_core[*id][state.cursor];
    ++state.cursor;

    const uint64_t issued_at = state.ready_cycle + op.delta;
    state.ready_cycle = issued_at + cores_[*id]->access(op, issued_at);
    last_served_ = *id;

    ++access_count_;
    if (config_.check_invariants) {
      verify_line(op.addr & ~(config_.block_bytes - 1), *id);
    }
  }
}

// Caps the collected violations: once a protocol is broken it tends to stay
// broken, and the first few are the ones worth reading.
void Simulator::verify_line(uint64_t addr, uint32_t core_id) {
  constexpr size_t kMaxViolations = 20;
  if (violations_.size() >= kMaxViolations) {
    return;
  }

  std::vector<CacheState> states(cores_.size(), CacheState::I);
  for (size_t id = 0; id < cores_.size(); ++id) {
    states[id] = cores_[id]->snoop(addr);
  }

  if (const auto problem = check_coherence_invariant(states)) {
    violations_.push_back({access_count_, core_id, addr, *problem});
  }
}

uint64_t Simulator::makespan() const {
  uint64_t last = 0;
  for (const CoreSchedule &state : schedule_) {
    last = std::max(last, state.ready_cycle);
  }
  return last;
}
