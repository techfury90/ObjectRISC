> ⚠️ **STATUS (2026-06-23, session 3): the "REFINED PLAN" below is NOT
> directly executable as scoped — its site map is incomplete and K=5 is
> insufficient. Both unblock paths are Phase-4-scale; the object API stays
> blocked by deliberate decision. Read "SESSION-3 FINDINGS" (after the
> REFINED PLAN) BEFORE attempting either path again.**

---

# ⚡ SESSION-5 RECON (2026-06-28): blocker RE-CONFIRMED on main; tight spike plan for the COMPILER fix

> ## ✅ LANDED — approach A (the compiler fix). The spike SUCCEEDED.
> **Root cause (refined):** NOT the OREFST result flag — pcc's coarse per-block call
> liveness (`regs.c::insnwalk`) marks ALL four OR `livecall()` regs live at a call and
> only releases the ones an argument consumes, so a void-arg call (`other()`) leaves OR
> arg-regs spuriously live to the top of the block; a single OR scratch upstream then
> interferes with all four → capability byte-spill → "op U*". Harmless on big GPR files,
> fatal only for the 4-wide OR class.
> **Fix (`PRUNE_CALLLIVE`, ~60 lines):** after a call's args are walked, clear the OR
> arg-regs it marked live but the call doesn't consume (a clobbered-unread reg is
> genuinely dead). RESTRICTED TO CLASSC → integer codegen untouched; target-gated (orisc
> only); cross-call values stay protected by the call-site interference edges. Files:
> `mip/regs.c` (PRUNE_CALLLIVE in insnwalk, CALL + UCALL paths), `arch/orisc/macdefs.h`
> (gate + rationale), `arch/orisc/local.c` (removed the line-143 uerror),
> `test_oref_spill.sh` + new `examples/cc/oref_callresult.c`.
> **Verified:** E/g2/E2 compile at K=4; ALL 37 OS translation units + supervisor.orx/
> shell.orx BYTE-IDENTICAL to baseline (zero runtime change); test_oref_spill/oref_calls/
> manyargs PASS; independently re-confirmed. NO register migration, NO ABI change.
> **Follow-on:** build the `__or`-value obj API on obj.h. The recon + spike plan below is
> retained for history.

techfury90's call: cross the `__or`-native bridge SOON — the handle API's 16-slot `OBJ_NHANDLE` ceiling + manual cap lifecycle is compounding tech debt for the object-dense north-stars (document arch = per-block caps; object-console = typed-OREF pipelines; capability widgets). The old "buildable on handles" finding held only for the *run model* (mdview's offset/len spans), NOT the *object model*.

**Decision: take the COMPILER fix (no OS/API risk), not the K=6 register migration.** This recon re-confirmed the blocker + pinned the sites so a fresh session starts loaded.

## Re-confirmed repro (rebuild pcc FIRST: `sh tools/cc/build.sh` — /tmp/pcc-build gets cleaned on reboot)
Minimal standalone-compile cases (decls: `void *__or echo(void *__or o); void other(void);`):
- **E2** `{ void *__or x=p; return x; }` → COMPILES (baseline).
- **E** `{ void *__or x=p; other(); return x; }` → FAILS `Cannot generate code, node ... op U*` — the SPURIOUS byte-spill of the OREFTY value across the call (nothing genuinely live; x is objstore-homed, reloaded per use).
- **g2** `{ void *__or x=echo(p); other(); return x; }` → FAILS at the `local.c:143` uerror (call-result-store guard).
Compile path: `$CPP -I<dir> ... f.c > f.i; $CCOM < f.i > f.s` (ccom is where codegen + the failure happen; no libc needed for the repro).

## Fix sites (line numbers on main 2026-06-28 — re-grep, code drifts)
- `arch/orisc/table.c:449-453` — the OREFST store pattern `{ ASSIGN, FOREFF|INCREG, SOREG/TOREF, SCREG/TOREF, NCREG, RDEST, "ZI" }`. **The `NCREG, RDEST` is the phantom** (a CLASSC result allocated even under FOREFF); zzzcode `I` (local2.c) loads the O12 anchor into that scratch then `orefst`.
- `arch/orisc/local.c:127-147` — the `clocal(ASSIGN)` uerror (line 143) pre-empting g2. **Remove LAST**, only after E compiles.
- `mip/regs.c` + `mip/reader.c` — the core spiller (`storemod`, `dospill`, `longtemp`/`shorttemp`/`shstore`, interference `addalledges` over `livecall`). Where "op U*" originates. SESSION-3's `MYSTOREMOD`/`spilloff`/`orisc_orhome` hooks are NOT on main (cleanly reverted) — re-land only for approach B.

## Target + two approaches (try A first)
Goal: **E compiles at K=4** (current register count — no migration). The spurious spill = the OREFST NCREG result treated as live across the call.
- **(A) Kill the phantom liveness (preferred; no spill-machinery change).** FOREFF→RNULL alone did NOT work (SESSION-3: the phantom is the OREFTY VALUE's live range, not the store's result flag). Work the interference/liveness: an OREFTY node re-materialized (OREFLD) at each use must NOT be in `livecall` across a call. Find why the OREFST scratch/result NCREG lands in the cross-call live set; make the store's scratch dead-after-store (its subtree must not escape).
- **(B) Route OREFTY spills to the objstore (fallback).** Re-land MYSTOREMOD/storemod override + `spilloff()` (SESSION-3 got the OFFSET right). Remaining bug: the within-block node-result reload mis-classes the spilled OREFTY to a CLASSB GPR pair → "op OREG". Fix the reload to emit `orefld` (CLASSC), not a GPR-pair load.

## Validation ladder (K=4, after EVERY codegen change)
1. E2 compiles; **E compiles** (the win); then remove local.c:143 → g2 compiles.
2. Runtime: `test_oref_spill` (exit 42), `test_oref_calls`, `test_manyargs`.
3. FULL device smoke (rebuild: `rm build/liborisc.ora; make -s lib; make -s -B`). A miscompile SILENTLY breaks the OS — also diff the `.s` of a representative libc file (task.c) before/after; any unexpected change is a red flag.
4. THEN build the `__or`-value obj API (obj.h → `void *__or` variants of obj_alloc/derive/send/recv) + convert one program as proof (Phase 3 proper).

## Fallback
If (A)+(B) stall after a bounded effort, the K=6 register migration (SESSION-3 site map, ~230 sites, public OR-ABI change) is the alternative — bigger + mechanical, no compiler-internals risk.

**RUN THE FIX FRESH — not the tail of a long session. OS-critical libc codegen with miscompilation risk.**

---

# REFINED PLAN (superseded — see SESSION-3 FINDINGS): free O15 → callee-saved CLASSC (K=5)

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

# SESSION-3 FINDINGS (2026-06-23): both paths are Phase-4-scale

A focused session executed the REFINED PLAN and, in parallel, probed a
compiler-only alternative. Both hit walls. Decision: **accept the
call-result-store / `__or`-value-object-API limitation for now** (same
as the 2026-06-23 decision in `project_or_spill_phase2`). Everything
below was reverted; `main`/baseline is untouched and green.

## Empirical register thresholds (standalone compile, not runtime)
Mark O15 (and O14/O11) `SCREG|PERMREG`, COLORMAP CLASSC `num<K`, disable
the local.c call-result-store uerror, then compile:
- **K=4** (today): `g2`/`E` fail. `E` is `void *__or x=p; other(); return x`.
- **K=5** (free O15 only — the REFINED PLAN's bet): `g2` AND `E` STILL
  FAIL. Only `g3` (`x=echo(p); use(x); use(x); return x`) newly compiles.
  ⇒ **K=5 is insufficient; the REFINED PLAN's headline does not reach the
  goal.** (The plan's own step-4 fallback "free a 2nd register for K=6"
  is what's actually needed.)
- **K=6** (free O15 + a 2nd reg): `g2`, `E`, and a two-`__or`-value
  case all compile. But this is COMPILE-ONLY with INCOMPLETE callee-save
  (no objstore save/restore) — NOT runtime-validated.

## Why the register migration is Phase-4-scale (REFINED PLAN site map is wrong)
The plan's "~32 sites, 13 files" covered libc + 3 OS programs only. That
migration was done and is mechanically correct (codegen verified: ALLOC
1576→1584 ⇒ 1712 bytes, boot-data slot at byte 1704 just past the 1696
anchor, `orefst o3, 1704(o12)` in task_init parks boot O3 — which DOES
survive ObjAllocStore #0x106; the sim only writes GPR2+OPR1). **But it
broke the OS** (`test_hostfs`, `test_wm_smoke`, `test_vec_smoke`,
`test_raster_smoke`), because:
- The **boot-OR contract is a PUBLIC API** (liborisc.h §"host_io"):
  *"O11=boot stack, O14=boot self, O15=boot data — program parks o2/o4/o3
  here at startup."* EVERY program that uses term/host_io/grid/raster/
  vector follows it and reads O15 directly.
- The plan's grep scoped `tools/cc/lib` + `ouroboros` and **missed
  `examples/` and the inline test programs**: ~9 example C programs
  (vec_smoke, raster_smoke, wm_smoke, fb_smoke, fb_local_smoke, ptr_smoke,
  kbd_echo, paint, host_cat) each have their OWN `restore_or_state()`
  doing `omov o3, o15` (23 O15 refs; O11 ~20 more), plus raw-asm demos
  (hello_terminal.s, parallel_primes.s, chunkboot.s) and `test_hostfs.sh`'s
  inline program.
- Several of those (host_cat, kbd_echo, paint, the asm demos, test_hostfs)
  **never call task_init**, so they have NO O12 table and CANNOT read an
  O12 slot without being reworked to set one up.
- Programs that DO call task_init (vec_smoke etc.) still broke until THEIR
  `restore_or_state` is migrated too — and term_init's masking re-park is
  why `test_shell`/`test_supervisor` happened to still pass.
⇒ Freeing O15 is a **public-API contract change touching every program**
(~230 sites incl. O11), exactly the Phase-4 effort the memory shelved.
To do it: change the contract so task_init parks boot refs into O12 slots
and ALL programs call task_init + drop manual O-register parking; convert
or carve out the raw-asm demos; update liborisc.h's documented contract.

## Why the compiler-only alternative is also deep (and the REAL root cause)
Alternative probed: route OREFTY (capability) spills to the per-frame
OBJSTORE instead of the (impossible) byte stack. Hooks landed cleanly —
`MYSTOREMOD`+a `storemod` override rewrite an OREFTY spill node into the
OREG-of-TOREF "home" leaf, and a `spilloff()` helper at regs.c's three
spill sites (longtemp/shstore/shorttemp) gives it an objstore slot
(`orisc_orspill_slot()` bumping `orisc_orhome`). The OFFSET routing works.
BUT pcc's spill machinery has THREE paths and OREFTY breaks two:
- `longtemp` (named cross-block temps) → clean storemod rewrite: OK.
- `shorttemp`/`shstore`/`dospill` (within-block NODE-RESULT spills) → tree
  surgery (`*r=*p`, in-place rewrite) that mis-classes the OREFTY reload
  to a CLASSB GPR pair (`r16!r17!`) ⇒ "Cannot generate code op OREG".
- Routing CLASSC node-results to `longtemp` instead **infinite-loops**
  (longtemp only rewrites TEMP nodes, not node-results).

**Deeper finding — the spills are SPURIOUS (false pressure):** `E`
(`x=p; other(); return x`) generates the SAME 2-CREG code as `E2`
(`x=p; return x`) plus a harmless `jal other` — NOTHING is live across
the call (x lives in its objstore home). Yet pcc spills the FOR-EFFECT
result of the `x=p` OREFST across the call (confirmed: storemod is invoked
on the ASSIGN node, op 49). The OREFST pattern's `NCREG RDEST` leaves a
phantom live CREG the allocator thinks must survive the call. So the real
fix is to KILL the spurious spill (table.c OREFST result semantics /
liveness), NOT to make capability spilling work — and that's a careful
pcc-internals change with miscompilation risk to an OS-critical libc.

## Bottom line for the next attempt
Pick ONE and budget for Phase-4:
1. **Compiler (preferred — no OS/API risk):** fix the spurious cross-call
   OREFTY spill (false pressure from OREFST `NCREG RDEST` / the
   store-for-effect leaving a live CREG). If that alone makes g2/E compile
   at K=4, the whole register migration becomes unnecessary.
2. **Register migration (K=6):** the full public-API contract change
   above — convert every program + the contract doc + the asm demos.
Re-run the K=4/5/6 experiment first to re-confirm thresholds (code drifts).

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
