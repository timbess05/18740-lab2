#!/usr/bin/env python3
"""
Trace generator for the out-of-order rename/issue lab testbench.

Emits instruction traces in the format consumed by TOP_TB's parse_trace() in
verilog_src/tb_top.sv, which scans each line with:

    $fscanf(f, "%d | %d, %d -> %d | %d, %d\\n",
            inst_ID, src1_areg, src2_areg, areg, exec_duration, inst_type);

so one line looks like:

    6 | 5, 6 -> 7 | 4, 0
    ^   ^  ^    ^   ^  ^
    |   |  |    |   |  +-- inst_type   (0=regular, 1=branch, 2=mispredicting branch)
    |   |  |    |   +----- exec_duration in cycles
    |   |  |    +--------- destination architectural register
    |   |  +-------------- source 2 architectural register
    |   +----------------- source 1 architectural register
    +--------------------- instruction ID (must be dense and start at 0)

The trace file must contain nothing but instruction lines: parse_trace() has no
notion of comments or blank lines. Generation metadata therefore goes to stderr
(or to --stats-file), never into the trace itself.

Examples
--------
    # Regenerate every shipped trace into traces/
    ./gen_trace.py --all --outdir traces

    # One named preset
    ./gen_trace.py --preset dependent_chain -o traces/instructions.txt

    # Hand-rolled trace
    ./gen_trace.py --num-inst 500 --dep-density 0.8 --dep-window 3 \\
                   --max-exec-duration 12 --seed 7 -o traces/instructions.txt
"""

import argparse
import os
import random
import re
import sys
from typing import Dict, List, Optional, Set, Tuple


# Python 3.6 compatibility: no dataclasses, and no builtin generics in
# annotations. Plain classes and typing aliases keep this runnable on the
# stock interpreter of an RHEL 8 machine.
def _replace(obj, **changes):
    """Return a copy of a simple settings object with some fields changed."""
    new = obj.__class__()
    new.__dict__.update(obj.__dict__)
    new.__dict__.update(changes)
    return new

# ---------------------------------------------------------------------------
# Design parameters, mirrored from verilog_src/global_defines.svh
# ---------------------------------------------------------------------------

TYPE_REGULAR = 0
TYPE_BRANCH = 1
TYPE_FLUSH = 2

TYPE_NAMES = {TYPE_REGULAR: "regular", TYPE_BRANCH: "branch", TYPE_FLUSH: "flush"}


class DesignParams(object):
    """Parameters read from global_defines.svh (with fallbacks to its defaults)."""

    def __init__(self, rob_size=128, iq_size=64, ppl_width=4, arch_reg=32,
                 inst_num=4096, phys_reg=256, max_exec_duration=256,
                 full_threshold=1000, drain_cycles=1000,
                 source="built-in defaults"):
        self.rob_size = rob_size
        self.iq_size = iq_size
        self.ppl_width = ppl_width
        self.arch_reg = arch_reg
        self.inst_num = inst_num
        self.phys_reg = phys_reg
        self.max_exec_duration = max_exec_duration
        self.full_threshold = full_threshold
        self.drain_cycles = drain_cycles
        self.source = source


_PARAM_RE = re.compile(r"^\s*parameter\s+(\w+)\s*=\s*([^;/]+?)\s*;?\s*(?://.*)?$")


def _eval_param(expr: str, known: Dict[str, int]) -> Optional[int]:
    """Evaluate a trivial Verilog parameter expression (ints, +, -, *, names)."""
    expr = expr.strip()
    if "$clog2" in expr:  # bit-width params; the generator never needs them
        return None
    # Only allow digits, identifiers, whitespace and + - * ( )
    if not re.fullmatch(r"[\w\s+\-*()]+", expr):
        return None
    try:
        return int(eval(expr, {"__builtins__": {}}, dict(known)))  # noqa: S307
    except Exception:
        return None


def load_design_params(path: Optional[str]) -> DesignParams:
    """Parse global_defines.svh so traces track whatever the handout specifies.

    Falls back to the built-in defaults for anything missing or unparseable.
    Note that the shipped global_defines.svh is missing semicolons on several
    parameter lines, so the trailing semicolon is treated as optional.
    """
    params = DesignParams()
    if not path or not os.path.isfile(path):
        return params

    found: Dict[str, int] = {}
    with open(path, "r") as fh:
        for line in fh:
            m = _PARAM_RE.match(line)
            if not m:
                continue
            name, expr = m.group(1), m.group(2)
            value = _eval_param(expr, found)
            if value is not None:
                found[name] = value

    mapping = {
        "rob_size": "ROB_SIZE",
        "iq_size": "IQ_SIZE",
        "ppl_width": "PPL_WIDTH",
        "arch_reg": "ARCH_REG",
        "inst_num": "INST_NUM",
        "phys_reg": "PHYS_REG",
        "max_exec_duration": "MAX_EXEC_DURATION",
        "full_threshold": "FULL_THRESHOLD",
        "drain_cycles": "DRAIN_CYCLES",
    }
    kwargs = {attr: found[key] for attr, key in mapping.items() if key in found}
    if kwargs:
        kwargs["source"] = path
    return _replace(params, **kwargs)


# ---------------------------------------------------------------------------
# Trace generation
# ---------------------------------------------------------------------------


class Inst(object):
    def __init__(self, id=0, src1=0, src2=0, dest=0, duration=1, type=0):
        self.id = id
        self.src1 = src1
        self.src2 = src2
        self.dest = dest
        self.duration = duration
        self.type = type

    def line(self) -> str:
        return (
            f"{self.id} | {self.src1}, {self.src2} -> {self.dest} "
            f"| {self.duration}, {self.type}"
        )


class GenConfig(object):
    def __init__(self, num_inst=64, seed=0, dep_density=0.6, dep_window=8,
                 min_exec_duration=1, max_exec_duration=8,
                 long_tail_rate=0.0, long_tail_duration=40,
                 dest_pool=0,                 # 0 => all writable arch regs
                 branch_rate=0.0, mispredict_rate=0.0, branch_max_duration=4,
                 min_flush_spacing=16, flush_tail_guard=32,
                 reserve_zero_reg=True):
        self.num_inst = num_inst
        self.seed = seed
        self.dep_density = dep_density
        self.dep_window = dep_window
        self.min_exec_duration = min_exec_duration
        self.max_exec_duration = max_exec_duration
        self.long_tail_rate = long_tail_rate
        self.long_tail_duration = long_tail_duration
        self.dest_pool = dest_pool
        self.branch_rate = branch_rate
        self.mispredict_rate = mispredict_rate
        self.branch_max_duration = branch_max_duration
        self.min_flush_spacing = min_flush_spacing
        self.flush_tail_guard = flush_tail_guard
        self.reserve_zero_reg = reserve_zero_reg


def _pick_source(
    rng: random.Random,
    last_writer: Dict[int, int],
    cfg: GenConfig,
    arch_reg: int,
) -> int:
    """Pick a source register, biased toward recently written ones.

    dep_density controls how often a source is a true dependency at all;
    dep_window controls how far back into the write history we reach. A small
    window produces long serial dependence chains (low ILP, stresses IQ wakeup
    and in-order commit); a large window produces wide independent work.
    """
    if last_writer and rng.random() < cfg.dep_density:
        recent = sorted(last_writer, key=lambda r: last_writer[r], reverse=True)
        return rng.choice(recent[: max(1, cfg.dep_window)])
    return rng.randrange(arch_reg)


def _pick_duration(rng: random.Random, cfg: GenConfig) -> int:
    if cfg.long_tail_rate and rng.random() < cfg.long_tail_rate:
        lo = max(cfg.min_exec_duration, cfg.max_exec_duration)
        return rng.randint(lo, max(lo, cfg.long_tail_duration))
    return rng.randint(cfg.min_exec_duration, cfg.max_exec_duration)


def generate(cfg: GenConfig, dp: DesignParams) -> List[Inst]:
    rng = random.Random(cfg.seed)

    first_dest = 1 if cfg.reserve_zero_reg else 0
    if first_dest >= dp.arch_reg:
        raise ValueError("arch_reg too small to reserve a zero register")

    dest_pool = list(range(first_dest, dp.arch_reg))
    if cfg.dest_pool:
        dest_pool = dest_pool[: max(1, min(cfg.dest_pool, len(dest_pool)))]

    last_writer: Dict[int, int] = {}
    insts: List[Inst] = []
    last_flush_at = -(10**9)

    for i in range(cfg.num_inst):
        src1 = _pick_source(rng, last_writer, cfg, dp.arch_reg)
        src2 = _pick_source(rng, last_writer, cfg, dp.arch_reg)
        dest = rng.choice(dest_pool)

        itype = TYPE_REGULAR
        if cfg.branch_rate and rng.random() < cfg.branch_rate:
            itype = TYPE_BRANCH
            # A mispredicting branch flushes every instruction dispatched after
            # it, so keep flushes spaced apart (non-overlapping windows) and
            # away from the tail of the trace (something must survive to commit).
            can_flush = (
                cfg.mispredict_rate > 0
                and i - last_flush_at >= cfg.min_flush_spacing
                and i < cfg.num_inst - cfg.flush_tail_guard
            )
            if can_flush and rng.random() < cfg.mispredict_rate:
                itype = TYPE_FLUSH
                last_flush_at = i

        if itype == TYPE_REGULAR:
            duration = _pick_duration(rng, cfg)
        else:
            # Branch resolution latency sets the flush window width; keep it
            # short so a mispredict does not swallow the rest of the trace.
            duration = rng.randint(
                cfg.min_exec_duration,
                max(cfg.min_exec_duration, cfg.branch_max_duration),
            )

        insts.append(Inst(i, src1, src2, dest, duration, itype))
        last_writer[dest] = i

    return insts


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------

# The exact shape parse_trace() scans, used to re-parse our own output.
LINE_RE = re.compile(
    r"^(\d+) \| (\d+), (\d+) -> (\d+) \| (\d+), (\d+)$"
)


def validate(insts: List[Inst], dp: DesignParams) -> List[str]:
    """Return a list of hard errors. Empty list means the trace is legal."""
    errors: List[str] = []

    if not insts:
        return ["trace is empty"]
    if len(insts) > dp.inst_num:
        errors.append(
            f"{len(insts)} instructions exceeds INST_NUM={dp.inst_num}; "
            f"inst_ID would not fit in INST_BIT bits"
        )

    for idx, ins in enumerate(insts):
        where = f"line {idx + 1} (id {ins.id})"
        # Dense, ascending IDs starting at 0: the scoreboard is indexed by
        # inst_ID and dispatch order is file order.
        if ins.id != idx:
            errors.append(f"{where}: inst_ID must equal its line index ({idx})")
        for name, reg in (("src1", ins.src1), ("src2", ins.src2), ("dest", ins.dest)):
            if not 0 <= reg < dp.arch_reg:
                errors.append(f"{where}: {name}={reg} outside 0..{dp.arch_reg - 1}")
        if ins.duration < 1:
            errors.append(
                f"{where}: exec_duration={ins.duration} must be >= 1 "
                f"(time_left is decremented before the ==0 test)"
            )
        if ins.duration >= dp.max_exec_duration:
            errors.append(
                f"{where}: exec_duration={ins.duration} must be < "
                f"MAX_EXEC_DURATION={dp.max_exec_duration}"
            )
        if ins.type not in TYPE_NAMES:
            errors.append(f"{where}: unknown inst_type={ins.type}")
        # Round-trip each rendered line through the testbench's scan format.
        if not LINE_RE.match(ins.line()):
            errors.append(f"{where}: rendered line does not match the $fscanf format")

    if insts[-1].type == TYPE_FLUSH:
        errors.append("last instruction is a mispredicting branch; nothing survives it")

    return errors


# ---------------------------------------------------------------------------
# Idealized machine model (rough feasibility estimate, not a golden model)
# ---------------------------------------------------------------------------


class ModelResult(object):
    def __init__(self):
        self.cycles = 0
        self.dispatch_cycles = 0
        self.drain_cycles = 0
        self.max_consecutive_full = 0
        self.critical_path = 0
        self.issue_utilization = 0.0
        self.warnings = []


def model_trace(insts: List[Inst], dp: DesignParams) -> ModelResult:
    """Approximate how a correct design would execute this trace.

    This exists to catch traces that would abort the simulation before any
    check runs: TOP_TB gives up after FULL_THRESHOLD consecutive full cycles,
    and only allows DRAIN_CYCLES after the last dispatch. It models an
    oldest-ready-first IQ, in-order commit, and one freed preg per commit.
    Flushes are deliberately not modeled, so branch traces are approximated as
    if every instruction retires.
    """
    res = ModelResult()

    n = len(insts)
    producer: Dict[int, int] = {}
    src_prod: List[Tuple[Optional[int], Optional[int]]] = []
    for ins in insts:
        src_prod.append((producer.get(ins.src1), producer.get(ins.src2)))
        producer[ins.dest] = ins.id

    # Dependence-only critical path, ignoring all structural limits.
    finish = [0] * n
    for ins in insts:
        p1, p2 = src_prod[ins.id]
        ready = max(finish[p1] if p1 is not None else 0,
                    finish[p2] if p2 is not None else 0)
        finish[ins.id] = ready + ins.duration
    res.critical_path = max(finish) if n else 0

    free_pregs = dp.phys_reg - dp.arch_reg
    executed: Set[int] = set()
    rob: List[int] = []           # inst ids, in order
    completed: Set[int] = set()
    iq: List[int] = []            # inst ids awaiting issue
    executing: List[list] = []    # [inst_id, cycles_left]

    dispatched = 0
    cycle = 0
    consecutive_full = 0
    issue_slots_used = 0
    limit = dp.full_threshold * 4 + dp.drain_cycles + res.critical_path + 10_000

    while (dispatched < n or rob or iq or executing) and cycle < limit:
        cycle += 1

        rob_full = (dp.rob_size - len(rob)) < dp.ppl_width
        iq_full = (dp.iq_size - len(iq)) < dp.ppl_width
        rru_full = free_pregs < dp.ppl_width
        any_full = rob_full or iq_full or rru_full

        consecutive_full = consecutive_full + 1 if any_full else 0
        res.max_consecutive_full = max(res.max_consecutive_full, consecutive_full)

        # Dispatch
        if not any_full:
            for _ in range(dp.ppl_width):
                if dispatched >= n:
                    break
                ins = insts[dispatched]
                rob.append(ins.id)
                iq.append(ins.id)
                free_pregs -= 1
                dispatched += 1
                if dispatched == n:
                    res.dispatch_cycles = cycle

        # Issue: oldest ready first, up to PPL_WIDTH
        issued_now = 0
        for inst_id in list(iq):
            if issued_now >= dp.ppl_width:
                break
            p1, p2 = src_prod[inst_id]
            if (p1 is None or p1 in executed) and (p2 is None or p2 in executed):
                iq.remove(inst_id)
                executing.append([inst_id, insts[inst_id].duration])
                issued_now += 1
        issue_slots_used += issued_now

        # Execute
        for entry in executing:
            entry[1] -= 1
        for entry in [e for e in executing if e[1] <= 0]:
            executing.remove(entry)
            executed.add(entry[0])
            completed.add(entry[0])

        # Commit in order
        for _ in range(dp.ppl_width):
            if rob and rob[0] in completed:
                rob.pop(0)
                free_pregs += 1
            else:
                break

    res.cycles = cycle
    res.dispatch_cycles = res.dispatch_cycles or cycle
    res.drain_cycles = max(0, cycle - res.dispatch_cycles)
    if cycle and dp.ppl_width:
        res.issue_utilization = issue_slots_used / (cycle * dp.ppl_width)

    if cycle >= limit:
        res.warnings.append(
            "model did not drain; the trace may deadlock or is pathologically long"
        )
    if res.max_consecutive_full >= dp.full_threshold:
        res.warnings.append(
            f"~{res.max_consecutive_full} consecutive full cycles vs "
            f"FULL_THRESHOLD={dp.full_threshold}: the testbench would abort early. "
            f"Lower --max-exec-duration or --num-inst."
        )
    elif res.max_consecutive_full >= dp.full_threshold // 2:
        res.warnings.append(
            f"~{res.max_consecutive_full} consecutive full cycles is within 2x of "
            f"FULL_THRESHOLD={dp.full_threshold}; a slower design may abort early"
        )
    if res.drain_cycles >= dp.drain_cycles:
        res.warnings.append(
            f"~{res.drain_cycles} cycles needed after the last dispatch vs "
            f"DRAIN_CYCLES={dp.drain_cycles}: instructions would still be in flight "
            f"when the checks run"
        )
    return res


# ---------------------------------------------------------------------------
# Stats reporting
# ---------------------------------------------------------------------------


def report(insts: List[Inst], dp: DesignParams, cfg: GenConfig,
           res: ModelResult, path: str) -> str:
    n = len(insts)
    counts = {t: 0 for t in TYPE_NAMES}
    for ins in insts:
        counts[ins.type] += 1

    producer: Dict[int, int] = {}
    true_deps = 0
    cold_srcs = 0
    distances: List[int] = []
    for ins in insts:
        for src in (ins.src1, ins.src2):
            p = producer.get(src)
            if p is None:
                cold_srcs += 1
            else:
                true_deps += 1
                distances.append(ins.id - p)
        producer[ins.dest] = ins.id

    total_srcs = 2 * n
    avg_dur = sum(i.duration for i in insts) / n
    avg_dist = sum(distances) / len(distances) if distances else 0.0
    ideal = -(-n // dp.ppl_width)  # ceil

    lines = [
        f"trace              : {path}",
        f"design params from : {dp.source}",
        f"                     ROB={dp.rob_size} IQ={dp.iq_size} "
        f"PPL_WIDTH={dp.ppl_width} ARCH={dp.arch_reg} PHYS={dp.phys_reg}",
        f"seed               : {cfg.seed}",
        "",
        f"instructions       : {n}",
        f"  regular          : {counts[TYPE_REGULAR]}",
        f"  branch           : {counts[TYPE_BRANCH]}",
        f"  branch (flushes) : {counts[TYPE_FLUSH]}",
        "",
        f"exec duration      : {min(i.duration for i in insts)}"
        f"..{max(i.duration for i in insts)} (avg {avg_dur:.2f})",
        f"true dependencies  : {true_deps}/{total_srcs} sources "
        f"({100.0 * true_deps / total_srcs:.1f}%)",
        f"cold sources       : {cold_srcs} (architectural at reset, always ready)",
        f"avg dep distance   : {avg_dist:.1f} instructions",
        f"distinct dest regs : {len({i.dest for i in insts})}",
        "",
        "modeled execution (idealized, for sanity only)",
        f"  total cycles     : {res.cycles} (ideal floor {ideal})",
        f"  drain cycles     : {res.drain_cycles} (budget {dp.drain_cycles})",
        f"  max full streak  : {res.max_consecutive_full} "
        f"(abort at {dp.full_threshold})",
        f"  dep critical path: {res.critical_path} cycles",
        f"  issue slot usage : {100.0 * res.issue_utilization:.1f}%",
    ]
    for w in res.warnings:
        lines.append(f"  WARNING: {w}")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Presets
# ---------------------------------------------------------------------------

PRESETS: Dict[str, Tuple[str, GenConfig]] = {
    "basic": (
        "Small and hand-checkable. Moderate dependencies, short latencies, "
        "no branches. Start here when bringing a design up.",
        GenConfig(num_inst=64, seed=1, dep_density=0.55, dep_window=6,
                  max_exec_duration=6),
    ),
    "dependent_chain": (
        "Near-serial dependence chain. Low ILP; stresses IQ wakeup and "
        "in-order commit while most of the machine sits idle.",
        GenConfig(num_inst=200, seed=2, dep_density=0.95, dep_window=2,
                  max_exec_duration=5, dest_pool=6),
    ),
    "wide_ilp": (
        "Mostly independent work. Stresses parallel issue, the RRU free list "
        "and same-cycle rename of a full dispatch batch.",
        GenConfig(num_inst=400, seed=3, dep_density=0.15, dep_window=28,
                  max_exec_duration=4),
    ),
    "preg_pressure": (
        "Long latencies over many destination registers so the ROB fills and "
        "physical registers stay allocated. Exercises rru_full / rob_full.",
        GenConfig(num_inst=400, seed=4, dep_density=0.4, dep_window=16,
                  max_exec_duration=10, long_tail_rate=0.12,
                  long_tail_duration=45),
    ),
    "stress": (
        "Long mixed-behaviour trace. The default regression trace.",
        GenConfig(num_inst=2000, seed=5, dep_density=0.6, dep_window=10,
                  max_exec_duration=12, long_tail_rate=0.05,
                  long_tail_duration=30),
    ),
    "branch_basic": (
        "Small trace with a handful of mispredicting branches.",
        GenConfig(num_inst=150, seed=6, dep_density=0.55, dep_window=6,
                  max_exec_duration=6, branch_rate=0.12, mispredict_rate=0.4,
                  min_flush_spacing=20, flush_tail_guard=30),
    ),
    "branch_stress": (
        "Long trace with frequent mispredicts and recovery.",
        GenConfig(num_inst=1200, seed=7, dep_density=0.6, dep_window=10,
                  max_exec_duration=10, branch_rate=0.15, mispredict_rate=0.5,
                  min_flush_spacing=16, flush_tail_guard=40),
    ),
}

PRESET_FILENAMES = {name: f"instructions_{name}.txt" for name in PRESETS}


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    mode = p.add_argument_group("what to generate")
    mode.add_argument("--preset", choices=sorted(PRESETS),
                      help="generate a named scenario")
    mode.add_argument("--all", action="store_true",
                      help="generate every preset into --outdir")
    mode.add_argument("--list-presets", action="store_true",
                      help="describe the presets and exit")

    out = p.add_argument_group("output")
    out.add_argument("-o", "--output", help="trace file to write (default stdout)")
    out.add_argument("--outdir", default="traces",
                     help="directory for --all (default: traces)")
    out.add_argument("--stats-file", help="also write the stats report here")
    out.add_argument("--quiet", action="store_true",
                     help="suppress the stats report on stderr")
    out.add_argument("--trailing-newline", action="store_true",
                     help="end the file with a newline (off by default: it "
                          "makes $feof-based parsers read one extra record)")

    shape = p.add_argument_group("trace shape")
    shape.add_argument("--num-inst", type=int, help="number of instructions")
    shape.add_argument("--seed", type=int, help="RNG seed (traces are reproducible)")
    shape.add_argument("--dep-density", type=float,
                       help="0..1 probability a source is a true dependency")
    shape.add_argument("--dep-window", type=int,
                       help="how many recent writers a dependency may target; "
                            "small = long serial chains, large = wide ILP")
    shape.add_argument("--min-exec-duration", type=int)
    shape.add_argument("--max-exec-duration", type=int)
    shape.add_argument("--long-tail-rate", type=float,
                       help="0..1 fraction of instructions with a long latency")
    shape.add_argument("--long-tail-duration", type=int)
    shape.add_argument("--dest-pool", type=int,
                       help="restrict destinations to this many arch registers "
                            "(0 = all); small values raise WAW/WAR pressure")
    shape.add_argument("--allow-zero-dest", action="store_true",
                       help="allow r0 as a destination (default: r0 is reserved "
                            "as an always-ready architectural source)")

    br = p.add_argument_group("branches / flushes")
    br.add_argument("--branch-rate", type=float,
                    help="0..1 fraction of instructions that are branches")
    br.add_argument("--mispredict-rate", type=float,
                    help="0..1 fraction of branches that mispredict (TYPE_FLUSH)")
    br.add_argument("--branch-max-duration", type=int,
                    help="max branch resolution latency; sets flush window width")
    br.add_argument("--min-flush-spacing", type=int,
                    help="minimum instructions between two mispredicts")
    br.add_argument("--flush-tail-guard", type=int,
                    help="no mispredicts within this many instructions of the end")

    dsn = p.add_argument_group("design parameters")
    dsn.add_argument("--defines", default=None,
                     help="path to global_defines.svh (default: auto-detect "
                          "next to this script)")
    dsn.add_argument("--no-model", action="store_true",
                     help="skip the idealized execution model in the stats")

    return p


def cfg_from_args(base: GenConfig, args: argparse.Namespace) -> GenConfig:
    overrides = {}
    for name in ("num_inst", "seed", "dep_density", "dep_window",
                 "min_exec_duration", "max_exec_duration", "long_tail_rate",
                 "long_tail_duration", "dest_pool", "branch_rate",
                 "mispredict_rate", "branch_max_duration", "min_flush_spacing",
                 "flush_tail_guard"):
        value = getattr(args, name, None)
        if value is not None:
            overrides[name] = value
    if args.allow_zero_dest:
        overrides["reserve_zero_reg"] = False
    return _replace(base, **overrides)


def autodetect_defines(explicit: Optional[str]) -> Optional[str]:
    if explicit:
        return explicit
    here = os.path.dirname(os.path.abspath(__file__))
    for candidate in (
        os.path.join(here, "verilog_src", "global_defines.svh"),
        os.path.join(here, "global_defines.svh"),
    ):
        if os.path.isfile(candidate):
            return candidate
    return None


def write_trace(insts: List[Inst], path: Optional[str], trailing_newline: bool) -> None:
    body = "\n".join(ins.line() for ins in insts)
    if trailing_newline:
        body += "\n"
    if path:
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, "w") as fh:
            fh.write(body)
    else:
        sys.stdout.write(body + ("" if trailing_newline else "\n"))


def emit(cfg: GenConfig, dp: DesignParams, path: Optional[str],
         args: argparse.Namespace) -> int:
    insts = generate(cfg, dp)

    errors = validate(insts, dp)
    if errors:
        print(f"[FAIL] generated trace is not legal ({len(errors)} problems):",
              file=sys.stderr)
        for e in errors[:20]:
            print(f"  - {e}", file=sys.stderr)
        if len(errors) > 20:
            print(f"  ... and {len(errors) - 20} more", file=sys.stderr)
        return 1

    res = ModelResult() if args.no_model else model_trace(insts, dp)
    write_trace(insts, path, args.trailing_newline)

    text = report(insts, dp, cfg, res, path or "<stdout>")
    if not args.quiet:
        print(text, file=sys.stderr)
        print("", file=sys.stderr)
    if args.stats_file:
        with open(args.stats_file, "a") as fh:
            fh.write(text + "\n\n")
    return 0


def main(argv: List[str]) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    if args.list_presets:
        for name in sorted(PRESETS):
            desc, cfg = PRESETS[name]
            print(f"{name}  ->  {PRESET_FILENAMES[name]}")
            print(f"    {desc}")
            print(f"    {cfg.num_inst} instructions, dep_density={cfg.dep_density}, "
                  f"dep_window={cfg.dep_window}, "
                  f"exec<={cfg.max_exec_duration}, branch_rate={cfg.branch_rate}")
            print()
        return 0

    dp = load_design_params(autodetect_defines(args.defines))

    if args.all:
        rc = 0
        for name in sorted(PRESETS):
            cfg = cfg_from_args(PRESETS[name][1], args)
            path = os.path.join(args.outdir, PRESET_FILENAMES[name])
            rc |= emit(cfg, dp, path, args)
        return rc

    base = PRESETS[args.preset][1] if args.preset else GenConfig()
    cfg = cfg_from_args(base, args)

    if not args.preset and args.num_inst is None:
        print("[INFO] no --preset or --num-inst given; using built-in defaults "
              f"({cfg.num_inst} instructions). See --list-presets.", file=sys.stderr)

    return emit(cfg, dp, args.output, args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
