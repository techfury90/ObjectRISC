/*
 * ortag.h — userland object type-tag (OTAG) space.
 *
 * Every object carries a 16-bit type tag, set at ObjAlloc/ObjAllocStore and
 * read with objor_tag (OTAG). The firmware/system tags occupy 0x41xx
 * (OBJ_TAG_* in obj.h: CODE/STACK/DATA/SERVICE). This header reserves the
 * 0x42xx range for the userland object model, so a generic dispatcher can
 * switch on "what kind of object is this":
 *
 *   0x420x  collections (base-type containers)
 *   0x421x  document model
 *   0x422x  (reserved: widgets)
 *
 * NOTE: orvec.s / orbuf.s / doc.s inline these literals (they are bare asm and
 * do not include this header); this file is the single source of truth for C
 * consumers and the place the numbering is documented. Keep them in sync.
 */

#ifndef ORTAG_H
#define ORTAG_H

/* collections */
#define TAG_ORVEC      0x4200   /* growable capability array (orvec.s) */
#define TAG_ORBUF      0x4201   /* growable byte buffer (orbuf.s) */

/* document model */
#define TAG_DOCUMENT   0x4210   /* Document OR-header: blocks + text-log slots */
#define TAG_BLOCK      0x4211   /* Block: self-contained byte object */
#define TAG_STYLE      0x4212   /* Style object (deferred; v1 uses style-id ints) */

#endif /* ORTAG_H */
