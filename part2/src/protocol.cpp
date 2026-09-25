// Shared protocol helpers. The state machines live in protocol_<name>.cpp.

#include "protocol.hpp"

#include <stdexcept>
#include <string>

const char *to_string(BusReqType type) {
  switch (type) {
  case BusReqType::Read:
    return "Read";
  case BusReqType::ReadX:
    return "ReadX";
  case BusReqType::Upgrade:
    return "Upgrade";
  case BusReqType::Writeback:
    return "Writeback";
  }
  return "?";
}

std::optional<uint32_t> find_supplier(const std::vector<CacheState> &peer_states,
                                      uint32_t requestor_id,
                                      std::initializer_list<CacheState> priority) {
  for (CacheState wanted : priority) {
    for (uint32_t core_id = 0; core_id < peer_states.size(); ++core_id) {
      if (core_id != requestor_id && peer_states[core_id] == wanted) {
        return core_id;
      }
    }
  }
  return std::nullopt;
}

std::vector<uint32_t> find_holders(const std::vector<CacheState> &peer_states,
                                   uint32_t requestor_id) {
  std::vector<uint32_t> holders;
  for (uint32_t core_id = 0; core_id < peer_states.size(); ++core_id) {
    if (core_id != requestor_id && peer_states[core_id] != CacheState::I) {
      holders.push_back(core_id);
    }
  }
  return holders;
}

namespace protocol_detail {

[[noreturn]] void reject_state(const char *protocol, AccessType access, CacheState state,
                               const char *states) {
  throw std::runtime_error(std::string(protocol) + ": no rule for a " +
                           (access == AccessType::Read ? "read" : "write") + " in state " +
                           to_string(state) + " (" + protocol + " has only " + states + ")");
}

[[noreturn]] void reject_request(const char *protocol, BusReqType type, const char *reason) {
  throw std::runtime_error(std::string(protocol) + " never issues " + to_string(type) + ": " +
                           reason);
}

} // namespace protocol_detail
