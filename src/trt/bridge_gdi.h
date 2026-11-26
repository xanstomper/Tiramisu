// tiramisu/src/trt/bridge_gdi.h
// GDI32 for the guest (bridge_gdi.cpp).
//
// A small GDI OBJECT + DC handle table: fonts, brushes, regions, bitmaps
// are host-side records; DCs come from user32 GetDC. The console has no
// GDI display surface, but SDL2-class engines and dialog-based UIs still
// call the object/state surface at boot and expect real handles back.
// Nothing here draws; the records only make the lifecycle observable and
// keep the calls from being fail-visible thunks.
#ifndef TIRAMISU_BRIDGE_GDI_H
#define TIRAMISU_BRIDGE_GDI_H

#include "trt.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register gdi32.dll as BUILTIN (object/state/raster surface).
int trt_bridge_register_gdi(void);

// Drop every per-DC record this bridge holds (selection, saved state, raster
// surface) when a DC is released. Called by bridge_user32's DC release path.
void trt_gdi_forget_dc(void* hdc);

// ReleaseDC on a window DC: drop the selection/saved state and reset the clip,
// but KEEP the raster surface (the window still owns those pixels).
void trt_gdi_forget_draw_state(void* hdc);

// Colour of an HBRUSH/HPEN handle, stock objects included. user32's FillRect/
// FrameRect receive a brush handle rather than a DC selection, so they resolve
// it through this instead of keeping a second colour table.
u32 trt_gdi_brush_color(void* hbrush);

// Give a DC's raster surface its size up front (a window DC is sized to the
// client area instead of the display). Later draws reuse it.
void trt_gdi_surface_hint(void* hdc, i32 w, i32 h);

// DrawTextW/A workhorse: layout + draw into the DC surface honouring the
// DT_* flags desktop UIs use (LEFT/CENTER/RIGHT, TOP/VCENTER/BOTTOM,
// SINGLELINE, WORDBREAK, CALCRECT, NOPREFIX). Returns the text height.
int trt_gdi_draw_text(void* hdc, const u16* text, int count, void* rect,
                      u32 format);

#ifdef __cplusplus
}
#endif

#endif
