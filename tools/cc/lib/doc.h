/*
 * doc.h — the native document model (v1): a Document is a flat, growable
 * sequence of Blocks, built on the base types (orvec of block refs + an orbuf
 * text-log). The object-native generalisation of mdview's flat display list
 * (doc[] bytes + parallel (kind,off,len) item arrays): here each block is a
 * first-class CAPABILITY object, the sequence is unbounded, and blocks that
 * scroll out of view can be FROZEN (serialised to the log + freed) so live
 * descriptors stay ≈ the viewport while the document grows without bound.
 *
 * A BLOCK is a self-contained BYTE object (not an OR-header) — a fixed 16-byte
 * header followed by its text inline:
 *
 *     off 0:  kind      (u32, BLK_*)
 *     off 4:  style     (u32, a style-id; the Style object/table is deferred —
 *                        v1 is one style per block, TAG_STYLE unused yet)
 *     off 8:  text_len  (u32, bytes of text)
 *     off 12: reserved  (u32, 0)
 *     off 16: text      (text_len bytes)
 *
 * Being a self-describing byte blob is the point: FREEZE is just copying the
 * block's bytes to the log, and THAW is alloc + copy-back — no serialiser.
 * Object tag = TAG_BLOCK; the block KIND (paragraph/heading/…) is the header
 * field, distinct from the object tag.
 *
 * A DOCUMENT is a 2-slot OR-header (TAG_DOCUMENT): slot 0 = the blocks orvec,
 * slot 1 = the text-log orbuf. The header is a stable single capability for
 * the whole document; when the blocks orvec grows, the caller updates slot 0
 * with doc_set_blocks so the header ref stays valid. The block COUNT and any
 * freeze LOCATOR (position -> (log offset, len) for frozen blocks) are the
 * caller's / session's to track — like orvec/orbuf, the container holds no
 * hidden length. Deferred to later layers: the Section grouping level, Runs
 * (multiple styled spans within a block), Style objects, and the terminal-
 * local Rich Text Control renderer.
 *
 * FREEZE block at orvec slot i (scrolled out of view):
 *     b       = objor_vget(blocks, i);
 *     n       = block_bytelen(b);                 // OLEN — the whole block
 *     off     = tl_len;                           // current text-log length
 *     textlog = orbuf_append(textlog, tl_len, b, 0, n);   tl_len += n;
 *     ...record (off, n) for slot i in the caller's locator...
 *     objor_free(b);  objor_vclear(blocks, i);    // live descriptor released
 *
 * THAW block at slot i (back in view), from its recorded (off, n):
 *     b = objor_alloc(n, TAG_BLOCK, OBJ_CAP_R|OBJ_CAP_W|OBJ_CAP_V);
 *     orbuf_read(textlog, off, b, 0, n);
 *     objor_vset(blocks, i, b);
 */

#ifndef DOC_H
#define DOC_H

#include "liborisc.h"
#include "obj_or.h"
#include "orvec.h"
#include "orbuf.h"
#include "ortag.h"

/* Block header size; text begins here. */
#define BLOCK_HDR   16

/* Block kinds (the header `kind` field). */
#define BLK_PARA    0    /* body paragraph */
#define BLK_H1      1    /* heading level 1 */
#define BLK_H2      2    /* heading level 2 */
#define BLK_CODE    3    /* preformatted / code line */
#define BLK_RULE    4    /* horizontal rule (no text) */
#define BLK_LIST    5    /* list item */

/* --- Block: a self-contained byte object -------------------------------- */

/* Allocate a Block of `kind`/`style` and copy `text_len` bytes of text from
 * `src[src_off]` into it. Returns the block (R|W|V) or a null reference on
 * failure. `text_len` may be 0 (e.g. BLK_RULE). */
void *__or block_new(int kind, int style, void *__or src, int src_off,
                     int text_len);

int block_kind(void *__or b);      /* header kind field */
int block_style(void *__or b);     /* header style-id field */
int block_textlen(void *__or b);   /* header text_len field */
int block_bytelen(void *__or b);   /* whole object size (BLOCK_HDR+text_len) */

/* Copy the block's text (block_textlen bytes) into dst[dst_off]. */
void block_text(void *__or b, void *__or dst, int dst_off);

/* --- Document: a 2-slot OR-header --------------------------------------- */

/* New Document: blocks orvec of capacity `cap0` + an empty text-log orbuf.
 * Returns the Document header (R|W|V|C) or a null reference on failure. */
void *__or doc_new(int cap0);

void *__or doc_blocks(void *__or doc);    /* the blocks orvec (slot 0) */
void *__or doc_textlog(void *__or doc);   /* the text-log orbuf (slot 1) */

/* Replace the blocks-orvec slot (call after orvec_push grows it). */
void doc_set_blocks(void *__or doc, void *__or blocks);

/* Replace the text-log-orbuf slot (call after orbuf_append grows it). */
void doc_set_textlog(void *__or doc, void *__or textlog);

#endif /* DOC_H */
