// Report collection and formatting. See report.hpp for what the ratios mean.

#include "report.hpp"

#include <algorithm>
#include <iomanip>
#include <ios>

namespace {

double ratio(uint64_t numerator, uint64_t denominator) {
  return denominator == 0 ? 0.0 : static_cast<double>(numerator) / static_cast<double>(denominator);
}

void write_percent(std::ostream &out, double fraction) {
  const std::ios_base::fmtflags saved = out.flags();
  const std::streamsize precision = out.precision();
  out << std::fixed << std::setprecision(1) << (fraction * 100.0) << " %";
  out.flags(saved);
  out.precision(precision);
}

// "  label                 value"
void write_field(std::ostream &out, const char *label, uint64_t value) {
  out << "  " << std::left << std::setw(22) << label << std::right << std::setw(10) << value
      << "\n";
}

void write_field(std::ostream &out, const char *label, const std::string &value) {
  out << "  " << std::left << std::setw(22) << label << value << "\n";
}

void write_json_field(std::ostream &out, const char *key, uint64_t value, bool last = false) {
  out << "    \"" << key << "\": " << value << (last ? "\n" : ",\n");
}

void write_core_json(std::ostream &out, size_t index, const CoreStats &stats, bool last) {
  out << "    {\n";
  out << "      \"core\": " << index << ",\n";
  out << "      \"reads\": " << stats.reads << ",\n";
  out << "      \"writes\": " << stats.writes << ",\n";
  out << "      \"hits\": " << stats.hits << ",\n";
  out << "      \"misses\": " << stats.misses << ",\n";
  out << "      \"silent_hits\": " << stats.silent_hits << ",\n";
  out << "      \"upgrade_hits\": " << stats.upgrade_hits << ",\n";
  out << "      \"evictions\": " << stats.evictions << ",\n";
  out << "      \"dirty_evictions\": " << stats.dirty_evictions << ",\n";
  out << "      \"stall_cycles\": " << stats.stall_cycles << "\n";
  out << "    }" << (last ? "\n" : ",\n");
}

} // namespace

double Report::hit_rate() const { return ratio(totals.hits, accesses()); }

double Report::bus_utilisation() const { return ratio(bus.busy_cycles, makespan); }

double Report::memory_utilisation() const {
  return ratio(memory.service_cycles, std::max(makespan, memory_drained_at) * memory_banks);
}

double Report::stall_per_access() const { return ratio(totals.stall_cycles, accesses()); }

Report Report::collect(const Simulator &sim, const SimConfig &config,
                       const std::string &trace_path) {
  Report report;
  report.protocol = to_string(config.protocol);
  report.trace_path = trace_path;
  report.num_cores = sim.num_cores();
  report.cache_bytes = config.cache_bytes;
  report.cache_assoc = config.cache_assoc;
  report.block_bytes = config.block_bytes;
  report.ideal_bus = config.bus.infinite_bandwidth;
  report.ideal_memory = config.memory.infinite_bandwidth;
  report.bus_addr_cycles = config.bus.addr_cycles;
  report.bus_width_bytes = config.bus.width_bytes;
  report.memory_banks = config.memory.num_banks;

  report.makespan = sim.makespan();
  report.memory_drained_at = sim.memory().busy_until();
  report.bus = sim.bus().stats();
  report.memory = sim.memory().stats();

  report.cores.reserve(sim.num_cores());
  for (size_t index = 0; index < sim.num_cores(); ++index) {
    const CoreStats &stats = sim.core(index).stats();
    report.cores.push_back(stats);

    report.totals.reads += stats.reads;
    report.totals.writes += stats.writes;
    report.totals.hits += stats.hits;
    report.totals.misses += stats.misses;
    report.totals.silent_hits += stats.silent_hits;
    report.totals.upgrade_hits += stats.upgrade_hits;
    report.totals.evictions += stats.evictions;
    report.totals.dirty_evictions += stats.dirty_evictions;
    report.totals.stall_cycles += stats.stall_cycles;
  }
  return report;
}

void Report::write_text(std::ostream &out) const {
  out << "Configuration\n";
  write_field(out, "protocol", protocol);
  write_field(out, "trace", trace_path);
  write_field(out, "cores", static_cast<uint64_t>(num_cores));
  write_field(out, "cache", std::to_string(cache_bytes / 1024) + " KiB, " +
                              std::to_string(cache_assoc) + "-way, " +
                              std::to_string(block_bytes) + " B lines");
  write_field(out, "bus",
            ideal_bus ? std::string("ideal (zero cost)")
                      : std::to_string(bus_addr_cycles) + " addr cycles, " +
                            std::to_string(bus_width_bytes) + " B/cycle");
  write_field(out, "memory",
            ideal_memory ? std::string("ideal (zero cost)")
                         : std::to_string(memory_banks) + " banks");

  out << "\nRuntime\n";
  write_field(out, "makespan", makespan);
  write_field(out, "total stall", totals.stall_cycles);
  out << "  " << std::left << std::setw(22) << "stall per access" << std::right << std::setw(10)
      << std::fixed << std::setprecision(1) << stall_per_access() << "\n";
  out << "  " << std::left << std::setw(22) << "bus utilisation" << std::right << std::setw(10);
  write_percent(out, bus_utilisation());
  out << "\n  " << std::left << std::setw(22) << "memory utilisation" << std::right
      << std::setw(10);
  write_percent(out, memory_utilisation());
  out << "\n";

  out << "\nAccesses\n";
  write_field(out, "total", accesses());
  write_field(out, "reads", totals.reads);
  write_field(out, "writes", totals.writes);
  write_field(out, "hits", totals.hits);
  out << "  " << std::left << std::setw(22) << "hit rate" << std::right << std::setw(10);
  write_percent(out, hit_rate());
  out << "\n";
  write_field(out, "  silent hits", totals.silent_hits);
  write_field(out, "  upgrade hits", totals.upgrade_hits);
  write_field(out, "misses", totals.misses);
  write_field(out, "evictions", totals.evictions);
  write_field(out, "  dirty evictions", totals.dirty_evictions);

  out << "\nBus\n";
  write_field(out, "transactions", bus.transactions);
  write_field(out, "  Read", bus.reads);
  write_field(out, "  ReadX", bus.reads_exclusive);
  write_field(out, "  Upgrade", bus.upgrades);
  write_field(out, "  Writeback", bus.writebacks);
  write_field(out, "invalidations", bus.invalidations);
  write_field(out, "peer-supplied", bus.peer_supplied);
  write_field(out, "busy cycles", bus.busy_cycles);
  write_field(out, "arbitration cycles", bus.arbitration_cycles);

  out << "\nMemory\n";
  write_field(out, "accesses", memory.reads + memory.writes);
  write_field(out, "  reads", memory.reads);
  write_field(out, "  writes", memory.writes);
  write_field(out, "row hits", memory.row_hits);
  write_field(out, "row misses", memory.row_misses);
  write_field(out, "bank conflicts", memory.bank_conflicts);
  write_field(out, "queue cycles", memory.queue_cycles);
  write_field(out, "service cycles", memory.service_cycles);
  write_field(out, "drain tail", memory_drain_tail());

  out << "\nPer-core\n";
  out << "  " << std::right << std::setw(4) << "core" << std::setw(9) << "reads" << std::setw(9)
      << "writes" << std::setw(9) << "hits" << std::setw(9) << "misses" << std::setw(9)
      << "upgrades" << std::setw(11) << "stall" << "\n";
  for (size_t index = 0; index < cores.size(); ++index) {
    const CoreStats &stats = cores[index];
    out << "  " << std::right << std::setw(4) << index << std::setw(9) << stats.reads
        << std::setw(9) << stats.writes << std::setw(9) << stats.hits << std::setw(9)
        << stats.misses << std::setw(9) << stats.upgrade_hits << std::setw(11)
        << stats.stall_cycles << "\n";
  }
}

namespace {
// Kept next to write_row so the header and the values cannot drift apart.
constexpr int kRowWidths[] = {8, 11, 9, 9, 9, 9, 9, 9, 9};
const char *const kRowLabels[] = {"protocol", "makespan", "bus_util", "mem_util", "txns",
                                  "upgrades", "inval",    "mem_rd",   "mem_wr"};
} // namespace

void Report::write_row_header(std::ostream &out) {
  out << std::left << std::setw(kRowWidths[0]) << kRowLabels[0];
  for (size_t i = 1; i < sizeof(kRowWidths) / sizeof(kRowWidths[0]); ++i) {
    out << std::right << std::setw(kRowWidths[i]) << kRowLabels[i];
  }
  out << "\n";
}

void Report::write_row(std::ostream &out) const {
  const std::ios_base::fmtflags saved = out.flags();
  out << std::left << std::setw(kRowWidths[0]) << protocol << std::right;
  out << std::setw(kRowWidths[1]) << makespan;
  out << std::fixed << std::setprecision(1);
  out << std::setw(kRowWidths[2] - 1) << bus_utilisation() * 100.0 << "%";
  out << std::setw(kRowWidths[3] - 1) << memory_utilisation() * 100.0 << "%";
  out.flags(saved);
  out << std::right;
  out << std::setw(kRowWidths[4]) << bus.transactions;
  out << std::setw(kRowWidths[5]) << bus.upgrades;
  out << std::setw(kRowWidths[6]) << bus.invalidations;
  out << std::setw(kRowWidths[7]) << memory.reads;
  out << std::setw(kRowWidths[8]) << memory.writes << "\n";
  out.flags(saved);
}

void Report::write_json(std::ostream &out) const {
  const std::ios_base::fmtflags saved = out.flags();

  out << "{\n";
  out << "  \"config\": {\n";
  out << "    \"protocol\": \"" << protocol << "\",\n";
  out << "    \"trace\": \"" << trace_path << "\",\n";
  out << "    \"cores\": " << num_cores << ",\n";
  out << "    \"cache_bytes\": " << cache_bytes << ",\n";
  out << "    \"cache_assoc\": " << cache_assoc << ",\n";
  out << "    \"block_bytes\": " << block_bytes << ",\n";
  out << "    \"ideal_bus\": " << (ideal_bus ? "true" : "false") << ",\n";
  out << "    \"ideal_memory\": " << (ideal_memory ? "true" : "false") << ",\n";
  out << "    \"bus_addr_cycles\": " << bus_addr_cycles << ",\n";
  out << "    \"bus_width_bytes\": " << bus_width_bytes << ",\n";
  out << "    \"memory_banks\": " << memory_banks << "\n";
  out << "  },\n";

  out << std::fixed << std::setprecision(6);
  out << "  \"runtime\": {\n";
  out << "    \"makespan\": " << makespan << ",\n";
  out << "    \"stall_cycles\": " << totals.stall_cycles << ",\n";
  out << "    \"stall_per_access\": " << stall_per_access() << ",\n";
  out << "    \"bus_utilisation\": " << bus_utilisation() << ",\n";
  out << "    \"memory_utilisation\": " << memory_utilisation() << ",\n";
  out << "    \"hit_rate\": " << hit_rate() << "\n";
  out << "  },\n";
  out.flags(saved);

  out << "  \"accesses\": {\n";
  write_json_field(out, "total", accesses());
  write_json_field(out, "reads", totals.reads);
  write_json_field(out, "writes", totals.writes);
  write_json_field(out, "hits", totals.hits);
  write_json_field(out, "misses", totals.misses);
  write_json_field(out, "silent_hits", totals.silent_hits);
  write_json_field(out, "upgrade_hits", totals.upgrade_hits);
  write_json_field(out, "evictions", totals.evictions);
  write_json_field(out, "dirty_evictions", totals.dirty_evictions, true);
  out << "  },\n";

  out << "  \"bus\": {\n";
  write_json_field(out, "transactions", bus.transactions);
  write_json_field(out, "reads", bus.reads);
  write_json_field(out, "reads_exclusive", bus.reads_exclusive);
  write_json_field(out, "upgrades", bus.upgrades);
  write_json_field(out, "writebacks", bus.writebacks);
  write_json_field(out, "invalidations", bus.invalidations);
  write_json_field(out, "peer_supplied", bus.peer_supplied);
  write_json_field(out, "busy_cycles", bus.busy_cycles);
  write_json_field(out, "arbitration_cycles", bus.arbitration_cycles, true);
  out << "  },\n";

  out << "  \"memory\": {\n";
  write_json_field(out, "reads", memory.reads);
  write_json_field(out, "writes", memory.writes);
  write_json_field(out, "row_hits", memory.row_hits);
  write_json_field(out, "row_misses", memory.row_misses);
  write_json_field(out, "bank_conflicts", memory.bank_conflicts);
  write_json_field(out, "queue_cycles", memory.queue_cycles);
  write_json_field(out, "service_cycles", memory.service_cycles);
  write_json_field(out, "drained_at", memory_drained_at);
  write_json_field(out, "drain_tail", memory_drain_tail(), true);
  out << "  },\n";

  out << "  \"cores\": [\n";
  for (size_t index = 0; index < cores.size(); ++index) {
    write_core_json(out, index, cores[index], index + 1 == cores.size());
  }
  out << "  ]\n";
  out << "}\n";
}
