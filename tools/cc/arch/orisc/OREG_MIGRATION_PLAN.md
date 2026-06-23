# O-register migration → callee-saved CLASSC (unblocks `__or` call-result stores)

## Why

Phase 2 homes `__or` autos/params in a per-frame OBJSTORE and works for
params, loads, non-call stores, returns, and capabilities held across
calls. The one rejected case is storing a capability **call result**
into an `__or` home (`void *__or o = obj_alloc(...);`): the OREFST needs
a base scratch O-reg, but a call clobbers every caller-saved O-reg and,
with only 4 ORs in the file (O1–O4), the call result interferes with all
its sibling clobbers, so the allocator tries to byte-spill a capability
(impossible). The fix is **callee-saved CLASSC registers** so the result
(degree 4) becomes colourable and a value can live across a call.

**Hard prerequisite (the reason for this migration):** there is no free
O-register. O5–O11 and O13–O15 all hold long-lived capabilities that
libc reads *mid-function* via inline asm (`send o9`, `omov o2,o11`, …).
Callee-save/restore (save-at-entry/restore-at-exit) does NOT preserve a
value libc reads in the middle of a function via a libc call. So each
register must first stop being an asm-global: move its capability into a
slot of the O12 task-table objstore and `orefld` it into a scratch O-reg
(O1–O4, clobber-listed) at each use. Only then is the register a true
scratch the compiler can own (caller- or callee-saved) soundly.

## Complete site map

A full file:line map of every SET and READ site per register was
produced (Explore sweep, this session). Totals (READ sites):
O5=5, O6=4, O7=6, O8=16, O9=6, O10=4, O11=26, O13=15, O14=20, O15=21
(~117 reads + ~35 sets ≈ 150 sites across task.c, dir.c, orx.c, sup.c,
wm.c, pointer.c, vector.c, raster.c, term.c, grid.c, host_io.c,
linkboot.c). Re-run the map before starting (code drifts).

## Per-register transform

- SET `omov o<G>, oX`            → `orefst oX, SLOT_<G>(o12)`
- READ `omov oX, o<G>`           → `orefld oX, SLOT_<G>(o12)`
- READ `send o<G>` / `oisn r,o<G>` / `orefld oX,off(o<G>)` (o<G> as the
  objstore/recipient operand) → `orefld o<scratch>, SLOT_<G>(o12)` then
  use o<scratch>; add o<scratch> to the asm clobber list.

Pick the scratch from O1–O4 (caller-saved). Many sites already copy o<G>
into O1/O2/O3 for a SEND — those collapse to a single `orefld o1/2/3,
SLOT(o12)` (no extra instruction).

**Watch out:**
- O13 and O14 are *also reused as local scratch* inside dir.c / sup.c /
  orx.c (not as the boot-global). Those uses don't migrate to a slot —
  they need a different scratch once O13/O14 become compiler-owned, and
  the asm must clobber-list them. Audit each O13/O14 site as global vs
  scratch.
- O14 in orx_task_create probes override slots into O14 — pure scratch.
- O5/O6/O7/O8 already have ORX_SLOT_*_SAVE override slots; reuse vs add a
  persistent resident slot (decide per reg).
- The O11/O15 prologue "`omov o2,o11` / `omov o3,o15`" restore idiom
  (restore the boot stack/data refs into O2/O3 at function entry) appears
  ~25×/~21× — these become `orefld o2, SLOT_O11(o12)` etc.

## O12 slot layout

Add resident slots after the current end (ORX_STATE_BYTES is 1576 today;
the OR-spill anchor is at 1696). Append 8-byte slots for each migrated
global (O5,O6,O7,O8,O9,O10,O11,O13,O14,O15 boot/long-lived roles), bump
ORX_STATE_BYTES, and set them at task_init (boot O1/O2/O3/O4 harvest) and
the service inits (term_init, pointer_subscribe, hf_init). Keep the
anchor slot offset (macdefs.h ORSPILL_ANCHOR) ABOVE the new slots, or
renumber it and task.c together.

## Then the compiler side (the actual fix)

1. RSTATUS (macdefs.h): mark the freed regs (e.g. O9,O10,O11 — NOT O12,
   the task table / anchor base) `SCREG|PERMREG`. mkext regenerates
   `permregs[]` / `NPERMREG`. COLORMAP CLASSC → num < (4 + #callee-saved).
2. Prologue/epilogue (local2.c): for each used PERMREG CLASSC reg
   (p2env.p_regs CLASSC bits), OREFST it to a reg-save slot in the
   per-frame OBJSTORE on entry, OREFLD it back on exit. Reserve those
   slots (shift ORSPILL_BASE up past chain + reg-saves). This is the
   piece the RSTATUS comment warned about ("naive PERMREG spilled into an
   adjacent OR") — the OBJSTORE save is what makes it correct.
3. Remove the clocal(ASSIGN) `OREG(OREFTY)=CALL` uerror (local.c).
4. Validate: `void *__or o = obj_alloc(...); use(o); use(o);` compiles +
   runs; full device smoke suite passes after EACH register migrates
   (a missed read site breaks the OS silently — validate incrementally,
   never migrate two registers between smoke runs).

## Sequencing (lowest risk first)

1. O10 (pointer, isolated to pointer.c) — pilot the SET/READ transform.
2. O9 (term mailbox, term.c).
3. O5/O6/O7 (surfaces).
4. O8 (hostfsd mailbox, 16 sites).
5. O11, O15 (boot stack/data, ~26/~21 sites — the heavy ones).
6. O13, O14 (boot code/self + scratch reuse — most delicate).
Then the compiler-side callee-saved steps (1–4 above). Smoke-test after
every single register.
