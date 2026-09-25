// Trace file parsing. Format is documented in trace.hpp.

#include "trace.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

// Throws with "<path>:<line>: <reason>".
[[noreturn]] void parse_error(const std::string &path, size_t line_number,
                              const std::string &reason) {
  throw std::runtime_error(path + ":" + std::to_string(line_number) + ": " + reason);
}

// Parses a non-negative decimal field. `field_name` appears in error messages.
uint64_t parse_uint(const std::string &token, const std::string &path, size_t line_number,
                    const std::string &field_name) {
  if (token.empty() || token[0] == '-' || token[0] == '+') {
    parse_error(path, line_number, field_name + " must be a non-negative integer: '" + token + "'");
  }
  try {
    size_t digits_consumed = 0;
    const uint64_t value = std::stoull(token, &digits_consumed, 10);
    if (digits_consumed != token.size()) {
      parse_error(path, line_number, field_name + " is not an integer: '" + token + "'");
    }
    return value;
  } catch (const std::invalid_argument &) {
    parse_error(path, line_number, field_name + " is malformed: '" + token + "'");
  } catch (const std::out_of_range &) {
    parse_error(path, line_number, field_name + " is out of range: '" + token + "'");
  }
}

// Parses a 0x-prefixed hex byte address.
uint64_t parse_address(const std::string &token, const std::string &path, size_t line_number) {
  const bool has_hex_prefix =
      token.size() >= 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X');
  if (!has_hex_prefix) {
    parse_error(path, line_number, "address must start with '0x' or '0X': '" + token + "'");
  }
  const std::string hex_digits = token.substr(2);
  if (hex_digits.empty()) {
    parse_error(path, line_number, "address has no hex digits after '0x': '" + token + "'");
  }
  try {
    size_t digits_consumed = 0;
    const uint64_t address = std::stoull(hex_digits, &digits_consumed, 16);
    if (digits_consumed != hex_digits.size()) {
      parse_error(path, line_number, "address has non-hex characters: '" + token + "'");
    }
    return address;
  } catch (const std::invalid_argument &) {
    parse_error(path, line_number, "address is malformed: '" + token + "'");
  } catch (const std::out_of_range &) {
    parse_error(path, line_number, "address is out of range: '" + token + "'");
  }
}

uint64_t parse_core_id(const std::string &token, const std::string &path, size_t line_number) {
  const uint64_t core_id = parse_uint(token, path, line_number, "core_id");
  if (core_id >= kMaxCores) {
    parse_error(path, line_number,
                "core_id " + std::to_string(core_id) + " exceeds the maximum of " +
                    std::to_string(kMaxCores - 1));
  }
  return core_id;
}

MemOp::Op parse_op(const std::string &token, const std::string &path, size_t line_number) {
  if (token == "R")
    return MemOp::Op::Read;
  if (token == "W")
    return MemOp::Op::Write;
  parse_error(path, line_number, "unknown op '" + token + "' (expected R or W)");
}

// True for blank lines and comment lines, which carry no op.
bool is_skippable(const std::string &line) {
  const auto first_char = line.find_first_not_of(" \t\r");
  return first_char == std::string::npos || line[first_char] == '#';
}

} // namespace

// Reads every op line and appends it to its core's bucket, growing `per_core`
// as new core ids appear.
Trace::Trace(const std::string &path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    throw std::runtime_error("could not open trace file: " + path);
  }

  std::string line;
  size_t line_number = 0;
  while (std::getline(file, line)) {
    ++line_number;
    if (is_skippable(line))
      continue;

    std::istringstream fields(line);
    std::string delta_text, core_text, op_text, address_text, unexpected_text;
    if (!(fields >> delta_text >> core_text >> op_text >> address_text)) {
      parse_error(path, line_number,
                  "expected 4 whitespace-separated fields (delta core_id op addr)");
    }
    if (fields >> unexpected_text) {
      parse_error(path, line_number, "unexpected extra field: '" + unexpected_text + "'");
    }

    MemOp op;
    op.delta = parse_uint(delta_text, path, line_number, "delta");
    op.core_id = static_cast<uint32_t>(parse_core_id(core_text, path, line_number));
    op.op = parse_op(op_text, path, line_number);
    op.addr = parse_address(address_text, path, line_number);

    if (op.core_id >= per_core.size()) {
      per_core.resize(op.core_id + 1);
    }
    per_core[op.core_id].push_back(op);
  }
}

size_t Trace::total_ops() const {
  size_t count = 0;
  for (const auto &core_ops : per_core) {
    count += core_ops.size();
  }
  return count;
}
