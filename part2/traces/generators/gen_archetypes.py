#!/usr/bin/env python3
"""Generates the archetype traces the protocol sweep runs on.

Format: <delta> <core_id> <op> <0xaddr>, where delta is compute cycles since
this core's previous access COMPLETED (see include/trace.hpp). delta=0 means a
memory-bound core with no compute between accesses.

Each archetype isolates one reason a coherence state exists, so that a protocol
lacking that state shows a measurable cost and a protocol having it does not:

  read_share    many readers, rare writes         -- isolates S  (MI fails)
  private_rw    no sharing, read-then-write       -- isolates E
  dirty_share   reads keep finding dirty lines    -- isolates O
  migratory     read-modify-write moves core to core -- control, no state wins
  mixed         all four blended + capacity pressure -- capstone

DESIGN RULE: none of these may be round-structured. A trace written as "core 0
writes, then cores 1..N read" only behaves that way if the cores stay in
lockstep, and with per-core deltas and no barriers they do not -- one core
races through its whole stream while another is still starting. The rounds
exist in the file, not in time, and the measured workload then changes with the
performance model. Every pattern below holds for ANY interleaving instead.
"""
import argparse

BLOCK = 64

# One region per core. The stride is deliberately NOT a multiple of
# (row_bytes * banks): a round 1 MiB region aliases every core onto the same
# DRAM bank and row, and the whole workload then serialises on one bank, which
# looks exactly like a memory bandwidth limit but is an addressing artefact.
# Adding one row per core walks the regions across banks instead.
REGION = 1 << 20
BANK_SKEW = 8192  # one DRAM row, so core i starts on bank i % num_banks

# Regions above the per-core ones, for lines that are shared by construction.
SHARED_BASE = REGION * 64
MIGRATORY_BASE = REGION * 96
STREAM_BASE = REGION * 128


def region(core):
    return REGION * (core + 1) + BANK_SKEW * core


# Lines within a region must be spread across DRAM banks, not laid out
# contiguously. 64 contiguous 64 B lines occupy 4 KB, which is half of one 8 KB
# row -- so every one of them maps to the SAME bank, and the whole region's
# memory traffic serialises on it. That looks like a memory bandwidth limit and
# is really an addressing artefact: it inflated `migratory` to the point where a
# protocol's entire runtime was one bank's write queue.
#
# Bank and cache-set index come from disjoint address bits -- bank from the
# row-size bits and up, set from the block-offset bits and up -- so both can be
# varied at once. Striding by BANK_SKEW alone spreads banks but leaves every
# line in the SAME cache set (BANK_SKEW is a whole number of sets), which turns
# a DRAM bank conflict into cache thrashing. Adding a block per index varies the
# set too.
BANK_GROUPS = 8


def line(base, index):
    return base + (index % BANK_GROUPS) * BANK_SKEW + index * BLOCK


def header(title, *notes):
    return [f"# {title}"] + [f"# {note}" for note in notes] + [""]


def read_share(cores, scale, delta):
    """Many cores read a shared table; one core occasionally updates it.

    MI has no way to hold a line for reading without holding it exclusively, so
    every read steals the line from the last reader and the table ping-pongs
    continuously. Every other protocol parks the readers in S and hits.
    """
    lines = 64
    reads_per_line = 2 * scale
    out = header("read-mostly sharing -- isolates the S state",
                 f"{cores} cores read a shared {lines}-line table; core 0 rewrites",
                 "one line every few passes.",
                 "MI: every read takes the line exclusively -> continuous ping-pong.",
                 "MSI and richer: readers coexist in S and hit.")
    for core in range(cores):
        for pass_index in range(reads_per_line):
            for index in range(lines):
                addr = line(SHARED_BASE, index)
                # Core 0 refreshes a line now and then, so the table is not
                # purely read-only and the S copies do get invalidated.
                if core == 0 and index % 16 == 0 and pass_index % 3 == 0:
                    out.append(f"{delta} {core} W 0x{addr:X}")
                else:
                    out.append(f"{delta} {core} R 0x{addr:X}")
    return out


def private_rw(cores, scale, delta):
    """Each core reads then writes lines nobody else ever touches.

    With no sharing the only coherence cost is the write that follows each
    read. MESI/MOESI take the line in E and write silently; MSI/MOSI take it in
    S and must pay for an Upgrade they provably do not need.

    Memory traffic is identical under all five protocols, so this trace cannot
    distinguish O at all -- by design.
    """
    lines_per_core = 64 * scale
    out = header("private read-then-write -- isolates the E state",
                 f"{cores} cores x {lines_per_core} private lines, each read then written.",
                 "MESI/MOESI: read miss lands in E, the write is silent.",
                 "MSI/MOSI:   read miss lands in S, the write costs an Upgrade.")
    for core in range(cores):
        base = region(core)
        for index in range(lines_per_core):
            addr = line(base, index)
            out.append(f"{delta} {core} R 0x{addr:X}")
            out.append(f"{delta} {core} W 0x{addr:X}")
    return out


def dirty_share(cores, scale, delta):
    """Each core writes lines it owns and reads lines its neighbours own.

    Ownership, rather than producer/consumer pairs. A pair racing on one line
    is too tightly coupled to compare fairly: the number of hand-offs depends
    on how closely the two cores interleave, so a protocol that is slower per
    hand-off gets FEWER hand-offs and can finish sooner by doing less work.
    Spreading each core's reads over three neighbours' regions loosens that
    coupling -- a read finds a dirty line because someone owns it, not because
    of who happened to run first.

    Residual drift remains under heavy constraint; see traces/README.md. The
    traffic columns (memory writes in particular) are exact and protocol-
    determined, and are the sound basis for the O comparison.
    """
    lines_per_core = 16
    iterations = 32 * scale
    out = header("dirty read-sharing -- isolates the O state",
                 f"{cores} cores. Each writes lines it alone owns, and reads lines",
                 "owned by three neighbours, so reads keep finding dirty lines.",
                 "MOSI/MOESI: the owner keeps the dirty line and supplies it.",
                 "MSI/MESI:   the owner flushes to memory on each hand-off.")

    def owned(core, index):
        return line(region(core), index % lines_per_core)

    for core in range(cores):
        for iteration in range(iterations):
            out.append(f"{delta} {core} W 0x{owned(core, iteration):X}")
            for step in (1, 2, 3):
                neighbour = (core + step * 3) % cores
                if neighbour != core:
                    out.append(f"{delta} {core} R 0x{owned(neighbour, iteration):X}")
    return out


def migratory(cores, scale, delta):
    """Lines are read-modify-written, and the pair moves from core to core.

    A useful control: every access ends up needing exclusive ownership, so no
    amount of extra state avoids the transfer. It also inverts the usual
    ranking. MI issues ONE transaction per hand-off, because its read already
    takes the line exclusively and the write then hits; MSI/MESI/MOSI/MOESI
    issue TWO -- a Read that lands in S, then an Upgrade.

    MI pays for that with a memory write on every hand-off, so it wins on bus
    transactions and loses on memory traffic. Which protocol looks best here
    depends entirely on which resource is constrained, and no protocol wins on
    both. Real designs detect this pattern and promote the read to a ReadX.
    """
    lines = 32
    rounds = 2 * scale
    out = header("migratory sharing -- control, no extra state helps",
                 f"{lines} lines, each read-modify-written in turn by every core.",
                 "Every access needs exclusive ownership eventually, so S, E and O",
                 "cannot avoid the transfer.",
                 "MI: 1 transaction per hand-off (its read takes ownership) + 1 memory write.",
                 "Others: 2 transactions per hand-off (Read then Upgrade).")
    # Emitted per core, so no cross-core ordering is assumed: each core walks
    # every line, and the hand-off happens wherever the cores happen to meet.
    for core in range(cores):
        for round_index in range(rounds):
            for index in range(lines):
                addr = line(MIGRATORY_BASE, (index + core) % lines)
                out.append(f"{delta} {core} R 0x{addr:X}")
                out.append(f"{delta} {core} W 0x{addr:X}")
    return out


def mixed(cores, scale, delta):
    """Capstone: all four patterns interleaved, plus capacity pressure.

    No single optimisation dominates, so the protocols separate by how many of
    the patterns they handle.

    Also the only archetype with a streaming region: at the default 32 KiB
    cache (512 lines) a core touches roughly 200 distinct lines and still fits,
    so run it with --cache-bytes 8192 to force capacity evictions and exercise
    writebacks that are not coherence flushes.
    """
    private_lines = 32
    shared_lines = 32
    stream_lines = 768  # walked cyclically; see the docstring on cache sizing
    iterations = 12 * scale
    out = header("mixed workload -- capstone",
                 "Blends private read-then-write, read-mostly sharing, dirty",
                 "sharing, migratory hand-off and a streaming region.",
                 "Run with --cache-bytes 8192 to add capacity evictions.",
                 "No single state wins; protocols separate by how many patterns",
                 "they cover.")
    for core in range(cores):
        for iteration in range(iterations):
            # 1. Private read-then-write (exercises E).
            addr = line(region(core), iteration % private_lines)
            out.append(f"{delta} {core} R 0x{addr:X}")
            out.append(f"{delta} {core} W 0x{addr:X}")

            # 2. Read the shared table (exercises S).
            for offset in range(4):
                shared = line(SHARED_BASE, (iteration * 4 + offset) % shared_lines)
                out.append(f"{delta} {core} R 0x{shared:X}")

            # 3. Read a neighbour's dirty region (exercises O).
            neighbour = (core + 1) % cores
            dirty = line(region(neighbour), iteration % private_lines)
            out.append(f"{delta} {core} R 0x{dirty:X}")

            # 4. Migratory read-modify-write on a contended line.
            hot = line(MIGRATORY_BASE, iteration % 8)
            out.append(f"{delta} {core} R 0x{hot:X}")
            out.append(f"{delta} {core} W 0x{hot:X}")

            # 5. Stream through a region too large to cache.
            for offset in range(2):
                stream = line(STREAM_BASE + region(core),
                              (iteration * 2 + offset) % stream_lines)
                out.append(f"{delta} {core} R 0x{stream:X}")
    return out


GENERATORS = {
    "read_share": read_share,
    "private_rw": private_rw,
    "dirty_share": dirty_share,
    "migratory": migratory,
    "mixed": mixed,
}


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cores", type=int, default=16)
    parser.add_argument("--scale", type=int, default=4,
                        help="length multiplier (default 4, roughly 8k ops each)")
    parser.add_argument("--delta", type=int, default=0,
                        help="compute cycles between a core's accesses; 0 is "
                             "memory-bound, ~50 makes every protocol converge")
    parser.add_argument("--only", nargs="*", choices=sorted(GENERATORS),
                        help="generate only these archetypes")
    parser.add_argument("--out-dir", default="traces")
    args = parser.parse_args()

    names = args.only or sorted(GENERATORS)
    for name in names:
        lines = GENERATORS[name](args.cores, args.scale, args.delta)
        path = f"{args.out_dir}/{name}.trace"
        with open(path, "w") as handle:
            handle.write("\n".join(lines) + "\n")
        ops = sum(1 for line in lines if line and not line.startswith("#"))
        print(f"{path}: {ops} ops, {args.cores} cores")


if __name__ == "__main__":
    main()
