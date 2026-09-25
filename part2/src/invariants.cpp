// Single-writer/multiple-reader checking. See invariants.hpp.

#include "invariants.hpp"

#include <sstream>

namespace {

// "core 1 (M), core 3 (M)" -- the cores holding the line, for error messages.
std::string list_holders(const std::vector<CacheState> &states) {
  std::ostringstream out;
  bool first = true;
  for (uint32_t core_id = 0; core_id < states.size(); ++core_id) {
    if (states[core_id] == CacheState::I)
      continue;
    if (!first)
      out << ", ";
    out << "core " << core_id << " (" << to_string(states[core_id]) << ")";
    first = false;
  }
  return out.str();
}

} // namespace

std::optional<std::string> check_coherence_invariant(const std::vector<CacheState> &states) {
  uint32_t owners = 0;    // M, E or O: caches claiming ownership of the line
  uint32_t exclusive = 0; // M or E: owners that tolerate no other holder
  uint32_t sharers = 0;

  for (CacheState state : states) {
    switch (state) {
    case CacheState::M:
    case CacheState::E:
      ++exclusive;
      ++owners;
      break;
    case CacheState::O:
      ++owners;
      break;
    case CacheState::S:
      ++sharers;
      break;
    case CacheState::I:
      break;
    }
  }

  if (owners > 1) {
    return "two or more caches own the line (" + list_holders(states) +
           "); at most one may be in M, E or O";
  }
  if (exclusive == 1 && sharers > 0) {
    return "an exclusive holder coexists with sharers (" + list_holders(states) +
           "); a write to that line would not be seen by the sharers";
  }
  return std::nullopt;
}

std::string InvariantViolation::describe() const {
  std::ostringstream out;
  out << "access " << access_index << " (core " << core_id << ", line 0x" << std::hex << addr
      << std::dec << "): " << message;
  return out.str();
}
