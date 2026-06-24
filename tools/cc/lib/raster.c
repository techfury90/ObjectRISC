/*
 * raster.c — WM-mediated raster blit client (Phase 59 / WM γ.12).
 *
 * Phase 4: migrated onto the handle object API (obj.h). The per-task
 * RASTER cap is adopted from the dir-walk result into a handle, and the
 * blit / clear SENDs go through obj_send_bytes — the data-send keystone:
 * O2 = the boot stack/data segment holding the caller's pixel buffer
 * (so the WM can ObjFetchBytes it), R4..R7 = (op, packed_xy, packed_wh,
 * byte_offset), fire-and-forget (no reply mailbox). All the raw
 * orefld/omov/send inline asm is gone; only the boot-OR restore stays.
 */

#include "liborisc.h"
#include "obj.h"

/* VA layout — pick the right boot segment for the caller's buffer by
 * its address (same trick grid.c / the old raster.c used). */
#define DATA_VA       0x00040000U
#define STACK_BOTTOM  0x001f0000U

/* The WM-mediated raster service, as an object handle. */
static obj_t raster_svc_h = OBJ_NULL;

/* Restore the boot OPRs the SEND clobbered (O2 = stack, O3 = data). */
static void
_raster_restore_or(void)
{
	asm volatile("omov o2, o11");
	asm volatile("omov o3, o15");
}

int
raster_init_from_dir_result(void)
{
	if (obj_init() != 0)
		return -1;
	/* Adopt the cap wm_bind_surface(WSURF_RASTER) left in DIR_RESULT. */
	raster_svc_h = obj_adopt_dir_result();
	return (raster_svc_h < 0) ? -1 : 0;
}

int
raster_blit(int packed_xy, int packed_wh, const unsigned char *pixels)
{
	unsigned int va = (unsigned int)pixels;
	int byte_off, src;

	if (obj_isnull(raster_svc_h))
		return -1;
	/* The pixel buffer lives in our boot stack (locals) or data
	 * (statics); pick the segment + byte offset by VA. */
	if (va >= STACK_BOTTOM) {
		byte_off = (int)(va - STACK_BOTTOM);
		src = OBJ_SRC_STACK;
	} else {
		byte_off = (int)(va - DATA_VA);
		src = OBJ_SRC_DATA;
	}
	obj_send_bytes(raster_svc_h, src, OBJ_NULL,
	               RST_OP_BLIT, packed_xy, packed_wh, byte_off);
	_raster_restore_or();
	return 0;
}

int
raster_clear(void)
{
	if (obj_isnull(raster_svc_h))
		return -1;
	obj_send_bytes(raster_svc_h, OBJ_SRC_NONE, OBJ_NULL,
	               RST_OP_CLEAR, 0, 0, 0);
	_raster_restore_or();
	return 0;
}
