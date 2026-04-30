/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  copyright (c) 2011-2017 - Daniel De Matteis
 *  copyright (c) 2016-2019 - Brad Parker
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __GDI_DEFINES_H
#define __GDI_DEFINES_H

#include <stdint.h>

#include <retro_environment.h>
#include <boolean.h>

#include "../video_defines.h"

typedef struct gdi
{
#ifndef __WINRT__
   WNDCLASSEX wndclass;
#endif
   HDC winDC;
   HDC memDC;
   HDC texDC;
   HBITMAP bmp;
   HBITMAP bmp_old;
   uint16_t *temp_buf;
   uint8_t *menu_frame;
   size_t menu_frame_cap;

   /* Backing bitmap for the menu/widget compositing surface.
    * Distinct from gdi->bmp (which is a DDB sized to the core
    * frame): bmp_menu is a top-down 32-bit BGRA DIB section sized
    * to the *window* surface, pre-multiplied alpha-ready, used as
    * the back buffer when XMB/Ozone/MaterialUI or widgets draw via
    * gfx_display_ctx_gdi. The DIB lets us do fast solid-color
    * fills (FillRect + cached brush) and AlphaBlend composites
    * without round-tripping through DDB conversion every frame. */
   HBITMAP bmp_menu;
   HBITMAP bmp_menu_old;
   uint32_t *menu_pixels;          /* DIB-backing pointer; currently unused but kept for potential direct-access fast paths. */
   unsigned menu_surface_width;
   unsigned menu_surface_height;

   /* Pre-allocated brushes for solid-fill quads. The current brush is
    * cached and reused when consecutive quads share a colour, which
    * is common (Ozone draws hundreds of background quads in the
    * same theme colour per frame). */
   HBRUSH brush_cached;
   COLORREF brush_color_cached;
   bool brush_color_cached_valid;

   /* Scissor stack for gfx_display_ctx_gdi_scissor_{begin,end}. GDI
    * clip regions don't nest natively, so we save/restore the DC
    * clip region across begin/end. */
   int  scissor_saved;
   bool scissor_active;

   unsigned video_width;
   unsigned video_height;
   unsigned screen_width;
   unsigned screen_height;
   /* Surface (window) size last published via video_driver_set_size,
    * tracked here so gdi_alive can read it without locking.  Distinct
    * from video_width / video_height which is the core's frame size. */
   unsigned full_width;
   unsigned full_height;
   /* Actual size of gdi->bmp (the DDB).  Separate from video_width
    * because when RGUI is active we draw the menu (a different size
    * than the core) into bmp; without a dedicated tracker, the
    * comparison against video_width would trigger a destructive
    * DeleteObject + CreateCompatibleBitmap on every frame, racing
    * with WM_PAINT and producing visible flicker. */
   unsigned bmp_width;
   unsigned bmp_height;

   unsigned menu_width;
   unsigned menu_height;
   unsigned menu_pitch;
   unsigned video_pitch;
   unsigned video_bits;
   unsigned menu_bits;
   int win_major;
   int win_minor;

   bool rgb32;
   bool lte_win98;
   bool menu_enable;
   bool menu_full_screen;
   /* True while a textured menu (XMB/Ozone/MaterialUI) is being
    * composited onto bmp_menu in gfx_display_ctx_gdi_draw. RGUI
    * still pushes a 16-bit pixel buffer via set_texture_frame and
    * lives in the gdi->menu_frame path. */
   bool menu_textured_active;

   /* Aspect-ratio-aware viewport.  full_width/full_height hold the
    * window size; x/y/width/height hold the destination rect for
    * the core frame inside the window after applying aspect ratio
    * settings (Settings → Video → Scaling → Aspect Ratio).  Mirrors
    * the d3d8/d3d9 vp pattern: video_driver_update_viewport fills
    * this in, the frame's StretchBlt/StretchDIBits uses x/y/width/
    * height as its destination, and the area outside that rect is
    * cleared to black to produce letterbox/pillarbox bars.
    *
    * keep_aspect is set from video_info->force_aspect at init time
    * and toggled to true when the user changes aspect ratio.
    *
    * should_resize is the dirty flag: window-resize / aspect-ratio
    * changes / state-change pokes set it, gdi_frame consumes it by
    * recomputing the viewport at the start of the next frame. */
   video_viewport_t vp;
   bool keep_aspect;
   bool should_resize;
} gdi_t;

#endif
