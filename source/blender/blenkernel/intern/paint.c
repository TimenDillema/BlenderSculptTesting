/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2009 by Nicholas Bishop. All rights reserved. */

/** \file
 * \ingroup bke
 */

#include <stdlib.h>
#include <string.h>

#include "MEM_guardedalloc.h"

#include "DNA_brush_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_key_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"
#include "DNA_workspace_types.h"

#include "BLI_array.h"
#include "BLI_bitmap.h"
#include "BLI_hash.h"
#include "BLI_listbase.h"
#include "BLI_math_vector.h"
#include "BLI_string_utf8.h"
#include "BLI_string_utils.h"
#include "BLI_utildefines.h"

#include "BLT_translation.h"

#include "BKE_attribute.h"
#include "BKE_brush.h"
#include "BKE_ccg.h"
#include "BKE_colortools.h"
#include "BKE_context.h"
#include "BKE_crazyspace.h"
#include "BKE_deform.h"
#include "BKE_global.h"
#include "BKE_gpencil.h"
#include "BKE_idtype.h"
#include "BKE_image.h"
#include "BKE_key.h"
#include "BKE_lib_id.h"
#include "BKE_main.h"
#include "BKE_mesh.h"
#include "BKE_mesh_mapping.h"
#include "BKE_mesh_runtime.h"
#include "BKE_modifier.h"
#include "BKE_multires.h"
#include "BKE_object.h"
#include "BKE_paint.h"
#include "BKE_pbvh.h"
#include "BKE_subdiv_ccg.h"
#include "BKE_subsurf.h"
#include "BKE_undo_system.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "RNA_enum_types.h"

#include "BLO_read_write.h"

#include "bmesh.h"
#include "bmesh_log.h"

// TODO: figure out bad cross module refs
void SCULPT_on_sculptsession_bmesh_free(SculptSession *ss);
void SCULPT_undo_ensure_bmlog(Object *ob);

static void init_mdyntopo_layer(SculptSession *ss, PBVH *pbvh, int totvert);

const char *face_areas_layer_name = "_sculpt_face_areas";

static void palette_init_data(ID *id)
{
  Palette *palette = (Palette *)id;

  BLI_assert(MEMCMP_STRUCT_AFTER_IS_ZERO(palette, id));

  /* Enable fake user by default. */
  id_fake_user_set(&palette->id);
}

static void palette_copy_data(Main *UNUSED(bmain),
                              ID *id_dst,
                              const ID *id_src,
                              const int UNUSED(flag))
{
  Palette *palette_dst = (Palette *)id_dst;
  const Palette *palette_src = (const Palette *)id_src;

  BLI_duplicatelist(&palette_dst->colors, &palette_src->colors);
}

static void palette_free_data(ID *id)
{
  Palette *palette = (Palette *)id;

  BLI_freelistN(&palette->colors);
}

static void palette_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  Palette *palette = (Palette *)id;

  PaletteColor *color;
  BLO_write_id_struct(writer, Palette, id_address, &palette->id);
  BKE_id_blend_write(writer, &palette->id);

  for (color = palette->colors.first; color; color = color->next) {
    BLO_write_struct(writer, PaletteColor, color);
  }
}

static void palette_blend_read_data(BlendDataReader *reader, ID *id)
{
  Palette *palette = (Palette *)id;
  BLO_read_list(reader, &palette->colors);
}

static void palette_undo_preserve(BlendLibReader *UNUSED(reader), ID *id_new, ID *id_old)
{
  /* Whole Palette is preserved across undo-steps, and it has no extra pointer, simple. */
  /* NOTE: We do not care about potential internal references to self here, Palette has none. */
  /* NOTE: We do not swap IDProperties, as dealing with potential ID pointers in those would be
   *       fairly delicate. */
  BKE_lib_id_swap(NULL, id_new, id_old);
  SWAP(IDProperty *, id_new->properties, id_old->properties);
}

IDTypeInfo IDType_ID_PAL = {
    .id_code = ID_PAL,
    .id_filter = FILTER_ID_PAL,
    .main_listbase_index = INDEX_ID_PAL,
    .struct_size = sizeof(Palette),
    .name = "Palette",
    .name_plural = "palettes",
    .translation_context = BLT_I18NCONTEXT_ID_PALETTE,
    .flags = IDTYPE_FLAGS_NO_ANIMDATA,
    .asset_type_info = NULL,

    .init_data = palette_init_data,
    .copy_data = palette_copy_data,
    .free_data = palette_free_data,
    .make_local = NULL,
    .foreach_id = NULL,
    .foreach_cache = NULL,
    .foreach_path = NULL,
    .owner_get = NULL,

    .blend_write = palette_blend_write,
    .blend_read_data = palette_blend_read_data,
    .blend_read_lib = NULL,
    .blend_read_expand = NULL,

    .blend_read_undo_preserve = palette_undo_preserve,

    .lib_override_apply_post = NULL,
};

static void paint_curve_copy_data(Main *UNUSED(bmain),
                                  ID *id_dst,
                                  const ID *id_src,
                                  const int UNUSED(flag))
{
  PaintCurve *paint_curve_dst = (PaintCurve *)id_dst;
  const PaintCurve *paint_curve_src = (const PaintCurve *)id_src;

  if (paint_curve_src->tot_points != 0) {
    paint_curve_dst->points = MEM_dupallocN(paint_curve_src->points);
  }
}

static void paint_curve_free_data(ID *id)
{
  PaintCurve *paint_curve = (PaintCurve *)id;

  MEM_SAFE_FREE(paint_curve->points);
  paint_curve->tot_points = 0;
}

static void paint_curve_blend_write(BlendWriter *writer, ID *id, const void *id_address)
{
  PaintCurve *pc = (PaintCurve *)id;

  BLO_write_id_struct(writer, PaintCurve, id_address, &pc->id);
  BKE_id_blend_write(writer, &pc->id);

  BLO_write_struct_array(writer, PaintCurvePoint, pc->tot_points, pc->points);
}

static void paint_curve_blend_read_data(BlendDataReader *reader, ID *id)
{
  PaintCurve *pc = (PaintCurve *)id;
  BLO_read_data_address(reader, &pc->points);
}

IDTypeInfo IDType_ID_PC = {
    .id_code = ID_PC,
    .id_filter = FILTER_ID_PC,
    .main_listbase_index = INDEX_ID_PC,
    .struct_size = sizeof(PaintCurve),
    .name = "PaintCurve",
    .name_plural = "paint_curves",
    .translation_context = BLT_I18NCONTEXT_ID_PAINTCURVE,
    .flags = IDTYPE_FLAGS_NO_ANIMDATA,
    .asset_type_info = NULL,

    .init_data = NULL,
    .copy_data = paint_curve_copy_data,
    .free_data = paint_curve_free_data,
    .make_local = NULL,
    .foreach_id = NULL,
    .foreach_cache = NULL,
    .foreach_path = NULL,
    .owner_get = NULL,

    .blend_write = paint_curve_blend_write,
    .blend_read_data = paint_curve_blend_read_data,
    .blend_read_lib = NULL,
    .blend_read_expand = NULL,

    .blend_read_undo_preserve = NULL,

    .lib_override_apply_post = NULL,
};

const char PAINT_CURSOR_SCULPT[3] = {255, 100, 100};
const char PAINT_CURSOR_VERTEX_PAINT[3] = {255, 255, 255};
const char PAINT_CURSOR_WEIGHT_PAINT[3] = {200, 200, 255};
const char PAINT_CURSOR_TEXTURE_PAINT[3] = {255, 255, 255};

static ePaintOverlayControlFlags overlay_flags = 0;

void BKE_paint_invalidate_overlay_tex(Scene *scene, ViewLayer *view_layer, const Tex *tex)
{
  Paint *p = BKE_paint_get_active(scene, view_layer);
  if (!p) {
    return;
  }

  Brush *br = p->brush;
  if (!br) {
    return;
  }

  if (br->mtex.tex == tex) {
    overlay_flags |= PAINT_OVERLAY_INVALID_TEXTURE_PRIMARY;
  }
  if (br->mask_mtex.tex == tex) {
    overlay_flags |= PAINT_OVERLAY_INVALID_TEXTURE_SECONDARY;
  }
}

void BKE_paint_invalidate_cursor_overlay(Scene *scene, ViewLayer *view_layer, CurveMapping *curve)
{
  Paint *p = BKE_paint_get_active(scene, view_layer);
  if (p == NULL) {
    return;
  }

  Brush *br = p->brush;
  if (br && br->curve == curve) {
    overlay_flags |= PAINT_OVERLAY_INVALID_CURVE;
  }
}

void BKE_paint_invalidate_overlay_all(void)
{
  overlay_flags |= (PAINT_OVERLAY_INVALID_TEXTURE_SECONDARY |
                    PAINT_OVERLAY_INVALID_TEXTURE_PRIMARY | PAINT_OVERLAY_INVALID_CURVE);
}

ePaintOverlayControlFlags BKE_paint_get_overlay_flags(void)
{
  return overlay_flags;
}

void BKE_paint_set_overlay_override(eOverlayFlags flags)
{
  if (flags & BRUSH_OVERLAY_OVERRIDE_MASK) {
    if (flags & BRUSH_OVERLAY_CURSOR_OVERRIDE_ON_STROKE) {
      overlay_flags |= PAINT_OVERLAY_OVERRIDE_CURSOR;
    }
    if (flags & BRUSH_OVERLAY_PRIMARY_OVERRIDE_ON_STROKE) {
      overlay_flags |= PAINT_OVERLAY_OVERRIDE_PRIMARY;
    }
    if (flags & BRUSH_OVERLAY_SECONDARY_OVERRIDE_ON_STROKE) {
      overlay_flags |= PAINT_OVERLAY_OVERRIDE_SECONDARY;
    }
  }
  else {
    overlay_flags &= ~(PAINT_OVERRIDE_MASK);
  }
}

void BKE_paint_reset_overlay_invalid(ePaintOverlayControlFlags flag)
{
  overlay_flags &= ~(flag);
}

bool BKE_paint_ensure_from_paintmode(Scene *sce, ePaintMode mode)
{
  ToolSettings *ts = sce->toolsettings;
  Paint **paint_ptr = NULL;
  /* Some paint modes don't store paint settings as pointer, for these this can be set and
   * referenced by paint_ptr. */
  Paint *paint_tmp = NULL;

  switch (mode) {
    case PAINT_MODE_SCULPT:
      paint_ptr = (Paint **)&ts->sculpt;
      break;
    case PAINT_MODE_VERTEX:
      paint_ptr = (Paint **)&ts->vpaint;
      break;
    case PAINT_MODE_WEIGHT:
      paint_ptr = (Paint **)&ts->wpaint;
      break;
    case PAINT_MODE_TEXTURE_2D:
    case PAINT_MODE_TEXTURE_3D:
      paint_tmp = (Paint *)&ts->imapaint;
      paint_ptr = &paint_tmp;
      break;
    case PAINT_MODE_SCULPT_UV:
      paint_ptr = (Paint **)&ts->uvsculpt;
      break;
    case PAINT_MODE_GPENCIL:
      paint_ptr = (Paint **)&ts->gp_paint;
      break;
    case PAINT_MODE_VERTEX_GPENCIL:
      paint_ptr = (Paint **)&ts->gp_vertexpaint;
      break;
    case PAINT_MODE_SCULPT_GPENCIL:
      paint_ptr = (Paint **)&ts->gp_sculptpaint;
      break;
    case PAINT_MODE_WEIGHT_GPENCIL:
      paint_ptr = (Paint **)&ts->gp_weightpaint;
      break;
    case PAINT_MODE_SCULPT_CURVES:
      paint_ptr = (Paint **)&ts->curves_sculpt;
      break;
    case PAINT_MODE_INVALID:
      break;
  }
  if (paint_ptr) {
    BKE_paint_ensure(ts, paint_ptr);
    return true;
  }
  return false;
}

Paint *BKE_paint_get_active_from_paintmode(Scene *sce, ePaintMode mode)
{
  if (sce) {
    ToolSettings *ts = sce->toolsettings;

    switch (mode) {
      case PAINT_MODE_SCULPT:
        return &ts->sculpt->paint;
      case PAINT_MODE_VERTEX:
        return &ts->vpaint->paint;
      case PAINT_MODE_WEIGHT:
        return &ts->wpaint->paint;
      case PAINT_MODE_TEXTURE_2D:
      case PAINT_MODE_TEXTURE_3D:
        return &ts->imapaint.paint;
      case PAINT_MODE_SCULPT_UV:
        return &ts->uvsculpt->paint;
      case PAINT_MODE_GPENCIL:
        return &ts->gp_paint->paint;
      case PAINT_MODE_VERTEX_GPENCIL:
        return &ts->gp_vertexpaint->paint;
      case PAINT_MODE_SCULPT_GPENCIL:
        return &ts->gp_sculptpaint->paint;
      case PAINT_MODE_WEIGHT_GPENCIL:
        return &ts->gp_weightpaint->paint;
      case PAINT_MODE_SCULPT_CURVES:
        return &ts->curves_sculpt->paint;
      case PAINT_MODE_INVALID:
        return NULL;
      default:
        return &ts->imapaint.paint;
    }
  }

  return NULL;
}

const EnumPropertyItem *BKE_paint_get_tool_enum_from_paintmode(ePaintMode mode)
{
  switch (mode) {
    case PAINT_MODE_SCULPT:
      return rna_enum_brush_sculpt_tool_items;
    case PAINT_MODE_VERTEX:
      return rna_enum_brush_vertex_tool_items;
    case PAINT_MODE_WEIGHT:
      return rna_enum_brush_weight_tool_items;
    case PAINT_MODE_TEXTURE_2D:
    case PAINT_MODE_TEXTURE_3D:
      return rna_enum_brush_image_tool_items;
    case PAINT_MODE_SCULPT_UV:
      return rna_enum_brush_uv_sculpt_tool_items;
    case PAINT_MODE_GPENCIL:
      return rna_enum_brush_gpencil_types_items;
    case PAINT_MODE_VERTEX_GPENCIL:
      return rna_enum_brush_gpencil_vertex_types_items;
    case PAINT_MODE_SCULPT_GPENCIL:
      return rna_enum_brush_gpencil_sculpt_types_items;
    case PAINT_MODE_WEIGHT_GPENCIL:
      return rna_enum_brush_gpencil_weight_types_items;
    case PAINT_MODE_SCULPT_CURVES:
      return rna_enum_brush_curves_sculpt_tool_items;
    case PAINT_MODE_INVALID:
      break;
  }
  return NULL;
}

const char *BKE_paint_get_tool_prop_id_from_paintmode(ePaintMode mode)
{
  switch (mode) {
    case PAINT_MODE_SCULPT:
      return "sculpt_tool";
    case PAINT_MODE_VERTEX:
      return "vertex_tool";
    case PAINT_MODE_WEIGHT:
      return "weight_tool";
    case PAINT_MODE_TEXTURE_2D:
    case PAINT_MODE_TEXTURE_3D:
      return "image_tool";
    case PAINT_MODE_SCULPT_UV:
      return "uv_sculpt_tool";
    case PAINT_MODE_GPENCIL:
      return "gpencil_tool";
    case PAINT_MODE_VERTEX_GPENCIL:
      return "gpencil_vertex_tool";
    case PAINT_MODE_SCULPT_GPENCIL:
      return "gpencil_sculpt_tool";
    case PAINT_MODE_WEIGHT_GPENCIL:
      return "gpencil_weight_tool";
    case PAINT_MODE_SCULPT_CURVES:
      return "curves_sculpt_tool";
    case PAINT_MODE_INVALID:
      break;
  }

  /* Invalid paint mode. */
  return NULL;
}

Paint *BKE_paint_get_active(Scene *sce, ViewLayer *view_layer)
{
  if (sce && view_layer) {
    ToolSettings *ts = sce->toolsettings;

    if (view_layer->basact && view_layer->basact->object) {
      switch (view_layer->basact->object->mode) {
        case OB_MODE_SCULPT:
          return &ts->sculpt->paint;
        case OB_MODE_VERTEX_PAINT:
          return &ts->vpaint->paint;
        case OB_MODE_WEIGHT_PAINT:
          return &ts->wpaint->paint;
        case OB_MODE_TEXTURE_PAINT:
          return &ts->imapaint.paint;
        case OB_MODE_PAINT_GPENCIL:
          return &ts->gp_paint->paint;
        case OB_MODE_VERTEX_GPENCIL:
          return &ts->gp_vertexpaint->paint;
        case OB_MODE_SCULPT_GPENCIL:
          return &ts->gp_sculptpaint->paint;
        case OB_MODE_WEIGHT_GPENCIL:
          return &ts->gp_weightpaint->paint;
        case OB_MODE_SCULPT_CURVES:
          return &ts->curves_sculpt->paint;
        case OB_MODE_EDIT:
          return ts->uvsculpt ? &ts->uvsculpt->paint : NULL;
        default:
          break;
      }
    }

    /* default to image paint */
    return &ts->imapaint.paint;
  }

  return NULL;
}

Paint *BKE_paint_get_active_from_context(const bContext *C)
{
  Scene *sce = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  SpaceImage *sima;

  if (sce && view_layer) {
    ToolSettings *ts = sce->toolsettings;
    Object *obact = NULL;

    if (view_layer->basact && view_layer->basact->object) {
      obact = view_layer->basact->object;
    }

    if ((sima = CTX_wm_space_image(C)) != NULL) {
      if (obact && obact->mode == OB_MODE_EDIT) {
        if (sima->mode == SI_MODE_PAINT) {
          return &ts->imapaint.paint;
        }
        if (sima->mode == SI_MODE_UV) {
          return &ts->uvsculpt->paint;
        }
      }
      else {
        return &ts->imapaint.paint;
      }
    }
    else {
      return BKE_paint_get_active(sce, view_layer);
    }
  }

  return NULL;
}

ePaintMode BKE_paintmode_get_active_from_context(const bContext *C)
{
  Scene *sce = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  SpaceImage *sima;

  if (sce && view_layer) {
    Object *obact = NULL;

    if (view_layer->basact && view_layer->basact->object) {
      obact = view_layer->basact->object;
    }

    if ((sima = CTX_wm_space_image(C)) != NULL) {
      if (obact && obact->mode == OB_MODE_EDIT) {
        if (sima->mode == SI_MODE_PAINT) {
          return PAINT_MODE_TEXTURE_2D;
        }
        if (sima->mode == SI_MODE_UV) {
          return PAINT_MODE_SCULPT_UV;
        }
      }
      else {
        return PAINT_MODE_TEXTURE_2D;
      }
    }
    else if (obact) {
      switch (obact->mode) {
        case OB_MODE_SCULPT:
          return PAINT_MODE_SCULPT;
        case OB_MODE_VERTEX_PAINT:
          return PAINT_MODE_VERTEX;
        case OB_MODE_WEIGHT_PAINT:
          return PAINT_MODE_WEIGHT;
        case OB_MODE_TEXTURE_PAINT:
          return PAINT_MODE_TEXTURE_3D;
        case OB_MODE_EDIT:
          return PAINT_MODE_SCULPT_UV;
        default:
          return PAINT_MODE_TEXTURE_2D;
      }
    }
    else {
      /* default to image paint */
      return PAINT_MODE_TEXTURE_2D;
    }
  }

  return PAINT_MODE_INVALID;
}

ePaintMode BKE_paintmode_get_from_tool(const struct bToolRef *tref)
{
  if (tref->space_type == SPACE_VIEW3D) {
    switch (tref->mode) {
      case CTX_MODE_SCULPT:
        return PAINT_MODE_SCULPT;
      case CTX_MODE_PAINT_VERTEX:
        return PAINT_MODE_VERTEX;
      case CTX_MODE_PAINT_WEIGHT:
        return PAINT_MODE_WEIGHT;
      case CTX_MODE_PAINT_GPENCIL:
        return PAINT_MODE_GPENCIL;
      case CTX_MODE_PAINT_TEXTURE:
        return PAINT_MODE_TEXTURE_3D;
      case CTX_MODE_VERTEX_GPENCIL:
        return PAINT_MODE_VERTEX_GPENCIL;
      case CTX_MODE_SCULPT_GPENCIL:
        return PAINT_MODE_SCULPT_GPENCIL;
      case CTX_MODE_WEIGHT_GPENCIL:
        return PAINT_MODE_WEIGHT_GPENCIL;
      case CTX_MODE_SCULPT_CURVES:
        return PAINT_MODE_SCULPT_CURVES;
    }
  }
  else if (tref->space_type == SPACE_IMAGE) {
    switch (tref->mode) {
      case SI_MODE_PAINT:
        return PAINT_MODE_TEXTURE_2D;
      case SI_MODE_UV:
        return PAINT_MODE_SCULPT_UV;
    }
  }

  return PAINT_MODE_INVALID;
}

Brush *BKE_paint_brush(Paint *p)
{
  return p ? (p->brush_eval ? p->brush_eval : p->brush) : NULL;
}

void BKE_paint_brush_set(Paint *p, Brush *br)
{
  if (p) {
    id_us_min((ID *)p->brush);
    id_us_plus((ID *)br);
    p->brush = br;

    BKE_paint_toolslots_brush_update(p);
  }
}

void BKE_paint_runtime_init(const ToolSettings *ts, Paint *paint)
{
  if (paint == &ts->imapaint.paint) {
    paint->runtime.tool_offset = offsetof(Brush, imagepaint_tool);
    paint->runtime.ob_mode = OB_MODE_TEXTURE_PAINT;
  }
  else if (ts->sculpt && paint == &ts->sculpt->paint) {
    paint->runtime.tool_offset = offsetof(Brush, sculpt_tool);
    paint->runtime.ob_mode = OB_MODE_SCULPT;
  }
  else if (ts->vpaint && paint == &ts->vpaint->paint) {
    paint->runtime.tool_offset = offsetof(Brush, vertexpaint_tool);
    paint->runtime.ob_mode = OB_MODE_VERTEX_PAINT;
  }
  else if (ts->wpaint && paint == &ts->wpaint->paint) {
    paint->runtime.tool_offset = offsetof(Brush, weightpaint_tool);
    paint->runtime.ob_mode = OB_MODE_WEIGHT_PAINT;
  }
  else if (ts->uvsculpt && paint == &ts->uvsculpt->paint) {
    paint->runtime.tool_offset = offsetof(Brush, uv_sculpt_tool);
    paint->runtime.ob_mode = OB_MODE_EDIT;
  }
  else if (ts->gp_paint && paint == &ts->gp_paint->paint) {
    paint->runtime.tool_offset = offsetof(Brush, gpencil_tool);
    paint->runtime.ob_mode = OB_MODE_PAINT_GPENCIL;
  }
  else if (ts->gp_vertexpaint && paint == &ts->gp_vertexpaint->paint) {
    paint->runtime.tool_offset = offsetof(Brush, gpencil_vertex_tool);
    paint->runtime.ob_mode = OB_MODE_VERTEX_GPENCIL;
  }
  else if (ts->gp_sculptpaint && paint == &ts->gp_sculptpaint->paint) {
    paint->runtime.tool_offset = offsetof(Brush, gpencil_sculpt_tool);
    paint->runtime.ob_mode = OB_MODE_SCULPT_GPENCIL;
  }
  else if (ts->gp_weightpaint && paint == &ts->gp_weightpaint->paint) {
    paint->runtime.tool_offset = offsetof(Brush, gpencil_weight_tool);
    paint->runtime.ob_mode = OB_MODE_WEIGHT_GPENCIL;
  }
  else if (ts->curves_sculpt && paint == &ts->curves_sculpt->paint) {
    paint->runtime.tool_offset = offsetof(Brush, curves_sculpt_tool);
    paint->runtime.ob_mode = OB_MODE_SCULPT_CURVES;
  }
  else {
    BLI_assert_unreachable();
  }
}

uint BKE_paint_get_brush_tool_offset_from_paintmode(const ePaintMode mode)
{
  switch (mode) {
    case PAINT_MODE_TEXTURE_2D:
    case PAINT_MODE_TEXTURE_3D:
      return offsetof(Brush, imagepaint_tool);
    case PAINT_MODE_SCULPT:
      return offsetof(Brush, sculpt_tool);
    case PAINT_MODE_VERTEX:
      return offsetof(Brush, vertexpaint_tool);
    case PAINT_MODE_WEIGHT:
      return offsetof(Brush, weightpaint_tool);
    case PAINT_MODE_SCULPT_UV:
      return offsetof(Brush, uv_sculpt_tool);
    case PAINT_MODE_GPENCIL:
      return offsetof(Brush, gpencil_tool);
    case PAINT_MODE_VERTEX_GPENCIL:
      return offsetof(Brush, gpencil_vertex_tool);
    case PAINT_MODE_SCULPT_GPENCIL:
      return offsetof(Brush, gpencil_sculpt_tool);
    case PAINT_MODE_WEIGHT_GPENCIL:
      return offsetof(Brush, gpencil_weight_tool);
    case PAINT_MODE_SCULPT_CURVES:
      return offsetof(Brush, curves_sculpt_tool);
    case PAINT_MODE_INVALID:
      break; /* We don't use these yet. */
  }
  return 0;
}

PaintCurve *BKE_paint_curve_add(Main *bmain, const char *name)
{
  PaintCurve *pc;

  pc = BKE_id_new(bmain, ID_PC, name);

  return pc;
}

Palette *BKE_paint_palette(Paint *p)
{
  return p ? p->palette : NULL;
}

void BKE_paint_palette_set(Paint *p, Palette *palette)
{
  if (p) {
    id_us_min((ID *)p->palette);
    p->palette = palette;
    id_us_plus((ID *)p->palette);
  }
}

void BKE_paint_curve_set(Brush *br, PaintCurve *pc)
{
  if (br) {
    id_us_min((ID *)br->paint_curve);
    br->paint_curve = pc;
    id_us_plus((ID *)br->paint_curve);
  }
}

void BKE_paint_curve_clamp_endpoint_add_index(PaintCurve *pc, const int add_index)
{
  pc->add_index = (add_index || pc->tot_points == 1) ? (add_index + 1) : 0;
}

void BKE_palette_color_remove(Palette *palette, PaletteColor *color)
{
  if (BLI_listbase_count_at_most(&palette->colors, palette->active_color) ==
      palette->active_color) {
    palette->active_color--;
  }

  BLI_remlink(&palette->colors, color);

  if (palette->active_color < 0 && !BLI_listbase_is_empty(&palette->colors)) {
    palette->active_color = 0;
  }

  MEM_freeN(color);
}

void BKE_palette_clear(Palette *palette)
{
  BLI_freelistN(&palette->colors);
  palette->active_color = 0;
}

Palette *BKE_palette_add(Main *bmain, const char *name)
{
  Palette *palette = BKE_id_new(bmain, ID_PAL, name);
  return palette;
}

PaletteColor *BKE_palette_color_add(Palette *palette)
{
  PaletteColor *color = MEM_callocN(sizeof(*color), "Palette Color");
  BLI_addtail(&palette->colors, color);
  return color;
}

bool BKE_palette_is_empty(const struct Palette *palette)
{
  return BLI_listbase_is_empty(&palette->colors);
}

/* helper function to sort using qsort */
static int palettecolor_compare_hsv(const void *a1, const void *a2)
{
  const tPaletteColorHSV *ps1 = a1, *ps2 = a2;

  /* Hue */
  if (ps1->h > ps2->h) {
    return 1;
  }
  if (ps1->h < ps2->h) {
    return -1;
  }

  /* Saturation. */
  if (ps1->s > ps2->s) {
    return 1;
  }
  if (ps1->s < ps2->s) {
    return -1;
  }

  /* Value. */
  if (1.0f - ps1->v > 1.0f - ps2->v) {
    return 1;
  }
  if (1.0f - ps1->v < 1.0f - ps2->v) {
    return -1;
  }

  return 0;
}

/* helper function to sort using qsort */
static int palettecolor_compare_svh(const void *a1, const void *a2)
{
  const tPaletteColorHSV *ps1 = a1, *ps2 = a2;

  /* Saturation. */
  if (ps1->s > ps2->s) {
    return 1;
  }
  if (ps1->s < ps2->s) {
    return -1;
  }

  /* Value. */
  if (1.0f - ps1->v > 1.0f - ps2->v) {
    return 1;
  }
  if (1.0f - ps1->v < 1.0f - ps2->v) {
    return -1;
  }

  /* Hue */
  if (ps1->h > ps2->h) {
    return 1;
  }
  if (ps1->h < ps2->h) {
    return -1;
  }

  return 0;
}

static int palettecolor_compare_vhs(const void *a1, const void *a2)
{
  const tPaletteColorHSV *ps1 = a1, *ps2 = a2;

  /* Value. */
  if (1.0f - ps1->v > 1.0f - ps2->v) {
    return 1;
  }
  if (1.0f - ps1->v < 1.0f - ps2->v) {
    return -1;
  }

  /* Hue */
  if (ps1->h > ps2->h) {
    return 1;
  }
  if (ps1->h < ps2->h) {
    return -1;
  }

  /* Saturation. */
  if (ps1->s > ps2->s) {
    return 1;
  }
  if (ps1->s < ps2->s) {
    return -1;
  }

  return 0;
}

static int palettecolor_compare_luminance(const void *a1, const void *a2)
{
  const tPaletteColorHSV *ps1 = a1, *ps2 = a2;

  float lumi1 = (ps1->rgb[0] + ps1->rgb[1] + ps1->rgb[2]) / 3.0f;
  float lumi2 = (ps2->rgb[0] + ps2->rgb[1] + ps2->rgb[2]) / 3.0f;

  if (lumi1 > lumi2) {
    return -1;
  }
  if (lumi1 < lumi2) {
    return 1;
  }

  return 0;
}

void BKE_palette_sort_hsv(tPaletteColorHSV *color_array, const int totcol)
{
  /* Sort by Hue, Saturation and Value. */
  qsort(color_array, totcol, sizeof(tPaletteColorHSV), palettecolor_compare_hsv);
}

void BKE_palette_sort_svh(tPaletteColorHSV *color_array, const int totcol)
{
  /* Sort by Saturation, Value and Hue. */
  qsort(color_array, totcol, sizeof(tPaletteColorHSV), palettecolor_compare_svh);
}

void BKE_palette_sort_vhs(tPaletteColorHSV *color_array, const int totcol)
{
  /* Sort by Saturation, Value and Hue. */
  qsort(color_array, totcol, sizeof(tPaletteColorHSV), palettecolor_compare_vhs);
}

void BKE_palette_sort_luminance(tPaletteColorHSV *color_array, const int totcol)
{
  /* Sort by Luminance (calculated with the average, enough for sorting). */
  qsort(color_array, totcol, sizeof(tPaletteColorHSV), palettecolor_compare_luminance);
}

bool BKE_palette_from_hash(Main *bmain, GHash *color_table, const char *name, const bool linear)
{
  tPaletteColorHSV *color_array = NULL;
  tPaletteColorHSV *col_elm = NULL;
  bool done = false;

  const int totpal = BLI_ghash_len(color_table);

  if (totpal > 0) {
    color_array = MEM_calloc_arrayN(totpal, sizeof(tPaletteColorHSV), __func__);
    /* Put all colors in an array. */
    GHashIterator gh_iter;
    int t = 0;
    GHASH_ITER (gh_iter, color_table) {
      const uint col = POINTER_AS_INT(BLI_ghashIterator_getValue(&gh_iter));
      float r, g, b;
      float h, s, v;
      cpack_to_rgb(col, &r, &g, &b);
      rgb_to_hsv(r, g, b, &h, &s, &v);

      col_elm = &color_array[t];
      col_elm->rgb[0] = r;
      col_elm->rgb[1] = g;
      col_elm->rgb[2] = b;
      col_elm->h = h;
      col_elm->s = s;
      col_elm->v = v;
      t++;
    }
  }

  /* Create the Palette. */
  if (totpal > 0) {
    /* Sort by Hue and saturation. */
    BKE_palette_sort_hsv(color_array, totpal);

    Palette *palette = BKE_palette_add(bmain, name);
    if (palette) {
      for (int i = 0; i < totpal; i++) {
        col_elm = &color_array[i];
        PaletteColor *palcol = BKE_palette_color_add(palette);
        if (palcol) {
          copy_v3_v3(palcol->rgb, col_elm->rgb);
          if (linear) {
            linearrgb_to_srgb_v3_v3(palcol->rgb, palcol->rgb);
          }
        }
      }
      done = true;
    }
  }
  else {
    done = false;
  }

  if (totpal > 0) {
    MEM_SAFE_FREE(color_array);
  }

  return done;
}

bool BKE_paint_select_face_test(Object *ob)
{
  return ((ob != NULL) && (ob->type == OB_MESH) && (ob->data != NULL) &&
          (((Mesh *)ob->data)->editflag & ME_EDIT_PAINT_FACE_SEL) &&
          (ob->mode & (OB_MODE_VERTEX_PAINT | OB_MODE_WEIGHT_PAINT | OB_MODE_TEXTURE_PAINT)));
}

bool BKE_paint_select_vert_test(Object *ob)
{
  return ((ob != NULL) && (ob->type == OB_MESH) && (ob->data != NULL) &&
          (((Mesh *)ob->data)->editflag & ME_EDIT_PAINT_VERT_SEL) &&
          (ob->mode & OB_MODE_WEIGHT_PAINT || ob->mode & OB_MODE_VERTEX_PAINT));
}

bool BKE_paint_select_elem_test(Object *ob)
{
  return (BKE_paint_select_vert_test(ob) || BKE_paint_select_face_test(ob));
}

void BKE_paint_cavity_curve_preset(Paint *p, int preset)
{
  CurveMapping *cumap = NULL;
  CurveMap *cuma = NULL;

  if (!p->cavity_curve) {
    p->cavity_curve = BKE_curvemapping_add(1, 0, 0, 1, 1);
  }
  cumap = p->cavity_curve;
  cumap->flag &= ~CUMA_EXTEND_EXTRAPOLATE;
  cumap->preset = preset;

  cuma = cumap->cm;
  BKE_curvemap_reset(cuma, &cumap->clipr, cumap->preset, CURVEMAP_SLOPE_POSITIVE);
  BKE_curvemapping_changed(cumap, false);
}

eObjectMode BKE_paint_object_mode_from_paintmode(ePaintMode mode)
{
  switch (mode) {
    case PAINT_MODE_SCULPT:
      return OB_MODE_SCULPT;
    case PAINT_MODE_VERTEX:
      return OB_MODE_VERTEX_PAINT;
    case PAINT_MODE_WEIGHT:
      return OB_MODE_WEIGHT_PAINT;
    case PAINT_MODE_TEXTURE_2D:
    case PAINT_MODE_TEXTURE_3D:
      return OB_MODE_TEXTURE_PAINT;
    case PAINT_MODE_SCULPT_UV:
      return OB_MODE_EDIT;
    case PAINT_MODE_INVALID:
    default:
      return 0;
  }
}

bool BKE_paint_ensure(ToolSettings *ts, struct Paint **r_paint)
{
  Paint *paint = NULL;
  if (*r_paint) {
    /* Tool offset should never be 0 for initialized paint settings, so it's a reliable way to
     * check if already initialized. */
    if ((*r_paint)->runtime.tool_offset == 0) {
      /* Currently only image painting is initialized this way, others have to be allocated. */
      BLI_assert(ELEM(*r_paint, (Paint *)&ts->imapaint));

      BKE_paint_runtime_init(ts, *r_paint);
    }
    else {
      BLI_assert(ELEM(*r_paint,
                      /* Cast is annoying, but prevent NULL-pointer access. */
                      (Paint *)ts->gp_paint,
                      (Paint *)ts->gp_vertexpaint,
                      (Paint *)ts->gp_sculptpaint,
                      (Paint *)ts->gp_weightpaint,
                      (Paint *)ts->sculpt,
                      (Paint *)ts->vpaint,
                      (Paint *)ts->wpaint,
                      (Paint *)ts->uvsculpt,
                      (Paint *)ts->curves_sculpt,
                      (Paint *)&ts->imapaint));
#ifdef DEBUG
      struct Paint paint_test = **r_paint;
      BKE_paint_runtime_init(ts, *r_paint);
      /* Swap so debug doesn't hide errors when release fails. */
      SWAP(Paint, **r_paint, paint_test);
      BLI_assert(paint_test.runtime.ob_mode == (*r_paint)->runtime.ob_mode);
      BLI_assert(paint_test.runtime.tool_offset == (*r_paint)->runtime.tool_offset);
#endif
    }
    return true;
  }

  if (((VPaint **)r_paint == &ts->vpaint) || ((VPaint **)r_paint == &ts->wpaint)) {
    VPaint *data = MEM_callocN(sizeof(*data), __func__);
    paint = &data->paint;
  }
  else if ((Sculpt **)r_paint == &ts->sculpt) {
    Sculpt *data = MEM_callocN(sizeof(*data), __func__);
    paint = &data->paint;

    if (!data->channels) {
      data->channels = BKE_brush_channelset_create(__func__);
    }

    BKE_brush_check_toolsettings(data);

    /* Turn on X plane mirror symmetry by default */
    paint->symmetry_flags |= PAINT_SYMM_X;

    /* Make sure at least dyntopo subdivision is enabled */
    data->flags |= SCULPT_DYNTOPO_SUBDIVIDE | SCULPT_DYNTOPO_COLLAPSE | SCULPT_DYNTOPO_CLEANUP |
                   SCULPT_DYNTOPO_ENABLED;
  }
  else if ((GpPaint **)r_paint == &ts->gp_paint) {
    GpPaint *data = MEM_callocN(sizeof(*data), __func__);
    paint = &data->paint;
  }
  else if ((GpVertexPaint **)r_paint == &ts->gp_vertexpaint) {
    GpVertexPaint *data = MEM_callocN(sizeof(*data), __func__);
    paint = &data->paint;
  }
  else if ((GpSculptPaint **)r_paint == &ts->gp_sculptpaint) {
    GpSculptPaint *data = MEM_callocN(sizeof(*data), __func__);
    paint = &data->paint;
  }
  else if ((GpWeightPaint **)r_paint == &ts->gp_weightpaint) {
    GpWeightPaint *data = MEM_callocN(sizeof(*data), __func__);
    paint = &data->paint;
  }
  else if ((UvSculpt **)r_paint == &ts->uvsculpt) {
    UvSculpt *data = MEM_callocN(sizeof(*data), __func__);
    paint = &data->paint;
  }
  else if ((CurvesSculpt **)r_paint == &ts->curves_sculpt) {
    CurvesSculpt *data = MEM_callocN(sizeof(*data), __func__);
    paint = &data->paint;
  }
  else if (*r_paint == &ts->imapaint.paint) {
    paint = &ts->imapaint.paint;
  }

  paint->flags |= PAINT_SHOW_BRUSH;

  *r_paint = paint;

  BKE_paint_runtime_init(ts, paint);

  return false;
}

void BKE_paint_init(Main *bmain, Scene *sce, ePaintMode mode, const char col[3])
{
  UnifiedPaintSettings *ups = &sce->toolsettings->unified_paint_settings;
  Paint *paint = BKE_paint_get_active_from_paintmode(sce, mode);

  BKE_paint_ensure_from_paintmode(sce, mode);

  /* If there's no brush, create one */
  if (PAINT_MODE_HAS_BRUSH(mode)) {
    Brush *brush = BKE_paint_brush(paint);
    if (brush == NULL) {
      eObjectMode ob_mode = BKE_paint_object_mode_from_paintmode(mode);
      brush = BKE_brush_first_search(bmain, ob_mode);
      if (!brush) {
        brush = BKE_brush_add(bmain, "Brush", ob_mode);
        id_us_min(&brush->id); /* fake user only */
      }
      BKE_paint_brush_set(paint, brush);
    }
  }

  memcpy(paint->paint_cursor_col, col, 3);
  paint->paint_cursor_col[3] = 128;
  ups->last_stroke_valid = false;
  zero_v3(ups->average_stroke_accum);
  ups->average_stroke_counter = 0;
  if (!paint->cavity_curve) {
    BKE_paint_cavity_curve_preset(paint, CURVE_PRESET_LINE);
  }
}

void BKE_paint_free(Paint *paint)
{
  BKE_curvemapping_free(paint->cavity_curve);
  MEM_SAFE_FREE(paint->tool_slots);
}

void BKE_paint_copy(Paint *src, Paint *tar, const int flag)
{
  tar->brush = src->brush;
  tar->cavity_curve = BKE_curvemapping_copy(src->cavity_curve);
  tar->tool_slots = MEM_dupallocN(src->tool_slots);

  if ((flag & LIB_ID_CREATE_NO_USER_REFCOUNT) == 0) {
    id_us_plus((ID *)tar->brush);
    id_us_plus((ID *)tar->palette);
    if (src->tool_slots != NULL) {
      for (int i = 0; i < tar->tool_slots_len; i++) {
        id_us_plus((ID *)tar->tool_slots[i].brush);
      }
    }
  }
}

void BKE_paint_stroke_get_average(Scene *scene, Object *ob, float stroke[3])
{
  UnifiedPaintSettings *ups = &scene->toolsettings->unified_paint_settings;
  if (ups->last_stroke_valid && ups->average_stroke_counter > 0) {
    float fac = 1.0f / ups->average_stroke_counter;
    mul_v3_v3fl(stroke, ups->average_stroke_accum, fac);
  }
  else {
    copy_v3_v3(stroke, ob->obmat[3]);
  }
}

void BKE_paint_blend_write(BlendWriter *writer, Paint *p)
{
  if (p->cavity_curve) {
    BKE_curvemapping_blend_write(writer, p->cavity_curve);
  }
  BLO_write_struct_array(writer, PaintToolSlot, p->tool_slots_len, p->tool_slots);
}

void BKE_paint_blend_read_data(BlendDataReader *reader, const Scene *scene, Paint *p)
{
  if (p->num_input_samples < 1) {
    p->num_input_samples = 1;
  }

  BLO_read_data_address(reader, &p->cavity_curve);
  if (p->cavity_curve) {
    BKE_curvemapping_blend_read(reader, p->cavity_curve);
  }
  else {
    BKE_paint_cavity_curve_preset(p, CURVE_PRESET_LINE);
  }

  BLO_read_data_address(reader, &p->tool_slots);

  /* Workaround for invalid data written in older versions. */
  const size_t expected_size = sizeof(PaintToolSlot) * p->tool_slots_len;
  if (p->tool_slots && MEM_allocN_len(p->tool_slots) < expected_size) {
    MEM_freeN(p->tool_slots);
    p->tool_slots = MEM_callocN(expected_size, "PaintToolSlot");
  }

  BKE_paint_runtime_init(scene->toolsettings, p);
}

void BKE_paint_blend_read_lib(BlendLibReader *reader, Scene *sce, Paint *p)
{
  if (p) {
    BLO_read_id_address(reader, sce->id.lib, &p->brush);
    for (int i = 0; i < p->tool_slots_len; i++) {
      if (p->tool_slots[i].brush != NULL) {
        BLO_read_id_address(reader, sce->id.lib, &p->tool_slots[i].brush);
      }
    }
    BLO_read_id_address(reader, sce->id.lib, &p->palette);
    p->paint_cursor = NULL;

    BKE_paint_runtime_init(sce->toolsettings, p);
  }
}

bool paint_is_face_hidden(const MLoopTri *lt, const MVert *mvert, const MLoop *mloop)
{
  return ((mvert[mloop[lt->tri[0]].v].flag & ME_HIDE) ||
          (mvert[mloop[lt->tri[1]].v].flag & ME_HIDE) ||
          (mvert[mloop[lt->tri[2]].v].flag & ME_HIDE));
}

bool paint_is_grid_face_hidden(const uint *grid_hidden, int gridsize, int x, int y)
{
  /* skip face if any of its corners are hidden */
  return (BLI_BITMAP_TEST(grid_hidden, y * gridsize + x) ||
          BLI_BITMAP_TEST(grid_hidden, y * gridsize + x + 1) ||
          BLI_BITMAP_TEST(grid_hidden, (y + 1) * gridsize + x + 1) ||
          BLI_BITMAP_TEST(grid_hidden, (y + 1) * gridsize + x));
}

bool paint_is_bmesh_face_hidden(BMFace *f)
{
  BMLoop *l_iter;
  BMLoop *l_first;

  l_iter = l_first = BM_FACE_FIRST_LOOP(f);
  do {
    if (BM_elem_flag_test(l_iter->v, BM_ELEM_HIDDEN)) {
      return true;
    }
  } while ((l_iter = l_iter->next) != l_first);

  return false;
}

float paint_grid_paint_mask(const GridPaintMask *gpm, uint level, uint x, uint y)
{
  int factor = BKE_ccg_factor(level, gpm->level);
  int gridsize = BKE_ccg_gridsize(gpm->level);

  return gpm->data[(y * factor) * gridsize + (x * factor)];
}

/* threshold to move before updating the brush rotation */
#define RAKE_THRESHHOLD 20

void paint_update_brush_rake_rotation(UnifiedPaintSettings *ups, Brush *brush, float rotation)
{
  if (brush->mtex.brush_angle_mode & MTEX_ANGLE_RAKE) {
    ups->brush_rotation = rotation;
  }
  else {
    ups->brush_rotation = 0.0f;
  }

  if (brush->mask_mtex.brush_angle_mode & MTEX_ANGLE_RAKE) {
    ups->brush_rotation_sec = rotation;
  }
  else {
    ups->brush_rotation_sec = 0.0f;
  }
}

bool paint_calculate_rake_rotation(UnifiedPaintSettings *ups,
                                   Brush *brush,
                                   const float mouse_pos[2],
                                   const float initial_mouse_pos[2])
{
  bool ok = false;
  if ((brush->mtex.brush_angle_mode & MTEX_ANGLE_RAKE) ||
      (brush->mask_mtex.brush_angle_mode & MTEX_ANGLE_RAKE)) {
    const float r = RAKE_THRESHHOLD;
    float rotation;

    if (brush->flag & BRUSH_DRAG_DOT) {
      const float dx = mouse_pos[0] - initial_mouse_pos[0];
      const float dy = mouse_pos[1] - initial_mouse_pos[1];

      if (dx * dx + dy * dy > 0.5f) {
        ups->brush_rotation = ups->brush_rotation_sec = atan2f(dx, dy) + (float)M_PI;
        return true;
      }
      else {
        return false;
      }
    }

    float dpos[2];
    sub_v2_v2v2(dpos, ups->last_rake, mouse_pos);

    if (len_squared_v2(dpos) >= r * r) {
      rotation = atan2f(dpos[0], dpos[1]);

      copy_v2_v2(ups->last_rake, mouse_pos);

      ups->last_rake_angle = rotation;

      paint_update_brush_rake_rotation(ups, brush, rotation);
      ok = true;
    }
    /* make sure we reset here to the last rotation to avoid accumulating
     * values in case a random rotation is also added */
    else {
      paint_update_brush_rake_rotation(ups, brush, ups->last_rake_angle);
      ok = false;
    }
  }
  else {
    ups->brush_rotation = ups->brush_rotation_sec = 0.0f;
    ok = true;
  }
  return ok;
}

void BKE_sculptsession_free_deformMats(SculptSession *ss)
{
  MEM_SAFE_FREE(ss->orig_cos);
  MEM_SAFE_FREE(ss->deform_cos);
  MEM_SAFE_FREE(ss->deform_imats);
}

void BKE_sculptsession_free_vwpaint_data(struct SculptSession *ss)
{
  struct SculptVertexPaintGeomMap *gmap = NULL;
  if (ss->mode_type == OB_MODE_VERTEX_PAINT) {
    gmap = &ss->mode.vpaint.gmap;

    MEM_SAFE_FREE(ss->mode.vpaint.previous_color);
  }
  else if (ss->mode_type == OB_MODE_WEIGHT_PAINT) {
    gmap = &ss->mode.wpaint.gmap;

    MEM_SAFE_FREE(ss->mode.wpaint.alpha_weight);
    if (ss->mode.wpaint.dvert_prev) {
      BKE_defvert_array_free_elems(ss->mode.wpaint.dvert_prev, ss->totvert);
      MEM_freeN(ss->mode.wpaint.dvert_prev);
      ss->mode.wpaint.dvert_prev = NULL;
    }
  }
  else {
    return;
  }
  MEM_SAFE_FREE(gmap->vert_to_loop);
  MEM_SAFE_FREE(gmap->vert_map_mem);
  MEM_SAFE_FREE(gmap->vert_to_poly);
  MEM_SAFE_FREE(gmap->poly_map_mem);
}

/**
 * Write out the sculpt dynamic-topology #BMesh to the #Mesh.
 */
static void sculptsession_bm_to_me_update_data_only(Object *ob, bool reorder)
{
  SculptSession *ss = ob->sculpt;

  if (ss->bm) {
    if (ob->data) {
      // Mesh *me = BKE_object_get_original_mesh(ob);

      BM_mesh_bm_to_me(
          NULL,
          NULL,
          ss->bm,
          ob->data,
          (&(struct BMeshToMeshParams){.calc_object_remap = false,
                                       /*
                                        for memfile undo steps we need to
                                        save id and temporary layers
                                       */
                                       .copy_temp_cdlayers = false,
                                       .ignore_mesh_id_layers = false,
                                       .update_shapekey_indices = true,
                                       .cd_mask_extra = CD_MASK_MESH_ID | CD_MASK_DYNTOPO_VERT

          }));
    }
  }
}

void BKE_sculptsession_bm_to_me(Object *ob, bool reorder)
{
  if (ob && ob->sculpt) {
    sculptsession_bm_to_me_update_data_only(ob, reorder);

    /* Ensure the objects evaluated mesh doesn't hold onto arrays
     * now realloc'd in the mesh T34473. */
    DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  }
}

static void sculptsession_free_pbvh(Object *object)
{
  SculptSession *ss = object->sculpt;

  if (!ss) {
    return;
  }

  if (ss->pbvh) {
    BKE_pbvh_free(ss->pbvh);
    ss->pbvh = NULL;
  }

  MEM_SAFE_FREE(ss->face_areas);

  MEM_SAFE_FREE(ss->pmap);
  MEM_SAFE_FREE(ss->pmap_mem);

  MEM_SAFE_FREE(ss->epmap);
  MEM_SAFE_FREE(ss->epmap_mem);

  MEM_SAFE_FREE(ss->vemap);
  MEM_SAFE_FREE(ss->vemap_mem);

  MEM_SAFE_FREE(ss->preview_vert_index_list);
  ss->preview_vert_index_count = 0;

  MEM_SAFE_FREE(ss->preview_vert_index_list);

  MEM_SAFE_FREE(ss->vertex_info.connected_component);
  MEM_SAFE_FREE(ss->vertex_info.boundary);
  MEM_SAFE_FREE(ss->vertex_info.symmetrize_map);

  MEM_SAFE_FREE(ss->fake_neighbors.fake_neighbor_index);
}

void BKE_sculptsession_bm_to_me_for_render(Object *object)
{
  if (object && object->sculpt) {
    if (object->sculpt->bm) {
      /* Ensure no points to old arrays are stored in DM
       *
       * Apparently, we could not use DEG_id_tag_update
       * here because this will lead to the while object
       * surface to disappear, so we'll release DM in place.
       */
      BKE_object_free_derived_caches(object);

      sculptsession_bm_to_me_update_data_only(object, false);

      /* In contrast with sculptsession_bm_to_me no need in
       * DAG tag update here - derived mesh was freed and
       * old pointers are nowhere stored.
       */
    }
  }
}

void BKE_sculptsession_free(Object *ob)
{
  if (ob && ob->sculpt) {
    SculptSession *ss = ob->sculpt;

    if (ss->mdyntopo_verts) {
      MEM_freeN(ss->mdyntopo_verts);
      ss->mdyntopo_verts = NULL;
    }

    if (ss->bm_log && BM_log_free(ss->bm_log, true)) {
      ss->bm_log = NULL;
    }

    /*try to save current mesh*/
    if (ss->bm) {
      SCULPT_on_sculptsession_bmesh_free(ss);

      BKE_sculptsession_bm_to_me(ob, true);
      BM_mesh_free(ss->bm);
    }

    CustomData_free(&ss->temp_vdata, ss->temp_vdata_elems);
    CustomData_free(&ss->temp_pdata, ss->temp_pdata_elems);

    sculptsession_free_pbvh(ob);

    for (int i = 0; i < SCULPT_SCL_LAYER_MAX; i++) {
      MEM_SAFE_FREE(ss->custom_layers[i]);
    }

    MEM_SAFE_FREE(ss->pmap);
    MEM_SAFE_FREE(ss->pmap_mem);

    MEM_SAFE_FREE(ss->epmap);
    MEM_SAFE_FREE(ss->epmap_mem);

    MEM_SAFE_FREE(ss->vemap);
    MEM_SAFE_FREE(ss->vemap_mem);

    MEM_SAFE_FREE(ss->texcache);

    bool SCULPT_attr_release_layer(
        SculptSession * ss, Object * ob, struct SculptCustomLayer * scl);

    if (ss->layers_to_free) {
      for (int i = 0; i < ss->tot_layers_to_free; i++) {
        if (ss->layers_to_free[i]) {
          SCULPT_attr_release_layer(ss, ob, ss->layers_to_free[i]);
          // SCULPT_attr_release_layer frees layers_to_free[i] itself
        }
      }

      MEM_freeN(ss->layers_to_free);
    }

    if (ss->tex_pool) {
      BKE_image_pool_free(ss->tex_pool);
    }

    MEM_SAFE_FREE(ss->orig_cos);
    MEM_SAFE_FREE(ss->deform_cos);
    MEM_SAFE_FREE(ss->deform_imats);

    if (ss->pose_ik_chain_preview) {
      for (int i = 0; i < ss->pose_ik_chain_preview->tot_segments; i++) {
        MEM_SAFE_FREE(ss->pose_ik_chain_preview->segments[i].weights);
      }
      MEM_SAFE_FREE(ss->pose_ik_chain_preview->segments);
      MEM_SAFE_FREE(ss->pose_ik_chain_preview);
    }

    if (ss->boundary_preview) {
      MEM_SAFE_FREE(ss->boundary_preview->vertices);
      MEM_SAFE_FREE(ss->boundary_preview->edges);
      MEM_SAFE_FREE(ss->boundary_preview->distance);
      MEM_SAFE_FREE(ss->boundary_preview->edit_info);
      MEM_SAFE_FREE(ss->boundary_preview);
    }

    BKE_sculptsession_free_vwpaint_data(ob->sculpt);

    MEM_freeN(ss);

    ob->sculpt = NULL;
  }
}

MultiresModifierData *BKE_sculpt_multires_active(const Scene *scene, Object *ob)
{
  Mesh *me = (Mesh *)ob->data;
  ModifierData *md;
  VirtualModifierData virtualModifierData;

  if (ob->sculpt && ob->sculpt->bm) {
    /* can't combine multires and dynamic topology */
    return NULL;
  }

  if (!CustomData_get_layer(&me->ldata, CD_MDISPS)) {
    /* multires can't work without displacement layer */
    return NULL;
  }

  /* Weight paint operates on original vertices, and needs to treat multires as regular modifier
   * to make it so that PBVH vertices are at the multires surface. */
  if ((ob->mode & OB_MODE_SCULPT) == 0) {
    return NULL;
  }

  for (md = BKE_modifiers_get_virtual_modifierlist(ob, &virtualModifierData); md; md = md->next) {
    if (md->type == eModifierType_Multires) {
      MultiresModifierData *mmd = (MultiresModifierData *)md;

      if (!BKE_modifier_is_enabled(scene, md, eModifierMode_Realtime)) {
        continue;
      }

      if (mmd->sculptlvl > 0 && !(mmd->flags & eMultiresModifierFlag_UseSculptBaseMesh)) {
        return mmd;
      }

      return NULL;
    }
  }

  return NULL;
}

/* Checks if there are any supported deformation modifiers active */
static bool sculpt_modifiers_active(Scene *scene, Sculpt *sd, Object *ob)
{
  ModifierData *md;
  Mesh *me = (Mesh *)ob->data;
  VirtualModifierData virtualModifierData;

  if (ob->sculpt->bm || BKE_sculpt_multires_active(scene, ob)) {
    return false;
  }

  /* non-locked shape keys could be handled in the same way as deformed mesh */
  if ((ob->shapeflag & OB_SHAPE_LOCK) == 0 && me->key && ob->shapenr) {
    return true;
  }

  md = BKE_modifiers_get_virtual_modifierlist(ob, &virtualModifierData);

  /* exception for shape keys because we can edit those */
  for (; md; md = md->next) {
    const ModifierTypeInfo *mti = BKE_modifier_get_info(md->type);
    if (!BKE_modifier_is_enabled(scene, md, eModifierMode_Realtime)) {
      continue;
    }
    if (md->type == eModifierType_Multires && (ob->mode & OB_MODE_SCULPT)) {
      MultiresModifierData *mmd = (MultiresModifierData *)md;
      if (!(mmd->flags & eMultiresModifierFlag_UseSculptBaseMesh)) {
        continue;
      }
    }
    if (md->type == eModifierType_ShapeKey) {
      continue;
    }

    if (mti->type == eModifierTypeType_OnlyDeform) {
      return true;
    }
    if ((sd->flags & SCULPT_ONLY_DEFORM) == 0) {
      return true;
    }
  }

  return false;
}

char BKE_get_fset_boundary_symflag(Object *object)
{
  const Mesh *mesh = BKE_mesh_from_object(object);
  return mesh->flag & ME_SCULPT_MIRROR_FSET_BOUNDARIES ? mesh->symmetry : 0;
}

void BKE_sculptsession_ignore_uvs_set(Object *ob, bool value)
{
  ob->sculpt->ignore_uvs = value;

  if (ob->sculpt->pbvh) {
    BKE_pbvh_ignore_uvs_set(ob->sculpt->pbvh, value);
  }
}
/**
 * \param need_mask: So that the evaluated mesh that is returned has mask data.
 */
static void sculpt_update_object(Depsgraph *depsgraph,
                                 Object *ob,
                                 Mesh *me_eval,
                                 bool need_pmap,
                                 bool need_mask,
                                 bool UNUSED(need_colors))
{
  Scene *scene = DEG_get_input_scene(depsgraph);
  Sculpt *sd = scene->toolsettings->sculpt;
  SculptSession *ss = ob->sculpt;
  Mesh *me = BKE_object_get_original_mesh(ob);
  MultiresModifierData *mmd = BKE_sculpt_multires_active(scene, ob);
  const bool use_face_sets = (ob->mode & OB_MODE_SCULPT) != 0;

  ss->depsgraph = depsgraph;

  ss->bm_smooth_shading = scene->toolsettings->sculpt->flags & SCULPT_DYNTOPO_SMOOTH_SHADING;
  ss->ignore_uvs = me->flag & ME_SCULPT_IGNORE_UVS;

  ss->deform_modifiers_active = sculpt_modifiers_active(scene, sd, ob);
  ss->show_mask = (sd->flags & SCULPT_HIDE_MASK) == 0;
  ss->show_face_sets = (sd->flags & SCULPT_HIDE_FACE_SETS) == 0;

  ss->building_vp_handle = false;

  ss->scene = scene;
  if (sd->channels) {
    ss->save_temp_layers = BRUSHSET_GET_INT(sd->channels, save_temp_layers, NULL);
  }

  if (need_mask) {
    if (mmd == NULL) {
      BLI_assert(CustomData_has_layer(&me->vdata, CD_PAINT_MASK));
    }
    else {
      BLI_assert(CustomData_has_layer(&me->ldata, CD_GRID_PAINT_MASK));
    }
  }

  ss->shapekey_active = (mmd == NULL) ? BKE_keyblock_from_object(ob) : NULL;
  ss->boundary_symmetry = (int)BKE_get_fset_boundary_symflag(ob);

  /* NOTE: Weight pPaint require mesh info for loop lookup, but it never uses multires code path,
   * so no extra checks is needed here. */
  if (mmd) {
    ss->multires.active = true;
    ss->multires.modifier = mmd;
    ss->multires.level = mmd->sculptlvl;
    ss->totvert = me_eval->totvert;
    ss->totpoly = me_eval->totpoly;
    ss->totfaces = me->totpoly;
    ss->totloops = me->totloop;
    ss->totedges = me->totedge;

    /* These are assigned to the base mesh in Multires. This is needed because Face Sets operators
     * and tools use the Face Sets data from the base mesh when Multires is active. */
    ss->mvert = me->mvert;
    ss->medge = me->medge;
    ss->mloop = me->mloop;
    ss->mpoly = me->mpoly;
  }
  else {
    ss->totvert = me->totvert;
    ss->totpoly = me->totpoly;
    ss->totfaces = me->totpoly;
    ss->mvert = me->mvert;
    ss->medge = me->medge;
    ss->mpoly = me->mpoly;
    ss->mloop = me->mloop;
    ss->multires.active = false;
    ss->multires.modifier = NULL;
    ss->multires.level = 0;
    ss->vmask = CustomData_get_layer(&me->vdata, CD_PAINT_MASK);

    ss->totloops = me->totloop;
    ss->totedges = me->totedge;

    ss->vdata = &me->vdata;
    ss->edata = &me->edata;
    ss->ldata = &me->ldata;
    ss->pdata = &me->pdata;

    CustomDataLayer *cl;
    AttributeDomain domain;

    ss->vcol = NULL;
    ss->mcol = NULL;

    if (BKE_pbvh_get_color_layer(ss->pbvh, me, &cl, &domain)) {
      if (cl->type == CD_PROP_COLOR) {
        ss->vcol = cl->data;
      }
      else {
        ss->mcol = cl->data;
      }

      ss->vcol_domain = domain;
      ss->vcol_type = cl->type;
    }
    else {
      ss->vcol_type = -1;
    }
  }

  /* Sculpt Face Sets. */
  if (use_face_sets) {
    BLI_assert(CustomData_has_layer(&me->pdata, CD_SCULPT_FACE_SETS));
    ss->face_sets = CustomData_get_layer(&me->pdata, CD_SCULPT_FACE_SETS);
  }
  else {
    ss->face_sets = NULL;
  }

  ss->subdiv_ccg = me_eval->runtime.subdiv_ccg;
  ss->fast_draw = (scene->toolsettings->sculpt->flags & SCULPT_FAST_DRAW) != 0;

  PBVH *pbvh = BKE_sculpt_object_pbvh_ensure(depsgraph, ob);

  if (BKE_pbvh_type(pbvh) == PBVH_FACES) {
    ss->vert_normals = BKE_pbvh_get_vert_normals(ss->pbvh);
  }
  else {
    ss->vert_normals = NULL;
  }

  BLI_assert(pbvh == ss->pbvh);
  UNUSED_VARS_NDEBUG(pbvh);

  BKE_pbvh_subdiv_cgg_set(ss->pbvh, ss->subdiv_ccg);
  BKE_pbvh_face_sets_set(ss->pbvh, ss->face_sets);

  BKE_pbvh_face_sets_color_set(ss->pbvh, me->face_sets_color_seed, me->face_sets_color_default);

  if (need_pmap && ob->type == OB_MESH && !ss->pmap) {
    BKE_mesh_vert_poly_map_create(&ss->pmap,
                                  &ss->pmap_mem,
                                  me->mvert,
                                  me->medge,
                                  me->mpoly,
                                  me->mloop,
                                  me->totvert,
                                  me->totpoly,
                                  me->totloop,
                                  false);
  }

  pbvh_show_mask_set(ss->pbvh, ss->show_mask);
  pbvh_show_face_sets_set(ss->pbvh, ss->show_face_sets);

  if (ss->deform_modifiers_active) {
    if (!ss->orig_cos) {
      int a;

      BKE_sculptsession_free_deformMats(ss);

      ss->orig_cos = (ss->shapekey_active) ?
                         BKE_keyblock_convert_to_vertcos(ob, ss->shapekey_active) :
                         BKE_mesh_vert_coords_alloc(me, NULL);

      BKE_crazyspace_build_sculpt(depsgraph, scene, ob, &ss->deform_imats, &ss->deform_cos);
      BKE_pbvh_vert_coords_apply(ss->pbvh, ss->deform_cos, me->totvert);

      for (a = 0; a < me->totvert; a++) {
        invert_m3(ss->deform_imats[a]);
      }
    }
  }
  else {
    BKE_sculptsession_free_deformMats(ss);
  }

  if (ss->shapekey_active != NULL && ss->deform_cos == NULL) {
    ss->deform_cos = BKE_keyblock_convert_to_vertcos(ob, ss->shapekey_active);
  }

  /* if pbvh is deformed, key block is already applied to it */
  if (ss->shapekey_active) {
    bool pbvh_deformed = BKE_pbvh_is_deformed(ss->pbvh);
    if (!pbvh_deformed || ss->deform_cos == NULL) {
      float(*vertCos)[3] = BKE_keyblock_convert_to_vertcos(ob, ss->shapekey_active);

      if (vertCos) {
        if (!pbvh_deformed) {
          /* apply shape keys coordinates to PBVH */
          BKE_pbvh_vert_coords_apply(ss->pbvh, vertCos, me->totvert);
        }
        if (ss->deform_cos == NULL) {
          ss->deform_cos = vertCos;
        }
        if (vertCos != ss->deform_cos) {
          MEM_freeN(vertCos);
        }
      }
    }
  }

  int totvert = 0;

  switch (BKE_pbvh_type(pbvh)) {
    case PBVH_FACES:
      totvert = me->totvert;
      break;
    case PBVH_BMESH:
      totvert = ss->bm ? ss->bm->totvert : me->totvert;
      break;
    case PBVH_GRIDS:
      totvert = BKE_pbvh_get_grid_num_vertices(ss->pbvh);
      break;
  }

  BKE_sculptsession_check_sculptverts(ob->sculpt, pbvh, totvert);

  if (ss->bm && me->key && ob->shapenr != ss->bm->shapenr) {
    KeyBlock *actkey = BLI_findlink(&me->key->block, ss->bm->shapenr - 1);
    KeyBlock *newkey = BLI_findlink(&me->key->block, ob->shapenr - 1);

    bool updatePBVH = false;

    if (!actkey) {
      printf("%s: failed to find active shapekey\n", __func__);
      if (!ss->bm->shapenr || !CustomData_has_layer(&ss->bm->vdata, CD_SHAPEKEY)) {
        printf("allocating shapekeys. . .\n");

        // need to allocate customdata for keys
        for (KeyBlock *key = (KeyBlock *)me->key->block.first; key; key = key->next) {

          int idx = CustomData_get_named_layer_index(&ss->bm->vdata, CD_SHAPEKEY, key->name);

          if (idx == -1) {
            BM_data_layer_add_named(ss->bm, &ss->bm->vdata, CD_SHAPEKEY, key->name);
            BKE_sculptsession_update_attr_refs(ob);

            idx = CustomData_get_named_layer_index(&ss->bm->vdata, CD_SHAPEKEY, key->name);
            ss->bm->vdata.layers[idx].uid = key->uid;
          }

          int cd_shapeco = ss->bm->vdata.layers[idx].offset;
          BMVert *v;
          BMIter iter;

          BM_ITER_MESH (v, &iter, ss->bm, BM_VERTS_OF_MESH) {
            float *keyco = BM_ELEM_CD_GET_VOID_P(v, cd_shapeco);

            copy_v3_v3(keyco, v->co);
          }
        }
      }

      updatePBVH = true;
      ss->bm->shapenr = ob->shapenr;
    }

    if (!newkey) {
      printf("%s: failed to find new active shapekey\n", __func__);
    }

    if (actkey && newkey) {
      int cd_co1 = CustomData_get_named_layer_index(&ss->bm->vdata, CD_SHAPEKEY, actkey->name);
      int cd_co2 = CustomData_get_named_layer_index(&ss->bm->vdata, CD_SHAPEKEY, newkey->name);

      BMVert *v;
      BMIter iter;

      if (cd_co1 == -1) {  // non-recoverable error
        printf("%s: failed to find active shapekey in customdata.\n", __func__);
        return;
      }
      else if (cd_co2 == -1) {
        printf("%s: failed to find new shapekey in customdata; allocating . . .\n", __func__);

        BM_data_layer_add_named(ss->bm, &ss->bm->vdata, CD_SHAPEKEY, newkey->name);
        int idx = CustomData_get_named_layer_index(&ss->bm->vdata, CD_SHAPEKEY, newkey->name);

        int cd_co = ss->bm->vdata.layers[idx].offset;
        ss->bm->vdata.layers[idx].uid = newkey->uid;

        BKE_sculptsession_update_attr_refs(ob);

        BM_ITER_MESH (v, &iter, ss->bm, BM_VERTS_OF_MESH) {
          float *keyco = BM_ELEM_CD_GET_VOID_P(v, cd_co);
          copy_v3_v3(keyco, v->co);
        }

        cd_co2 = idx;
      }

      cd_co1 = ss->bm->vdata.layers[cd_co1].offset;
      cd_co2 = ss->bm->vdata.layers[cd_co2].offset;

      BM_ITER_MESH (v, &iter, ss->bm, BM_VERTS_OF_MESH) {
        float *co1 = BM_ELEM_CD_GET_VOID_P(v, cd_co1);
        float *co2 = BM_ELEM_CD_GET_VOID_P(v, cd_co2);

        copy_v3_v3(co1, v->co);
        copy_v3_v3(v->co, co2);
      }

      ss->bm->shapenr = ob->shapenr;

      updatePBVH = true;
    }

    if (updatePBVH && ss->pbvh) {
      PBVHNode **nodes;
      int totnode;
      BKE_pbvh_get_nodes(ss->pbvh, PBVH_Leaf, &nodes, &totnode);

      for (int i = 0; i < totnode; i++) {
        BKE_pbvh_node_mark_update(nodes[i]);
        BKE_pbvh_node_mark_update_tri_area(nodes[i]);
      }
    }
  }
}

void BKE_sculpt_update_object_before_eval(Object *ob)
{
  /* Update before mesh evaluation in the dependency graph. */
  SculptSession *ss = ob->sculpt;

  if (ss && (ss->building_vp_handle == false || ss->needs_pbvh_rebuild)) {
    if (ss->needs_pbvh_rebuild || (!ss->cache && !ss->filter_cache && !ss->expand_cache)) {
      /* We free pbvh on changes, except in the middle of drawing a stroke
       * since it can't deal with changing PVBH node organization, we hope
       * topology does not change in the meantime .. weak. */
      sculptsession_free_pbvh(ob);

      BKE_sculptsession_free_deformMats(ob->sculpt);

      /* In vertex/weight paint, force maps to be rebuilt. */
      BKE_sculptsession_free_vwpaint_data(ob->sculpt);
    }
    else if (ss->pbvh) {
      PBVHNode **nodes;
      int n, totnode;

      BKE_pbvh_search_gather(ss->pbvh, NULL, NULL, &nodes, &totnode);

      for (n = 0; n < totnode; n++) {
        BKE_pbvh_node_mark_update(nodes[n]);
      }

      MEM_SAFE_FREE(nodes);
    }
  }
}

void BKE_sculpt_update_object_after_eval(Depsgraph *depsgraph, Object *ob_eval)
{
  /* Update after mesh evaluation in the dependency graph, to rebuild PBVH or
   * other data when modifiers change the mesh. */
  Object *ob_orig = DEG_get_original_object(ob_eval);
  Mesh *me_eval = BKE_object_get_evaluated_mesh(ob_eval);
  Mesh *me_orig = BKE_object_get_original_mesh(ob_orig);

  BLI_assert(me_eval != NULL);
  sculpt_update_object(depsgraph, ob_orig, me_eval, false, false, false);
  BKE_sculptsession_sync_attributes(ob_orig, me_orig);
}

void BKE_sculpt_color_layer_create_if_needed(struct Object *object)
{
  Mesh *orig_me = BKE_object_get_original_mesh(object);

  int types[] = {CD_PROP_COLOR, CD_MLOOPCOL};
  bool has_color = false;

  for (int i = 0; i < ARRAY_SIZE(types); i++) {
    bool ok = CustomData_has_layer(&orig_me->vdata, types[i]);
    ok = ok || CustomData_has_layer(&orig_me->ldata, types[i]);

    if (ok) {
      has_color = true;
      break;
    }
  }

  CustomDataLayer *cl;
  if (has_color) {
    cl = BKE_id_attributes_active_color_get(&orig_me->id);

    if (!cl || !ELEM(cl->type, CD_PROP_COLOR, CD_MLOOPCOL)) {
      cl = NULL;

      /* find a color layer */
      for (int step = 0; !cl && step < 2; step++) {
        CustomData *cdata = step ? &orig_me->ldata : &orig_me->vdata;

        for (int i = 0; i < cdata->totlayer; i++) {
          if (ELEM(cdata->layers[i].type, CD_PROP_COLOR, CD_MLOOPCOL)) {
            cl = cdata->layers + i;
            break;
          }
        }
      }
    }
    else {
      cl = NULL; /* no need to update active layer */
    }
  }
  else {
    CustomData_add_layer(&orig_me->vdata, CD_PROP_COLOR, CD_DEFAULT, NULL, orig_me->totvert);
    cl = orig_me->vdata.layers + CustomData_get_layer_index(&orig_me->vdata, CD_PROP_COLOR);

    BKE_id_attributes_render_color_set(&orig_me->id, cl);
    BKE_id_attributes_active_color_set(&orig_me->id, cl);

    BKE_mesh_update_customdata_pointers(orig_me, true);
    DEG_id_tag_update(&orig_me->id, ID_RECALC_GEOMETRY_ALL_MODES);
  }

  if (cl) {
    BKE_id_attributes_active_color_set(&orig_me->id, cl);
  }

  BKE_sculptsession_sync_attributes(object, orig_me);
}

void BKE_sculpt_update_object_for_edit(
    Depsgraph *depsgraph, Object *ob_orig, bool need_pmap, bool need_mask, bool need_colors)
{
  /* Update from sculpt operators and undo, to update sculpt session
   * and PBVH after edits. */
  Scene *scene_eval = DEG_get_evaluated_scene(depsgraph);
  Object *ob_eval = DEG_get_evaluated_object(depsgraph, ob_orig);
  Mesh *me_eval = mesh_get_eval_final(depsgraph, scene_eval, ob_eval, &CD_MASK_BAREMESH);

  BLI_assert(ob_orig == DEG_get_original_object(ob_orig));

  sculpt_update_object(depsgraph, ob_orig, me_eval, true, need_mask, need_colors);
}

int BKE_sculpt_mask_layers_ensure(Object *ob, MultiresModifierData *mmd)
{
  const float *paint_mask;
  Mesh *me = ob->data;
  int ret = 0;

  paint_mask = CustomData_get_layer(&me->vdata, CD_PAINT_MASK);

  /* if multires is active, create a grid paint mask layer if there
   * isn't one already */
  if (mmd && !CustomData_has_layer(&me->ldata, CD_GRID_PAINT_MASK)) {
    GridPaintMask *gmask;
    int level = max_ii(1, mmd->sculptlvl);
    int gridsize = BKE_ccg_gridsize(level);
    int gridarea = gridsize * gridsize;
    int i, j;

    gmask = CustomData_add_layer(&me->ldata, CD_GRID_PAINT_MASK, CD_CALLOC, NULL, me->totloop);

    for (i = 0; i < me->totloop; i++) {
      GridPaintMask *gpm = &gmask[i];

      gpm->level = level;
      gpm->data = MEM_callocN(sizeof(float) * gridarea, "GridPaintMask.data");
    }

    /* if vertices already have mask, copy into multires data */
    if (paint_mask) {
      for (i = 0; i < me->totpoly; i++) {
        const MPoly *p = &me->mpoly[i];
        float avg = 0;

        /* mask center */
        for (j = 0; j < p->totloop; j++) {
          const MLoop *l = &me->mloop[p->loopstart + j];
          avg += paint_mask[l->v];
        }
        avg /= (float)p->totloop;

        /* fill in multires mask corner */
        for (j = 0; j < p->totloop; j++) {
          GridPaintMask *gpm = &gmask[p->loopstart + j];
          const MLoop *l = &me->mloop[p->loopstart + j];
          const MLoop *prev = ME_POLY_LOOP_PREV(me->mloop, p, j);
          const MLoop *next = ME_POLY_LOOP_NEXT(me->mloop, p, j);

          gpm->data[0] = avg;
          gpm->data[1] = (paint_mask[l->v] + paint_mask[next->v]) * 0.5f;
          gpm->data[2] = (paint_mask[l->v] + paint_mask[prev->v]) * 0.5f;
          gpm->data[3] = paint_mask[l->v];
        }
      }
    }

    ret |= SCULPT_MASK_LAYER_CALC_LOOP;
  }

  /* create vertex paint mask layer if there isn't one already */
  if (!paint_mask) {
    CustomData_add_layer(&me->vdata, CD_PAINT_MASK, CD_CALLOC, NULL, me->totvert);
    ret |= SCULPT_MASK_LAYER_CALC_VERT;
  }

  return ret;
}

void BKE_sculpt_toolsettings_data_ensure(struct Scene *scene)
{
  BKE_paint_ensure(scene->toolsettings, (Paint **)&scene->toolsettings->sculpt);

  Sculpt *sd = scene->toolsettings->sculpt;
  if (!sd->detail_size) {
    sd->detail_size = 8.0f;
  }

  if (!sd->dyntopo_radius_scale) {
    sd->dyntopo_radius_scale = 1.0f;
  }

  // we check these flags here in case versioning code fails
  if (!sd->detail_range || !sd->dyntopo_spacing) {
    sd->flags |= SCULPT_DYNTOPO_CLEANUP | SCULPT_DYNTOPO_ENABLED;
  }

  if (!sd->detail_range) {
    sd->detail_range = 0.4f;
  }

  if (!sd->detail_percent) {
    sd->detail_percent = 25;
  }

  if (!sd->dyntopo_spacing) {
    sd->dyntopo_spacing = 35;
  }

  if (sd->constant_detail == 0.0f) {
    sd->constant_detail = 3.0f;
  }

  /* Set sane default tiling offsets */
  if (!sd->paint.tile_offset[0]) {
    sd->paint.tile_offset[0] = 1.0f;
  }
  if (!sd->paint.tile_offset[1]) {
    sd->paint.tile_offset[1] = 1.0f;
  }
  if (!sd->paint.tile_offset[2]) {
    sd->paint.tile_offset[2] = 1.0f;
  }
}

static bool check_sculpt_object_deformed(Object *object, const bool for_construction)
{
  bool deformed = false;

  /* Active modifiers means extra deformation, which can't be handled correct
   * on birth of PBVH and sculpt "layer" levels, so use PBVH only for internal brush
   * stuff and show final evaluated mesh so user would see actual object shape.
   */
  deformed |= object->sculpt->deform_modifiers_active;

  if (for_construction) {
    deformed |= object->sculpt->shapekey_active != NULL;
  }
  else {
    /* As in case with modifiers, we can't synchronize deformation made against
     * PBVH and non-locked keyblock, so also use PBVH only for brushes and
     * final DM to give final result to user.
     */
    deformed |= object->sculpt->shapekey_active && (object->shapeflag & OB_SHAPE_LOCK) == 0;
  }

  return deformed;
}

void BKE_sculpt_face_sets_ensure_from_base_mesh_visibility(Mesh *mesh)
{
  const int face_sets_default_visible_id = 1;
  const int face_sets_default_hidden_id = -(face_sets_default_visible_id + 1);

  bool initialize_new_face_sets = false;

  if (CustomData_has_layer(&mesh->pdata, CD_SCULPT_FACE_SETS)) {
    /* Make everything visible. */
    int *current_face_sets = CustomData_get_layer(&mesh->pdata, CD_SCULPT_FACE_SETS);
    for (int i = 0; i < mesh->totpoly; i++) {
      current_face_sets[i] = abs(current_face_sets[i]);
    }
  }
  else {
    initialize_new_face_sets = true;
    int *new_face_sets = CustomData_add_layer(
        &mesh->pdata, CD_SCULPT_FACE_SETS, CD_CALLOC, NULL, mesh->totpoly);

    /* Initialize the new Face Set data-layer with a default valid visible ID and set the default
     * color to render it white. */
    for (int i = 0; i < mesh->totpoly; i++) {
      new_face_sets[i] = face_sets_default_visible_id;
    }
    mesh->face_sets_color_default = face_sets_default_visible_id;
  }

  int *face_sets = CustomData_get_layer(&mesh->pdata, CD_SCULPT_FACE_SETS);

  for (int i = 0; i < mesh->totpoly; i++) {
    if (!(mesh->mpoly[i].flag & ME_HIDE)) {
      continue;
    }

    if (initialize_new_face_sets) {
      /* When initializing a new Face Set data-layer, assign a new hidden Face Set ID to hidden
       * vertices. This way, we get at initial split in two Face Sets between hidden and
       * visible vertices based on the previous mesh visibly from other mode that can be
       * useful in some cases. */
      face_sets[i] = face_sets_default_hidden_id;
    }
    else {
      /* Otherwise, set the already existing Face Set ID to hidden. */
      face_sets[i] = -abs(face_sets[i]);
    }
  }
}

void BKE_sculpt_sync_face_sets_visibility_to_base_mesh(Mesh *mesh)
{
  int *face_sets = CustomData_get_layer(&mesh->pdata, CD_SCULPT_FACE_SETS);
  if (!face_sets) {
    return;
  }

  for (int i = 0; i < mesh->totpoly; i++) {
    const bool is_face_set_visible = face_sets[i] >= 0;
    SET_FLAG_FROM_TEST(mesh->mpoly[i].flag, !is_face_set_visible, ME_HIDE);
  }

  BKE_mesh_flush_hidden_from_polys(mesh);
}

void BKE_sculpt_sync_face_sets_visibility_to_grids(Mesh *mesh, SubdivCCG *subdiv_ccg)
{
  int *face_sets = CustomData_get_layer(&mesh->pdata, CD_SCULPT_FACE_SETS);
  if (!face_sets) {
    return;
  }

  if (!subdiv_ccg) {
    return;
  }

  CCGKey key;
  BKE_subdiv_ccg_key_top_level(&key, subdiv_ccg);
  for (int i = 0; i < mesh->totloop; i++) {
    const int face_index = BKE_subdiv_ccg_grid_to_face_index(subdiv_ccg, i);
    const bool is_hidden = (face_sets[face_index] < 0);

    /* Avoid creating and modifying the grid_hidden bitmap if the base mesh face is visible and
     * there is not bitmap for the grid. This is because missing grid_hidden implies grid is
     * fully visible. */
    if (is_hidden) {
      BKE_subdiv_ccg_grid_hidden_ensure(subdiv_ccg, i);
    }

    BLI_bitmap *gh = subdiv_ccg->grid_hidden[i];
    if (gh) {
      BLI_bitmap_set_all(gh, is_hidden, key.grid_area);
    }
  }
}

void BKE_sculpt_sync_face_set_visibility(struct Mesh *mesh, struct SubdivCCG *subdiv_ccg)
{
  BKE_sculpt_face_sets_ensure_from_base_mesh_visibility(mesh);
  BKE_sculpt_sync_face_sets_visibility_to_base_mesh(mesh);
  BKE_sculpt_sync_face_sets_visibility_to_grids(mesh, subdiv_ccg);
}

void BKE_sculpt_ensure_orig_mesh_data(Scene *scene, Object *object)
{
  Mesh *mesh = BKE_mesh_from_object(object);
  MultiresModifierData *mmd = BKE_sculpt_multires_active(scene, object);

  BLI_assert(object->mode == OB_MODE_SCULPT);

  /* Copy the current mesh visibility to the Face Sets. */
  BKE_sculpt_face_sets_ensure_from_base_mesh_visibility(mesh);
  if (object->sculpt != NULL) {
    /* If a sculpt session is active, ensure we have its faceset data porperly up-to-date. */
    object->sculpt->face_sets = CustomData_get_layer(&mesh->pdata, CD_SCULPT_FACE_SETS);

    /* NOTE: In theory we could add that on the fly when required by sculpt code.
     * But this then requires proper update of depsgraph etc. For now we play safe, optimization
     * is always possible later if it's worth it. */
    BKE_sculpt_mask_layers_ensure(object, mmd);
  }

  /* Tessfaces aren't used and will become invalid. */
  BKE_mesh_tessface_clear(mesh);

  /* We always need to flush updates from depsgraph here, since at the very least
   * `BKE_sculpt_face_sets_ensure_from_base_mesh_visibility()` will have updated some data layer
   * of the mesh.
   *
   * All known potential sources of updates:
   *   - Addition of, or changes to, the `CD_SCULPT_FACE_SETS` data layer
   *     (`BKE_sculpt_face_sets_ensure_from_base_mesh_visibility`).
   *   - Addition of a `CD_PAINT_MASK` data layer (`BKE_sculpt_mask_layers_ensure`).
   *   - Object has any active modifier (modifier stack can be different in Sculpt mode).
   *   - Multires:
   *     + Differences of subdiv levels between sculpt and object modes
   *       (`mmd->sculptlvl != mmd->lvl`).
   *     + Addition of a `CD_GRID_PAINT_MASK` data layer (`BKE_sculpt_mask_layers_ensure`).
   */
  DEG_id_tag_update(&object->id, ID_RECALC_GEOMETRY);
}

static PBVH *build_pbvh_for_dynamic_topology(Object *ob, bool update_sculptverts)
{
  PBVH *pbvh = BKE_pbvh_new();

  BKE_pbvh_set_symmetry(pbvh, 0, (int)BKE_get_fset_boundary_symflag(ob));

  BKE_pbvh_build_bmesh(pbvh,
                       BKE_object_get_original_mesh(ob),
                       ob->sculpt->bm,
                       ob->sculpt->bm_smooth_shading,
                       ob->sculpt->bm_log,
                       ob->sculpt->cd_vert_node_offset,
                       ob->sculpt->cd_face_node_offset,
                       ob->sculpt->cd_sculpt_vert,
                       ob->sculpt->cd_face_areas,
                       ob->sculpt->fast_draw,
                       update_sculptverts);
  pbvh_show_mask_set(pbvh, ob->sculpt->show_mask);
  pbvh_show_face_sets_set(pbvh, false);

  return pbvh;
}

static PBVH *build_pbvh_from_regular_mesh(Object *ob, Mesh *me_eval_deform, bool respect_hide)
{
  SculptSession *ss = ob->sculpt;
  Mesh *me = BKE_object_get_original_mesh(ob);
  const int looptris_num = poly_to_tri_count(me->totpoly, me->totloop);
  PBVH *pbvh = BKE_pbvh_new();
  BKE_pbvh_respect_hide_set(pbvh, respect_hide);

  MLoopTri *looptri = MEM_malloc_arrayN(looptris_num, sizeof(*looptri), __func__);

  BKE_mesh_recalc_looptri(me->mloop, me->mpoly, me->mvert, me->totloop, me->totpoly, looptri);

  BKE_sculpt_sync_face_set_visibility(me, NULL);

  if (!ss->pmap) {
    BKE_mesh_vert_poly_map_create(&ss->pmap,
                                  &ss->pmap_mem,
                                  me->mvert,
                                  me->medge,
                                  me->mpoly,
                                  me->mloop,
                                  me->totvert,
                                  me->totpoly,
                                  me->totloop,
                                  false);
  }

  BKE_sculptsession_check_sculptverts(ob->sculpt, pbvh, me->totvert);

  MEM_SAFE_FREE(ss->face_areas);
  ss->face_areas = MEM_calloc_arrayN(me->totpoly, sizeof(float) * 2, "ss->face_areas");

  BKE_pbvh_build_mesh(pbvh,
                      me,
                      me->mpoly,
                      me->mloop,
                      me->mvert,
                      ss->mdyntopo_verts,
                      me->totvert,
                      &me->vdata,
                      &me->ldata,
                      &me->pdata,
                      looptri,
                      looptris_num,
                      ss->fast_draw,
                      ss->face_areas,
                      ss->pmap);

  pbvh_show_mask_set(pbvh, ob->sculpt->show_mask);
  pbvh_show_face_sets_set(pbvh, ob->sculpt->show_face_sets);

  const bool is_deformed = check_sculpt_object_deformed(ob, true);
  if (is_deformed && me_eval_deform != NULL) {
    int totvert;
    float(*v_cos)[3] = BKE_mesh_vert_coords_alloc(me_eval_deform, &totvert);
    BKE_pbvh_vert_coords_apply(pbvh, v_cos, totvert);
    MEM_freeN(v_cos);
  }

  return pbvh;
}

static PBVH *build_pbvh_from_ccg(Object *ob, SubdivCCG *subdiv_ccg, bool respect_hide)
{
  SculptSession *ss = ob->sculpt;

  CCGKey key;
  BKE_subdiv_ccg_key_top_level(&key, subdiv_ccg);
  PBVH *pbvh = BKE_pbvh_new();
  BKE_pbvh_respect_hide_set(pbvh, respect_hide);

  Mesh *base_mesh = BKE_mesh_from_object(ob);
  BKE_sculpt_sync_face_set_visibility(base_mesh, subdiv_ccg);

  int totgridfaces = base_mesh->totpoly * (key.grid_size - 1) * (key.grid_size - 1);

  MEM_SAFE_FREE(ss->face_areas);
  ss->face_areas = MEM_calloc_arrayN(totgridfaces, sizeof(float) * 2, "ss->face_areas");

  CustomData_reset(&ob->sculpt->temp_vdata);
  CustomData_reset(&ob->sculpt->temp_pdata);

  BKE_pbvh_build_grids(pbvh,
                       subdiv_ccg->grids,
                       subdiv_ccg->num_grids,
                       &key,
                       (void **)subdiv_ccg->grid_faces,
                       subdiv_ccg->grid_flag_mats,
                       subdiv_ccg->grid_hidden,
                       ob->sculpt->fast_draw,
                       ss->face_areas);

  ss->temp_vdata_elems = BKE_pbvh_get_grid_num_vertices(pbvh);
  ss->temp_pdata_elems = ss->totfaces;

  BKE_sculptsession_check_sculptverts(ob->sculpt, pbvh, BKE_pbvh_get_grid_num_vertices(pbvh));

  pbvh_show_mask_set(pbvh, ob->sculpt->show_mask);
  pbvh_show_face_sets_set(pbvh, ob->sculpt->show_face_sets);

  return pbvh;
}

bool BKE_sculptsession_check_sculptverts(SculptSession *ss, PBVH *pbvh, int totvert)
{
  if (!ss->bm && (!ss->mdyntopo_verts || totvert != ss->mdyntopo_verts_size)) {
    init_mdyntopo_layer(ss, pbvh, totvert);
    return true;
  }

  BKE_pbvh_set_mdyntopo_verts(pbvh, ss->mdyntopo_verts);

  return false;
}

static void init_mdyntopo_layer_faces(SculptSession *ss, PBVH *pbvh, int totvert)
{
  if (ss->mdyntopo_verts) {
    MEM_freeN(ss->mdyntopo_verts);
  }

  ss->mdyntopo_verts = MEM_calloc_arrayN(totvert, sizeof(*ss->mdyntopo_verts), "mdyntopo_verts");
  ss->mdyntopo_verts_size = totvert;

  BKE_pbvh_set_mdyntopo_verts(pbvh, ss->mdyntopo_verts);

  MSculptVert *mv = ss->mdyntopo_verts;

  for (int i = 0; i < totvert; i++, mv++) {
    MV_ADD_FLAG(mv,
                SCULPTVERT_NEED_BOUNDARY | SCULPTVERT_NEED_VALENCE | SCULPTVERT_NEED_DISK_SORT);
    mv->stroke_id = -1;

    SculptVertRef vertex = {.i = i};

    BKE_pbvh_update_vert_boundary_faces(ss->face_sets,
                                        ss->mvert,
                                        ss->medge,
                                        ss->mloop,
                                        ss->mpoly,
                                        ss->mdyntopo_verts,
                                        ss->pmap,
                                        vertex);

    // can't fully update boundary here, so still flag for update
    MV_ADD_FLAG(mv, SCULPTVERT_NEED_BOUNDARY);
  }
}

static void init_mdyntopo_layer_grids(SculptSession *ss, PBVH *pbvh, int totvert)
{
  if (ss->mdyntopo_verts) {
    MEM_freeN(ss->mdyntopo_verts);
  }

  ss->mdyntopo_verts = MEM_calloc_arrayN(totvert, sizeof(*ss->mdyntopo_verts), "mdyntopo_verts");
  ss->mdyntopo_verts_size = totvert;

  BKE_pbvh_set_mdyntopo_verts(pbvh, ss->mdyntopo_verts);

  MSculptVert *mv = ss->mdyntopo_verts;

  for (int i = 0; i < totvert; i++, mv++) {
    MV_ADD_FLAG(mv,
                SCULPTVERT_NEED_BOUNDARY | SCULPTVERT_NEED_VALENCE | SCULPTVERT_NEED_DISK_SORT);
    mv->stroke_id = -1;

    SculptVertRef vertex = {.i = i};

    BKE_pbvh_update_vert_boundary_grids(pbvh, ss->subdiv_ccg, vertex);

    // can't fully update boundary here, so still flag for update
    MV_ADD_FLAG(mv, SCULPTVERT_NEED_BOUNDARY);
  }
}

static void init_mdyntopo_layer(SculptSession *ss, PBVH *pbvh, int totvert)
{
  if (BKE_pbvh_type(pbvh) == PBVH_FACES) {
    init_mdyntopo_layer_faces(ss, pbvh, totvert);
  }
  else if (BKE_pbvh_type(pbvh) == PBVH_GRIDS) {
    init_mdyntopo_layer_grids(ss, pbvh, totvert);
  }
}

PBVH *BKE_sculpt_object_pbvh_ensure(Depsgraph *depsgraph, Object *ob)
{
  if (ob == NULL || ob->sculpt == NULL) {
    return NULL;
  }

  Scene *scene = DEG_get_input_scene(depsgraph);

  bool respect_hide = true;
  if (ob->mode & (OB_MODE_VERTEX_PAINT | OB_MODE_WEIGHT_PAINT)) {
    if (!(BKE_paint_select_vert_test(ob) || BKE_paint_select_face_test(ob))) {
      respect_hide = false;
    }
  }

  PBVH *pbvh = ob->sculpt->pbvh;
  if (pbvh != NULL) {
    SCULPT_update_flat_vcol_shading(ob, scene);

    /* NOTE: It is possible that grids were re-allocated due to modifier
     * stack. Need to update those pointers. */
    if (BKE_pbvh_type(pbvh) == PBVH_GRIDS) {
      Object *object_eval = DEG_get_evaluated_object(depsgraph, ob);
      Mesh *mesh_eval = object_eval->data;
      SubdivCCG *subdiv_ccg = mesh_eval->runtime.subdiv_ccg;
      if (subdiv_ccg != NULL) {
        BKE_sculpt_bvh_update_from_ccg(pbvh, subdiv_ccg);
      }
    }
    else if (BKE_pbvh_type(pbvh) == PBVH_BMESH) {
      // Object *object_eval = DEG_get_evaluated_object(depsgraph, ob);

      BKE_sculptsession_sync_attributes(ob, BKE_object_get_original_mesh(ob));
    }
    return pbvh;
  }

  if (ob->sculpt->bm != NULL) {
    /* Sculpting on a BMesh (dynamic-topology) gets a special PBVH. */
    pbvh = build_pbvh_for_dynamic_topology(ob, false);

    ob->sculpt->pbvh = pbvh;
  }
  else {
#if 1  // def WHEN_GLOBAL_UNDO_WORKS
    /*detect if we are loading from an undo memfile step*/
    Mesh *mesh_orig = BKE_object_get_original_mesh(ob);
    bool is_dyntopo = (mesh_orig->flag & ME_SCULPT_DYNAMIC_TOPOLOGY);

    if (is_dyntopo) {
      BMesh *bm = BKE_sculptsession_empty_bmesh_create();

      ob->sculpt->bm = bm;

      BM_mesh_bm_from_me(NULL,
                         bm,
                         mesh_orig,
                         (&(struct BMeshFromMeshParams){.calc_face_normal = true,
                                                        .use_shapekey = true,
                                                        .active_shapekey = ob->shapenr,
                                                        .create_shapekey_layers = true,
                                                        .ignore_id_layers = false,
                                                        .copy_temp_cdlayers = true,

                                                        .cd_mask_extra = CD_MASK_DYNTOPO_VERT}));

      BKE_sculptsession_bmesh_add_layers(ob);
      SCULPT_undo_ensure_bmlog(ob);

      pbvh = build_pbvh_for_dynamic_topology(ob, true);

      BKE_sculptsession_update_attr_refs(ob);
    }
    else {
      Object *object_eval = DEG_get_evaluated_object(depsgraph, ob);
      Mesh *mesh_eval = object_eval->data;
      if (mesh_eval->runtime.subdiv_ccg != NULL) {
        pbvh = build_pbvh_from_ccg(ob, mesh_eval->runtime.subdiv_ccg, respect_hide);
      }
      else if (ob->type == OB_MESH) {
        Mesh *me_eval_deform = object_eval->runtime.mesh_deform_eval;
        pbvh = build_pbvh_from_regular_mesh(ob, me_eval_deform, respect_hide);
      }
    }
#else
    Object *object_eval = DEG_get_evaluated_object(depsgraph, ob);
    Mesh *mesh_eval = object_eval->data;
    if (mesh_eval->runtime.subdiv_ccg != NULL) {
      pbvh = build_pbvh_from_ccg(ob, mesh_eval->runtime.subdiv_ccg, respect_hide);
    }
    else if (ob->type == OB_MESH) {
      Mesh *me_eval_deform = object_eval->runtime.mesh_deform_eval;

      BKE_sculptsession_check_sculptverts(ob->sculpt, me_eval_deform->totvert);

      pbvh = build_pbvh_from_regular_mesh(ob, me_eval_deform, respect_hide);
    }
#endif
  }

  ob->sculpt->pbvh = pbvh;

  BKE_sculptsession_update_attr_refs(ob);

  if (pbvh) {
    SCULPT_update_flat_vcol_shading(ob, scene);
  }

  return pbvh;
}

void BKE_sculpt_bvh_update_from_ccg(PBVH *pbvh, SubdivCCG *subdiv_ccg)
{
  BKE_pbvh_grids_update(pbvh,
                        subdiv_ccg->grids,
                        (void **)subdiv_ccg->grid_faces,
                        subdiv_ccg->grid_flag_mats,
                        subdiv_ccg->grid_hidden);
}

bool BKE_sculptsession_use_pbvh_draw(const Object *ob, const View3D *v3d)
{
  SculptSession *ss = ob->sculpt;
  if (ss == NULL || ss->pbvh == NULL || ss->mode_type != OB_MODE_SCULPT) {
    return false;
  }

#if 0
  if (BKE_pbvh_type(ss->pbvh) == PBVH_GRIDS) {
    return !(v3d && (v3d->shading.type > OB_SOLID));
  }
#endif

  if (BKE_pbvh_type(ss->pbvh) == PBVH_FACES) {
    /* Regular mesh only draws from PBVH without modifiers and shape keys. */
    return !(ss->shapekey_active || ss->deform_modifiers_active);
  }

  /* Multires and dyntopo always draw directly from the PBVH. */
  return true;
}

/* Returns the Face Set random color for rendering in the overlay given its ID and a color seed. */
#define GOLDEN_RATIO_CONJUGATE 0.618033988749895f
void BKE_paint_face_set_overlay_color_get(const int face_set, const int seed, uchar r_color[4])
{
  float rgba[4];
  float random_mod_hue = GOLDEN_RATIO_CONJUGATE * (abs(face_set) + (seed % 10));
  random_mod_hue = random_mod_hue - floorf(random_mod_hue);
  const float random_mod_sat = BLI_hash_int_01(abs(face_set) + seed + 1);
  const float random_mod_val = BLI_hash_int_01(abs(face_set) + seed + 2);
  hsv_to_rgb(random_mod_hue,
             0.6f + (random_mod_sat * 0.25f),
             1.0f - (random_mod_val * 0.35f),
             &rgba[0],
             &rgba[1],
             &rgba[2]);
  rgba_float_to_uchar(r_color, rgba);
}

int BKE_sculptsession_get_totvert(const SculptSession *ss)
{
  switch (BKE_pbvh_type(ss->pbvh)) {
    case PBVH_FACES:
      return ss->totvert;
    case PBVH_BMESH:
      return BM_mesh_elem_count(BKE_pbvh_get_bmesh(ss->pbvh), BM_VERT);
    case PBVH_GRIDS:
      return BKE_pbvh_get_grid_num_vertices(ss->pbvh);
  }

  return 0;
}

/**
  Syncs customdata layers with internal bmesh, but ignores deleted layers.
*/
void BKE_sculptsession_sync_attributes(struct Object *ob, struct Mesh *me)
{
  SculptSession *ss = ob->sculpt;

  if (!ss) {
    return;
  }
  else if (!ss->bm) {
    BKE_sculptsession_update_attr_refs(ob);
    return;
  }

  bool modified = false;
  BMesh *bm = ss->bm;

  CustomData *cd1[4] = {&me->vdata, &me->edata, &me->ldata, &me->pdata};
  CustomData *cd2[4] = {&bm->vdata, &bm->edata, &bm->ldata, &bm->pdata};
  int badmask = CD_MASK_MLOOP | CD_MASK_MVERT | CD_MASK_MEDGE | CD_MASK_MPOLY | CD_MASK_ORIGINDEX |
                CD_MASK_ORIGSPACE | CD_MASK_MFACE;

  for (int i = 0; i < 4; i++) {
    CustomDataLayer **newlayers = NULL;
    BLI_array_declare(newlayers);

    CustomData *data1 = cd1[i];
    CustomData *data2 = cd2[i];

    if (!data1->layers) {
      modified |= data2->layers != NULL;
      continue;
    }

    for (int j = 0; j < data1->totlayer; j++) {
      CustomDataLayer *cl1 = data1->layers + j;

      if ((1 << cl1->type) & badmask) {
        continue;
      }

      int idx = CustomData_get_named_layer_index(data2, cl1->type, cl1->name);
      if (idx < 0) {
        BLI_array_append(newlayers, cl1);
      }
    }

    for (int j = 0; j < BLI_array_len(newlayers); j++) {
      BM_data_layer_add_named(bm, data2, newlayers[j]->type, newlayers[j]->name);
      modified = true;
    }

    /* sync various ids */
    for (int j = 0; j < data1->totlayer; j++) {
      CustomDataLayer *cl1 = data1->layers + j;

      if ((1 << cl1->type) & badmask) {
        continue;
      }

      int idx = CustomData_get_named_layer_index(data2, cl1->type, cl1->name);

      if (idx == -1) {
        continue;
      }

      CustomDataLayer *cl2 = data2->layers + idx;

      cl2->anonymous_id = cl1->anonymous_id;
      cl2->uid = cl1->uid;
    }

    bool typemap[CD_NUMTYPES] = {0};

    for (int j = 0; j < data1->totlayer; j++) {
      CustomDataLayer *cl1 = data1->layers + j;

      if ((1 << cl1->type) & badmask) {
        continue;
      }

      if (typemap[cl1->type]) {
        continue;
      }

      typemap[cl1->type] = true;

      // find first layer
      int baseidx = CustomData_get_layer_index(data2, cl1->type);

      if (baseidx < 0) {
        modified |= true;
        continue;
      }

      CustomDataLayer *cl2 = data2->layers + baseidx;

      int idx = CustomData_get_named_layer_index(data2, cl1->type, cl1[cl1->active].name);
      if (idx >= 0) {
        modified |= idx - baseidx != cl2->active;
        cl2->active = idx - baseidx;
      }

      idx = CustomData_get_named_layer_index(data2, cl1->type, cl1[cl1->active_rnd].name);
      if (idx >= 0) {
        modified |= idx - baseidx != cl2->active_rnd;
        cl2->active_rnd = idx - baseidx;
      }

      idx = CustomData_get_named_layer_index(data2, cl1->type, cl1[cl1->active_mask].name);
      if (idx >= 0) {
        modified |= idx - baseidx != cl2->active_mask;
        cl2->active_mask = idx - baseidx;
      }

      idx = CustomData_get_named_layer_index(data2, cl1->type, cl1[cl1->active_clone].name);
      if (idx >= 0) {
        modified |= idx - baseidx != cl2->active_clone;
        cl2->active_clone = idx - baseidx;
      }
    }

    BLI_array_free(newlayers);
  }

  if (modified && ss->bm) {
    CustomData_regen_active_refs(&ss->bm->vdata);
    CustomData_regen_active_refs(&ss->bm->edata);
    CustomData_regen_active_refs(&ss->bm->ldata);
    CustomData_regen_active_refs(&ss->bm->pdata);
  }

  BKE_sculptsession_update_attr_refs(ob);
}

BMesh *BKE_sculptsession_empty_bmesh_create()
{
  const BMAllocTemplate allocsize = {
      .totvert = 2048 * 16, .totface = 2048 * 16, .totloop = 4196 * 16, .totedge = 2048 * 16};

  BMesh *bm = BM_mesh_create(
      &allocsize,
      &((struct BMeshCreateParams){.use_toolflags = false,
                                   .create_unique_ids = true,
                                   .id_elem_mask = BM_VERT | BM_EDGE | BM_FACE,
                                   .id_map = true,
                                   .temporary_ids = false,
                                   .no_reuse_ids = false}));

  return bm;
}

char dyntopop_node_idx_layer_id[] = "_dyntopo_node_id";
char dyntopop_faces_areas_layer_id[] = "__dyntopo_face_areas";

void BKE_sculptsession_bmesh_add_layers(Object *ob)
{
  SculptSession *ss = ob->sculpt;

  int cd_node_layer_index, cd_face_node_layer_index;

  BMCustomLayerReq vlayers[] = {
      {CD_PAINT_MASK, NULL, 0},
      {CD_DYNTOPO_VERT, NULL, CD_FLAG_TEMPORARY | CD_FLAG_NOCOPY},
      {CD_PROP_INT32, dyntopop_node_idx_layer_id, CD_FLAG_TEMPORARY | CD_FLAG_NOCOPY}};

  BM_data_layers_ensure(ss->bm, &ss->bm->vdata, vlayers, ARRAY_SIZE(vlayers));

  ss->cd_vert_mask_offset = CustomData_get_offset(&ss->bm->vdata, CD_PAINT_MASK);

  BMCustomLayerReq flayers[] = {
      {CD_PROP_INT32, dyntopop_node_idx_layer_id, CD_FLAG_TEMPORARY | CD_FLAG_NOCOPY},
      {CD_PROP_FLOAT2, dyntopop_faces_areas_layer_id, CD_FLAG_TEMPORARY | CD_FLAG_NOCOPY},
  };
  BM_data_layers_ensure(ss->bm, &ss->bm->pdata, flayers, ARRAY_SIZE(flayers));

  // get indices again, as they might have changed after adding new layers
  cd_node_layer_index = CustomData_get_named_layer_index(
      &ss->bm->vdata, CD_PROP_INT32, dyntopop_node_idx_layer_id);
  cd_face_node_layer_index = CustomData_get_named_layer_index(
      &ss->bm->pdata, CD_PROP_INT32, dyntopop_node_idx_layer_id);

  ss->cd_sculpt_vert = CustomData_get_offset(&ss->bm->vdata, CD_DYNTOPO_VERT);

  ss->cd_vert_node_offset = CustomData_get_n_offset(
      &ss->bm->vdata,
      CD_PROP_INT32,
      cd_node_layer_index - CustomData_get_layer_index(&ss->bm->vdata, CD_PROP_INT32));

  ss->bm->vdata.layers[cd_node_layer_index].flag |= CD_FLAG_TEMPORARY | CD_FLAG_NOCOPY;

  ss->cd_face_node_offset = CustomData_get_n_offset(
      &ss->bm->pdata,
      CD_PROP_INT32,
      cd_face_node_layer_index - CustomData_get_layer_index(&ss->bm->pdata, CD_PROP_INT32));

  ss->bm->pdata.layers[cd_face_node_layer_index].flag |= CD_FLAG_TEMPORARY | CD_FLAG_NOCOPY;
  ss->cd_faceset_offset = CustomData_get_offset(&ss->bm->pdata, CD_SCULPT_FACE_SETS);

  ss->cd_face_areas = CustomData_get_named_layer_index(
      &ss->bm->pdata, CD_PROP_FLOAT2, dyntopop_faces_areas_layer_id);

  ss->cd_face_areas = ss->bm->pdata.layers[ss->cd_face_areas].offset;

  AttributeDomain domain;
  CustomDataLayer *cl;
  Mesh *me = BKE_object_get_original_mesh(ob);

  if (BKE_pbvh_get_color_layer(ss->pbvh, me, &cl, &domain)) {
    ss->vcol_domain = (int)domain;
    ss->vcol_type = cl->type;
    ss->cd_vcol_offset = cl->offset;
  }
  else {
    ss->cd_vcol_offset = -1;
    ss->vcol_type = -1;
    ss->vcol_domain = (int)ATTR_DOMAIN_NUM;
  }
}

static bool sculpt_attr_get_layer(SculptSession *ss,
                                  Object *ob,
                                  AttributeDomain domain,
                                  int proptype,
                                  const char *name,
                                  SculptCustomLayer *out,
                                  bool autocreate,
                                  SculptLayerParams *params)
{
  if (ss->save_temp_layers && !params->simple_array) {
    params->permanent = true;
  }

  bool simple_array = params->simple_array;
  bool permanent = params->permanent;
  bool nocopy = params->nocopy;
  bool nointerp = params->nointerp;

  out->params = *params;
  out->proptype = proptype;
  out->domain = domain;
  BLI_strncpy_utf8(out->name, name, sizeof(out->name));

  if (ss->pbvh && BKE_pbvh_type(ss->pbvh) == PBVH_GRIDS) {
    if (permanent) {
      printf(
          "%s: error: tried to make permanent customdata in multires mode; will make local "
          "array "
          "instead.\n",
          __func__);
      permanent = false;
    }
  }

  BLI_assert(!(simple_array && permanent));

  if (simple_array) {
    CustomData *cdata = NULL;
    int totelem = 0;

    PBVHType pbvhtype = ss->pbvh ? BKE_pbvh_type(ss->pbvh) : (ss->bm ? PBVH_BMESH : PBVH_FACES);

    switch (pbvhtype) {
      case PBVH_BMESH: {
        switch (domain) {
          case ATTR_DOMAIN_POINT:
            cdata = &ss->bm->vdata;
            totelem = ss->bm->totvert;
            break;
          case ATTR_DOMAIN_FACE:
            cdata = &ss->bm->pdata;
            totelem = ss->bm->totface;
            break;
          default:
            return false;
        }
        break;
      }
      case PBVH_GRIDS: {
        switch (domain) {
          case ATTR_DOMAIN_POINT:
            cdata = ss->vdata;
            totelem = BKE_sculptsession_get_totvert(ss);
            break;
          case ATTR_DOMAIN_FACE:
            cdata = ss->pdata;
            totelem = ss->totfaces;
            break;
          default:
            return false;
        }
        break;
      }
      case PBVH_FACES: {
        switch (domain) {
          case ATTR_DOMAIN_POINT:
            cdata = ss->vdata;
            totelem = ss->totvert;
            break;
          case ATTR_DOMAIN_FACE:
            cdata = ss->pdata;
            totelem = ss->totfaces;
            break;
          default:
            return false;
        }
        break;
      }
    }

    CustomData dummy = {0};
    CustomData_reset(&dummy);
    CustomData_add_layer(&dummy, proptype, CD_ASSIGN, NULL, 0);
    int elemsize = (int)CustomData_get_elem_size(dummy.layers);

    CustomData_free(&dummy, 0);

    out->data = MEM_calloc_arrayN(totelem, elemsize, __func__);

    out->is_cdlayer = false;
    out->from_bmesh = ss->bm != NULL;
    out->cd_offset = -1;
    out->layer = NULL;
    out->domain = domain;
    out->proptype = proptype;
    out->elemsize = elemsize;
    out->ready = true;

    /*grids cannot store normal customdata layers, and thus
      we cannot rely on the customdata api to keep track of
      and free their memory for us.

      so instead we queue them in a dynamic array inside of
      SculptSession.
      */
    if (ss->pbvh && BKE_pbvh_type(ss->pbvh) == PBVH_GRIDS) {
      ss->tot_layers_to_free++;

      if (!ss->layers_to_free) {
        ss->layers_to_free = MEM_calloc_arrayN(
            ss->tot_layers_to_free, sizeof(void *), "ss->layers_to_free");
      }
      else {
        ss->layers_to_free = MEM_recallocN(ss->layers_to_free,
                                           sizeof(void *) * ss->tot_layers_to_free);
      }

      SculptCustomLayer *cpy = MEM_callocN(sizeof(SculptCustomLayer), "SculptCustomLayer cpy");
      *cpy = *out;

      ss->layers_to_free[ss->tot_layers_to_free - 1] = cpy;
    }

    return true;
  }

  switch (BKE_pbvh_type(ss->pbvh)) {
    case PBVH_BMESH: {
      CustomData *cdata = NULL;
      out->from_bmesh = true;

      if (!ss->bm) {
        out->ready = false;
        return false;
      }

      switch (domain) {
        case ATTR_DOMAIN_POINT:
          cdata = &ss->bm->vdata;
          break;
        case ATTR_DOMAIN_FACE:
          cdata = &ss->bm->pdata;
          break;
        default:
          out->ready = false;
          return false;
      }

      int idx = CustomData_get_named_layer_index(cdata, proptype, name);

      if (idx < 0) {
        if (!autocreate) {
          out->ready = false;
          return false;
        }

        BM_data_layer_add_named(ss->bm, cdata, proptype, name);
        idx = CustomData_get_named_layer_index(cdata, proptype, name);

        BKE_sculptsession_bmesh_attr_update_internal(ob);

        if (!permanent) {
          cdata->layers[idx].flag |= CD_FLAG_TEMPORARY | CD_FLAG_NOCOPY;
        }
      }

      if (nocopy) {
        cdata->layers[idx].flag |= CD_FLAG_ELEM_NOCOPY;
      }
      if (nointerp) {
        cdata->layers[idx].flag |= CD_FLAG_ELEM_NOINTERP;
      }

      out->data = NULL;
      out->is_cdlayer = true;
      out->layer = cdata->layers + idx;
      out->cd_offset = out->layer->offset;
      out->elemsize = CustomData_get_elem_size(out->layer);

      break;
    }
    case PBVH_FACES: {
      CustomData *cdata = NULL;
      int totelem = 0;

      out->from_bmesh = false;

      switch (domain) {
        case ATTR_DOMAIN_POINT:
          totelem = ss->totvert;
          cdata = ss->vdata;
          break;
        case ATTR_DOMAIN_FACE:
          totelem = ss->totfaces;
          cdata = ss->pdata;
          break;
        default:
          out->ready = false;
          return false;
      }

      int idx = CustomData_get_named_layer_index(cdata, proptype, name);

      if (idx < 0) {
        if (!autocreate) {
          out->ready = false;
          return false;
        }

        CustomData_add_layer_named(cdata, proptype, CD_CALLOC, NULL, totelem, name);
        idx = CustomData_get_named_layer_index(cdata, proptype, name);
      }

      if (!permanent) {
        cdata->layers[idx].flag |= CD_FLAG_TEMPORARY | CD_FLAG_NOCOPY;
      }

      out->data = NULL;
      out->is_cdlayer = true;
      out->layer = cdata->layers + idx;
      out->cd_offset = -1;
      out->data = out->layer->data;
      out->elemsize = CustomData_get_elem_size(out->layer);

      break;
    }
    case PBVH_GRIDS: {
      CustomData *cdata = NULL;
      int totelem = 0;

      out->from_bmesh = false;

      switch (domain) {
        case ATTR_DOMAIN_POINT:
          totelem = BKE_pbvh_get_grid_num_vertices(ss->pbvh);
          cdata = &ss->temp_vdata;
          break;
        case ATTR_DOMAIN_FACE:
          totelem = ss->totfaces;
          cdata = &ss->temp_pdata;
        default:
          out->ready = false;
          return false;
      }

      int idx = CustomData_get_named_layer_index(cdata, proptype, name);

      if (idx < 0) {
        if (!autocreate) {
          out->ready = false;
          return false;
        }

        CustomData_add_layer_named(cdata, proptype, CD_CALLOC, NULL, totelem, name);
        idx = CustomData_get_named_layer_index(cdata, proptype, name);

        // if (!permanent) {
        cdata->layers[idx].flag |= CD_FLAG_TEMPORARY | CD_FLAG_NOCOPY;
        //}
      }

      if (nocopy) {
        cdata->layers[idx].flag |= CD_FLAG_ELEM_NOCOPY;
      }
      if (nointerp) {
        cdata->layers[idx].flag |= CD_FLAG_ELEM_NOINTERP;
      }

      out->data = NULL;
      out->is_cdlayer = true;
      out->layer = cdata->layers + idx;
      out->cd_offset = -1;
      out->data = out->layer->data;
      out->elemsize = CustomData_get_elem_size(out->layer);

      break;
    }
  }

  out->ready = true;

  return true;
}

bool BKE_sculptsession_attr_get_layer(Object *ob,
                                       AttributeDomain domain,
                                       int proptype,
                                       const char *name,
                                       SculptCustomLayer *scl,
                                       SculptLayerParams *params)
{
  SculptSession *ss = ob->sculpt;

  bool ret = sculpt_attr_get_layer(ss, ob, domain, proptype, name, scl, true, params);
  BKE_sculptsession_update_attr_refs(ob);

  return ret;
}

void BKE_sculptsession_bmesh_attr_update_internal(Object *ob)
{
  SculptSession *ss = ob->sculpt;

  BKE_sculptsession_bmesh_add_layers(ob);

  if (ss->pbvh) {
    BKE_pbvh_update_offsets(ss->pbvh,
                            ss->cd_vert_node_offset,
                            ss->cd_face_node_offset,
                            ss->cd_sculpt_vert,
                            ss->cd_face_areas);
  }
  if (ss->bm_log) {
    BM_log_set_cd_offsets(ss->bm_log, ss->cd_sculpt_vert);
  }
}

void BKE_sculptsession_update_attr_refs(Object *ob)
{
  SculptSession *ss = ob->sculpt;

  /* run twice, in case SCULPT_attr_get_layer had to recreate a layer and
     messed up the ordering. */
  for (int i = 0; i < 2; i++) {
    for (int j = 0; j < SCULPT_SCL_LAYER_MAX; j++) {
      SculptCustomLayer *scl = ss->custom_layers[j];

      if (!scl || !scl->ready) {
        continue;
      }

      if (!scl->released && !scl->params.simple_array) {
        sculpt_attr_get_layer(
            ss, ob, scl->domain, scl->proptype, scl->name, scl, true, &scl->params);
      }
    }

    if (ss->bm) {
      BKE_sculptsession_bmesh_attr_update_internal(ob);
    }
  }

  if (ss->pbvh) {
    Mesh *me = BKE_object_get_original_mesh(ob);
    AttributeDomain domain;
    CustomDataLayer *layer = NULL;

    BKE_pbvh_get_color_layer(ss->pbvh, me, &layer, &domain);

    if (!layer) {
      ss->vcol_domain = ATTR_DOMAIN_NUM;
      ss->vcol_type = -1;
      ss->cd_vcol_offset = -1;
      ss->vcol = NULL;
    }
    else {
      ss->vcol_domain = domain;
      ss->vcol_type = layer->type;

      if (ss->bm) {
        ss->cd_vcol_offset = layer->offset;
      }
      else {
        ss->vcol = layer->data;
      }
    }
  }

  if (ss->bm) {
    ss->totuv = CustomData_number_of_layers(&ss->bm->ldata, CD_MLOOPUV);
  }
  else {
    ss->totuv = ss->ldata ? CustomData_number_of_layers(ss->ldata, CD_MLOOPUV) : 0;
  }
}

bool BKE_paint_uses_channels(ePaintMode mode)
{
  return mode == PAINT_MODE_SCULPT;
}

bool BKE_sculptsession_attr_release_layer(Object *ob, SculptCustomLayer *scl)
{
  SculptSession *ss = ob->sculpt;
  AttributeDomain domain = scl->domain;

  if (scl->released) {
    return false;
  }

  // remove from layers_to_free list if necassary
  for (int i = 0; scl->data && i < ss->tot_layers_to_free; i++) {
    if (ss->layers_to_free[i] && ss->layers_to_free[i]->data == scl->data) {
      MEM_freeN(ss->layers_to_free[i]);
      ss->layers_to_free[i] = NULL;
    }
  }

  scl->released = true;

  if (!scl->from_bmesh) {
    // for now, don't clean up bmesh temp layers
    if (scl->is_cdlayer && BKE_pbvh_type(ss->pbvh) != PBVH_GRIDS) {
      CustomData *cdata = NULL;
      int totelem = 0;

      switch (domain) {
        case ATTR_DOMAIN_POINT:
          cdata = ss->vdata;
          totelem = ss->totvert;
          break;
        case ATTR_DOMAIN_FACE:
          cdata = ss->pdata;
          totelem = ss->totfaces;
          break;
        default:
          printf("error, unknown domain in %s\n", __func__);
          return false;
      }

      CustomData_free_layer(cdata, scl->layer->type, totelem, scl->layer - cdata->layers);
      BKE_sculptsession_update_attr_refs(ob);
    }
    else {
      MEM_SAFE_FREE(scl->data);
    }

    scl->data = NULL;
  }
  return true;
}
