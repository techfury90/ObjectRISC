/*
 * objslot.c — obj.h handle-table slot <-> O-register moves.
 *
 * Split out of obj.c (Phase 4) for a purely mechanical reason: growing
 * the handle table from 8 to 16 slots doubled the case count in these
 * six switches, and the extra generated labels pushed obj.c's pcc label
 * numbers past ~1000, tripping a backend asm-emission bug that corrupted
 * an unrelated `\n` in obj_send's SEND template (`addu r6, r4, r0` ran
 * into the next line). Keeping these (label-heavy, asm-only) switches in
 * their own small translation unit holds obj.c's label numbers down so
 * its fragile multi-line SEND asm emits cleanly. No behaviour change.
 *
 * Each function is a single immediate-offset OREFLD/OREFST per handle:
 * the architecture's OREFLD/OREFST take a CONSTANT 16-bit offset, so the
 * per-slot offset (OBJ_TABLE_OFFSET + h*8) can't be computed at runtime —
 * hence the switch. The offsets are hard-coded and guarded by the static
 * check in obj.c (OBJ_TABLE_OFFSET == 1704).
 *
 * Non-static (unlike obj.c's other internals) so obj.c can call them
 * across the TU boundary; the "OR survives the call" discipline holds —
 * pcc never models an O-register as a value, so the register each
 * function leaves set (or reads) is intact across the call, exactly as
 * when these were file-local to obj.c.
 */

#include "liborisc.h"   /* task_t, used by obj.h's obj_register_task decl */
#include "obj.h"

void
obj__load_o1(obj_t h)
{
	switch (h) {
	case 0: asm volatile("orefld o1, 1704(o12)"); break;
	case 1: asm volatile("orefld o1, 1712(o12)"); break;
	case 2: asm volatile("orefld o1, 1720(o12)"); break;
	case 3: asm volatile("orefld o1, 1728(o12)"); break;
	case 4: asm volatile("orefld o1, 1736(o12)"); break;
	case 5: asm volatile("orefld o1, 1744(o12)"); break;
	case 6: asm volatile("orefld o1, 1752(o12)"); break;
	case 7: asm volatile("orefld o1, 1760(o12)"); break;
	case 8: asm volatile("orefld o1, 1768(o12)"); break;
	case 9: asm volatile("orefld o1, 1776(o12)"); break;
	case 10: asm volatile("orefld o1, 1784(o12)"); break;
	case 11: asm volatile("orefld o1, 1792(o12)"); break;
	case 12: asm volatile("orefld o1, 1800(o12)"); break;
	case 13: asm volatile("orefld o1, 1808(o12)"); break;
	case 14: asm volatile("orefld o1, 1816(o12)"); break;
	case 15: asm volatile("orefld o1, 1824(o12)"); break;
	}
}

void
obj__load_o2(obj_t h)
{
	switch (h) {
	case 0: asm volatile("orefld o2, 1704(o12)"); break;
	case 1: asm volatile("orefld o2, 1712(o12)"); break;
	case 2: asm volatile("orefld o2, 1720(o12)"); break;
	case 3: asm volatile("orefld o2, 1728(o12)"); break;
	case 4: asm volatile("orefld o2, 1736(o12)"); break;
	case 5: asm volatile("orefld o2, 1744(o12)"); break;
	case 6: asm volatile("orefld o2, 1752(o12)"); break;
	case 7: asm volatile("orefld o2, 1760(o12)"); break;
	case 8: asm volatile("orefld o2, 1768(o12)"); break;
	case 9: asm volatile("orefld o2, 1776(o12)"); break;
	case 10: asm volatile("orefld o2, 1784(o12)"); break;
	case 11: asm volatile("orefld o2, 1792(o12)"); break;
	case 12: asm volatile("orefld o2, 1800(o12)"); break;
	case 13: asm volatile("orefld o2, 1808(o12)"); break;
	case 14: asm volatile("orefld o2, 1816(o12)"); break;
	case 15: asm volatile("orefld o2, 1824(o12)"); break;
	}
}

void
obj__load_o3(obj_t h)
{
	switch (h) {
	case 0: asm volatile("orefld o3, 1704(o12)"); break;
	case 1: asm volatile("orefld o3, 1712(o12)"); break;
	case 2: asm volatile("orefld o3, 1720(o12)"); break;
	case 3: asm volatile("orefld o3, 1728(o12)"); break;
	case 4: asm volatile("orefld o3, 1736(o12)"); break;
	case 5: asm volatile("orefld o3, 1744(o12)"); break;
	case 6: asm volatile("orefld o3, 1752(o12)"); break;
	case 7: asm volatile("orefld o3, 1760(o12)"); break;
	case 8: asm volatile("orefld o3, 1768(o12)"); break;
	case 9: asm volatile("orefld o3, 1776(o12)"); break;
	case 10: asm volatile("orefld o3, 1784(o12)"); break;
	case 11: asm volatile("orefld o3, 1792(o12)"); break;
	case 12: asm volatile("orefld o3, 1800(o12)"); break;
	case 13: asm volatile("orefld o3, 1808(o12)"); break;
	case 14: asm volatile("orefld o3, 1816(o12)"); break;
	case 15: asm volatile("orefld o3, 1824(o12)"); break;
	}
}

void
obj__load_o4(obj_t h)
{
	switch (h) {
	case 0: asm volatile("orefld o4, 1704(o12)"); break;
	case 1: asm volatile("orefld o4, 1712(o12)"); break;
	case 2: asm volatile("orefld o4, 1720(o12)"); break;
	case 3: asm volatile("orefld o4, 1728(o12)"); break;
	case 4: asm volatile("orefld o4, 1736(o12)"); break;
	case 5: asm volatile("orefld o4, 1744(o12)"); break;
	case 6: asm volatile("orefld o4, 1752(o12)"); break;
	case 7: asm volatile("orefld o4, 1760(o12)"); break;
	case 8: asm volatile("orefld o4, 1768(o12)"); break;
	case 9: asm volatile("orefld o4, 1776(o12)"); break;
	case 10: asm volatile("orefld o4, 1784(o12)"); break;
	case 11: asm volatile("orefld o4, 1792(o12)"); break;
	case 12: asm volatile("orefld o4, 1800(o12)"); break;
	case 13: asm volatile("orefld o4, 1808(o12)"); break;
	case 14: asm volatile("orefld o4, 1816(o12)"); break;
	case 15: asm volatile("orefld o4, 1824(o12)"); break;
	}
}

void
obj__store_o1(obj_t h)
{
	switch (h) {
	case 0: asm volatile("orefst o1, 1704(o12)"); break;
	case 1: asm volatile("orefst o1, 1712(o12)"); break;
	case 2: asm volatile("orefst o1, 1720(o12)"); break;
	case 3: asm volatile("orefst o1, 1728(o12)"); break;
	case 4: asm volatile("orefst o1, 1736(o12)"); break;
	case 5: asm volatile("orefst o1, 1744(o12)"); break;
	case 6: asm volatile("orefst o1, 1752(o12)"); break;
	case 7: asm volatile("orefst o1, 1760(o12)"); break;
	case 8: asm volatile("orefst o1, 1768(o12)"); break;
	case 9: asm volatile("orefst o1, 1776(o12)"); break;
	case 10: asm volatile("orefst o1, 1784(o12)"); break;
	case 11: asm volatile("orefst o1, 1792(o12)"); break;
	case 12: asm volatile("orefst o1, 1800(o12)"); break;
	case 13: asm volatile("orefst o1, 1808(o12)"); break;
	case 14: asm volatile("orefst o1, 1816(o12)"); break;
	case 15: asm volatile("orefst o1, 1824(o12)"); break;
	}
}

void
obj__store_o2(obj_t h)
{
	switch (h) {
	case 0: asm volatile("orefst o2, 1704(o12)"); break;
	case 1: asm volatile("orefst o2, 1712(o12)"); break;
	case 2: asm volatile("orefst o2, 1720(o12)"); break;
	case 3: asm volatile("orefst o2, 1728(o12)"); break;
	case 4: asm volatile("orefst o2, 1736(o12)"); break;
	case 5: asm volatile("orefst o2, 1744(o12)"); break;
	case 6: asm volatile("orefst o2, 1752(o12)"); break;
	case 7: asm volatile("orefst o2, 1760(o12)"); break;
	case 8: asm volatile("orefst o2, 1768(o12)"); break;
	case 9: asm volatile("orefst o2, 1776(o12)"); break;
	case 10: asm volatile("orefst o2, 1784(o12)"); break;
	case 11: asm volatile("orefst o2, 1792(o12)"); break;
	case 12: asm volatile("orefst o2, 1800(o12)"); break;
	case 13: asm volatile("orefst o2, 1808(o12)"); break;
	case 14: asm volatile("orefst o2, 1816(o12)"); break;
	case 15: asm volatile("orefst o2, 1824(o12)"); break;
	}
}
