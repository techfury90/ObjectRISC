# Device tests

Headless end-to-end tests that exercise the wire-level protocols
of the device tools — no Tk window required.

## Running

    bash tools/devices/tests/test_kbd_echo.sh

## What's here

| File                | Role                                                       |
|---------------------|------------------------------------------------------------|
| `fake_terminal.py`  | Headless stand-in for `oriscterm`. Connects to oriscbar at a chosen pid, accepts a keyboard-subscribe SEND, and emits a sequence of synthetic key events back to the subscriber. |
| `test_kbd_echo.sh`  | Builds `kbd_echo.orx`, launches oriscbar + fake_terminal + the demo CPU, sends `"AB"` followed by ESC, and verifies cpu0's stdout contains the expected echo lines. |

## Why a fake terminal

The real `oriscterm` requires a Tk window and a human pressing
keys, neither of which is testable in CI. `fake_terminal.py` does
the wire-level handshake exactly the way oriscterm does (per
[`tools/devices/README.md`](../README.md)), so the demo CPU can't
tell the difference — but it injects keystrokes via the
command line instead of the keyboard.

## Adding more tests

Each `test_*.sh` is self-contained: build the `.orx`, launch the
crossbar + a fake device + the CPU, assert on stdout/exit. See
`test_kbd_echo.sh` for the pattern.
