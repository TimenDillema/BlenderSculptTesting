/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2020 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup edsculpt
 */

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_hash.h"
#include "BLI_math.h"
#include "BLI_math_color_blend.h"
#include "BLI_task.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_scene_types.h"

#include "BKE_brush.h"
#include "BKE_colortools.h"
#include "BKE_context.h"
#include "BKE_mesh.h"
#include "BKE_mesh_mapping.h"
#include "BKE_object.h"
#include "BKE_paint.h"
#include "BKE_pbvh.h"
#include "BKE_scene.h"

#include "DEG_depsgraph.h"

#include "IMB_colormanagement.h"

#include "WM_api.h"
#include "WM_message.h"
#include "WM_toolsystem.h"
#include "WM_types.h"

#include "ED_object.h"
#include "ED_screen.h"
#include "ED_sculpt.h"
#include "paint_intern.h"
#include "sculpt_intern.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "UI_interface.h"

#include "IMB_imbuf.h"

#include "bmesh.h"

#include <math.h>
#include <stdlib.h>

static void do_color_smooth_task_cb_exec(void *__restrict userdata,
                                         const int n,
                                         const TaskParallelTLS *__restrict tls)
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ob->sculpt;
  const Brush *brush = data->brush;
  const float bstrength = ss->cache->bstrength;

  PBVHVertexIter vd;

  SculptBrushTest test;
  SculptBrushTestFn sculpt_brush_test_sq_fn = SCULPT_brush_test_init_with_falloff_shape(
      ss, &test, data->brush->falloff_shape);
  const int thread_id = BLI_task_parallel_thread_id(tls);

  BKE_pbvh_vertex_iter_begin (ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE) {
    if (!sculpt_brush_test_sq_fn(&test, vd.co)) {
      continue;
    }
    const float fade = bstrength * SCULPT_brush_strength_factor(ss,
                                                                brush,
                                                                vd.co,
                                                                sqrtf(test.dist),
                                                                vd.no,
                                                                vd.fno,
                                                                vd.mask ? *vd.mask : 0.0f,
                                                                vd.vertex,
                                                                thread_id);

    float smooth_color[4];
    float color[4];

    SCULPT_neighbor_color_average(ss, smooth_color, vd.vertex);

    SCULPT_vertex_color_get(ss, vd.vertex, color);
    blend_color_interpolate_float(color, color, smooth_color, fade);
    SCULPT_vertex_color_set(ss, vd.vertex, color);

    if (vd.mvert) {
      BKE_pbvh_vert_mark_update(ss->pbvh, vd.vertex);
    }
  }
  BKE_pbvh_vertex_iter_end;
}

static void do_paint_brush_task_cb_ex(void *__restrict userdata,
                                      const int n,
                                      const TaskParallelTLS *__restrict tls)
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ob->sculpt;
  const Brush *brush = data->brush;
  const float bstrength = fabsf(ss->cache->bstrength);
  float hue_offset = data->hue_offset;

  const SculptCustomLayer *buffer_scl = data->scl;
  const SculptCustomLayer *stroke_id_scl = data->scl2;

  const bool do_accum = SCULPT_get_int(ss, accumulate, NULL, brush);

  PBVHVertexIter vd;

  SculptBrushTest test;
  SculptBrushTestFn sculpt_brush_test_sq_fn = SCULPT_brush_test_init_with_falloff_shape(
      ss, &test, data->brush->falloff_shape);
  const int thread_id = BLI_task_parallel_thread_id(tls);

  float brush_color[4];
  copy_v4_v4(brush_color, data->brush_color);

  IMB_colormanagement_srgb_to_scene_linear_v3(brush_color);

  if (hue_offset != 0.5f) {
    hue_offset = (hue_offset * 2.0 - 1.0) * 0.5f;
    float hsv[3];

    rgb_to_hsv_v(brush_color, hsv);

    hsv[0] += hue_offset;
    hsv[0] -= floorf(hsv[0]);

    hsv_to_rgb_v(hsv, brush_color);
  }

  /* get un-pressure-mapped alpha */
  float alpha = BKE_brush_channelset_get_final_float(
      BKE_paint_brush(&data->sd->paint)->channels, data->sd->channels, "strength", NULL);

  BKE_pbvh_vertex_iter_begin (ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE) {
    // SCULPT_orig_vert_data_update(&orig_data, vd.vertex);
    SCULPT_vertex_check_origdata(ss, vd.vertex);

    // check if we have a new stroke, in which we need to zero
    // our temp layer.  do this here before the brush check
    // to ensure any geomtry dyntopo might subdivide has
    // valid state.
    int *stroke_id = (int *)SCULPT_attr_vertex_data(vd.vertex, stroke_id_scl);
    float *color_buffer = (float *)SCULPT_attr_vertex_data(vd.vertex,
                                                           buffer_scl);  // mv->origcolor;

    if (*stroke_id != ss->stroke_id) {
      *stroke_id = ss->stroke_id;
      zero_v4(color_buffer);
    }

    bool affect_vertex = false;
    float distance_to_stroke_location = 0.0f;
    if (brush->tip_roundness < 1.0f) {
      affect_vertex = SCULPT_brush_test_cube(&test, vd.co, data->mat, brush->tip_roundness);
      distance_to_stroke_location = ss->cache->radius * test.dist;
    }
    else {
      affect_vertex = sculpt_brush_test_sq_fn(&test, vd.co);
      distance_to_stroke_location = sqrtf(test.dist);
    }

    if (!affect_vertex) {
      continue;
    }

    float fade = bstrength * SCULPT_brush_strength_factor(ss,
                                                          brush,
                                                          vd.co,
                                                          distance_to_stroke_location,
                                                          vd.no,
                                                          vd.fno,
                                                          vd.mask ? *vd.mask : 0.0f,
                                                          vd.vertex,
                                                          thread_id);

    /* Density. */
    float noise = 1.0f;
    const float density = ss->cache->paint_brush.density;
    if (density < 1.0f) {
      const float hash_noise = BLI_hash_int_01(ss->cache->density_seed * 1000 * vd.index);
      if (hash_noise > density) {
        noise = density * hash_noise;
        fade = fade * noise;
      }
    }

    /* Brush paint color, brush test falloff and flow. */
    float paint_color[4];
    float wet_mix_color[4];
    float buffer_color[4];

    mul_v4_v4fl(paint_color, brush_color, fade * ss->cache->paint_brush.flow);
    mul_v4_v4fl(wet_mix_color, data->wet_mix_sampled_color, fade * ss->cache->paint_brush.flow);

    /* Interpolate with the wet_mix color for wet paint mixing. */
    blend_color_interpolate_float(
        paint_color, paint_color, wet_mix_color, ss->cache->paint_brush.wet_mix);
    blend_color_mix_float(color_buffer, color_buffer, paint_color);

    /* Final mix over the color/original-color using brush alpha. */
    mul_v4_v4fl(buffer_color, color_buffer, alpha);

    float vcolor[4];
    SCULPT_vertex_color_get(ss, vd.vertex, vcolor);

    if (do_accum) {
      mul_v4_fl(buffer_color, fade);

      IMB_blend_color_float(vcolor, vcolor, buffer_color, brush->blend);
      vcolor[3] = 1.0f;
    }
    else {
      MSculptVert *mv = SCULPT_vertex_get_sculptvert(ss, vd.vertex);
      IMB_blend_color_float(vcolor, mv->origcolor, buffer_color, brush->blend);
    }

    CLAMP4(vcolor, 0.0f, 1.0f);
    SCULPT_vertex_color_set(ss, vd.vertex, vcolor);

    if (vd.mvert) {
      BKE_pbvh_vert_mark_update(ss->pbvh, vd.vertex);
    }
  }
  BKE_pbvh_vertex_iter_end;
}

typedef struct SampleWetPaintTLSData {
  int tot_samples;
  float color[4];
} SampleWetPaintTLSData;

static void do_sample_wet_paint_task_cb(void *__restrict userdata,
                                        const int n,
                                        const TaskParallelTLS *__restrict tls)
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ob->sculpt;
  SampleWetPaintTLSData *swptd = tls->userdata_chunk;
  PBVHVertexIter vd;

  SculptBrushTest test;
  SculptBrushTestFn sculpt_brush_test_sq_fn = SCULPT_brush_test_init_with_falloff_shape(
      ss, &test, data->brush->falloff_shape);

  test.radius *= data->brush->wet_paint_radius_factor;
  test.radius_squared = test.radius * test.radius;

  BKE_pbvh_vertex_iter_begin (ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE) {
    if (!sculpt_brush_test_sq_fn(&test, vd.co)) {
      continue;
    }

    float col[4];
    SCULPT_vertex_color_get(ss, vd.vertex, col);

    add_v4_v4(swptd->color, col);
    swptd->tot_samples++;
  }
  BKE_pbvh_vertex_iter_end;
}

static void sample_wet_paint_reduce(const void *__restrict UNUSED(userdata),
                                    void *__restrict chunk_join,
                                    void *__restrict chunk)
{
  SampleWetPaintTLSData *join = chunk_join;
  SampleWetPaintTLSData *swptd = chunk;

  join->tot_samples += swptd->tot_samples;
  add_v4_v4(join->color, swptd->color);
}

void SCULPT_do_paint_brush(Sculpt *sd, Object *ob, PBVHNode **nodes, int totnode)
{
  SculptSession *ss = ob->sculpt;
  Brush *brush = BKE_paint_brush(&sd->paint);

  if (!SCULPT_has_colors(ss)) {
    return;
  }

  if (SCULPT_stroke_is_first_brush_step_of_symmetry_pass(ss->cache)) {
    if (SCULPT_stroke_is_first_brush_step(ss->cache)) {
      ss->cache->density_seed = BLI_hash_int_01(ss->cache->location[0] * 1000);
    }
    return;
  }

  BKE_curvemapping_init(brush->curve);

  float area_no[3];
  float mat[4][4];
  float scale[4][4];
  float tmat[4][4];

  /* If the brush is round the tip does not need to be aligned to the surface, so this saves a
   * whole iteration over the affected nodes. */
  if (brush->tip_roundness < 1.0f) {
    SCULPT_calc_area_normal(sd, ob, nodes, totnode, area_no);

    cross_v3_v3v3(mat[0], area_no, ss->cache->grab_delta_symmetry);
    mat[0][3] = 0;
    cross_v3_v3v3(mat[1], area_no, mat[0]);
    mat[1][3] = 0;
    copy_v3_v3(mat[2], area_no);
    mat[2][3] = 0;
    copy_v3_v3(mat[3], ss->cache->location);
    mat[3][3] = 1;
    normalize_m4(mat);

    scale_m4_fl(scale, ss->cache->radius);
    mul_m4_m4m4(tmat, mat, scale);
    mul_v3_fl(tmat[1], SCULPT_get_float(ss, tip_scale_x, sd, brush));
    invert_m4_m4(mat, tmat);
    if (is_zero_m4(mat)) {
      return;
    }
  }

  float brush_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};

  if (ss->cache->invert) {
    SCULPT_get_vector(ss, secondary_color, brush_color, sd, brush);
  }
  else {
    SCULPT_get_vector(ss, color, brush_color, sd, brush);
  }

  /* Smooth colors mode. */
  if (ss->cache->alt_smooth) {
    SculptThreadedTaskData data = {
        .sd = sd,
        .ob = ob,
        .brush = brush,
        .nodes = nodes,
        .mat = mat,
        .brush_color = brush_color,
    };

    TaskParallelSettings settings;
    BKE_pbvh_parallel_range_settings(&settings, true, totnode);
    BLI_task_parallel_range(0, totnode, &data, do_color_smooth_task_cb_exec, &settings);
    return;
  }

  /* Regular Paint mode. */

  /* Wet paint color sampling. */
  float wet_color[4] = {0.0f};
  if (ss->cache->paint_brush.wet_mix > 0.0f) {
    SculptThreadedTaskData task_data = {
        .sd = sd,
        .ob = ob,
        .nodes = nodes,
        .hue_offset = SCULPT_get_float(ss, hue_offset, sd, brush),
        .brush = brush,
        .brush_color = brush_color,
    };

    SampleWetPaintTLSData swptd;
    swptd.tot_samples = 0;
    zero_v4(swptd.color);

    TaskParallelSettings settings_sample;
    BKE_pbvh_parallel_range_settings(&settings_sample, true, totnode);
    settings_sample.func_reduce = sample_wet_paint_reduce;
    settings_sample.userdata_chunk = &swptd;
    settings_sample.userdata_chunk_size = sizeof(SampleWetPaintTLSData);
    BLI_task_parallel_range(0, totnode, &task_data, do_sample_wet_paint_task_cb, &settings_sample);

    if (swptd.tot_samples > 0 && is_finite_v4(swptd.color)) {
      copy_v4_v4(wet_color, swptd.color);
      mul_v4_fl(wet_color, 1.0f / swptd.tot_samples);
      CLAMP4(wet_color, 0.0f, 1.0f);

      if (ss->cache->first_time) {
        copy_v4_v4(ss->cache->wet_mix_prev_color, wet_color);
      }
      blend_color_interpolate_float(wet_color,
                                    wet_color,
                                    ss->cache->wet_mix_prev_color,
                                    ss->cache->paint_brush.wet_persistence);
      copy_v4_v4(ss->cache->wet_mix_prev_color, wet_color);
      CLAMP4(ss->cache->wet_mix_prev_color, 0.0f, 1.0f);
    }
  }

  SculptCustomLayer buffer_scl;
  SculptCustomLayer stroke_id_scl;
  SculptLayerParams params = {.permanent = false, .simple_array = false};
  SculptLayerParams params_id = {
      .permanent = false, .simple_array = false, .nocopy = false, .nointerp = true};

  // reuse smear's buffer name

  SCULPT_attr_ensure_layer(
      ss, ob, ATTR_DOMAIN_POINT, CD_PROP_COLOR, "_sculpt_smear_previous", &params);
  SCULPT_attr_ensure_layer(ss,
                           ob,
                           ATTR_DOMAIN_POINT,
                           CD_PROP_INT32,
                           SCULPT_SCL_GET_NAME(SCULPT_SCL_LAYER_STROKE_ID),
                           &params_id);

  SCULPT_attr_get_layer(
      ss, ob, ATTR_DOMAIN_POINT, CD_PROP_COLOR, "_sculpt_smear_previous", &buffer_scl, &params);
  SCULPT_attr_get_layer(ss,
                        ob,
                        ATTR_DOMAIN_POINT,
                        CD_PROP_INT32,
                        SCULPT_SCL_GET_NAME(SCULPT_SCL_LAYER_STROKE_ID),
                        &stroke_id_scl,
                        &params_id);

  /* Threaded loop over nodes. */
  SculptThreadedTaskData data = {
      .sd = sd,
      .ob = ob,
      .brush = brush,
      .nodes = nodes,
      .wet_mix_sampled_color = wet_color,
      .mat = mat,
      .hue_offset = SCULPT_get_float(ss, hue_offset, sd, brush),
      .scl = &buffer_scl,
      .scl2 = &stroke_id_scl,
      .brush_color = brush_color,
  };

  TaskParallelSettings settings;
  BKE_pbvh_parallel_range_settings(&settings, true, totnode);
  BLI_task_parallel_range(0, totnode, &data, do_paint_brush_task_cb_ex, &settings);

  if (brush->vcol_boundary_factor > 0.0f) {
    // SCULPT_smooth_vcol_boundary(sd, ob, nodes, totnode, brush->vcol_boundary_factor);
  }
}

static void do_smear_brush_task_cb_exec(void *__restrict userdata,
                                        const int n,
                                        const TaskParallelTLS *__restrict tls)
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ob->sculpt;
  const Brush *brush = data->brush;
  const float bstrength = ss->cache->bstrength;

  const float blend = SCULPT_get_float(ss, smear_deform_blend, NULL, brush);

  PBVHVertexIter vd;

  SculptBrushTest test;
  SculptBrushTestFn sculpt_brush_test_sq_fn = SCULPT_brush_test_init_with_falloff_shape(
      ss, &test, data->brush->falloff_shape);
  const int thread_id = BLI_task_parallel_thread_id(tls);

  BKE_pbvh_vertex_iter_begin (ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE) {
    if (!sculpt_brush_test_sq_fn(&test, vd.co)) {
      continue;
    }
    const float fade = bstrength * SCULPT_brush_strength_factor(ss,
                                                                brush,
                                                                vd.co,
                                                                sqrtf(test.dist),
                                                                vd.no,
                                                                vd.fno,
                                                                vd.mask ? *vd.mask : 0.0f,
                                                                vd.vertex,
                                                                thread_id);

    float current_disp[3];
    float current_disp_norm[3];
    float interp_color[4];
    float *prev_color = (float *)SCULPT_attr_vertex_data(vd.vertex, data->scl);

    copy_v4_v4(interp_color, prev_color);

    switch (brush->smear_deform_type) {
      case BRUSH_SMEAR_DEFORM_DRAG:
        sub_v3_v3v3(current_disp, ss->cache->location, ss->cache->last_location);
        break;
      case BRUSH_SMEAR_DEFORM_PINCH:
        sub_v3_v3v3(current_disp, ss->cache->location, vd.co);
        break;
      case BRUSH_SMEAR_DEFORM_EXPAND:
        sub_v3_v3v3(current_disp, vd.co, ss->cache->location);
        break;
    }
    normalize_v3_v3(current_disp_norm, current_disp);
    mul_v3_v3fl(current_disp, current_disp_norm, ss->cache->bstrength);

    SculptVertexNeighborIter ni;
    SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, vd.vertex, ni) {
      float vertex_disp[3];
      float vertex_disp_norm[3];
      sub_v3_v3v3(vertex_disp, SCULPT_vertex_co_get(ss, ni.vertex), vd.co);
      const float *neighbor_color = SCULPT_attr_vertex_data(ni.vertex, data->scl);

      normalize_v3_v3(vertex_disp_norm, vertex_disp);
      if (dot_v3v3(current_disp_norm, vertex_disp_norm) >= 0.0f) {
        continue;
      }
      const float color_interp = clamp_f(
          -dot_v3v3(current_disp_norm, vertex_disp_norm), 0.0f, 1.0f);
      float color_mix[4];
      copy_v4_v4(color_mix, neighbor_color);
      mul_v4_fl(color_mix, color_interp * fade);
      blend_color_mix_float(interp_color, interp_color, color_mix);
    }
    SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);

    float vcolor[4];

    SCULPT_vertex_color_get(ss, vd.vertex, vcolor);
    blend_color_interpolate_float(vcolor, prev_color, interp_color, fade * blend);
    clamp_v4(vcolor, 0.0f, 1.0f);
    SCULPT_vertex_color_set(ss, vd.vertex, vcolor);

    if (vd.mvert) {
      BKE_pbvh_vert_mark_update(ss->pbvh, vd.vertex);
    }
  }
  BKE_pbvh_vertex_iter_end;
}

static void do_smear_store_prev_colors_task_cb_exec(void *__restrict userdata,
                                                    const int n,
                                                    const TaskParallelTLS *__restrict UNUSED(tls))
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ob->sculpt;

  PBVHVertexIter vd;
  BKE_pbvh_vertex_iter_begin (ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE) {
    SCULPT_vertex_color_get(ss, vd.vertex, (float *)SCULPT_attr_vertex_data(vd.vertex, data->scl));
  }
  BKE_pbvh_vertex_iter_end;
}

void SCULPT_do_smear_brush(Sculpt *sd, Object *ob, PBVHNode **nodes, int totnode)
{
  SculptSession *ss = ob->sculpt;
  Brush *brush = BKE_paint_brush(&sd->paint);

  if (!SCULPT_has_colors(ss)) {
    return;
  }

  SCULPT_vertex_random_access_ensure(ss);

  SculptCustomLayer prev_scl;
  SculptLayerParams params = {.permanent = false, .simple_array = false};

  SCULPT_attr_ensure_layer(
      ss, ob, ATTR_DOMAIN_POINT, CD_PROP_COLOR, "_sculpt_smear_previous", &params);
  SCULPT_attr_get_layer(
      ss, ob, ATTR_DOMAIN_POINT, CD_PROP_COLOR, "_sculpt_smear_previous", &prev_scl, &params);

  BKE_curvemapping_init(brush->curve);

  SculptThreadedTaskData data = {
      .sd = sd,
      .ob = ob,
      .scl = &prev_scl,
      .brush = brush,
      .nodes = nodes,
  };

  TaskParallelSettings settings;
  BKE_pbvh_parallel_range_settings(&settings, true, totnode);

  /* Smooth colors mode. */
  if (ss->cache->alt_smooth) {
    BLI_task_parallel_range(0, totnode, &data, do_color_smooth_task_cb_exec, &settings);
  }
  else {
    /* Smear mode. */
    BLI_task_parallel_range(0, totnode, &data, do_smear_store_prev_colors_task_cb_exec, &settings);
    BLI_task_parallel_range(0, totnode, &data, do_smear_brush_task_cb_exec, &settings);
  }
}
