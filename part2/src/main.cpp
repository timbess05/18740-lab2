// Command-line driver: parse options, run one trace, print the report.
//
// One run per invocation. A protocol sweep is a shell loop:
//
//   for p in MI MSI MESI MOSI MOESI; do
//     ./bin/sim --trace t.trace --protocol $p --json out/$p.json --quiet
//   done

#include "report.hpp"
#include "simulator.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <string>

namespace {

const char *kUsage = R"(usage: sim --trace FILE [options]

  --trace FILE            trace to run (required)
  --protocol NAME         MI | MSI | MESI | MOSI | MOESI     (default MSI)
  --cores N               model N cores                      (default: from trace)

Cache geometry
  --cache-bytes N                                            (default 32768)
  --cache-assoc N                                            (default 4)
  --block-bytes N                                            (default 64)

Interconnect
  --bus-addr-cycles N     arbitration + command phase        (default 4)
  --bus-width-bytes N     data bus width                     (default 16)

Memory
  --mem-banks N           bank-level parallelism             (default 8)
  --mem-row-bytes N                                          (default 8192)
  --mem-write-recover N   extra cycles a write holds its bank (default 18).
                          Writes are the only memory traffic the O state
                          removes, so this scales O's benefit directly.

Performance model
  --perfect               zero-cost bus and memory
  --perfect-bus           zero-cost bus only
  --perfect-memory        zero-cost memory only

Correctness
  --check                 check the protocol's state machine. Runs an
                          exhaustive battery of fabricated peer configurations
                          before the trace, checks every real transaction
                          against the same FSM rules, and verifies
                          single-writer/multiple-reader after every access.
                          Exits 2 on any failure. Covers MESI, MOSI and MOESI;
                          MI and MSI ship as references and are not checked.

Output
  --json FILE             also write a JSON report
  --row                   print one fixed-width line instead of the full report
  --row-header            print just the column header for --row, then exit
  --quiet                 suppress the text report
)";

// Reads the value following `--flag`, advancing the index past it.
const char *take_value(int &index, int argc, char **argv) {
  if (index + 1 >= argc) {
    throw std::runtime_error(std::string("missing value after ") + argv[index]);
  }
  return argv[++index];
}

uint64_t take_uint(int &index, int argc, char **argv) {
  const char *text = take_value(index, argc, argv);
  const std::string flag = argv[index - 1];
  // std::stoull accepts a leading '-' and wraps it, so "-3" would arrive as a
  // huge value and be reported as one. Reject the sign explicitly.
  if (text[0] == '-' || text[0] == '+') {
    throw std::runtime_error(flag + " expects a non-negative integer, got '" + std::string(text) +
                             "'");
  }
  try {
    size_t consumed = 0;
    const uint64_t value = std::stoull(text, &consumed, 10);
    if (consumed != std::string(text).size()) {
      throw std::invalid_argument("trailing characters");
    }
    return value;
  } catch (const std::exception &) {
    throw std::runtime_error(flag + " expects a non-negative integer, got '" + text + "'");
  }
}

struct Options {
  SimConfig config;
  std::string trace_path;
  std::string json_path;
  bool quiet = false;
  bool row = false;
};

Options parse_args(int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string flag = argv[index];

    if (flag == "--help" || flag == "-h") {
      std::fputs(kUsage, stdout);
      std::exit(0);
    } else if (flag == "--trace") {
      options.trace_path = take_value(index, argc, argv);
    } else if (flag == "--protocol") {
      options.config.protocol = parse_protocol(take_value(index, argc, argv));
    } else if (flag == "--cores") {
      options.config.num_cores = static_cast<uint32_t>(take_uint(index, argc, argv));
    } else if (flag == "--cache-bytes") {
      options.config.cache_bytes = static_cast<size_t>(take_uint(index, argc, argv));
    } else if (flag == "--cache-assoc") {
      options.config.cache_assoc = static_cast<size_t>(take_uint(index, argc, argv));
    } else if (flag == "--block-bytes") {
      options.config.block_bytes = static_cast<size_t>(take_uint(index, argc, argv));
    } else if (flag == "--bus-addr-cycles") {
      options.config.bus.addr_cycles = static_cast<uint32_t>(take_uint(index, argc, argv));
    } else if (flag == "--bus-width-bytes") {
      options.config.bus.width_bytes = static_cast<uint32_t>(take_uint(index, argc, argv));
    } else if (flag == "--mem-banks") {
      options.config.memory.num_banks = static_cast<uint32_t>(take_uint(index, argc, argv));
    } else if (flag == "--mem-row-bytes") {
      options.config.memory.row_bytes = static_cast<uint32_t>(take_uint(index, argc, argv));
    } else if (flag == "--mem-write-recover") {
      options.config.memory.write_recover = static_cast<uint32_t>(take_uint(index, argc, argv));
    } else if (flag == "--perfect") {
      options.config.set_perfect(true);
    } else if (flag == "--perfect-bus") {
      options.config.bus.infinite_bandwidth = true;
    } else if (flag == "--perfect-memory") {
      options.config.memory.infinite_bandwidth = true;
    } else if (flag == "--json") {
      options.json_path = take_value(index, argc, argv);
    } else if (flag == "--check") {
      options.config.check_invariants = true;
    } else if (flag == "--row") {
      options.row = true;
    } else if (flag == "--row-header") {
      Report::write_row_header(std::cout);
      std::exit(0);
    } else if (flag == "--quiet") {
      options.quiet = true;
    } else {
      throw std::runtime_error("unknown option '" + flag + "' (try --help)");
    }
  }

  if (options.trace_path.empty()) {
    throw std::runtime_error("--trace is required (try --help)");
  }
  return options;
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Options options = parse_args(argc, argv);

    Simulator sim(options.config, Trace(options.trace_path));
    sim.run();

    const Report report = Report::collect(sim, options.config, options.trace_path);

    if (options.row) {
      report.write_row(std::cout);
    } else if (!options.quiet) {
      report.write_text(std::cout);
    }
    const ScenarioReport &fsm = sim.fsm_report();
    if (options.config.check_invariants && fsm.scenarios > 0 && !options.quiet && !options.row) {
      std::fprintf(stderr, "fsm: %zu scenarios, %zu/%zu arcs exercised, %s\n", fsm.scenarios,
                   fsm.arcs_exercised, fsm.arcs_declared,
                   fsm.failures.empty() ? "all correct" : "FAILURES BELOW");
    }
    if (!fsm.failures.empty() || !sim.fsm_failures().empty()) {
      std::fprintf(stderr, "error: %s does not match the %s state machine\n",
                   to_string(options.config.protocol), to_string(options.config.protocol));
      for (const std::string &failure : fsm.failures) {
        std::fprintf(stderr, "  [scenario] %s\n", failure.c_str());
      }
      for (const std::string &failure : sim.fsm_failures()) {
        std::fprintf(stderr, "  [on trace] %s\n", failure.c_str());
      }
      return 2;
    }

    if (!sim.violations().empty()) {
      std::fprintf(stderr, "error: %s violates coherence (%zu violation(s)):\n",
                   to_string(options.config.protocol), sim.violations().size());
      for (const InvariantViolation &violation : sim.violations()) {
        std::fprintf(stderr, "  %s\n", violation.describe().c_str());
      }
      return 2;
    }

    if (!options.json_path.empty()) {
      std::ofstream json(options.json_path);
      if (!json) {
        throw std::runtime_error("could not open '" + options.json_path + "' for writing");
      }
      report.write_json(json);
    }
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "error: %s\n", error.what());
    return 1;
  }
}
