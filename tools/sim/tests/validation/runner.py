#!/usr/bin/env python3
"""Object RISC simulator validation harness.

Walks tests/validation/<category>/*.s, parses each file's expectation
header (special @-comments at the top of the source), assembles via
asmorisc, runs simorisc, and checks the outcome against the
expectations.

Expectation directives — written as `; @key: value` in the .s header,
above any code:

    @description: <free text>          one-line description for the report
    @category:    <override>           override the directory name
    @expect-exit: <int>                exact process exit code
    @expect-stdout: "<string>"         exact stdout (with C-style escapes)
    @expect-stdout-hex: <hex bytes>    stdout as hex (e.g. "48 65 6c")
    @expect-trap: <name>               trap by name (e.g. arithmetic-overflow)
    @expect-trap-pc: 0xPPPP            faulting PC (architectural)
    @expect-stderr-contains: <text>    substring that must appear in stderr
    @max-cycles: <int>                 cap, default 100000

A test may have multiple @expect-* directives; all must hold for PASS.
A test with neither @expect-trap nor @expect-exit is rejected.
"""
from __future__ import annotations

import argparse
import dataclasses
import re
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]   # repo root
ASMORISC = ROOT / "tools" / "asm" / "asmorisc"
SIMORISC = ROOT / "tools" / "sim" / "simorisc"
VALIDATION_DIR = Path(__file__).resolve().parent
DEFAULT_MAX_CYCLES = 100_000


# --- Expectation parsing -----------------------------------------------------

_DIRECTIVE_RE = re.compile(r"^\s*;\s*@([\w-]+):\s*(.*?)\s*$")
_C_ESCAPES = {
    "n": "\n", "t": "\t", "r": "\r",
    "\\": "\\", '"': '"', "0": "\0",
}


def _decode_string(quoted: str) -> bytes:
    """Parse a double-quoted string with C-style escapes into raw bytes."""
    if not (quoted.startswith('"') and quoted.endswith('"')):
        raise ValueError(f"expected quoted string, got {quoted!r}")
    body = quoted[1:-1]
    out = bytearray()
    i = 0
    while i < len(body):
        c = body[i]
        if c != "\\":
            out.extend(c.encode("utf-8"))
            i += 1
            continue
        i += 1
        if i >= len(body):
            raise ValueError("trailing backslash in string literal")
        esc = body[i]
        if esc == "x":
            if i + 2 >= len(body):
                raise ValueError("incomplete \\x escape")
            out.append(int(body[i + 1:i + 3], 16))
            i += 3
            continue
        if esc not in _C_ESCAPES:
            raise ValueError(f"unknown escape \\{esc}")
        out.append(ord(_C_ESCAPES[esc]))
        i += 1
    return bytes(out)


@dataclasses.dataclass
class Expectations:
    description: str = ""
    expect_exit: int | None = None
    expect_stdout: bytes | None = None
    expect_trap: str | None = None
    expect_trap_pc: int | None = None
    expect_stderr_contains: list[str] = dataclasses.field(default_factory=list)
    max_cycles: int = DEFAULT_MAX_CYCLES
    processors: int = 1

    @property
    def is_trap_test(self) -> bool:
        return self.expect_trap is not None


def parse_expectations(source: str) -> Expectations:
    exp = Expectations()
    for line in source.splitlines():
        if not line.strip():
            continue
        if not line.lstrip().startswith(";"):
            break
        m = _DIRECTIVE_RE.match(line)
        if not m:
            continue
        key, value = m.group(1), m.group(2)
        if key == "description":
            exp.description = value
        elif key == "expect-exit":
            exp.expect_exit = int(value, 0)
        elif key == "expect-stdout":
            exp.expect_stdout = _decode_string(value)
        elif key == "expect-stdout-hex":
            exp.expect_stdout = bytes.fromhex(value.replace(" ", ""))
        elif key == "expect-trap":
            exp.expect_trap = value
        elif key == "expect-trap-pc":
            exp.expect_trap_pc = int(value, 0)
        elif key == "expect-stderr-contains":
            exp.expect_stderr_contains.append(value)
        elif key == "max-cycles":
            exp.max_cycles = int(value, 0)
        elif key == "processors":
            exp.processors = int(value, 0)
        elif key == "category":
            pass    # informational
        else:
            raise ValueError(f"unknown directive @{key}")
    if exp.expect_exit is None and exp.expect_trap is None and \
            exp.expect_stdout is None:
        raise ValueError("test has no @expect-exit, @expect-trap, "
                         "or @expect-stdout directive")
    return exp


# --- Test execution ----------------------------------------------------------

@dataclasses.dataclass
class Result:
    name: str
    category: str
    description: str
    passed: bool
    failure_reason: str = ""
    stdout: bytes = b""
    stderr: str = ""
    exit_code: int = 0


_TRAP_LINE_RE = re.compile(
    r"simorisc: trap (?P<name>[\w-]+) "
    r"\(cause 0x[0-9a-fA-F]+\) "
    r"at PC=0x(?P<pc>[0-9a-fA-F]+)"
)


def _check(exp: Expectations, exit_code: int, stdout: bytes, stderr: str,
           result: Result) -> None:
    """Verify all expectations against captured output. Updates result in place."""
    if exp.expect_trap is not None:
        m = _TRAP_LINE_RE.search(stderr)
        if not m:
            result.failure_reason = (
                f"expected trap '{exp.expect_trap}' but no trap in stderr"
            )
            return
        if m.group("name") != exp.expect_trap:
            result.failure_reason = (
                f"expected trap '{exp.expect_trap}' but got '{m.group('name')}'"
            )
            return
        if exp.expect_trap_pc is not None:
            actual_pc = int(m.group("pc"), 16)
            if actual_pc != exp.expect_trap_pc:
                result.failure_reason = (
                    f"trap PC: expected 0x{exp.expect_trap_pc:08x}, "
                    f"got 0x{actual_pc:08x}"
                )
                return

    if exp.expect_exit is not None:
        if exit_code != exp.expect_exit:
            result.failure_reason = (
                f"exit code: expected {exp.expect_exit}, got {exit_code}"
            )
            return

    if exp.expect_stdout is not None:
        if stdout != exp.expect_stdout:
            result.failure_reason = (
                f"stdout mismatch:\n  expected: {exp.expect_stdout!r}\n"
                f"  got:      {stdout!r}"
            )
            return

    for needle in exp.expect_stderr_contains:
        if needle not in stderr:
            result.failure_reason = (
                f"stderr missing required substring {needle!r}"
            )
            return

    result.passed = True


def run_one(source_path: Path, build_dir: Path) -> Result:
    category = source_path.parent.name
    name = source_path.stem
    result = Result(name=name, category=category, description="", passed=False)

    source = source_path.read_text()
    try:
        exp = parse_expectations(source)
    except ValueError as e:
        result.failure_reason = f"directive parse error: {e}"
        return result
    result.description = exp.description

    orx_path = build_dir / category / f"{name}.orx"
    orx_path.parent.mkdir(parents=True, exist_ok=True)

    proc = subprocess.run(
        ["python3", str(ASMORISC), str(source_path), "-o", str(orx_path)],
        capture_output=True, text=True,
    )
    if proc.returncode != 0:
        result.failure_reason = (
            f"assembler failed (exit {proc.returncode}):\n{proc.stderr}"
        )
        return result

    sim_args = ["python3", str(SIMORISC),
                "--max-cycles", str(exp.max_cycles)]
    if exp.processors > 1:
        sim_args += ["--processors", str(exp.processors)]
    sim_args.append(str(orx_path))
    proc = subprocess.run(sim_args, capture_output=True)
    result.stdout = proc.stdout
    result.stderr = proc.stderr.decode("utf-8", "replace")
    result.exit_code = proc.returncode

    _check(exp, result.exit_code, result.stdout, result.stderr, result)
    return result


# --- CLI ---------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(
        description="Object RISC simulator validation suite runner",
    )
    ap.add_argument(
        "filter", nargs="?", default="",
        help="substring filter on category/name (default: run all)",
    )
    ap.add_argument(
        "-v", "--verbose", action="store_true",
        help="print description and per-test detail even on PASS",
    )
    ap.add_argument(
        "--build-dir", default="/tmp/orisc-validation",
        help="where to drop assembled .orx files (default: /tmp/orisc-validation)",
    )
    ap.add_argument(
        "--list", action="store_true",
        help="list discovered tests and exit",
    )
    args = ap.parse_args()

    build_dir = Path(args.build_dir)
    build_dir.mkdir(parents=True, exist_ok=True)

    sources = sorted(p for p in VALIDATION_DIR.glob("*/*.s") if p.is_file())
    if args.filter:
        sources = [p for p in sources
                   if args.filter in f"{p.parent.name}/{p.stem}"]

    if args.list:
        for p in sources:
            print(f"{p.parent.name}/{p.stem}")
        return 0

    if not sources:
        print("no tests matched", file=sys.stderr)
        return 1

    results = []
    current_category = None
    for src in sources:
        cat = src.parent.name
        if cat != current_category:
            print(f"\n=== {cat} ===")
            current_category = cat
        r = run_one(src, build_dir)
        results.append(r)
        marker = "PASS" if r.passed else "FAIL"
        if r.passed and not args.verbose:
            print(f"  {marker}  {r.name}")
        else:
            line = f"  {marker}  {r.name}"
            if r.description:
                line += f"  ({r.description})"
            print(line)
            if not r.passed:
                print(f"        {r.failure_reason}")

    passed = sum(1 for r in results if r.passed)
    failed = len(results) - passed
    print(f"\n{passed} passed, {failed} failed (of {len(results)})")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
