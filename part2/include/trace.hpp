// Trace file format
// -----------------
//   <delta> <core_id> <op> <0xaddr>
//
//   delta    non-negative decimal integer; compute cycles before this access
//            (see MemOp below). NOT an absolute timestamp.
//   core_id  non-negative decimal integer
//   op       R | W
//   addr     hex byte address, must start with 0x or 0X
//
// Blank lines and lines whose first non-space character is '#' are ignored.
// Exactly four fields per op line; a fifth field is an error.
//
// Ordering: a core's ops execute in the order they appear in the file. There
// is no cross-core ordering constraint and no global sort requirement, so a
// trace can be built by concatenating per-core streams:
//
//     cat core0.trace core1.trace core2.trace > mixed.trace
//
// Because deltas are relative, any non-negative value is valid and there is
// nothing to validate beyond the field types.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// A single memory access from a trace.
//
// `delta` is the core's compute time before this access: the number of cycles
// between the completion of this core's previous access and the issue of this
// one. For a core's first access, it is measured from cycle 0.
//
//     issue(N) = completion(N-1) + delta(N)
//     issue(0) = delta(0)
//
// Cores are blocking: one outstanding request each. A core does not issue
// access N until access N-1 has completed, so `delta` is never overwritten by
// a stall. The same trace therefore describes the same workload under both the
// perfect and the realistic performance model, which absolute timestamps would
// not (with absolute timestamps, any spacing smaller than the stall is
// discarded, and every trace collapses to back-to-back issue).
//
// `delta` controls memory intensity directly: 0 means no compute between
// accesses (maximum memory pressure), larger values mean a more compute-bound
// core.
struct MemOp {
  enum class Op : uint8_t { Read, Write };

  uint64_t delta = 0;   // compute cycles before this access (see comment above)
  uint32_t core_id = 0; // 0 .. num_cores-1
  Op op = Op::Read;
  uint64_t addr = 0; // byte address; NOT block-aligned (alignment is the
                     // cache's job, so traces can express false sharing)
};

// The whole trace, parsed and bucketed by core.
//
// `per_core` is public and immutable in practice: the Simulator walks it with
// a per-core cursor rather than popping, so the trace stays intact and a run
// can be repeated or inspected after the fact.
struct Trace {
  // Reads and parses `path`. Throws std::runtime_error on a malformed line,
  // with "<path>:<line>: <reason>" in the message.
  explicit Trace(const std::string &path);

  // per_core[c] holds core c's ops in file order. Sized to (max core_id + 1),
  // so a core that appears in no op line still gets an empty slot.
  std::vector<std::vector<MemOp>> per_core;

  // Derived from the trace itself: max core_id + 1. A config may model MORE
  // cores than this (the extras stay idle) but never fewer.
  size_t num_cores() const { return per_core.size(); }
  size_t total_ops() const;
};

// Guards against a typo'd core_id (e.g. `0 99999 R 0x1000`) turning into a
// 99999-entry per_core vector.
inline constexpr uint32_t kMaxCores = 1024;
