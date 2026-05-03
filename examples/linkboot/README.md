# Generic link-boot loader

A small `.orx` that any extra Object RISC CPU can be booted with —
the loader announces itself to a "boot master" service on the
crossbar, waits for the master to SEND it a code reference, copies
the code into a fresh local code object, maps it executable, and
JRs into the loaded module. The loader's own `.orx` carries no
application code; the actual program is decided at runtime.

Useful for spinning up dynamic worker pools, demand-loaded service
handlers, or anything where a CPU's role isn't fixed when the
system is launched.

## Files

| File                   | Role                                                                                |
|------------------------|-------------------------------------------------------------------------------------|
| `gen_linkboot.py`      | Generator. Run this to produce the four files below.                                |
| `linkboot.s`           | Standalone loader `.orx` source.                                                    |
| `master.s`             | Standalone asm master `.orx` source. Drives one loader, sends an 8-instruction module. |
| `demo.s`               | Combined master + loader for `simorisc --processors 2`. Branches on PROCID.         |
| `run.sh`               | Multi-process runner with the asm master, under `oriscrun`.                         |
| `run_python_master.sh` | Multi-process runner with [`tools/devices/linkbootd`](../../tools/devices/linkbootd) (Python boot server) standing in for the asm master. Scales to N loader CPUs (`NCPUS=N`). |

`linkboot.s`, `master.s`, `demo.s`, and the validation test at
[`tools/sim/tests/validation/11_multicpu/13_linkboot_loader.s`](../../tools/sim/tests/validation/11_multicpu/13_linkboot_loader.s)
are all regenerated from `gen_linkboot.py`. Edit the generator, not
the outputs.

## Try it

```sh
# Single-process (in-process crossbar; faster, deterministic):
python3 examples/linkboot/gen_linkboot.py
python3 tools/asm/asmorisc examples/linkboot/demo.s -o /tmp/demo.orx
python3 tools/sim/simorisc --processors 2 /tmp/demo.orx
# → Booted!

# Multi-process with the asm master (one boot, fixed pid layout):
examples/linkboot/run.sh
# → [xbar] oriscbar READY ...
# → [cpu1] Booted!

# Multi-process with the Python boot server (linkbootd):
examples/linkboot/run_python_master.sh                # 1 loader
NCPUS=4 examples/linkboot/run_python_master.sh        # 4 loaders
# → [cpu1] Booted!
# → [cpu2] Booted!
# → [cpu3] Booted!
# → [cpu4] Booted!
```

## How it works

### Discovery: the announce

A linkboot CPU has no idea what code it's supposed to run. Boot:

1. The loader derives an `R|S`-only view of its own self-service
   (`O4` at boot) so it can hand the master a capability that's
   strong enough to SEND it a boot request, weak enough that the
   master can't free it or further derive it.
2. It attaches a receive queue to its own self-service (so the
   incoming boot request lands in a queue rather than spawning a
   handler task).
3. It SENDs to whatever ref is in `O5` — by simorisc convention,
   `O5` holds the lowest-PID other CPU's service ref. The
   announce payload carries:
   - `O2` = the derived `R|S` self-ref
   - `R4` = the loader's PROCID (informational)
   - `R5` = `0` (announcement protocol version)
4. It blocks on `ReceiveQueuePoll` waiting for the master's reply.

### Boot: the master replies

The master's responsibility is two SENDs deep:

1. Wait for the announce on its own self-service queue. When it
   arrives, the loader's `R|S` self-ref is in `O2` and the loader's
   PROCID is in `R3` (queue dispatch puts sender's `R4..R7` into
   `R3..R6`).
2. Build the boot SEND aimed at the loader:
   - `O1` = loader's R|S self-ref (recipient)
   - `O2` = code reference (any byte-typed object the loader can OL
     from; the master just hands over its own data segment, which
     contains the module image at offset 0)
   - `O3` = optional data ref to install as the module's `O3`
     (the demo passes the same ref; modules that need cleaner
     separation can pass a separate object)
   - `R4` = byte length of the module image (≤ 256 in v1; bumping
     `MAX_WORDS` in `gen_linkboot.py` raises this)
   - `R5` = entry offset within the loaded image (0 = start)
3. TaskExit. The receiver does the rest.

### Copy: unrolled OLW with early exit

The architecturally interesting bit. The loader can't `MapObject`
the source (`MapObject` requires `home == calling CPU`), and `OLW`'s
offset is encoded in the instruction (16-bit immediate, no register-
indexed form). So the loader can't write a register-driven copy
loop — every word it reads from the master needs a different
hardcoded offset.

Solution: the loader contains 64 unrolled `olw + sw + count + check + branch`
stanzas, one per word, with a `beq r1, r20, copy_done` after each
that bails out as soon as enough words have been copied. The `MAX_WORDS`
constant in `gen_linkboot.py` controls the upper bound; the loader's
text grows linearly (~5 instructions per word).

After the copy:

1. `ObjDerive` to drop the W cap (R|X|C only).
2. `MapObject` as R|X to get the executable VA.
3. Add the entry offset.
4. Set `O1` to the loaded code ref (so the module can read its own
   image — needed because remote `ConsoleWrite` can't see master's
   descriptors in `--connect` mode).
5. `JR` into the module. No return.

### The module

The demo module is 32 bytes of code followed by 8 bytes of inline
message (`"Booted!\n"`), copied as one 40-byte image. The module
just calls `ConsoleWrite` with `O1 = its own loaded code ref` and
offset 32, then TaskExits with code 0.

## Limits and where to go next

- **Module size capped at `MAX_WORDS * 4` bytes.** Hardcoded by the
  unrolled copy. Bumping it raises the loader's text size linearly.
- **No streaming protocol.** A single SEND carries the whole boot
  request; the module image lives entirely in one source object.
- **No version negotiation.** The announce's `R5 = 0` is a placeholder
  for a future protocol version; only one is defined.
- **`ConsoleWrite` from cross-process refs doesn't work.** The
  `simorisc` `ConsoleWrite` primitive walks descriptor tables
  directly; in `--connect` mode each process has only its own. The
  module-self-ref-in-`O1` trick sidesteps this for the demo, but a
  module that wants to read the master's data over the wire would
  need a different I/O path (or a fix in `simorisc` to route
  `ConsoleWrite` through `OBJ_READ_REQ`).
