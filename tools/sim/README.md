# simorisc — Object RISC ISA Simulator

A pure-Python (3.10+, stdlib only) instruction-set simulator for the
Object RISC architecture. Loads `.orx` binaries per `CONTRACT.md`, sets
up the initial task state, and executes Object RISC instructions with
the architectural semantics of Volume II. Supports both single-CPU and
multi-CPU configurations connected by a wire-level crossbar.

When run multi-process, an external [`oriscbar`](oriscbar) is the
crossbar daemon and `simorisc --bar <socket>` is one CPU client of
it. Devices (such as the Tk-based [`oriscterm`](../devices)) speak
the same wire protocol — see
[`tools/devices/README.md`](../devices/README.md) for the full
attachment lifecycle, framing, and SEND-payload conventions.

## Usage

    tools/sim/simorisc <program.orx>
    tools/sim/simorisc --processors 2 <program.orx>     # multi-CPU
    tools/sim/simorisc --trace <program.orx>            # per-instruction trace
    tools/sim/simorisc --max-cycles 50000 <program.orx>

`--trace` writes one line per retired instruction to stderr (so stdout
remains the program's output), plus crossbar packet hex dumps when
multiple CPUs talk to each other:

    CPU0 0x00010008  omov o1, o5                       ; o1 = ...
    CPU0 0x00010028  send o1
      ; CROSSBAR CPU0 -> CPU1 SEND_DELIVER trans=0 len=14w bytes=000120000000...
    CPU1 0x00010060  omov o1, o3
      ; CROSSBAR CPU1 -> CPU0 OBJ_READ_REQ trans=0 len=4w bytes=01001000...
      ; CROSSBAR CPU0 -> CPU1 OBJ_READ_RESP trans=0 len=1w bytes=00011100...

## Build & test

    sh tools/sim/tests/run-tests.sh                          # hello-world smoke
    python3 tools/sim/tests/test_wire_format.py              # wire-format unit tests
    python3 tools/sim/tests/validation/runner.py             # full validation suite

## What's implemented

### ISA (Volume II)
- All Sections 5–11 instructions: integer arithmetic, GPR loads/stores,
  branches and jumps with one-instruction architectural delay slots,
  object register ops (`OMOV/ONULL/OEQ/OISN/OLEN/OTAG/OHOME/OCAP`),
  object loads/stores (`OL{B,BU,H,HU,W}/OS{B,H,W}`) with
  null/stale/bounds/capability checks, `OREFLD`/`OREFST` for OR-typed
  storage with the OBJSTORE flag, `OFENCE`, `SEND`, and `CALL`.

### Firmware primitives
- **`0x001 TaskExit`** — terminates the current task; on a CPU's last
  task this contributes to the simulator's exit code.
- **`0x100 ObjAlloc`** / **`0x106 ObjAllocStore`** — allocate
  byte-typed and OR-typed (OBJSTORE) objects.
- **`0x101 ObjFree`** — bumps generation, recycles slot.
- **`0x103 ObjDerive`** — produces a weaker reference.
- **`0x110 MapObject`** — installs a VA→object mapping with R/W/X
  protection bounded by the calling reference's caps.
- **`0x200 InstallHandler`** — registers a SEND handler on a target
  object; works on any code object that's executably mapped on the
  local CPU (loadable modules, not just the boot text).
- **`0x203 ReceiveQueueAttach`** / **`0x204 ReceiveQueuePoll`** —
  per-object receive queues; poll blocks the CPU until a SEND arrives
  (or timeout).
- **`0x301 ReadCycles`** — returns the calling CPU's
  retired-instruction count (`R3`). Useful for benchmarking.
- **`0x320 ConsoleWrite`** — writes object storage bytes to host
  stdout.
- Every other primitive number returns `R2 = 4` (`ENOSYS`).

### Multi-CPU + wire-level crossbar
- `--processors N` (1 ≤ N ≤ 16) instantiates N CPUs sharing an
  `InProcessCrossbar`. The same `.orx` is loaded on each; programs
  branch on `R7` (PROCID).
- Each CPU receives a service object: `O4` = my own (full caps), `O5+`
  = the other CPUs' services (R+S only).
- All cross-CPU traffic is wire-format `SEND_DELIVER`,
  `OBJ_READ_REQ/RESP`, `OBJ_WRITE_REQ/RESP` packets per Volume IV §3,
  with proper headers, payload words, and trailing XOR checksums.
  Visible at `--trace` as `CROSSBAR …` lines with full packet hex
  dumps.
- Remote `OL*`/`OS*` blocks the issuing CPU until the home CPU's
  autonomous "memory controller" returns an `OBJ_READ_RESP` /
  `OBJ_WRITE_RESP`. The blocking is real — at the scheduler level —
  not a synchronous shortcut.
- The crossbar lives behind a `Crossbar` abstract interface; the
  in-process default routes by direct method dispatch but the same
  interface is what a future `SocketCrossbar` will implement to put
  CPUs (and devices) in separate processes.

### Trap model
- Precise exceptions; on any architectural trap the simulator prints
  the cause name and code, faulting PC, and a register-state slice,
  then exits non-zero. There is no firmware vector table — `CALL` goes
  directly to a Python primitive dispatcher.

## What's stubbed

- **Privileged instructions** (`LCTRL/SCTRL/ERET/WAIT/TLBP/TLBR/TLBWI/TLBWR`)
  decode to `reserved-instruction`. None of the demos exercise them.
- **The descriptor cache and TLB are not modeled as caches**: every
  access walks the (very small) VA→object map and looks up the
  descriptor directly. The generation-counter coherence model (Volume
  III §4) means this is observably equivalent.
- **Hypervisor primitives, time/clocks, and most device I/O** beyond
  the console are not implemented.
- **`DESC_REQ/RESP`, `DESC_FORWARD`, `DESC_INVALIDATE`** packet types
  are reserved in the simulator but not emitted: descriptor lookups
  for inspection ops (`OLEN/OTAG/OHOME/OCAP`) still go directly to
  the home CPU's table without a wire round-trip. Future revisions
  will add these once object migration is implemented.

## Design notes

- **Object table.** A flat Python `list[Optional[Descriptor]]` indexed
  by reference's `local table index` field. Generation is checked on
  every OL/OS dereference and at the home end of every wire request.
- **References** are raw 64-bit ints packed per Volume III §2.1.
  Helpers `make_ref()` and `ref_*()` keep the bit layout in one place.
- **Memory translation.** A list of `(va_lo, va_hi, descriptor_idx,
  base_offset, prot)` tuples populated by `init_cpu` (boot mappings)
  and `MapObject` (loadable modules / runtime allocations). Linear
  scan on every access — fine at the scale of a handful of mappings.
- **Branch delay slots** are modeled with a `next_pc` register: `pc`
  is the current instruction, `next_pc` is what `pc` becomes after
  retirement. `CALL` is special-cased to skip the delay slot per
  Volume II §11.
- **Wire packets.** `pack_packet`/`unpack_packet` plus
  per-message-type builders (e.g. `build_send_deliver`,
  `build_obj_read_req`) live near the top of the file. Each packet is
  `header (8B) | payload (4N B) | checksum (4B)`, all big-endian.
  The `Crossbar` interface dispatches packets by `dst_pid` to a
  registered `Port`; CPUs are Ports.
- **Blocking.** Two `BlockedSignal` subclasses: `BlockedOnQueue`
  (raised by `ReceiveQueuePoll`) and `BlockedOnResponse` (raised by
  remote OL/OS after queueing the request). The scheduler's
  `_try_unblock` resolves both by either delivering the awaited
  message and advancing PC, or trapping with the architectural cause
  on a non-OK response.
