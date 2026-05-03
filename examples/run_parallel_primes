#!/usr/bin/env python3
"""run_parallel_primes — parameterized parallel π(N) demo.

Substitutes N (upper bound) and K (number of worker CPUs) into
parallel_primes.s, assembles it, and spawns the appropriate
oriscbar + oriscterm + (1 + K) simorisc processes via oriscrun.

The coordinator runs as PID 0 and also computes one share of the
work; workers run as PID 1..K. The terminal device runs at PID 16.

Usage:
    examples/run_parallel_primes              # defaults: N=2000, w=3
    examples/run_parallel_primes -N 5000      # 5000, 3 workers
    examples/run_parallel_primes -N 10000 -w 8

K must be in [1, 10] — the program's worker-dispatch jump table has
ten entries, matching the ten OR slots O6..O15 that hold worker
service references at boot.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "examples" / "parallel_primes.s"
ASM = ROOT / "tools" / "asm" / "asmorisc"
RUN = ROOT / "tools" / "oriscrun"


def main() -> int:
    ap = argparse.ArgumentParser(prog="run_parallel_primes",
                                 description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-N", type=int, default=2000,
                    help="upper bound for prime counting (default 2000)")
    ap.add_argument("-w", "--workers", type=int, default=3,
                    help="number of worker CPUs (default 3, max 10)")
    ap.add_argument("--leader-timeout", type=float, default=120.0,
                    help="oriscrun leader timeout in seconds (default 120)")
    ap.add_argument("--linger", type=float, default=3.0,
                    help="seconds the terminal stays after the leader exits (default 3)")
    ap.add_argument("--keep", action="store_true",
                    help="keep the generated .s and .orx files for inspection")
    args = ap.parse_args()

    if args.workers < 1 or args.workers > 10:
        ap.error("workers must be in [1, 10]")
    if args.N < 4:
        ap.error("N must be >= 4 (need at least one prime to count)")

    # Substitute the .word config values in a copy of the source.
    src_text = SRC.read_text()
    src_text = re.sub(r"^(config_N:\s+\.word\s+)\d+",
                      r"\g<1>" + str(args.N), src_text, count=1, flags=re.M)
    src_text = re.sub(r"^(config_K:\s+\.word\s+)\d+",
                      r"\g<1>" + str(args.workers), src_text, count=1, flags=re.M)

    if "config_N:" not in src_text or "config_K:" not in src_text:
        sys.exit("run_parallel_primes: failed to find config labels in source")

    workdir = tempfile.mkdtemp(prefix="parallel_primes-")
    gen_s   = Path(workdir) / "parallel_primes.s"
    gen_orx = Path(workdir) / "parallel_primes.orx"
    gen_s.write_text(src_text)

    # Assemble.
    rc = subprocess.run([sys.executable, str(ASM), str(gen_s),
                         "-o", str(gen_orx)]).returncode
    if rc != 0:
        sys.exit(f"run_parallel_primes: assembler failed ({rc})")

    # Build the oriscrun command line.
    cmd = [sys.executable, str(RUN), "--terminal", "pid=16"]

    # Coordinator at PID 0 with --service for terminal + each worker.
    services = ["service=16=1@9"]
    for w in range(1, args.workers + 1):
        services.append(f"service={w}=4@9")
    cmd.extend(["--cpu",
                f"pid=0:program={gen_orx}," + ",".join(services)])

    # Workers PID 1..K in serve mode.
    for w in range(1, args.workers + 1):
        cmd.extend(["--cpu", f"pid={w}:program={gen_orx},serve"])

    cmd.extend(["--leader-timeout", str(args.leader_timeout),
                "--linger", str(args.linger)])

    if args.keep:
        sys.stderr.write(f"[run_parallel_primes] generated: {gen_s}\n")
        sys.stderr.write(f"[run_parallel_primes] assembled: {gen_orx}\n")

    # exec oriscrun (no shell intermediate, no stray child).
    os.execvp(cmd[0], cmd)


if __name__ == "__main__":
    sys.exit(main())
