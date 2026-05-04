# Dhrystone on Object RISC

A port of Reinhold Weicker's Dhrystone v2.1 benchmark (1984 + Andrew
C. Lowry's 1985 update) to the Object RISC C runtime.

## Try it

```sh
examples/cc/dhrystone/run.sh
# Iterations: 5000
# ...
# Cycles per iteration: 4065
#
# At nominal clock rates (Vol I §3):
#   16 MHz: ~3936 dhry/s   = ~2.2 DMIPS
#   20 MHz: ~4920 dhry/s   = ~2.8 DMIPS
```

Or with a different iteration count:

```sh
DHRY_RUNS=10000 examples/cc/dhrystone/run.sh
```

The "DMIPS" line normalizes against the historical VAX 11/780 baseline
(1757 dhrystones/sec = 1 DMIPS) and quotes the OR-1000's nominal
16 / 20 MHz clock rates from Volume I §3. The cycle count itself is
exact — measured via the `ReadCycles` firmware primitive (Vol VI
§8.1) — so the DMIPS figure you'd see on actual silicon scales
directly with whatever clock rate that silicon happens to run at.

## What's "ported" vs "verbatim"

The benchmark's algorithm and instruction mix are unchanged. The
adaptations to make it build under our C runtime:

- `printf` → `print_str` / `print_int` from liborisc.
- `malloc` → two static `Rec_Type` instances (`Rec_1`, `Rec_2`).
- `clock()` / `time()` → `read_cycles` and `time_now_us` from
  liborisc's clock.c.
- `Number_Of_Runs` hardcoded via `DHRY_RUNS` (no scanf prompt).
- `register` keyword dropped — pcc's orisc backend doesn't gain
  anything from it given the current allocator state.
- struct assignments (`*Ptr_Val_Par->Ptr_Comp = *Ptr_Glob;`)
  rewritten as `memcpy` calls — pcc's orisc backend doesn't yet
  generate STASG. Cycle cost is similar but not identical.
- Global variable order tweaked so all int-aligned globals come
  before the `char` ones — pcc's orisc backend doesn't currently
  emit `.align` directives between globals of different alignment
  requirements.

These are noted at the relevant spots in `dhry.c`. None of them
change the operations Dhrystone counts; they're just compatibility
shims.

## What the numbers mean (1986 context)

For comparison with what Object RISC's contemporaries could do:

| Machine                   | DMIPS |
|---------------------------|-------|
| VAX 11/780                | 1.0   |
| Object RISC OR-1000 16MHz | 2.2   |
| Object RISC OR-1000 20MHz | 2.8   |
| Intel 80386 (20 MHz)      | ~6    |
| Sun SPARC v7 (16.7 MHz)   | ~8    |
| MIPS R2000 (16.7 MHz)     | ~8    |

The OR-1000 lands in the lower half of period-RISC range. Most of
the gap to MIPS / SPARC is the toolchain — pcc's orisc backend is
a faithful port of the MIPS template but hasn't been tuned, so
each Dhrystone iteration costs ~4000 cycles instead of the
~500-cycle figure the canonical RISCs reported. With register
allocation improvements, inline struct copy, etc., we'd close most
of that gap. Dhrystone-as-shipped is a useful regression baseline
to measure that work against.
