/*
 * Object RISC backend for pcc — instruction selection patterns.
 *
 * STUB — empty pattern table. The compiler will refuse to generate
 * code for any node not matched by an entry below; right now the
 * only entries are the structural placeholders pcc requires (an
 * empty leading entry, the standard DF rewrites, the FREE sentinel,
 * and the tablesize declaration).
 *
 * The real work — hundreds of patterns covering integer arithmetic,
 * logicals, shifts, branches, loads/stores, function calls, plus the
 * Object RISC novel ops (OMOV/OEQ/OISN/OLEN/OTAG/OHOME/OCAP for
 * inspection, OL{B,BU,H,HU,W} / OS{B,H,W} for typed access,
 * OREFLD/OREFST for OBJSTORE access, OFENCE for ordering, SEND for
 * message passing) — comes next session.
 *
 * Reference templates: arch/nova/table.c (smallest, ~270 lines), the
 * top-level MIPS table.c (closest target shape, ~1300 lines).
 */

#include "pass2.h"

#define ANYSIGNED   TINT|TLONG|TSHORT|TCHAR
#define ANYUSIGNED  TUNSIGNED|TULONG|TUSHORT|TUCHAR
#define ANYFIXED    ANYSIGNED|ANYUSIGNED
#define TUWORD      TUNSIGNED|TULONG
#define TSWORD      TINT|TLONG
#define TWORD       TUWORD|TSWORD

struct optab table[] = {
/* First entry must be empty. */
{ -1, FOREFF, SANY, TANY, SANY, TANY, 0, 0, "", },

/*
 * Default catch-all rewrites — these tell pcc to break the operation
 * into smaller sub-operations and re-match. Without them, even basic
 * tree shapes won't decompose into something the (currently empty)
 * concrete pattern set can match.
 */
#define DF(x) FORREW, SANY, TANY, SANY, TANY, REWRITE, x, ""

{ UMUL,    DF(UMUL),    },
{ ASSIGN,  DF(ASSIGN),  },
{ STASG,   DF(STASG),   },
{ FLD,     DF(FLD),     },
{ OPLEAF,  DF(NAME),    },
{ OPUNARY, DF(UMINUS),  },
{ OPANY,   DF(BITYPE),  },

{ FREE, FREE, FREE, FREE, FREE, FREE, FREE, FREE, "stub: no orisc patterns yet\n" },
};

int tablesize = sizeof(table) / sizeof(table[0]);
