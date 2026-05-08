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
LIBORISC_OBJS_S  := $(BUILD)/lib/preempt_handler.oro
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

# --- ouroboros programs ----------------------------------------------

PROGRAM_SRCS := $(wildcard ouroboros/programs/*.c)
PROGRAM_ORXS := $(patsubst ouroboros/programs/%.c,$(BUILD)/programs/%.orx,$(PROGRAM_SRCS))

# --- top-level targets ------------------------------------------------

.PHONY: all boot clean lib programs shell supervisor help

all: $(LIBORISC) $(SHELL_ORX) $(SUPERVISOR_ORX) $(PROGRAM_ORXS)

lib: $(LIBORISC)

shell: $(SHELL_ORX)

supervisor: $(SUPERVISOR_ORX)

programs: $(PROGRAM_ORXS)

boot: all
	bash scripts/boot.sh

clean:
	rm -rf $(BUILD)

help:
	@echo "Targets:"
	@echo "  make           — build liborisc + supervisor + shell + programs"
	@echo "  make boot      — build, then start Ouroboros"
	@echo "  make lib       — build just liborisc.ora"
	@echo "  make supervisor— build just the supervisor"
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
	$(ORLD) -o $@ $(RUNTIME) $(BUILD)/programs/shell.oro $(LIBORISC)

# Supervisor — Ouroboros's init / spawn server (Phase 45a). The
# top-level .orx that CPU 0's leader runs.

$(BUILD)/supervisor.oro: ouroboros/supervisor.c | $(BUILD)
	$(CPP)  $(CFLAGS) $< > $(@:.oro=.i)
	$(CCOM) < $(@:.oro=.i) > $(@:.oro=.s)
	$(ASMORISC) -r $(@:.oro=.s) -o $@

$(SUPERVISOR_ORX): $(BUILD)/supervisor.oro $(RUNTIME) $(LIBORISC) | $(BUILD)
	$(ORLD) -o $@ $(RUNTIME) $(BUILD)/supervisor.oro $(LIBORISC)

# Programs — uniform c→oro→orx pipeline.

$(BUILD)/programs/%.oro: ouroboros/programs/%.c | $(BUILD)/programs
	$(CPP)  $(CFLAGS) $< > $(@:.oro=.i)
	$(CCOM) < $(@:.oro=.i) > $(@:.oro=.s)
	$(ASMORISC) -r $(@:.oro=.s) -o $@

$(BUILD)/programs/%.orx: $(BUILD)/programs/%.oro $(RUNTIME) $(LIBORISC) | $(BUILD)/programs
	$(ORLD) -o $@ $(RUNTIME) $< $(LIBORISC)

# --- output dirs (order-only prereqs) ---------------------------------

$(BUILD) $(BUILD)/lib $(BUILD)/runtime $(BUILD)/programs:
	@mkdir -p $@
