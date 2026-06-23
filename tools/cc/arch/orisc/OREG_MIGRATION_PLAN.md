# REFINED PLAN (execute this): free O15 → callee-saved CLASSC (K=5)

Supersedes the full 10-register migration below for the immediate goal:
make the `__or`-value object API viable. You do NOT need all 10 registers
— freeing ONE clean register gives K=5, which makes the call result's
degree-4 interference colourable (it currently byte-spills, impossible
for a capability). Proven by examples g (`x=echo(p);return x` — compiles)
vs g2 (`x=echo(p);other();return x` — spills at K=4).

## Why O15 (boot data ref)
Cleanest single register to free: single-role; SET by task_init (so no
pre-O12 hazard like O9's supervisor/WM mailbox, which is allocated before
the O12 table exists); no scratch-reuse tangle (unlike O13/O14); not
entangled with the supervisor terminal-pass-through override (unlike the
surfaces O5/O6/O7). ~32 asm sites across: tools/cc/lib/{task,term,host_io,
grid,raster,vector,orx,linkboot,wm,pointer}.c + ouroboros/{oriscwm,
supervisor}.c + ouroboros/programs/login.c. Re-grep `\bo15\b` before and
after — a missed read silently breaks the boot.

## Steps (do them in order; FULL device smoke after each)
1. Foundation: liborisc.h += `OR_BOOT_DATA_SLOT_OFFSET` (1704) and the
   `ORSTR(x)` stringify helper (`#define ORSTR_(x) #x` / `#define ORSTR(x)
   ORSTR_(x)`) so the slot offset splices into asm without a %N operand.
   task.c: ORX_STATE_BYTES 1576->1584 (the OR-spill anchor stays at 1696).
2. Migrate the O15 global -> the O12 slot (inert for the compiler; the OS
   just reads boot data from O12 instead of the register):
   - SET in task.c: drop `omov o15, o3`; after `omov o12, o1` add
     `orefst o3, " ORSTR(OR_BOOT_DATA_SLOT_OFFSET) "(o12)` (boot O3
     survives ObjAllocStore — it clobbers only O1 + GPRs).
   - term.c re-parks boot data the same way.
   - Every READ `omov o2,o15` / `omov o3,o15` ->
     `orefld o2/o3, " ORSTR(OR_BOOT_DATA_SLOT_OFFSET) "(o12)`.
   - Sweep all 13 files; confirm `grep -rn '\bo15\b'` is empty.
   - SMOKE: test_shell, test_wm_smoke, test_supervisor, test_kbd_echo,
     test_hostfs, test_concurrent, test_directory, test_vec_smoke,
     test_raster_smoke (rebuild libc: `rm build/liborisc.ora; make -s lib`,
     and `make -s -B` for the programs). Commit this as a standalone step.
3. Free + callee-save O15:
   - macdefs.h RSTATUS: O15 entry 0 -> SCREG|PERMREG (mkext regenerates
     permregs[]/NPERMREG from RSTATUS). COLORMAP CLASSC: `num < 5`.
   - local2.c prologue/eoftn: for each USED callee-saved CLASSC reg
     (scan p2env.p_regs CLASSC bits, i.e. regs 48..63 in p_regs[1]), save
     it to a reserved slot of the per-frame OBJSTORE on entry (OREFST) and
     restore on exit (OREFLD). Reserve those slots in the objstore layout
     (shift `__or` homes up past chain + reg-saves). THIS IS THE NEW,
     RISKY PIECE — the RSTATUS comment warns a naive PERMREG marking made
     pcc spill an OR's old value into an *adjacent* OR; the OBJSTORE save
     is what makes it correct. Validate carefully.
4. Validate the unblock: examples g and g2 (write them: `void *__or
   echo(void *__or o){return o;}` etc.), the call-result-store runtime
   test, test_oref_spill, test_oref_calls, test_manyargs, + FULL smoke.
   If g2 still spills at K=5, free a 2nd clean register (O11 boot stack,
   identical pattern) for K=6.
5. THEN build the __or-value object API (obj.h/obj.c): obj_alloc /
   obj_alloc_store / obj_derive / obj_free / obj_send / obj_recv / obj_eq
   / obj_isnull / obj_len / obj_tag, taking/returning `void *__or`, plus
   centralized OBJ_TAG_* / OBJ_CAP_* constants. Convert one program as a
   proof. (This is Phase 3 proper; user picked `__or`-value + one real
   conversion.)

Context: memory `project_or_spill_phase2`. Abandoned dead-end: a
hard-coded-O2 call-result store (clocal offset-bias mark + SORCALL /
zzzcode 'J') fixes only the isolated store, not g2 — don't revive it; the
register free is the real fix.

---

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
