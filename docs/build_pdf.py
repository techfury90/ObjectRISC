#!/usr/bin/env python3
"""Combine the seven Object RISC volumes into a single structured markdown,
ready for pandoc to typeset. Each volume becomes a chapter in the resulting
book; sections within a volume retain their original numbering."""

import re
from pathlib import Path

VOLUMES = [
    ('OVERVIEW.md',                    'I',   'Overview'),
    ('INSTRUCTION_SET.md',             'II',  'Instruction Set'),
    ('OBJECT_SYSTEM.md',               'III', 'Object System'),
    ('INTERCONNECT_PROTOCOL.md',       'IV',  'Interconnect Protocol'),
    ('REFERENCE_IMPLEMENTATION.md',    'V',   'Reference Implementation'),
    ('SYSTEM_FIRMWARE_INTERFACE.md',   'VI',  'System Firmware Interface'),
    ('PROGRAMMING_PRACTICE.md',        'VII', 'Programming Practice'),
]

base = Path(__file__).parent
parts = []

for fname, num, title in VOLUMES:
    text = (base / fname).read_text()

    # Find the first numbered section heading "## 1. ..." — everything before
    # it (the per-file title, subtitle, byline, separator) is dropped.
    match = re.search(r'^## 1\. ', text, re.MULTILINE)
    if not match:
        raise ValueError(f"No '## 1.' heading found in {fname}")
    body = text[match.start():]

    # Strip the trailing per-file colophon if present.
    body = re.sub(
        r'\n+— \*The Object RISC Architecture Group, 1986\*\s*$',
        '',
        body,
    )

    parts.append(f"# Volume {num} — {title}\n\n{body.strip()}\n")

combined = '\n'.join(parts) + (
    "\n\n# Colophon\n\n"
    "This reference comprises Volumes I through VII of the Object RISC "
    "architecture documentation, Revision 0.1, dated 1986. The volumes "
    "have been combined into a single document for ease of navigation "
    "and cross-reference. Page numbering is sequential across the work; "
    "the table of contents is hyperlinked.\n\n"
    "— *The Object RISC Architecture Group*\n"
)

out_path = base / 'OBJECT_RISC.md'
out_path.write_text(combined)
print(f"wrote {out_path} ({len(combined):,} chars, "
      f"{combined.count(chr(10)):,} lines)")
