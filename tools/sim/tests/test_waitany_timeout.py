#!/usr/bin/env python3
"""test_waitany_timeout.py — unit test for the WaitAnyQueue (#0x206) optional
wall-clock timeout (R6 microseconds, 0 = infinite).

Drives `System._try_unblock_queue_set` — the resume path — directly with a mock
CPU. That method reads only the cpu + the BlockedOnQueueSet (never `self`), so we
can exercise it without booting a full machine:

  - an EXPIRED deadline resumes the CALL with ERR_ETIMEOUT (R3=0, PC advanced by
    4 on both pc/next_pc, blocked_on cleared, returns True);
  - an INFINITE wait (deadline None, the historic R6=0 behaviour) and a
    NOT-YET-DUE deadline both stay parked (return False, no register writes);
  - a genuinely READY queue still resumes ERR_OK even with a deadline set — the
    timeout edit must not disturb the normal wake.
"""
import importlib.machinery
import importlib.util
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
_loader = importlib.machinery.SourceFileLoader("simorisc", str(ROOT / "tools/sim/simorisc"))
sim = importlib.util.module_from_spec(importlib.util.spec_from_loader("simorisc", _loader))
_loader.exec_module(sim)


class Desc:
    def __init__(self, queue):
        self.queue = queue


class MockCPU:
    def __init__(self, descriptors=None):
        self.descriptors = descriptors if descriptors is not None else [None]
        self.pc = 0x1000
        self.next_pc = 0x1004
        self.blocked_on = "parked"
        self.gpr = {}

    def set_gpr(self, i, v):
        self.gpr[i] = v


def resume(cpu, block):
    # self is unused by _try_unblock_queue_set, so None is fine.
    return sim.System._try_unblock_queue_set(None, cpu, block)


fails = 0
def check(cond, msg):
    global fails
    if not cond:
        print("FAIL:", msg)
        fails += 1


# 1. Expired deadline → ERR_ETIMEOUT, PC advanced, unblocked.
cpu = MockCPU()
r = resume(cpu, sim.BlockedOnQueueSet([], deadline=time.time() - 1.0))
check(r is True, "expired deadline should unblock (return True)")
check(cpu.gpr.get(2) == sim.ERR_ETIMEOUT, f"R2 should be ERR_ETIMEOUT(7), got {cpu.gpr.get(2)}")
check(cpu.gpr.get(3) == 0, "R3 should be 0")
check(cpu.pc == 0x1004 and cpu.next_pc == 0x1008,
      f"PC must advance by 4 (both): pc={cpu.pc:#x} next_pc={cpu.next_pc:#x}")
check(cpu.blocked_on is None, "must clear blocked_on")

# 2. Infinite wait (deadline None, i.e. R6=0) → stay parked.
cpu = MockCPU()
r = resume(cpu, sim.BlockedOnQueueSet([], deadline=None))
check(r is False and cpu.blocked_on == "parked", "infinite wait must stay parked")
check(cpu.gpr.get(2) is None, "infinite wait must not touch R2")

# 3. Not-yet-due deadline, no ready queue → stay parked.
cpu = MockCPU()
r = resume(cpu, sim.BlockedOnQueueSet([], deadline=time.time() + 100.0))
check(r is False and cpu.blocked_on == "parked", "future deadline must stay parked")

# 4. A ready queue still resumes ERR_OK even with a deadline set.
cpu = MockCPU(descriptors=[None, Desc([("msg",)])])   # idx 1 holds a non-empty queue
r = resume(cpu, sim.BlockedOnQueueSet([1], deadline=time.time() + 100.0))
check(r is True, "ready queue should unblock (return True)")
check(cpu.gpr.get(2) == sim.ERR_OK, f"R2 should be ERR_OK, got {cpu.gpr.get(2)}")
check(cpu.blocked_on is None, "ready queue must clear blocked_on")

if fails == 0:
    print("PASS: WaitAnyQueue timeout — ETIMEOUT on expiry, parked on infinite/future, OK on ready")
import sys
sys.exit(1 if fails else 0)
