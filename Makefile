# Object RISC + Ouroboros — top-level build.
#
#   make            — build liborisc, the shell, and every program in
#                     ouroboros/programs/
#   make boot       — build everything, then start Ouroboros (oriscbar
#                     + oriscterm + hostfsd + the shell CPU). The shell
#                     comes up subscribed to the keyboard with the OS
#                     banner; type `help` for the command list.
#   make clean      — remove the entire build/ tree.
#
# Build artefacts land under build/. The Makefile expects pcc to live
# at $(PCC_BUILD); set PCC_BUILD=… on the command line if yours is
# elsewhere. The asmorisc/orld/oar tools come from the in-tree
# tools/asm and tools/ld.

PCC_BUILD ?= /tmp/pcc-build
CPP       := $(PCC_BUILD)/cc/cpp/orisc-unknown-none-cpp
CCOM      := $(PCC_BUILD)/cc/ccom/orisc-unknown-none-ccom
ASMORISC  := python3 tools/asm/asmorisc
ORLD      := python3 tools/ld/orld
OAR       := python3 tools/ld/oar

CFLAGS    := -I tools/cc/arch/orisc -I tools/cc/lib

BUILD     := build

# --- liborisc archive -------------------------------------------------
# Every .c under tools/cc/lib/ becomes a .oro, then they're combined
# (along with preempt_handler.oro from .s) into liborisc.ora.

LIBORISC_SRCS_C  := $(wildcard tools/cc/lib/*.c)
LIBORISC_OBJS_C  := $(patsubst tools/cc/lib/%.c,$(BUILD)/lib/%.oro,$(LIBORISC_SRCS_C))
# Hand-written asm members: the preempt handler, obj_or.s (the `void *__or`
# capability-value object API), and orvec.s (the growable capability array).
# The latter two are bare asm because their ops take `__or` params (and
# orvec_push loops over them), which would trigger pcc-orisc's per-frame
# OBJSTORE prologue — and that miscompiles here (see orvec.s / obj_or.h).
LIBORISC_OBJS_S  := $(BUILD)/lib/preempt_handler.oro $(BUILD)/lib/obj_or.oro \
                    $(BUILD)/lib/orvec.oro
LIBORISC_OBJS    := $(LIBORISC_OBJS_C) $(LIBORISC_OBJS_S)

LIBORISC := $(BUILD)/liborisc.ora

# --- shared runtime objects (crt0, console_io) ------------------------

CRT0_ORO := $(BUILD)/runtime/crt0.oro
CIO_ORO  := $(BUILD)/runtime/console_io.oro
RUNTIME  := $(CRT0_ORO) $(CIO_ORO)

# --- the shell + the supervisor ---------------------------------------
# The shell embeds a banner via -DBUILD_BANNER. Default to the
# production string; override with `make SHELL_BUILD_BANNER='"…"'`
# (e.g., `make SHELL_BUILD_BANNER='"Object RISC Shell (TEST)"'` from
# inside a test harness).
#
# Phase 45a/b: the shell is now a program loaded by the supervisor at
# boot, so it lives in $(BUILD)/programs/ alongside edit/dhry/etc.
# The supervisor (CPU 0's leader) is the new top-level .orx — built
# by `make supervisor`, included in `all` so `make boot` gets it.

SHELL_BUILD_BANNER ?= "Object RISC Shell"
SHELL_ORX          := $(BUILD)/programs/shell.orx
SUPERVISOR_ORX     := $(BUILD)/supervisor.orx
ORISCWM_ORX        := $(BUILD)/oriscwm.orx
TERMFW_ORX         := $(BUILD)/termfw.orx

# --- ouroboros programs ----------------------------------------------

PROGRAM_SRCS := $(wildcard ouroboros/programs/*.c)
PROGRAM_ORXS := $(patsubst ouroboros/programs/%.c,$(BUILD)/programs/%.orx,$(PROGRAM_SRCS))

# --- top-level targets ------------------------------------------------

.PHONY: all boot clean lib programs shell supervisor oriscwm termfw help

all: $(LIBORISC) $(SHELL_ORX) $(SUPERVISOR_ORX) $(ORISCWM_ORX) $(TERMFW_ORX) $(PROGRAM_ORXS)

lib: $(LIBORISC)

shell: $(SHELL_ORX)

supervisor: $(SUPERVISOR_ORX)

oriscwm: $(ORISCWM_ORX)

termfw: $(TERMFW_ORX)

programs: $(PROGRAM_ORXS)

boot: all
	bash scripts/boot.sh

clean:
	rm -rf $(BUILD)

help:
	@echo "Targets:"
	@echo "  make           — build liborisc + supervisor + WM + shell + programs"
	@echo "  make boot      — build, then start Ouroboros"
	@echo "  make lib       — build just liborisc.ora"
	@echo "  make supervisor— build just the supervisor"
	@echo "  make oriscwm   — build just the window manager"
	@echo "  make shell     — build just the shell"
	@echo "  make programs  — build just the programs"
	@echo "  make clean     — remove build/"

# --- pattern rules ----------------------------------------------------

# Each .c → .oro: cpp + ccom + asmorisc -r. Output dir created by an
# order-only prerequisite. We keep the .i and .s as side-effects in
# the build dir for inspection.

$(BUILD)/lib/%.oro: tools/cc/lib/%.c | $(BUILD)/lib
	$(CPP)  $(CFLAGS) $< > $(@:.oro=.i)
	$(CCOM) < $(@:.oro=.i) > $(@:.oro=.s)
	$(ASMORISC) -r $(@:.oro=.s) -o $@

$(BUILD)/lib/preempt_handler.oro: tools/cc/lib/preempt_handler.s | $(BUILD)/lib
	$(ASMORISC) -r $< -o $@

$(BUILD)/lib/obj_or.oro: tools/cc/lib/obj_or.s | $(BUILD)/lib
	$(ASMORISC) -r $< -o $@

$(BUILD)/lib/orvec.oro: tools/cc/lib/orvec.s | $(BUILD)/lib
	$(ASMORISC) -r $< -o $@

$(LIBORISC): $(LIBORISC_OBJS) | $(BUILD)
	$(OAR) c $@ $(LIBORISC_OBJS)

# Runtime objects (crt0, console_io) come from the arch tree.

$(CRT0_ORO): tools/cc/arch/orisc/crt0.s | $(BUILD)/runtime
	$(ASMORISC) -r $< -o $@

$(CIO_ORO): tools/cc/arch/orisc/console_io.s | $(BUILD)/runtime
	$(ASMORISC) -r $< -o $@

# Shell — special-cased so we can pass -DBUILD_BANNER.
# Lives under $(BUILD)/programs/ now (Phase 45a) so the supervisor
# can hostfsd-load it from /programs/shell.orx at boot.

$(BUILD)/programs/shell.oro: ouroboros/shell.c | $(BUILD)/programs
	$(CPP)  $(CFLAGS) -DBUILD_BANNER='$(SHELL_BUILD_BANNER)' $< > $(@:.oro=.i)
	$(CCOM) < $(@:.oro=.i) > $(@:.oro=.s)
	$(ASMORISC) -r $(@:.oro=.s) -o $@

$(SHELL_ORX): $(BUILD)/programs/shell.oro $(RUNTIME) $(LIBORISC) | $(BUILD)/programs
	$(ORLD) --local-ok -o $@ $(RUNTIME) $(BUILD)/programs/shell.oro $(LIBORISC)

# Supervisor — Ouroboros's init / spawn server (Phase 45a). The
# top-level .orx that CPU 0's leader runs.

$(BUILD)/supervisor.oro: ouroboros/supervisor.c | $(BUILD)
	$(CPP)  $(CFLAGS) $< > $(@:.oro=.i)
	$(CCOM) < $(@:.oro=.i) > $(@:.oro=.s)
	$(ASMORISC) -r $(@:.oro=.s) -o $@

$(SUPERVISOR_ORX): $(BUILD)/supervisor.oro $(RUNTIME) $(LIBORISC) | $(BUILD)
	$(ORLD) -o $@ $(RUNTIME) $(BUILD)/supervisor.oro $(LIBORISC)

# oriscwm — Ouroboros's window manager (.orx, runs on its own CPU).
# The CPU it runs on dir-walks /sys/term/0/{console,keyboard} for
# the underlying surface caps and registers itself at /sys/wm/0
# so client programs (the leader supervisor, etc.) can discover it.

$(BUILD)/oriscwm.oro: ouroboros/oriscwm.c | $(BUILD)
	$(CPP)  $(CFLAGS) $< > $(@:.oro=.i)
	$(CCOM) < $(@:.oro=.i) > $(@:.oro=.s)
	$(ASMORISC) -r $(@:.oro=.s) -o $@

$(ORISCWM_ORX): $(BUILD)/oriscwm.oro $(RUNTIME) $(LIBORISC) | $(BUILD)
	$(ORLD) -o $@ $(RUNTIME) $(BUILD)/oriscwm.oro $(LIBORISC)

# termfw — Object RISC terminal firmware boot image (.orx).  M1: framebuffer
# self-test + Lucida Typewriter splash; standalone (no supervisor/WM/dir).  In
# ouroboros/ (not programs/) because it #includes wm_fonts.h from the same dir.

$(BUILD)/termfw.oro: ouroboros/termfw.c | $(BUILD)
	$(CPP)  $(CFLAGS) $< > $(@:.oro=.i)
	$(CCOM) < $(@:.oro=.i) > $(@:.oro=.s)
	$(ASMORISC) -r $(@:.oro=.s) -o $@

$(TERMFW_ORX): $(BUILD)/termfw.oro $(RUNTIME) $(LIBORISC) | $(BUILD)
	$(ORLD) -o $@ $(RUNTIME) $(BUILD)/termfw.oro $(LIBORISC)

# Programs — uniform c→oro→orx pipeline.

$(BUILD)/programs/%.oro: ouroboros/programs/%.c | $(BUILD)/programs
	$(CPP)  $(CFLAGS) $< > $(@:.oro=.i)
	$(CCOM) < $(@:.oro=.i) > $(@:.oro=.s)
	$(ASMORISC) -r $(@:.oro=.s) -o $@

# Interactive UI apps prefer co-resident (on-terminal) execution — being on the
# same CPU as the WM avoids a cross-CPU RPC per console/grid op.  Linked with
# orld --local-ok (sets the .orx LOCAL_OK flag); the supervisor honours it when
# the app is launched from a terminal.  All other programs default to compute.
$(BUILD)/programs/mdview.orx $(BUILD)/programs/mouse_paint.orx $(BUILD)/programs/font_demo.orx: ORLD_LOCAL_FLAG := --local-ok

$(BUILD)/programs/%.orx: $(BUILD)/programs/%.oro $(RUNTIME) $(LIBORISC) | $(BUILD)/programs
	$(ORLD) $(ORLD_LOCAL_FLAG) -o $@ $(RUNTIME) $< $(LIBORISC)

# --- output dirs (order-only prereqs) ---------------------------------

$(BUILD) $(BUILD)/lib $(BUILD)/runtime $(BUILD)/programs:
	@mkdir -p $@
