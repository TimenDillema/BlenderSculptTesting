/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 by Nicholas Bishop. All rights reserved. */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include "DNA_brush_types.h"
#include "DNA_key_types.h"
#include "DNA_listBase.h"
#include "DNA_meshdata_types.h"
#include "DNA_vec_types.h"

#include "BLI_bitmap.h"
#include "BLI_compiler_compat.h"
#include "BLI_gsqueue.h"
#include "BLI_threads.h"

#include "ED_view3d.h"

#include "BKE_attribute.h"
#include "BKE_brush_engine.h"
#include "BKE_paint.h"
#include "BKE_pbvh.h"

#include "bmesh.h"

struct AutomaskingCache;
struct KeyBlock;
struct Object;
struct SculptUndoNode;
struct bContext;
struct BrushChannelSet;
struct TaskParallelTLS;

enum ePaintSymmetryFlags;

/*
maximum symmetry passes returned by SCULPT_get_symmetry_pass.
enough for about ~30 radial symmetry passes, which seems like plenty

used by various code that needs to statically store per-pass state.
*/
#define SCULPT_MAX_SYMMETRY_PASSES 255

/* Updates */

/* -------------------------------------------------------------------- */
/** \name Sculpt Types
 * \{ */

enum { SCULPT_SHARP_SIMPLE, SCULPT_SHARP_PLANE };

typedef enum SculptUpdateType {
  SCULPT_UPDATE_COORDS = 1 << 0,
  SCULPT_UPDATE_MASK = 1 << 1,
  SCULPT_UPDATE_VISIBILITY = 1 << 2,
  SCULPT_UPDATE_COLOR = 1 << 3,
} SculptUpdateType;

typedef struct SculptCursorGeometryInfo {
  float location[3];
  float back_location[3];
  float normal[3];
  float active_vertex_co[3];
} SculptCursorGeometryInfo;

struct _SculptNeighborRef {
  SculptVertRef vertex;
  SculptEdgeRef edge;
};

#define SCULPT_VERTEX_NEIGHBOR_FIXED_CAPACITY 12

typedef struct SculptVertexNeighborIter {
  /* Storage */
  struct _SculptNeighborRef *neighbors;
  int *neighbor_indices;

  int size;
  int capacity;
  struct _SculptNeighborRef neighbors_fixed[SCULPT_VERTEX_NEIGHBOR_FIXED_CAPACITY];
  int neighbor_indices_fixed[SCULPT_VERTEX_NEIGHBOR_FIXED_CAPACITY];

  /* Internal iterator. */
  int num_duplicates;
  int i;

  /* Public */
  SculptVertRef vertex;
  SculptEdgeRef edge;
  int index;
  bool has_edge;  // does this iteration step have an edge, fake neighbors do not
  bool is_duplicate;
  bool no_free;
} SculptVertexNeighborIter;

/* this is a bitmask */
typedef enum SculptCornerType {
  SCULPT_CORNER_NONE = 0,
  SCULPT_CORNER_MESH = 1 << 0,
  SCULPT_CORNER_FACE_SET = 1 << 1,
  SCULPT_CORNER_SEAM = 1 << 2,
  SCULPT_CORNER_SHARP = 1 << 3,
  SCULPT_CORNER_UV = 1 << 4,
} SculptCornerType;

typedef enum SculptBoundaryType {
  SCULPT_BOUNDARY_MESH = 1 << 0,
  SCULPT_BOUNDARY_FACE_SET = 1 << 1,
  SCULPT_BOUNDARY_SEAM = 1 << 2,
  SCULPT_BOUNDARY_SHARP = 1 << 3,
  SCULPT_BOUNDARY_UV = 1 << 4,
  SCULPT_BOUNDARY_ALL = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4),
  SCULPT_BOUNDARY_DEFAULT = (1 << 0) | (1 << 3) | (1 << 4)  // mesh and sharp
} SculptBoundaryType;

typedef struct SculptFaceSetIsland {
  SculptFaceRef *faces;
  int totface;
} SculptFaceSetIsland;

typedef struct SculptFaceSetIslands {
  SculptFaceSetIsland *islands;
  int totisland;
} SculptFaceSetIslands;

/* Sculpt Original Data */
typedef struct {
  struct BMLog *bm_log;

  struct SculptUndoNode *unode;
  int datatype;
  float (*coords)[3];
  float (*normals)[3];
  const float *vmasks;
  float (*colors)[4];
  float _no[3];

  /* Original coordinate, normal, and mask. */
  const float *co;
  const float *no;
  float mask;
  const float *col;
  struct PBVH *pbvh;
  struct SculptSession *ss;
} SculptOrigVertData;

typedef struct SculptSmoothArgs {
  float projection, slide_fset, bound_smooth;
  SculptCustomLayer *bound_scl;
  bool do_origco : 1;
  bool do_weighted_smooth : 1;
  bool preserve_fset_boundaries : 1;
  float bound_smooth_radius;  // if 0, ss->cache->radius will be used
  float vel_smooth_fac;
  SculptCustomLayer *vel_scl;
  float bevel_smooth_factor;
} SculptSmoothArgs;

typedef struct {
  GSQueue *queue;
  BLI_bitmap *visited_vertices;
} SculptFloodFill;

typedef enum eBoundaryAutomaskMode {
  AUTOMASK_INIT_BOUNDARY_EDGES = 1,
  AUTOMASK_INIT_BOUNDARY_FACE_SETS = 2,
} eBoundaryAutomaskMode;

typedef enum {
  SCULPT_UNDO_COORDS = 1 << 0,
  SCULPT_UNDO_HIDDEN = 1 << 1,
  SCULPT_UNDO_MASK = 1 << 2,
  SCULPT_UNDO_DYNTOPO_BEGIN = 1 << 3,
  SCULPT_UNDO_DYNTOPO_END = 1 << 4,
  SCULPT_UNDO_DYNTOPO_SYMMETRIZE = 1 << 5,
  SCULPT_UNDO_GEOMETRY = 1 << 6,
  SCULPT_UNDO_FACE_SETS = 1 << 7,
  SCULPT_UNDO_COLOR = 1 << 8,
} SculptUndoType;

/* Storage of geometry for the undo node.
 * Is used as a storage for either original or modified geometry. */
typedef struct SculptUndoNodeGeometry {
  /* Is used for sanity check, helping with ensuring that two and only two
   * geometry pushes happened in the undo stack. */
  bool is_initialized;

  CustomData vdata;
  CustomData edata;
  CustomData ldata;
  CustomData pdata;
  int totvert;
  int totedge;
  int totloop;
  int totpoly;
} SculptUndoNodeGeometry;

typedef struct SculptUndoNode {
  struct SculptUndoNode *next, *prev;

  SculptUndoType type;

  char idname[MAX_ID_NAME]; /* Name instead of pointer. */
  void *node;               /* only during push, not valid afterwards! */

  float (*co)[3];
  float (*orig_co)[3];
  float (*no)[3];
  float (*col)[4];
  float *mask;
  int totvert;

  /* non-multires */
  int maxvert;          /* to verify if totvert it still the same */
  SculptVertRef *index; /* to restore into right location */
  BLI_bitmap *vert_hidden;

  /* multires */
  int maxgrid;  /* same for grid */
  int gridsize; /* same for grid */
  int totgrid;  /* to restore into right location */
  int *grids;   /* to restore into right location */
  BLI_bitmap **grid_hidden;

  /* bmesh */
  struct BMLogEntry *bm_entry;
  bool applied;

  /* shape keys */
  char shapeName[sizeof(((KeyBlock *)0))->name];

  /* Geometry modification operations.
   *
   * Original geometry is stored before some modification is run and is used to restore state of
   * the object when undoing the operation
   *
   * Modified geometry is stored after the modification and is used to redo the modification. */
  bool geometry_clear_pbvh;
  SculptUndoNodeGeometry geometry_original;
  SculptUndoNodeGeometry geometry_modified;

  /* Geometry at the bmesh enter moment. */
  SculptUndoNodeGeometry geometry_bmesh_enter;

  /* pivot */
  float pivot_pos[3];
  float pivot_rot[4];

  /* Sculpt Face Sets */
  int *face_sets;

  // dyntopo stuff

  int *nodemap;
  int nodemap_size;
  int typemask;

  size_t undo_size;
  // int gen, lasthash;
} SculptUndoNode;

/* Factor of brush to have rake point following behind
 * (could be configurable but this is reasonable default). */
#define SCULPT_RAKE_BRUSH_FACTOR 0.25f

struct SculptRakeData {
  float follow_dist;
  float follow_co[3];
};

/* Single struct used by all BLI_task threaded callbacks, let's avoid adding 10's of those... */
typedef struct SculptThreadedTaskData {
  struct bContext *C;
  struct Sculpt *sd;
  struct Object *ob;
  struct SculptSession *ss;
  const struct Brush *brush;
  struct PBVHNode **nodes;
  int totnode;

  struct VPaint *vp;
  struct VPaintData *vpd;
  struct WPaintData *wpd;
  struct WeightPaintInfo *wpi;
  unsigned int *lcol;
  struct Mesh *me;
  /* For passing generic params. */
  void *custom_data;

  /* Data specific to some callbacks. */

  /* NOTE: even if only one or two of those are used at a time,
   *       keeping them separated, names help figuring out
   *       what it is, and memory overhead is ridiculous anyway. */
  float flippedbstrength;
  float angle;
  float strength;
  bool smooth_mask;
  bool has_bm_orco;

  struct SculptProjectVector *spvc;
  float *offset;
  float *grab_delta;
  float *cono;
  float *area_no;
  float *area_no_sp;
  float *area_co;
  float (*mat)[4];
  float (*vertCos)[3];

  /* When true, the displacement stored in the proxies will be aplied to the original coordinates
   * instead of to the current coordinates. */
  bool use_proxies_orco;

  /* X and Z vectors aligned to the stroke direction for operations where perpendicular vectors to
   * the stroke direction are needed. */
  float (*stroke_xz)[3];

  int filter_type;
  float filter_strength;
  float *filter_fill_color;

  bool use_area_cos;
  bool use_area_nos;

  /* 0=towards view, 1=flipped */
  float (*area_cos)[3];
  float (*area_nos)[3];
  int *count_no;
  int *count_co;

  bool any_vertex_sampled;

  float *wet_mix_sampled_color;
  float hue_offset;

  float *prev_mask;
  float *new_mask;
  float *next_mask;
  float mask_interpolation;

  float *pose_factor;
  float *pose_initial_co;
  int pose_chain_segment;

  float multiplane_scrape_angle;
  float multiplane_scrape_planes[2][4];

  float max_distance_squared;
  float nearest_vertex_search_co[3];

  /* Stabilized strength for the Clay Thumb brush. */
  float clay_strength;

  int mask_expand_update_it;
  bool mask_expand_invert_mask;
  bool mask_expand_use_normals;
  bool mask_expand_keep_prev_mask;
  bool mask_expand_create_face_set;

  float transform_mats[8][4][4];
  float elastic_transform_mat[4][4];
  float elastic_transform_pivot[3];
  float elastic_transform_radius;

  /* Boundary brush */
  float boundary_deform_strength;

  float cloth_time_step;
  SculptClothSimulation *cloth_sim;
  float *cloth_sim_initial_location;
  float cloth_sim_radius;

  float dirty_mask_min;
  float dirty_mask_max;
  bool dirty_mask_dirty_only;

  /* Mask By Color Tool */

  float mask_by_color_threshold;
  bool mask_by_color_invert;
  bool mask_by_color_preserve_mask;

  /* Index of the vertex that is going to be used as a reference for the colors. */
  SculptVertRef mask_by_color_vertex;
  float *mask_by_color_floodfill;

  int face_set, face_set2;
  int filter_undo_type;

  int mask_init_mode;
  int mask_init_seed;

  ThreadMutex mutex;

  // Layer brush
  int cd_temp, cd_temp2, cd_temp3, cd_sculpt_vert;

  float smooth_projection;
  float rake_projection;
  SculptCustomLayer *scl, *scl2;
  bool do_origco;
  float *brush_color;

  float fset_slide, bound_smooth;
  float crease_pinch_factor;
  bool use_curvature;
  float vel_smooth_fac;
  int iterations;
} SculptThreadedTaskData;

/*************** Brush testing declarations ****************/
typedef struct SculptBrushTest {
  float radius_squared;
  float radius;
  float location[3];
  float dist;
  int mirror_symmetry_pass;

  int radial_symmetry_pass;
  float symm_rot_mat_inv[4][4];

  /* For circle (not sphere) projection. */
  float plane_view[4];

  /* Some tool code uses a plane for its calculations. */
  float plane_tool[4];

  /* View3d clipping - only set rv3d for clipping */
  struct RegionView3D *clip_rv3d;
} SculptBrushTest;

typedef bool (*SculptBrushTestFn)(SculptBrushTest *test, const float co[3]);

typedef struct {
  struct Sculpt *sd;
  struct SculptSession *ss;
  float radius_squared;
  const float *center;
  bool original;
  /* This ignores fully masked and fully hidden nodes. */
  bool ignore_fully_ineffective;
  struct Object *ob;
  struct Brush *brush;
} SculptSearchSphereData;

typedef struct {
  struct Sculpt *sd;
  struct SculptSession *ss;
  float radius_squared;
  bool original;
  bool ignore_fully_ineffective;
  struct DistRayAABB_Precalc *dist_ray_to_aabb_precalc;
} SculptSearchCircleData;

#define SCULPT_CLAY_STABILIZER_LEN 10
#define SCULPT_SPEED_MA_SIZE 4
#define GRAB_DELTA_MA_SIZE 3

typedef struct AutomaskingSettings {
  /* Flags from eAutomasking_flag. */
  int flags;
  int initial_face_set;
  int current_face_set;  // used by faceset draw tool
  float concave_factor;
  float normal_limit, normal_falloff;
  float view_normal_limit, view_normal_falloff;
  bool original_normal;
} AutomaskingSettings;

typedef struct AutomaskingCache {
  AutomaskingSettings settings;
  /* Precomputed auto-mask factor indexed by vertex, owned by the auto-masking system and
   * initialized in #SCULPT_automasking_cache_init when needed. */
  // float *factor;
  SculptCustomLayer *factorlayer;
} AutomaskingCache;

typedef struct StrokeCache {
  BrushMappingData input_mapping;

  /* Invariants */
  float initial_radius;
  float scale[3];
  int flag;
  float clip_tolerance[3];
  float clip_mirror_mtx[4][4];
  float initial_mouse[2];

  struct BrushChannelSet *channels_final;

  /* Variants */
  float radius;
  float radius_squared;
  float true_location[3];
  float true_last_location[3];
  float location[3];
  float last_location[3];

  /* Used for alternating between deformation in brushes that need to apply different ones to
   * achieve certain effects. */
  int iteration_count;

  /* Original pixel radius with the pressure curve applied for dyntopo detail size */
  float dyntopo_pixel_radius;

  bool is_last_valid;

  bool pen_flip;
  bool invert;
  float pressure;
  float bstrength;
  float normal_weight; /* from brush (with optional override) */
  float x_tilt;
  float y_tilt;

  /* Position of the mouse corresponding to the stroke location, modified by the paint_stroke
   * operator according to the stroke type. */
  float mouse[2];
  /* Position of the mouse event in screen space, not modified by the stroke type. */
  float mouse_event[2];

  float (*prev_colors)[4];

  /* Multires Displacement Smear. */
  float (*prev_displacement)[3];
  float (*limit_surface_co)[3];

  /* The rest is temporary storage that isn't saved as a property */

  bool first_time; /* Beginning of stroke may do some things special */

  /* from ED_view3d_ob_project_mat_get() */
  float projection_mat[4][4];

  /* Clean this up! */
  struct ViewContext *vc;
  struct Brush *brush;

  float special_rotation;
  float grab_delta[3], grab_delta_symmetry[3];
  float old_grab_location[3], orig_grab_location[3];

  // next_grab_delta is same as grab_delta except in smooth rake mode
  float prev_grab_delta[3], next_grab_delta[3];
  float prev_grab_delta_symmetry[3], next_grab_delta_symmetry[3];
  float grab_delta_avg[GRAB_DELTA_MA_SIZE][3];
  int grab_delta_avg_cur;

  /* screen-space rotation defined by mouse motion */
  float rake_rotation[4], rake_rotation_symmetry[4];
  bool is_rake_rotation_valid;
  struct SculptRakeData rake_data;

  /* Geodesic distances. */
  float *geodesic_dists[PAINT_SYMM_AREAS];

  /* Face Sets */
  int paint_face_set;

  /* Symmetry index between 0 and 7 bit combo 0 is Brush only;
   * 1 is X mirror; 2 is Y mirror; 3 is XY; 4 is Z; 5 is XZ; 6 is YZ; 7 is XYZ */
  int symmetry;
  int boundary_symmetry;    // controls splitting face sets by mirror axis
  int mirror_symmetry_pass; /* The symmetry pass we are currently on between 0 and 7. */
  float true_view_normal[3];
  float view_normal[3];

  float view_origin[3];
  float true_view_origin[3];

  /* sculpt_normal gets calculated by calc_sculpt_normal(), then the
   * sculpt_normal_symm gets updated quickly with the usual symmetry
   * transforms */
  float sculpt_normal[3];
  float sculpt_normal_symm[3];

  /* Used for area texture mode, local_mat gets calculated by
   * calc_brush_local_mat() and used in tex_strength(). */
  float brush_local_mat[4][4];

  float plane_offset[3]; /* used to shift the plane around when doing tiled strokes */
  int tile_pass;

  float last_center[3];
  int radial_symmetry_pass;
  float symm_rot_mat[4][4];
  float symm_rot_mat_inv[4][4];
  bool original;
  float anchored_location[3];

  /* Fairing. */

  /* Paint Brush. */
  struct {
    float hardness;
    float flow;
    float wet_mix;
    float wet_persistence;
    float density;
  } paint_brush;

  /* Pose brush */
  struct SculptPoseIKChain *pose_ik_chain;

  /* Enhance Details. */
  float (*detail_directions)[3];

  /* Clay Thumb brush */
  /* Angle of the front tilting plane of the brush to simulate clay accumulation. */
  float clay_thumb_front_angle;
  /* Stores pressure samples to get an stabilized strength and radius variation. */
  float clay_pressure_stabilizer[SCULPT_CLAY_STABILIZER_LEN];
  int clay_pressure_stabilizer_index;

  /* Cloth brush */
  struct SculptClothSimulation *cloth_sim;
  float initial_location[3];
  float true_initial_location[3];
  float initial_normal[3];
  float true_initial_normal[3];

  /* Boundary brush */
  struct SculptBoundary *boundaries[PAINT_SYMM_AREAS];

  /* Surface Smooth Brush */
  /* Stores the displacement produced by the laplacian step of HC smooth. */
  float (*surface_smooth_laplacian_disp)[3];

  /* Layer brush */
  float *layer_displacement_factor;
  int *layer_stroke_id;

  float vertex_rotation; /* amount to rotate the vertices when using rotate brush */
  struct Dial *dial;

  char saved_active_brush_name[MAX_ID_NAME];
  char saved_mask_brush_tool;
  int saved_smooth_size; /* smooth tool copies the size of the current tool */
  bool alt_smooth;

  /* Scene Project Brush */
  struct SnapObjectContext *snap_context;
  struct Depsgraph *depsgraph;

  float plane_trim_squared;

  bool supports_gravity;
  float true_gravity_direction[3];
  float gravity_direction[3];

  /* Auto-masking. */
  AutomaskingCache *automasking;

  float stroke_local_mat[4][4];
  float multiplane_scrape_angle;

  float wet_mix_prev_color[4];
  float density_seed;

  rcti previous_r; /* previous redraw rectangle */
  rcti current_r;  /* current redraw rectangle */

  float stroke_distance;    // copy of PaintStroke->stroke_distance
  float stroke_distance_t;  // copy of PaintStroke->stroke_distance_t

  float last_dyntopo_t;
  float last_smooth_t[SCULPT_MAX_SYMMETRY_PASSES];
  float last_rake_t[SCULPT_MAX_SYMMETRY_PASSES];

  int layer_disp_map_size;
  BLI_bitmap *layer_disp_map;

  struct PaintStroke *stroke;
  struct bContext *C;

  struct BrushCommandList *commandlist;
  bool use_plane_trim;

  struct NeighborCache *ncache;
  float speed_avg[SCULPT_SPEED_MA_SIZE];  // moving average for speed
  int speed_avg_cur;
  double last_speed_time;

  // if nonzero, override brush sculpt tool
  int tool_override;
  BrushChannelSet *tool_override_channels;
} StrokeCache;

/* Sculpt Filters */
typedef enum SculptFilterOrientation {
  SCULPT_FILTER_ORIENTATION_LOCAL = 0,
  SCULPT_FILTER_ORIENTATION_WORLD = 1,
  SCULPT_FILTER_ORIENTATION_VIEW = 2,
} SculptFilterOrientation;

/* Defines how transform tools are going to apply its displacement. */
typedef enum SculptTransformDisplacementMode {
  /* Displaces the elements from their original coordinates. */
  SCULPT_TRANSFORM_DISPLACEMENT_ORIGINAL = 0,
  /* Displaces the elements incrementally from their previous position. */
  SCULPT_TRANSFORM_DISPLACEMENT_INCREMENTAL = 1,
} SculptTransformDisplacementMode;

/* Sculpt Expand. */
typedef enum eSculptExpandFalloffType {
  SCULPT_EXPAND_FALLOFF_GEODESIC,
  SCULPT_EXPAND_FALLOFF_TOPOLOGY,
  SCULPT_EXPAND_FALLOFF_TOPOLOGY_DIAGONALS,
  SCULPT_EXPAND_FALLOFF_NORMALS,
  SCULPT_EXPAND_FALLOFF_SPHERICAL,
  SCULPT_EXPAND_FALLOFF_BOUNDARY_TOPOLOGY,
  SCULPT_EXPAND_FALLOFF_BOUNDARY_FACE_SET,
  SCULPT_EXPAND_FALLOFF_ACTIVE_FACE_SET,
  SCULPT_EXPAND_FALLOFF_POLY_LOOP,
} eSculptExpandFalloffType;

typedef enum eSculptExpandTargetType {
  SCULPT_EXPAND_TARGET_MASK,
  SCULPT_EXPAND_TARGET_FACE_SETS,
  SCULPT_EXPAND_TARGET_COLORS,
} eSculptExpandTargetType;

typedef enum eSculptExpandRecursionType {
  SCULPT_EXPAND_RECURSION_TOPOLOGY,
  SCULPT_EXPAND_RECURSION_GEODESICS,
} eSculptExpandRecursionType;

#define EXPAND_SYMM_AREAS 8

typedef struct ExpandCache {
  /* Target data elements that the expand operation will affect. */
  eSculptExpandTargetType target;

  /* Falloff data. */
  eSculptExpandFalloffType falloff_type;

  /* Indexed by vertex index, precalculated falloff value of that vertex (without any falloff
   * editing modification applied). */
  float *vert_falloff;
  /* Max falloff value in *vert_falloff. */
  float max_vert_falloff;

  /* Indexed by base mesh poly index, precalculated falloff value of that face. These values are
   * calculated from the per vertex falloff (*vert_falloff) when needed. */
  float *face_falloff;
  float max_face_falloff;

  /* Falloff value of the active element (vertex or base mesh face) that Expand will expand to. */
  float active_falloff;

  /* When set to true, expand skips all falloff computations and considers all elements as enabled.
   */
  bool all_enabled;

  /* Initial mouse and cursor data from where the current falloff started. This data can be changed
   * during the execution of Expand by moving the origin. */
  float initial_mouse_move[2];
  float initial_mouse[2];
  SculptVertRef initial_active_vertex;
  int initial_active_face_set;

  /* Maximum number of vertices allowed in the SculptSession for previewing the falloff using
   * geodesic distances. */
  int max_geodesic_move_preview;

  /* Original falloff type before starting the move operation. */
  eSculptExpandFalloffType move_original_falloff_type;
  /* Falloff type using when moving the origin for preview. */
  eSculptExpandFalloffType move_preview_falloff_type;

  /* Face set ID that is going to be used when creating a new Face Set. */
  int next_face_set;

  /* Face Set ID of the Face set selected for editing. */
  int update_face_set;

  /* Mouse position since the last time the origin was moved. Used for reference when moving the
   * initial position of Expand. */
  float original_mouse_move[2];

  /* Active components checks. */
  /* Indexed by symmetry pass index, contains the connected component ID found in
   * SculptSession->vertex_info.connected_component. Other connected components not found in this
   * array will be ignored by Expand. */
  int active_connected_components[EXPAND_SYMM_AREAS];

  /* Snapping. */
  /* GSet containing all Face Sets IDs that Expand will use to snap the new data. */
  GSet *snap_enabled_face_sets;

  /* Texture distortion data. */
  Brush *brush;
  struct Scene *scene;
  struct MTex *mtex;

  /* Controls how much texture distortion will be applied to the current falloff */
  float texture_distortion_strength;

  /* Cached PBVH nodes. This allows to skip gathering all nodes from the PBVH each time expand
   * needs to update the state of the elements. */
  PBVHNode **nodes;
  int totnode;

  /* Expand state options. */

  /* Number of loops (times that the falloff is going to be repeated). */
  int loop_count;

  /* Invert the falloff result. */
  bool invert;

  /* When set to true, preserves the previous state of the data and adds the new one on top. */
  bool preserve;

  /* When true, preserve mode will flip in inverse mode */
  bool preserve_flip_inverse;

  /* When set to true, the mask or colors will be applied as a gradient. */
  bool falloff_gradient;

  /* When set to true, Expand will use the Brush falloff curve data to shape the gradient. */
  bool brush_gradient;

  /* When set to true, Expand will move the origin (initial active vertex and cursor position)
   * instead of updating the active vertex and active falloff. */
  bool move;

  /* When set to true, Expand will snap the new data to the Face Sets IDs found in
   * *original_face_sets. */
  bool snap;

  /* When set to true, Expand will use the current Face Set ID to modify an existing Face Set
   * instead of creating a new one. */
  bool modify_active_face_set;

  /* When set to true, Expand will reposition the sculpt pivot to the boundary of the expand result
   * after finishing the operation. */
  bool reposition_pivot;

  /* Color target data type related data. */
  float fill_color[4];
  short blend_mode;

  /* Face Sets at the first step of the expand operation, before starting modifying the active
   * vertex and active falloff. These are not the original Face Sets of the sculpt before starting
   * the operator as they could have been modified by Expand when initializing the operator and
   * before starting changing the active vertex. These Face Sets are used for restoring and
   * checking the Face Sets state while the Expand operation modal runs. */
  int *initial_face_sets;

  /* Original data of the sculpt as it was before running the Expand operator. */
  float *original_mask;
  int *original_face_sets;
  float (*original_colors)[4];
} ExpandCache;

typedef enum eSculptGradientType {
  SCULPT_GRADIENT_LINEAR,
  SCULPT_GRADIENT_SPHERICAL,
  SCULPT_GRADIENT_RADIAL,
  SCULPT_GRADIENT_ANGLE,
  SCULPT_GRADIENT_REFLECTED,
} eSculptGradientType;
typedef struct SculptGradientContext {
  eSculptGradientType gradient_type;
  ViewContext vc;

  int symm;

  int update_type;
  float line_points[2][2];

  float line_length;

  float depth_point[3];

  float gradient_plane[4];
  float initial_location[3];

  float gradient_line[3];
  float initial_projected_location[2];

  float strength;
  void (*sculpt_gradient_begin)(struct bContext *);

  void (*sculpt_gradient_apply_for_element)(struct Sculpt *,
                                            struct SculptSession *,
                                            SculptOrigVertData *orig_data,
                                            PBVHVertexIter *vd,
                                            float gradient_value,
                                            float fade_value);
  void (*sculpt_gradient_node_update)(struct PBVHNode *);
  void (*sculpt_gradient_end)(struct bContext *);
} SculptGradientContext;

/* IPMask filter vertex callback function. */
typedef float(SculptIPMaskFilterStepVertexCB)(struct SculptSession *, SculptVertRef, float *);

typedef struct MaskFilterDeltaStep {
  int totelem;
  int *index;
  float *delta;
} MaskFilterDeltaStep;

typedef struct FilterCache {
  bool enabled_axis[3];
  bool enabled_force_axis[3];
  int random_seed;

  /* Used for alternating between filter operations in filters that need to apply different ones to
   * achieve certain effects. */
  int iteration_count;

  /* Stores the displacement produced by the laplacian step of HC smooth. */
  float (*surface_smooth_laplacian_disp)[3];
  float surface_smooth_shape_preservation;
  float surface_smooth_current_vertex;

  /* Sharpen mesh filter. */
  float sharpen_smooth_ratio;
  float sharpen_intensify_detail_strength;
  int sharpen_curvature_smooth_iterations;
  float *sharpen_factor;
  float (*detail_directions)[3];

  /* Sphere mesh filter. */
  float sphere_center[3];
  float sphere_radius;

  /* Filter orientation. */
  SculptFilterOrientation orientation;
  float obmat[4][4];
  float obmat_inv[4][4];
  float viewmat[4][4];
  float viewmat_inv[4][4];

  /* Displacement eraser. */
  float (*limit_surface_co)[3];

  /* unmasked nodes */
  PBVHNode **nodes;
  int totnode;

  /* Cloth filter. */
  SculptClothSimulation *cloth_sim;
  float cloth_sim_pinch_point[3];

  /* mask expand iteration caches */
  int mask_update_current_it;
  int mask_update_last_it;
  int *mask_update_it;
  float *normal_factor;
  float *edge_factor;
  float *prev_mask;
  float mask_expand_initial_co[3];

  int new_face_set;
  int *prev_face_set;

  int active_face_set;

  /* Transform. */
  SculptTransformDisplacementMode transform_displacement_mode;

  /* Gradient. */
  SculptGradientContext *gradient_context;

  /* Auto-masking. */
  AutomaskingCache *automasking;

  /* Mask Filter. */
  int mask_filter_current_step;
  float *mask_filter_ref;
  SculptIPMaskFilterStepVertexCB *mask_filter_step_forward;
  SculptIPMaskFilterStepVertexCB *mask_filter_step_backward;

  GHash *mask_delta_step;

  bool preserve_fset_boundaries;
  bool weighted_smooth;
  float hard_edge_fac;
  bool hard_edge_mode;
  float bound_smooth_radius;
  float bevel_smooth_fac;
} FilterCache;

typedef struct SculptCurvatureData {
  float ks[3];
  float principle[3][3];  // normalized
} SculptCurvatureData;

typedef struct SculptFaceSetDrawData {
  struct Sculpt *sd;
  struct Object *ob;
  PBVHNode **nodes;
  int totnode;
  struct Brush *brush;
  float bstrength;

  int faceset;
  int count;
  bool use_fset_curve;
  bool use_fset_strength;

  float *prev_stroke_direction;
  float *stroke_direction;
  float *next_stroke_direction;
  struct BrushChannel *curve_ch;
} SculptFaceSetDrawData;

enum eDynTopoWarnFlag {
  DYNTOPO_WARN_EDATA = (1 << 1),
  DYNTOPO_WARN_MODIFIER = (1 << 3),
  DYNTOPO_ERROR_MULTIRES = (1 << 4)
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sculpt Poll Functions
 * \{ */

bool SCULPT_mode_poll(struct bContext *C);
bool SCULPT_mode_poll_view3d(struct bContext *C);
/**
 * Checks for a brush, not just sculpt mode.
 */
bool SCULPT_poll(struct bContext *C);
bool SCULPT_poll_view3d(struct bContext *C);

bool SCULPT_vertex_colors_poll(struct bContext *C);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sculpt Update Functions
 * \{ */

void SCULPT_flush_update_step(bContext *C, SculptUpdateType update_flags);
void SCULPT_flush_update_done(const bContext *C, Object *ob, SculptUpdateType update_flags);

void SCULPT_pbvh_clear(Object *ob);

/**
 * Flush displacement from deformed PBVH to original layer.
 */
void SCULPT_flush_stroke_deform(struct Sculpt *sd, Object *ob, bool is_proxy_used);

/**
 * Should be used after modifying the mask or Face Sets IDs.
 */
void SCULPT_tag_update_overlays(bContext *C);
/** \} */

/* -------------------------------------------------------------------- */
/** \name Stroke Functions
 * \{ */

/* Stroke */

/**
 * Do a ray-cast in the tree to find the 3d brush location
 * (This allows us to ignore the GL depth buffer)
 * Returns 0 if the ray doesn't hit the mesh, non-zero otherwise.
 */
bool SCULPT_stroke_get_location(struct bContext *C, float out[3], const float mouse[2]);
/**
 * Gets the normal, location and active vertex location of the geometry under the cursor. This also
 * updates the active vertex and cursor related data of the SculptSession using the mouse position
 */
bool SCULPT_cursor_geometry_info_update(bContext *C,
                                        SculptCursorGeometryInfo *out,
                                        const float mouse[2],
                                        bool use_sampled_normal,
                                        bool use_back_depth);
void SCULPT_geometry_preview_lines_update(bContext *C, struct SculptSession *ss, float radius);

void SCULPT_stroke_modifiers_check(const bContext *C, Object *ob, const Brush *brush);
float SCULPT_raycast_init(struct ViewContext *vc,
                          const float mouse[2],
                          float ray_start[3],
                          float ray_end[3],
                          float ray_normal[3],
                          bool original);

/* Symmetry */
char SCULPT_mesh_symmetry_xyz_get(Object *object);

/**
 * Returns true when the step belongs to the stroke that is directly performed by the brush and
 * not by one of the symmetry passes.
 */
bool SCULPT_stroke_is_main_symmetry_pass(struct StrokeCache *cache);
/**
 * Return true only once per stroke on the first symmetry pass, regardless of the symmetry passes
 * enabled.
 *
 * This should be used for functionality that needs to be computed once per stroke of a particular
 * tool (allocating memory, updating random seeds...).
 */
bool SCULPT_stroke_is_first_brush_step(struct StrokeCache *cache);
/**
 * Returns true on the first brush step of each symmetry pass.
 */
bool SCULPT_stroke_is_first_brush_step_of_symmetry_pass(struct StrokeCache *cache);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sculpt mesh accessor API
 * \{ */

/** Ensure random access; required for PBVH_BMESH */
void SCULPT_vertex_random_access_ensure(struct SculptSession *ss);

/** Ensure random access; required for PBVH_BMESH */
void SCULPT_face_random_access_ensure(struct SculptSession *ss);

int SCULPT_vertex_valence_get(const struct SculptSession *ss, SculptVertRef vertex);
int SCULPT_vertex_count_get(const struct SculptSession *ss);

bool SCULPT_vertex_color_get(const SculptSession *ss, SculptVertRef vertex, float out[4]);
void SCULPT_vertex_color_set(const SculptSession *ss, SculptVertRef vertex, float color[4]);
bool SCULPT_has_colors(const SculptSession *ss);

const float *SCULPT_vertex_co_get(struct SculptSession *ss, SculptVertRef index);
void SCULPT_vertex_normal_get(SculptSession *ss, SculptVertRef index, float no[3]);
float SCULPT_vertex_mask_get(struct SculptSession *ss, SculptVertRef index);
float *SCULPT_vertex_origco_get(SculptSession *ss, SculptVertRef vertex);
float *SCULPT_vertex_origno_get(SculptSession *ss, SculptVertRef vertex);

const float *SCULPT_vertex_persistent_co_get(SculptSession *ss, SculptVertRef index);
void SCULPT_vertex_persistent_normal_get(SculptSession *ss, SculptVertRef index, float no[3]);

bool SCULPT_has_persistent_base(SculptSession *ss);

/**
 * Coordinates used for manipulating the base mesh when Grab Active Vertex is enabled.
 */
const float *SCULPT_vertex_co_for_grab_active_get(SculptSession *ss, SculptVertRef vertex);

/**
 * Returns the info of the limit surface when multi-res is available,
 * otherwise it returns the current coordinate of the vertex.
 */
void SCULPT_vertex_limit_surface_get(SculptSession *ss, SculptVertRef vertex, float r_co[3]);

/**
 * Returns the pointer to the coordinates that should be edited from a brush tool iterator
 * depending on the given deformation target.
 */
float *SCULPT_brush_deform_target_vertex_co_get(SculptSession *ss,
                                                int deform_target,
                                                PBVHVertexIter *iter);

void SCULPT_vertex_neighbors_get(const struct SculptSession *ss,
                                 const SculptVertRef vref,
                                 const bool include_duplicates,
                                 SculptVertexNeighborIter *iter);

/* Iterator over neighboring vertices. */
#define SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN(ss, v_index, neighbor_iterator) \
  SCULPT_vertex_neighbors_get(ss, v_index, false, &neighbor_iterator); \
  for (neighbor_iterator.i = 0; neighbor_iterator.i < neighbor_iterator.size; \
       neighbor_iterator.i++) { \
    neighbor_iterator.has_edge = neighbor_iterator.neighbors[neighbor_iterator.i].edge.i != \
                                 SCULPT_REF_NONE; \
    neighbor_iterator.vertex = neighbor_iterator.neighbors[neighbor_iterator.i].vertex; \
    neighbor_iterator.edge = neighbor_iterator.neighbors[neighbor_iterator.i].edge; \
    neighbor_iterator.index = neighbor_iterator.neighbor_indices[neighbor_iterator.i];

/* Iterate over neighboring and duplicate vertices (for PBVH_GRIDS). Duplicates come
 * first since they are nearest for floodfill. */
#define SCULPT_VERTEX_DUPLICATES_AND_NEIGHBORS_ITER_BEGIN(ss, v_index, neighbor_iterator) \
  SCULPT_vertex_neighbors_get(ss, v_index, true, &neighbor_iterator); \
  for (neighbor_iterator.i = neighbor_iterator.size - 1; neighbor_iterator.i >= 0; \
       neighbor_iterator.i--) { \
    neighbor_iterator.has_edge = neighbor_iterator.neighbors[neighbor_iterator.i].edge.i != \
                                 SCULPT_REF_NONE; \
    neighbor_iterator.vertex = neighbor_iterator.neighbors[neighbor_iterator.i].vertex; \
    neighbor_iterator.edge = neighbor_iterator.neighbors[neighbor_iterator.i].edge; \
    neighbor_iterator.index = neighbor_iterator.neighbor_indices[neighbor_iterator.i]; \
    neighbor_iterator.is_duplicate = (neighbor_iterator.i >= \
                                      neighbor_iterator.size - neighbor_iterator.num_duplicates);

#define SCULPT_VERTEX_NEIGHBORS_ITER_END(neighbor_iterator) \
  } \
  if (!neighbor_iterator.no_free && \
      neighbor_iterator.neighbors != neighbor_iterator.neighbors_fixed) { \
    MEM_freeN(neighbor_iterator.neighbors); \
    MEM_freeN(neighbor_iterator.neighbor_indices); \
  } \
  ((void)0)

#define SCULPT_VERTEX_NEIGHBORS_ITER_FREE(neighbor_iterator) \
  if (neighbor_iterator.neighbors && !neighbor_iterator.no_free && \
      neighbor_iterator.neighbors != neighbor_iterator.neighbors_fixed) { \
    MEM_freeN(neighbor_iterator.neighbors); \
    MEM_freeN(neighbor_iterator.neighbor_indices); \
  } \
  ((void)0)

SculptVertRef SCULPT_active_vertex_get(SculptSession *ss);
const float *SCULPT_active_vertex_co_get(SculptSession *ss);
void SCULPT_active_vertex_normal_get(SculptSession *ss, float normal[3]);

/* Returns PBVH deformed vertices array if shape keys or deform modifiers are used, otherwise
 * returns mesh original vertices array. */
struct MVert *SCULPT_mesh_deformed_mverts_get(SculptSession *ss);

/* Fake Neighbors */

#define FAKE_NEIGHBOR_NONE -1

void SCULPT_fake_neighbors_ensure(struct Sculpt *sd, Object *ob, float max_dist);
void SCULPT_fake_neighbors_enable(Object *ob);
void SCULPT_fake_neighbors_disable(Object *ob);
void SCULPT_fake_neighbors_free(struct Object *ob);

/* Vertex Info. */
void SCULPT_boundary_info_ensure(Object *object);

/* Boundary Info needs to be initialized in order to use this function. */
SculptCornerType SCULPT_vertex_is_corner(const SculptSession *ss,
                                         const SculptVertRef index,
                                         SculptCornerType cornertype);

/* Boundary Info needs to be initialized in order to use this function. */
SculptBoundaryType SCULPT_vertex_is_boundary(const SculptSession *ss,
                                             const SculptVertRef index,
                                             SculptBoundaryType boundary_types);

void SCULPT_connected_components_ensure(Object *ob);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sculpt Visibility API
 * \{ */

void SCULPT_vertex_visible_set(SculptSession *ss, SculptVertRef vertex, bool visible);
bool SCULPT_vertex_visible_get(SculptSession *ss, SculptVertRef vertex);

void SCULPT_visibility_sync_all_face_sets_to_vertices(struct Object *ob);
void SCULPT_visibility_sync_all_vertex_to_face_sets(struct SculptSession *ss);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Face API
 * \{ */

SculptEdgeRef sculpt_poly_loop_initial_edge_from_cursor(Object *ob);
BLI_bitmap *sculpt_poly_loop_from_cursor(struct Object *ob);

SculptFaceSetIslands *SCULPT_face_set_islands_get(SculptSession *ss, int fset);
void SCULPT_face_set_islands_free(SculptSession *ss, SculptFaceSetIslands *islands);

SculptFaceSetIsland *SCULPT_face_set_island_get(SculptSession *ss, SculptFaceRef face, int fset);
void SCULPT_face_set_island_free(SculptFaceSetIsland *island);

void SCULPT_face_normal_get(SculptSession *ss, SculptFaceRef face, float no[3]);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Face Sets API
 * \{ */

int SCULPT_active_face_set_get(SculptSession *ss);
int SCULPT_vertex_face_set_get(SculptSession *ss, SculptVertRef vertex);
void SCULPT_vertex_face_set_set(SculptSession *ss, SculptVertRef vertex, int face_set);

bool SCULPT_vertex_has_face_set(SculptSession *ss, SculptVertRef vertex, int face_set);
bool SCULPT_vertex_has_unique_face_set(const SculptSession *ss, SculptVertRef vertex);

int SCULPT_face_set_next_available_get(SculptSession *ss);

void SCULPT_face_set_visibility_set(SculptSession *ss, int face_set, bool visible);
bool SCULPT_vertex_all_face_sets_visible_get(const SculptSession *ss, SculptVertRef vertex);
bool SCULPT_vertex_any_face_set_visible_get(SculptSession *ss, SculptVertRef vertex);

void SCULPT_face_sets_visibility_invert(SculptSession *ss);
void SCULPT_face_sets_visibility_all_set(SculptSession *ss, bool visible);

int SCULPT_face_set_get(SculptSession *ss, SculptFaceRef face);
int SCULPT_face_set_set(SculptSession *ss, SculptFaceRef face, int fset);

int SCULPT_face_set_original_get(SculptSession *ss, SculptFaceRef face);

int SCULPT_face_set_flag_get(SculptSession *ss, SculptFaceRef face, char flag);
int SCULPT_face_set_flag_set(SculptSession *ss, SculptFaceRef face, char flag, bool state);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Original Data API
 * \{ */

MSculptVert *SCULPT_vertex_get_sculptvert(const SculptSession *ss, SculptVertRef vertex);

/**
 * DEPRECATED: use SCULPT_vertex_check_origdata and SCULPT_vertex_get_sculptvert
 * Initialize a #SculptOrigVertData for accessing original vertex data;
 * handles #BMesh, #Mesh, and multi-resolution.
 */

void SCULPT_orig_vert_data_init(SculptOrigVertData *data,
                                Object *ob,
                                PBVHNode *node,
                                SculptUndoType type);

/**
 * DEPRECATED: use SCULPT_vertex_check_origdata and SCULPT_vertex_get_sculptvert
 * Update a #SculptOrigVertData for a particular vertex from the PBVH iterator.
 */
void SCULPT_orig_vert_data_update(SculptOrigVertData *orig_data, SculptVertRef vertex);

/**
 * DEPRECATED: use SCULPT_vertex_check_origdata and SCULPT_vertex_get_sculptvert
 * Initialize a #SculptOrigVertData for accessing original vertex data;
 * handles #BMesh, #Mesh, and multi-resolution.
 */
void SCULPT_orig_vert_data_unode_init(SculptOrigVertData *data,
                                      Object *ob,
                                      struct SculptUndoNode *unode);

void SCULPT_face_check_origdata(SculptSession *ss, SculptFaceRef face);
bool SCULPT_vertex_check_origdata(SculptSession *ss, SculptVertRef vertex);

/** check that original face set values are up to date
 * TODO: rename to SCULPT_face_set_original_ensure
 */
void SCULPT_face_ensure_original(SculptSession *ss, struct Object *ob);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Brush Utilities.
 * \{ */

BLI_INLINE bool SCULPT_tool_needs_all_pbvh_nodes(const Brush *brush)
{
  if (brush->sculpt_tool == SCULPT_TOOL_ELASTIC_DEFORM) {
    /* Elastic deformations in any brush need all nodes to avoid artifacts as the effect
     * of the Kelvinlet is not constrained by the radius. */
    return true;
  }

  if (brush->sculpt_tool == SCULPT_TOOL_POSE) {
    /* Pose needs all nodes because it applies all symmetry iterations at the same time
     * and the IK chain can grow to any area of the model. */
    /* TODO: This can be optimized by filtering the nodes after calculating the chain. */
    return true;
  }

  if (brush->sculpt_tool == SCULPT_TOOL_BOUNDARY) {
    /* Boundary needs all nodes because it is not possible to know where the boundary
     * deformation is going to be propagated before calculating it. */
    /* TODO: after calculating the boundary info in the first iteration, it should be
     * possible to get the nodes that have vertices included in any boundary deformation
     * and cache them. */
    return true;
  }

  if (brush->sculpt_tool == SCULPT_TOOL_SNAKE_HOOK &&
      brush->snake_hook_deform_type == BRUSH_SNAKE_HOOK_DEFORM_ELASTIC) {
    /* Snake hook in elastic deform type has same requirements as the elastic deform tool. */
    return true;
  }
  return false;
}

void SCULPT_calc_brush_plane(struct Sculpt *sd,
                             struct Object *ob,
                             struct PBVHNode **nodes,
                             int totnode,
                             float r_area_no[3],
                             float r_area_co[3]);

void SCULPT_calc_area_normal(
    Sculpt *sd, Object *ob, PBVHNode **nodes, int totnode, float r_area_no[3]);
/**
 * This calculates flatten center and area normal together,
 * amortizing the memory bandwidth and loop overhead to calculate both at the same time.
 */
void SCULPT_calc_area_normal_and_center(
    Sculpt *sd, Object *ob, PBVHNode **nodes, int totnode, float r_area_no[3], float r_area_co[3]);
void SCULPT_calc_area_center(
    Sculpt *sd, Object *ob, PBVHNode **nodes, int totnode, float r_area_co[3]);

SculptVertRef SCULPT_nearest_vertex_get(struct Sculpt *sd,
                                        struct Object *ob,
                                        const float co[3],
                                        float max_distance,
                                        bool use_original);

int SCULPT_plane_point_side(const float co[3], const float plane[4]);
int SCULPT_plane_trim(const struct StrokeCache *cache,
                      const struct Brush *brush,
                      const float val[3]);
/**
 * Handles clipping against a mirror modifier and #SCULPT_LOCK_X/Y/Z axis flags.
 */
void SCULPT_clip(Sculpt *sd, SculptSession *ss, float co[3], const float val[3]);

float SCULPT_brush_plane_offset_get(Sculpt *sd, SculptSession *ss);

ePaintSymmetryAreas SCULPT_get_vertex_symm_area(const float co[3]);
bool SCULPT_check_vertex_pivot_symmetry(const float vco[3], const float pco[3], char symm);
/**
 * Checks if a vertex is inside the brush radius from any of its mirrored axis.
 */
bool SCULPT_is_vertex_inside_brush_radius_symm(const float vertex[3],
                                               const float br_co[3],
                                               float radius,
                                               char symm);
bool SCULPT_is_symmetry_iteration_valid(char i, char symm);
void SCULPT_flip_v3_by_symm_area(float v[3],
                                 ePaintSymmetryFlags symm,
                                 ePaintSymmetryAreas symmarea,
                                 const float pivot[3]);
void SCULPT_flip_quat_by_symm_area(float quat[4],
                                   ePaintSymmetryFlags symm,
                                   ePaintSymmetryAreas symmarea,
                                   const float pivot[3]);

/**
 * Initialize a point-in-brush test
 */
void SCULPT_brush_test_init(struct SculptSession *ss, SculptBrushTest *test);

bool SCULPT_brush_test_sphere(SculptBrushTest *test, const float co[3]);
bool SCULPT_brush_test_sphere_sq(SculptBrushTest *test, const float co[3]);
bool SCULPT_brush_test_sphere_fast(const SculptBrushTest *test, const float co[3]);
bool SCULPT_brush_test_cube(SculptBrushTest *test,
                            const float co[3],
                            const float local[4][4],
                            float roundness);
bool SCULPT_brush_test_circle_sq(SculptBrushTest *test, const float co[3]);
/**
 * Test AABB against sphere.
 */
bool SCULPT_search_sphere_cb(PBVHNode *node, void *data_v);
/**
 * 2D projection (distance to line).
 */
bool SCULPT_search_circle_cb(PBVHNode *node, void *data_v);

/**
 * Initialize a point-in-brush test with a given falloff shape
 *
 * \param falloff_shape PAINT_FALLOFF_SHAPE_SPHERE or PAINT_FALLOFF_SHAPE_TUBE
 * \return The brush falloff function
 */
SculptBrushTestFn SCULPT_brush_test_init_with_falloff_shape(SculptSession *ss,
                                                            SculptBrushTest *test,
                                                            char falloff_shape);
const float *SCULPT_brush_frontface_normal_from_falloff_shape(SculptSession *ss,
                                                              char falloff_shape);

/**
 * Return a multiplier for brush strength on a particular vertex.
 */
float SCULPT_brush_strength_factor(struct SculptSession *ss,
                                   const struct Brush *br,
                                   const float point[3],
                                   float len,
                                   const float vno[3],
                                   const float fno[3],
                                   const float mask,
                                   const SculptVertRef vertex_index,
                                   const int thread_id);
/**
 * Returns a multiplier for brush strength on a particular vertex, and gets the albedo, emission,
 * roughness, and metallic maps from the brush texture nodes
 */

float SCULPT_brush_strength_factor_pbr_channels(struct SculptSession *ss,
                                                const struct Brush *br,
                                                const float point[3],
                                                float len,
                                                const float vno[3],
                                                const float fno[3],
                                                const float mask,
                                                const SculptVertRef vertex_index,
                                                const int thread_id,
                                                float rgba[4],
                                                float *emission,
                                                float *roughness,
                                                float *metallic);

/**
 * Tilts a normal by the x and y tilt values using the view axis.
 */
void SCULPT_tilt_apply_to_normal(float r_normal[3],
                                 struct StrokeCache *cache,
                                 float tilt_strength);

/**
 * Get effective surface normal with pen tilt and tilt strength applied to it.
 */
void SCULPT_tilt_effective_normal_get(const SculptSession *ss, const Brush *brush, float r_no[3]);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Flood Fill
 * \{ */

void SCULPT_floodfill_init(struct SculptSession *ss, SculptFloodFill *flood);
void SCULPT_floodfill_add_active(struct Sculpt *sd,
                                 struct Object *ob,
                                 struct SculptSession *ss,
                                 SculptFloodFill *flood,
                                 float radius);
void SCULPT_floodfill_add_initial_with_symmetry(struct Sculpt *sd,
                                                struct Object *ob,
                                                struct SculptSession *ss,
                                                SculptFloodFill *flood,
                                                SculptVertRef index,
                                                float radius);

void SCULPT_floodfill_add_initial(SculptFloodFill *flood, SculptVertRef index);
void SCULPT_floodfill_add_and_skip_initial(struct SculptSession *ss,
                                           SculptFloodFill *flood,
                                           SculptVertRef vertex);
void SCULPT_floodfill_execute(struct SculptSession *ss,
                              SculptFloodFill *flood,
                              bool (*func)(SculptSession *ss,
                                           SculptVertRef from_v,
                                           SculptVertRef to_v,
                                           bool is_duplicate,
                                           void *userdata),
                              void *userdata);
void SCULPT_floodfill_free(SculptFloodFill *flood);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Dynamic topology
 * \{ */

/** Enable dynamic topology; mesh will be triangulated */
void SCULPT_dynamic_topology_enable_ex(struct Main *bmain,
                                       struct Depsgraph *depsgraph,
                                       Scene *scene,
                                       Object *ob);
void SCULPT_dynamic_topology_disable(bContext *C, struct SculptUndoNode *unode);
void sculpt_dynamic_topology_disable_with_undo(struct Main *bmain,
                                               struct Depsgraph *depsgraph,
                                               Scene *scene,
                                               Object *ob);

/**
 * Returns true if the stroke will use dynamic topology, false
 * otherwise.
 *
 * Factors: some brushes like grab cannot do dynamic topology.
 * Others, like smooth, are better without.
 * Same goes for alt-key smoothing.
 */
bool SCULPT_stroke_is_dynamic_topology(const SculptSession *ss, const Brush *brush);

void SCULPT_dynamic_topology_triangulate(struct SculptSession *ss, struct BMesh *bm);
void SCULPT_dyntopo_node_layers_add(struct SculptSession *ss, Object *ob);
void SCULPT_dyntopo_save_origverts(struct SculptSession *ss);
void SCULPT_dyntopo_node_layers_update_offsets(SculptSession *ss, Object *ob);
void SCULPT_dynamic_topology_sync_layers(Object *ob, struct Mesh *me);

enum eDynTopoWarnFlag SCULPT_dynamic_topology_check(Scene *scene, Object *ob);

/** \} */

void SCULPT_combine_transform_proxies(Sculpt *sd, Object *ob);

/* -------------------------------------------------------------------- */
/** \name Auto-masking.
 * \{ */

float SCULPT_automasking_factor_get(struct AutomaskingCache *automasking,
                                    SculptSession *ss,
                                    SculptVertRef vert);
bool SCULPT_automasking_needs_normal(const SculptSession *ss, const Brush *brush);

/* Returns the automasking cache depending on the active tool. Used for code that can run both for
 * brushes and filter. */
struct AutomaskingCache *SCULPT_automasking_active_cache_get(SculptSession *ss);

struct AutomaskingCache *SCULPT_automasking_cache_init(Sculpt *sd, const Brush *brush, Object *ob);
void SCULPT_automasking_cache_free(SculptSession *ss,
                                   Object *ob,
                                   struct AutomaskingCache *automasking);

bool SCULPT_is_automasking_mode_enabled(const SculptSession *ss,
                                        const Sculpt *sd,
                                        const Brush *br,
                                        const eAutomasking_flag mode);
bool SCULPT_is_automasking_enabled(Sculpt *sd, const SculptSession *ss, const Brush *br);
void SCULPT_automasking_step_update(struct AutomaskingCache *automasking,
                                    SculptSession *ss,
                                    Sculpt *sd,
                                    const Brush *brush);

void SCULPT_boundary_automasking_init(Object *ob,
                                      eBoundaryAutomaskMode mode,
                                      int propagation_steps,
                                      SculptCustomLayer *factorlayer);
/** \} */

/* -------------------------------------------------------------------- */
/** \name Geodesic distances.
 * \{ */

/**
 * Returns an array indexed by vertex index containing the geodesic distance to the closest vertex
 * in the initial vertex set. The caller is responsible for freeing the array.
 * Geodesic distances will only work when used with PBVH_FACES, for other types of PBVH it will
 * fallback to euclidean distances to one of the initial vertices in the set.
 */
float *SCULPT_geodesic_distances_create(struct Object *ob,
                                        struct GSet *initial_vertices,
                                        const float limit_radius,
                                        SculptVertRef *r_closest_verts,
                                        float (*vertco_override)[3]);
float *SCULPT_geodesic_from_vertex_and_symm(struct Sculpt *sd,
                                            struct Object *ob,
                                            const SculptVertRef vertex,
                                            const float limit_radius);
float *SCULPT_geodesic_from_vertex(Object *ob,
                                   const SculptVertRef vertex,
                                   const float limit_radius);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Filter API
 * \{ */

void SCULPT_filter_cache_init(struct bContext *C,
                              struct Object *ob,
                              Sculpt *sd,
                              const int undo_type);
void SCULPT_filter_cache_free(SculptSession *ss, struct Object *ob);

void SCULPT_mask_filter_smooth_apply(
    Sculpt *sd, Object *ob, PBVHNode **nodes, int totnode, int smooth_iterations);

/* Filter orientation utils. */
void SCULPT_filter_to_orientation_space(float r_v[3], struct FilterCache *filter_cache);
void SCULPT_filter_to_object_space(float r_v[3], struct FilterCache *filter_cache);
void SCULPT_filter_zero_disabled_axis_components(float r_v[3], struct FilterCache *filter_cache);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cloth Simulation.
 * \{ */

/* Main cloth brush function */
void SCULPT_do_cloth_brush(struct Sculpt *sd,
                           struct Object *ob,
                           struct PBVHNode **nodes,
                           int totnode);

void SCULPT_cloth_simulation_free(struct SculptClothSimulation *cloth_sim);

/* Public functions. */

struct SculptClothSimulation *SCULPT_cloth_brush_simulation_create(
    struct SculptSession *ss,
    struct Object *ob,
    const float cloth_mass,
    const float cloth_damping,
    const float cloth_softbody_strength,
    const bool use_collisions,
    const bool needs_deform_coords,
    const bool use_bending);

void SCULPT_cloth_brush_simulation_init(struct SculptSession *ss,
                                        struct SculptClothSimulation *cloth_sim);

void SCULPT_cloth_sim_activate_nodes(struct SculptClothSimulation *cloth_sim,
                                     PBVHNode **nodes,
                                     int totnode);

void SCULPT_cloth_brush_store_simulation_state(struct SculptSession *ss,
                                               struct SculptClothSimulation *cloth_sim);

void SCULPT_cloth_brush_do_simulation_step(struct Sculpt *sd,
                                           struct Object *ob,
                                           struct SculptClothSimulation *cloth_sim,
                                           struct PBVHNode **nodes,
                                           int totnode);

void SCULPT_cloth_brush_ensure_nodes_constraints(struct Sculpt *sd,
                                                 struct Object *ob,
                                                 struct PBVHNode **nodes,
                                                 int totnode,
                                                 struct SculptClothSimulation *cloth_sim,
                                                 float initial_location[3],
                                                 float radius);

/**
 * Cursor drawing function.
 */
void SCULPT_cloth_simulation_limits_draw(const SculptSession *ss,
                                         const Sculpt *sd,
                                         const uint gpuattr,
                                         const struct Brush *brush,
                                         const float location[3],
                                         const float normal[3],
                                         float rds,
                                         float line_width,
                                         const float outline_col[3],
                                         float alpha);
void SCULPT_cloth_plane_falloff_preview_draw(uint gpuattr,
                                             struct SculptSession *ss,
                                             const float outline_col[3],
                                             float outline_alpha);

PBVHNode **SCULPT_cloth_brush_affected_nodes_gather(SculptSession *ss,
                                                    Brush *brush,
                                                    int *r_totnode);

BLI_INLINE bool SCULPT_is_cloth_deform_brush(const Brush *brush)
{
  return (brush->sculpt_tool == SCULPT_TOOL_CLOTH && ELEM(brush->cloth_deform_type,
                                                          BRUSH_CLOTH_DEFORM_GRAB,
                                                          BRUSH_CLOTH_DEFORM_SNAKE_HOOK)) ||
         /* All brushes that are not the cloth brush deform the simulation using softbody
          * constraints instead of applying forces. */
         (brush->sculpt_tool != SCULPT_TOOL_CLOTH &&
          brush->deform_target == BRUSH_DEFORM_TARGET_CLOTH_SIM);
}
/** \} */

/* -------------------------------------------------------------------- */
/** \name Smoothing API
 * \{ */

/**
 * For bmesh: Average surrounding verts based on an orthogonality measure.
 * Naturally converges to a quad-like structure.
 */
void SCULPT_bmesh_four_neighbor_average(SculptSession *ss,
                                        float avg[3],
                                        float direction[3],
                                        struct BMVert *v,
                                        float projection,
                                        bool check_fsets,
                                        int cd_temp,
                                        int cd_sculpt_vert,
                                        bool do_origco);

void SCULPT_neighbor_coords_average(SculptSession *ss,
                                    float result[3],
                                    SculptVertRef index,
                                    float projection,
                                    bool check_fsets,
                                    bool weighted);
float SCULPT_neighbor_mask_average(SculptSession *ss, SculptVertRef index);
void SCULPT_neighbor_color_average(SculptSession *ss, float result[4], SculptVertRef index);

/* Mask the mesh boundaries smoothing only the mesh surface without using automasking. */

void SCULPT_neighbor_coords_average_interior(SculptSession *ss,
                                             float result[3],
                                             SculptVertRef vertex,
                                             SculptSmoothArgs *args);

BLI_INLINE bool SCULPT_need_reproject(SculptSession *ss)
{
  return ss->bm && CustomData_has_layer(&ss->bm->ldata, CD_MLOOPUV);
}

void SCULPT_reproject_cdata(SculptSession *ss,
                            SculptVertRef vertex,
                            float origco[3],
                            float origno[3]);

void SCULPT_smooth_vcol_boundary(
    Sculpt *sd, Object *ob, PBVHNode **nodes, const int totnode, float bstrength);
void SCULPT_smooth(Sculpt *sd,
                   Object *ob,
                   PBVHNode **nodes,
                   const int totnode,
                   float bstrength,
                   const bool smooth_mask,
                   float projection,
                   bool do_origco);
void SCULPT_do_smooth_brush(
    Sculpt *sd, Object *ob, PBVHNode **nodes, int totnode, float projection, bool do_origco);

/* Surface Smooth Brush. */

void SCULPT_surface_smooth_laplacian_step(SculptSession *ss,
                                          float *disp,
                                          const float co[3],
                                          struct SculptCustomLayer *scl,
                                          const SculptVertRef v_index,
                                          const float origco[3],
                                          const float alpha,
                                          const float projection,
                                          bool check_fsets,
                                          bool weighted);

void SCULPT_surface_smooth_displace_step(SculptSession *ss,
                                         float *co,
                                         struct SculptCustomLayer *scl,
                                         const SculptVertRef v_index,
                                         const float beta,
                                         const float fade);

void SCULPT_do_surface_smooth_brush(Sculpt *sd, Object *ob, PBVHNode **nodes, int totnode);

/* Slide/Relax */

void SCULPT_relax_vertex(SculptSession *ss,
                         PBVHVertexIter *vd,
                         float factor,
                         SculptBoundaryType boundary_mask,
                         float *r_final_pos);

/** \} */

/**
 * Expose 'calc_area_normal' externally (just for vertex paint).
 */
bool SCULPT_pbvh_calc_area_normal(const struct Brush *brush,
                                  Object *ob,
                                  PBVHNode **nodes,
                                  int totnode,
                                  bool use_threading,
                                  float r_area_no[3]);

/**
 * Flip all the edit-data across the axis/axes specified by \a symm.
 * Used to calculate multiple modifications to the mesh when symmetry is enabled.
 */
void SCULPT_cache_calc_brushdata_symm(StrokeCache *cache,
                                      const char symm,
                                      const char axis,
                                      const float angle);
void SCULPT_cache_free(SculptSession *ss, struct Object *ob, StrokeCache *cache);

/* -------------------------------------------------------------------- */
/** \name Sculpt Undo
 * \{ */

/* -------------------------------------------------------------------- */
/** \name Sculpt Undo
 * \{ */

SculptUndoNode *SCULPT_undo_push_node(Object *ob, PBVHNode *node, SculptUndoType type);
SculptUndoNode *SCULPT_undo_get_node(PBVHNode *node, SculptUndoType type);
SculptUndoNode *SCULPT_undo_get_first_node(void);
void SCULPT_undo_push_begin(struct Object *ob, const char *name);
void SCULPT_undo_push_end(struct Object *ob);
void SCULPT_undo_push_end_ex(struct Object *ob, const bool use_nested_undo);

/** \} */

/** \} */

void SCULPT_vertcos_to_key(Object *ob, KeyBlock *kb, const float (*vertCos)[3]);

/**
 * Copy the PBVH bounding box into the object's bounding box.
 */
void SCULPT_update_object_bounding_box(struct Object *ob);

/**
 * Get a screen-space rectangle of the modified area.
 */
bool SCULPT_get_redraw_rect(struct ARegion *region,
                            struct RegionView3D *rv3d,
                            Object *ob,
                            rcti *rect);

/* Operators. */

/* -------------------------------------------------------------------- */
/** \name Expand Operator
 * \{ */

void SCULPT_OT_expand(struct wmOperatorType *ot);
void sculpt_expand_modal_keymap(struct wmKeyConfig *keyconf);
/** \} */

/* -------------------------------------------------------------------- */
/** \name Gesture Operators
 * \{ */

void SCULPT_OT_face_set_lasso_gesture(struct wmOperatorType *ot);
void SCULPT_OT_face_set_box_gesture(struct wmOperatorType *ot);

void SCULPT_OT_trim_lasso_gesture(struct wmOperatorType *ot);
void SCULPT_OT_trim_box_gesture(struct wmOperatorType *ot);

void SCULPT_OT_project_line_gesture(struct wmOperatorType *ot);
void SCULPT_OT_project_lasso_gesture(struct wmOperatorType *ot);
void SCULPT_OT_project_box_gesture(struct wmOperatorType *ot);

void SCULPT_OT_face_set_by_topology(struct wmOperatorType *ot);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Face Set Operators
 * \{ */

void SCULPT_OT_face_sets_randomize_colors(struct wmOperatorType *ot);
void SCULPT_OT_face_sets_change_visibility(struct wmOperatorType *ot);
void SCULPT_OT_face_sets_init(struct wmOperatorType *ot);
void SCULPT_OT_face_sets_create(struct wmOperatorType *ot);
void SCULPT_OT_face_sets_edit(struct wmOperatorType *ot);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Transform Operators
 * \{ */

void SCULPT_OT_set_pivot_position(struct wmOperatorType *ot);
/** \} */

/* -------------------------------------------------------------------- */
/** \name Filter Operators
 * \{ */

/* Mesh Filter. */

void SCULPT_OT_mesh_filter(struct wmOperatorType *ot);

/* Cloth Filter. */

void SCULPT_OT_cloth_filter(struct wmOperatorType *ot);

/* Color Filter. */

void SCULPT_OT_color_filter(struct wmOperatorType *ot);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mask Operators
 * \{ */

/* Mask filter and Dirty Mask. */

void SCULPT_OT_mask_filter(struct wmOperatorType *ot);
void SCULPT_OT_dirty_mask(struct wmOperatorType *ot);

/* Mask and Face Sets Expand. */

void SCULPT_OT_mask_expand(struct wmOperatorType *ot);

/* Mask Init. */

void SCULPT_OT_mask_init(struct wmOperatorType *ot);
void SCULPT_OT_ipmask_filter(struct wmOperatorType *ot);

/** \} */

/* Detail size. */

/* -------------------------------------------------------------------- */
/** \name Dyntopo/Retopology Operators
 * \{ */

void SCULPT_OT_detail_flood_fill(struct wmOperatorType *ot);
void SCULPT_OT_sample_detail_size(struct wmOperatorType *ot);
void SCULPT_OT_set_detail_size(struct wmOperatorType *ot);
void SCULPT_OT_dyntopo_detail_size_edit(struct wmOperatorType *ot);
/** \} */

/* Dyntopo. */

void SCULPT_OT_dynamic_topology_toggle(struct wmOperatorType *ot);

/* sculpt_brush_types.c */

/* -------------------------------------------------------------------- */
/** \name Brushes
 * \{ */

/* Pose Brush. */

/**
 * Main Brush Function.
 */
void SCULPT_do_pose_brush(struct Sculpt *sd,
                          struct Object *ob,
                          struct PBVHNode **nodes,
                          int totnode);
/**
 * Calculate the pose origin and (Optionally the pose factor)
 * that is used when using the pose brush.
 *
 * \param r_pose_origin: Must be a valid pointer.
 * \param r_pose_factor: Optional, when set to NULL it won't be calculated.
 */
void SCULPT_pose_calc_pose_data(struct Sculpt *sd,
                                struct Object *ob,
                                struct SculptSession *ss,
                                float initial_location[3],
                                float radius,
                                float pose_offset,
                                float *r_pose_origin,
                                float *r_pose_factor);
void SCULPT_pose_brush_init(struct Sculpt *sd,
                            struct Object *ob,
                            struct SculptSession *ss,
                            struct Brush *br);
struct SculptPoseIKChain *SCULPT_pose_ik_chain_init(struct Sculpt *sd,
                                                    struct Object *ob,
                                                    struct SculptSession *ss,
                                                    struct Brush *br,
                                                    const float initial_location[3],
                                                    float radius);
void SCULPT_pose_ik_chain_free(struct SculptPoseIKChain *ik_chain);

/* Boundary Brush. */

/**
 * Main function to get #SculptBoundary data both for brush deformation and viewport preview.
 * Can return NULL if there is no boundary from the given vertex using the given radius.
 */
struct SculptBoundary *SCULPT_boundary_data_init(struct Sculpt *sd,
                                                 Object *object,
                                                 Brush *brush,
                                                 const SculptVertRef initial_vertex,
                                                 const float radius);
void SCULPT_boundary_data_free(struct SculptBoundary *boundary);

/* Main Brush Function. */
void SCULPT_do_boundary_brush(struct Sculpt *sd,
                              struct Object *ob,
                              struct PBVHNode **nodes,
                              int totnode);

void SCULPT_boundary_edges_preview_draw(uint gpuattr,
                                        struct SculptSession *ss,
                                        const float outline_col[3],
                                        float outline_alpha);
void SCULPT_boundary_pivot_line_preview_draw(uint gpuattr, struct SculptSession *ss);

void SCULPT_do_twist_brush(struct Sculpt *sd,
                           struct Object *ob,
                           struct PBVHNode **nodes,
                           int totnode);
void SCULPT_do_fill_brush(struct Sculpt *sd,
                          struct Object *ob,
                          struct PBVHNode **nodes,
                          int totnode);
void SCULPT_do_scrape_brush(struct Sculpt *sd,
                            struct Object *ob,
                            struct PBVHNode **nodes,
                            int totnode);
void SCULPT_do_clay_thumb_brush(struct Sculpt *sd,
                                struct Object *ob,
                                struct PBVHNode **nodes,
                                int totnode);
void SCULPT_do_flatten_brush(struct Sculpt *sd,
                             struct Object *ob,
                             struct PBVHNode **nodes,
                             int totnode);
void SCULPT_do_clay_brush(struct Sculpt *sd,
                          struct Object *ob,
                          struct PBVHNode **nodes,
                          int totnode);
void SCULPT_do_clay_strips_brush(struct Sculpt *sd,
                                 struct Object *ob,
                                 struct PBVHNode **nodes,
                                 int totnode);
void SCULPT_do_snake_hook_brush(struct Sculpt *sd,
                                struct Object *ob,
                                struct PBVHNode **nodes,
                                int totnode);
void SCULPT_do_thumb_brush(struct Sculpt *sd,
                           struct Object *ob,
                           struct PBVHNode **nodes,
                           int totnode);
void SCULPT_do_rotate_brush(struct Sculpt *sd,
                            struct Object *ob,
                            struct PBVHNode **nodes,
                            int totnode);
void SCULPT_do_layer_brush(struct Sculpt *sd,
                           struct Object *ob,
                           struct PBVHNode **nodes,
                           int totnode);
void SCULPT_do_inflate_brush(struct Sculpt *sd,
                             struct Object *ob,
                             struct PBVHNode **nodes,
                             int totnode);
void SCULPT_do_nudge_brush(struct Sculpt *sd,
                           struct Object *ob,
                           struct PBVHNode **nodes,
                           int totnode);
void SCULPT_do_crease_brush(struct Sculpt *sd,
                            struct Object *ob,
                            struct PBVHNode **nodes,
                            int totnode);
void SCULPT_do_pinch_brush(struct Sculpt *sd,
                           struct Object *ob,
                           struct PBVHNode **nodes,
                           int totnode);
void SCULPT_do_grab_brush(struct Sculpt *sd,
                          struct Object *ob,
                          struct PBVHNode **nodes,
                          int totnode);
void SCULPT_do_elastic_deform_brush(struct Sculpt *sd,
                                    struct Object *ob,
                                    struct PBVHNode **nodes,
                                    int totnode);
void SCULPT_do_draw_sharp_brush(struct Sculpt *sd,
                                struct Object *ob,
                                struct PBVHNode **nodes,
                                int totnode);
void SCULPT_do_scene_project_brush(struct Sculpt *sd,
                                   struct Object *ob,
                                   struct PBVHNode **nodes,
                                   int totnode);
void SCULPT_do_slide_brush(struct Sculpt *sd,
                           struct Object *ob,
                           struct PBVHNode **nodes,
                           int totnode);
void SCULPT_do_relax_brush(Sculpt *sd, Object *ob, PBVHNode **nodes, int totnode);
void SCULPT_do_fairing_brush(struct Sculpt *sd,
                             struct Object *ob,
                             struct PBVHNode **nodes,
                             int totnode);
void SCULPT_do_displacement_smear_brush(struct Sculpt *sd,
                                        struct Object *ob,
                                        struct PBVHNode **nodes,
                                        int totnode);
void SCULPT_do_displacement_eraser_brush(struct Sculpt *sd,
                                         struct Object *ob,
                                         struct PBVHNode **nodes,
                                         int totnode);
void SCULPT_do_pbr_brush(struct Sculpt *sd,
                         struct Object *ob,
                         struct PBVHNode **nodes,
                         int totnode);
void SCULPT_do_draw_brush(struct Sculpt *sd,
                          struct Object *ob,
                          struct PBVHNode **nodes,
                          int totnode);
void SCULPT_do_mask_brush_draw(struct Sculpt *sd,
                               struct Object *ob,
                               struct PBVHNode **nodes,
                               int totnode);
void SCULPT_do_mask_brush(struct Sculpt *sd,
                          struct Object *ob,
                          struct PBVHNode **nodes,
                          int totnode);
void SCULPT_bmesh_topology_rake(struct Sculpt *sd,
                                struct Object *ob,
                                struct PBVHNode **nodes,
                                const int totnode,
                                float bstrength,
                                bool needs_origco);

void SCULPT_stroke_cache_snap_context_init(struct bContext *C, struct Object *ob);
void SCULPT_fairing_brush_exec_fairing_for_cache(struct Sculpt *sd, struct Object *ob);
void SCULPT_do_auto_face_set(Sculpt *sd, Object *ob, PBVHNode **nodes, int totnode);

void do_draw_face_sets_brush_task_cb_ex(void *__restrict userdata,
                                        const int n,
                                        const struct TaskParallelTLS *__restrict tls);

void SCULPT_enhance_details_brush(struct Sculpt *sd,
                                  struct Object *ob,
                                  struct PBVHNode **nodes,
                                  const int totnode,
                                  int presteps);
void SCULPT_do_displacement_heal_brush(struct Sculpt *sd,
                                       struct Object *ob,
                                       struct PBVHNode **nodes,
                                       int totnode);

void SCULPT_do_array_brush(Sculpt *sd, Object *ob, PBVHNode **nodes, int totnode);
void SCULPT_array_datalayers_free(SculptArray *array, Object *ob);
void SCULPT_array_path_draw(const uint gpuattr, Brush *brush, SculptSession *ss);

/* Directional Smooth Brush. */
void SCULPT_do_directional_smooth_brush(Sculpt *sd, Object *ob, PBVHNode **nodes, int totnode);

/* Uniform Weights Smooth Brush. */
void SCULPT_do_uniform_weights_smooth_brush(Sculpt *sd, Object *ob, PBVHNode **nodes, int totnode);

void SCULPT_do_draw_face_sets_brush(Sculpt *sd, Object *ob, PBVHNode **nodes, int totnode);
void SCULPT_do_paint_brush(Sculpt *sd, Object *ob, PBVHNode **nodes, int totnode);
void SCULPT_do_smear_brush(Sculpt *sd, Object *ob, PBVHNode **nodes, int totnode);

void SCULPT_do_multiplane_scrape_brush(Sculpt *sd, Object *ob, PBVHNode **nodes, int totnode);
void SCULPT_multiplane_scrape_preview_draw(uint gpuattr,
                                           Brush *brush,
                                           SculptSession *ss,
                                           const float outline_col[3],
                                           float outline_alpha);
/* Symmetrize Map. */
void SCULPT_do_symmetrize_brush(Sculpt *sd, Object *ob, PBVHNode **nodes, int totnode);

void SCULPT_uv_brush(Sculpt *sd, Object *ob, PBVHNode **nodes, int totnode);

/** \} */

/* end sculpt_brush_types.c */

/* sculpt_ops.c */
void SCULPT_OT_brush_stroke(struct wmOperatorType *ot);

/* end sculpt_ops.c */

SculptBoundaryType SCULPT_edge_is_boundary(const SculptSession *ss,
                                           const SculptEdgeRef edge,
                                           SculptBoundaryType typemask);
void SCULPT_edge_get_verts(const SculptSession *ss,
                           const SculptEdgeRef edge,
                           SculptVertRef *r_v1,
                           SculptVertRef *r_v2);
SculptVertRef SCULPT_edge_other_vertex(const SculptSession *ss,
                                       const SculptEdgeRef edge,
                                       const SculptVertRef vertex);

#define SCULPT_REPLAY
#ifdef SCULPT_REPLAY
struct SculptReplayLog;
struct SculptBrushSample;

#  ifdef WIN32
#    define REPLAY_EXPORT __declspec(dllexport)
#  else
#    define REPLAY_EXPORT
#  endif

void SCULPT_replay_log_free(struct SculptReplayLog *log);
struct SculptReplayLog *SCULPT_replay_log_create();
void SCULPT_replay_log_end();
void SCULPT_replay_log_start();
char *SCULPT_replay_serialize();
void SCULPT_replay_log_append(struct Sculpt *sd, struct SculptSession *ss, struct Object *ob);
void SCULPT_replay_test(void);

#endif

void SCULPT_undo_ensure_bmlog(struct Object *ob);
bool SCULPT_ensure_dyntopo_node_undo(struct Object *ob,
                                     struct PBVHNode *node,
                                     SculptUndoType type,
                                     int extraType);

struct BMesh *SCULPT_dyntopo_empty_bmesh();

/* initializes customdata layer used by SCULPT_neighbor_coords_average_interior when bound_smooth >
 * 0.0f*/
void SCULPT_bound_smooth_ensure(SculptSession *ss, struct Object *ob);

/* -------------------------------------------------------------------- */
/** \name Sculpt Custom Attribute API
 *
API for custom (usually temporary) attributes.
Vertices and faces are supported.

Note that attributes must be created at once,
*then* loaded into SculptCustomLayer structures, e.g.:

    SCULPT_attr_ensure_layer(ss, ob, ATTR_DOMAIN_POINT, CD_PROP_FLOAT, "attr1", params);
    SCULPT_attr_ensure_layer(ss, ob, ATTR_DOMAIN_POINT, CD_PROP_FLOAT, "attr2", params);
    SCULPT_attr_ensure_layer(ss, ob, ATTR_DOMAIN_POINT, CD_PROP_FLOAT, "attr3", params);

    SculptCustomLayer scl1, scl2, scl3;

    SCULPT_attr_get_layer(ss, ob, ATTR_DOMAIN_POINT, CD_PROP_FLOAT, "attr1", &scl1, params);
    SCULPT_attr_get_layer(ss, ob, ATTR_DOMAIN_POINT, CD_PROP_FLOAT, "attr2", &scl2, params);
    SCULPT_attr_get_layer(ss, ob, ATTR_DOMAIN_POINT, CD_PROP_FLOAT, "attr3", &scl3, params);

Access per element data with SCULPT_attr_vertex_data and SCULPT_attr_face_data:.

    float *f = SCULPT_attr_vertex_data(sculpt_vertex, &scl1);

Layers that are reused by different parts of the sculpt code
should be cached in SculptSession->custom_layers, the entries of which
map to the SculptStandardAttr enum in BKE_paint.h.

These layers will not be created for you, though once they exist they won't
be pruned until the user exits sculpt mode.  To use:

    if (!ss->custom_layers[SCULPT_SCL_FAIRING_MASK]) {
      ss->custom_layers[SCULPT_SCL_FAIRING_MASK] = MEM_callocN(sizeof(SculptLayerRef),
"SculptLayerRef"); SCULPT_attr_get_layer(ss, ob, ATTR_DOMAIN_POINT, CD_PROP_FLOAT,
"_sculpt_fairing_mask", ss->custom_layers[SCULPT_SCL_FAIRING_MASK], params);
    }

Note that SCULPT_attr_get_layer will automatically update the customdata
offsets for all non-NULL entries in ss->custom_layers automatically.

TODO: prune CD_TEMPORARY layers on sculpt mode exit if PBVH_FACES is active;
      in this case the layers are stored in the original meshes's CustomData
      structs and will remain there until the user saves/loads the file.
 * \{ */

/** returns true if layer was successfully created */
bool SCULPT_attr_ensure_layer(SculptSession *ss,
                              Object *ob,
                              AttributeDomain domain,
                              int proptype,
                              const char *name,
                              SculptLayerParams *params);
bool SCULPT_attr_get_layer(SculptSession *ss,
                           Object *ob,
                           AttributeDomain domain,
                           int proptype,
                           const char *name,
                           SculptCustomLayer *scl,
                           SculptLayerParams *params);
bool SCULPT_attr_release_layer(SculptSession *ss, struct Object *ob, SculptCustomLayer *scl);
bool SCULPT_attr_has_layer(SculptSession *ss,
                           AttributeDomain domain,
                           int proptype,
                           const char *name);

/** updates all entries (e.g. customdata offsets) in ss->custom_layers */
void SCULPT_update_customdata_refs(SculptSession *ss, Object *ob);

/** Calls MEM_freeN on all entries in ss->custom_layers then
 * sets the whole array to nullptrs; note that no customdata
 * layer is actually freed */
void SCULPT_clear_scl_pointers(SculptSession *ss);

static inline void *SCULPT_attr_vertex_data(const SculptVertRef vertex,
                                            const SculptCustomLayer *scl)
{
  if (scl->data) {
    char *p = (char *)scl->data;
    int idx = (int)vertex.i;

    if (scl->from_bmesh) {
      BMElem *v = (BMElem *)vertex.i;
      idx = v->head.index;
    }

    return p + scl->elemsize * (int)idx;
  }
  else {
    BMElem *v = (BMElem *)vertex.i;
    return BM_ELEM_CD_GET_VOID_P(v, scl->cd_offset);
  }

  return NULL;
}

// arg, duplicate functions!
static inline void *SCULPT_attr_face_data(const SculptFaceRef vertex, const SculptCustomLayer *scl)
{
  if (scl->data) {
    char *p = (char *)scl->data;
    int idx = (int)vertex.i;

    if (scl->from_bmesh) {
      BMElem *v = (BMElem *)vertex.i;
      idx = v->head.index;
    }

    return p + scl->elemsize * (int)idx;
  }
  else {
    BMElem *v = (BMElem *)vertex.i;
    return BM_ELEM_CD_GET_VOID_P(v, scl->cd_offset);
  }

  return NULL;
}

void SCULPT_release_attributes(SculptSession *ss, struct Object *ob, bool non_customdata_only);
/** \} */

/*

DEPRECATED in favor of SCULPT_attr_ensure_layer
which works with all three PBVH types

Ensure a named temporary layer exists, creating it if necassary.
The layer will be marked with CD_FLAG_TEMPORARY.
*/
void SCULPT_dyntopo_ensure_templayer(
    SculptSession *ss, struct Object *ob, int type, const char *name, bool not_temporary);

bool SCULPT_dyntopo_has_templayer(SculptSession *ss, int type, const char *name);

/* Get a named temporary vertex customdata layer offset, if it exists.  If not
  -1 is returned.*/
int SCULPT_dyntopo_get_templayer(SculptSession *ss, int type, const char *name);

int SCULPT_get_tool(const SculptSession *ss, const struct Brush *br);

/* Sculpt API to get brush channel data
If ss->cache exists then ss->cache->channels_final
will be used, otherwise brush and tool settings channels
will be used (taking inheritence into account).
*/

/* -------------------------------------------------------------------- */
/** \name Brush channel accessor API
 * \{ */

/** Get brush channel value.  The channel will be
    fetched from ss->cache->channels_final.  If
    ss->cache is NULL, channel will be fetched
    from sd->channels and br->channels taking
    inheritance flags into account.

    Note that sd or br may be NULL, but not
    both.*/
float SCULPT_get_float_intern(const SculptSession *ss,
                              const char *idname,
                              const Sculpt *sd,
                              const Brush *br);
#define SCULPT_get_float(ss, idname, sd, br) \
  SCULPT_get_float_intern(ss, BRUSH_BUILTIN_##idname, sd, br)

int SCULPT_get_int_intern(const SculptSession *ss,
                          const char *idname,
                          const Sculpt *sd,
                          const Brush *br);
#define SCULPT_get_int(ss, idname, sd, br) \
  SCULPT_get_int_intern(ss, BRUSH_BUILTIN_##idname, sd, br)
#define SCULPT_get_bool(ss, idname, sd, br) SCULPT_get_int(ss, idname, sd, br)

int SCULPT_get_vector_intern(
    const SculptSession *ss, const char *idname, float out[4], const Sculpt *sd, const Brush *br);
#define SCULPT_get_vector(ss, idname, out, sd, br) \
  SCULPT_get_vector_intern(ss, BRUSH_BUILTIN_##idname, out, sd, br)

BrushChannel *SCULPT_get_final_channel_intern(const SculptSession *ss,
                                              const char *idname,
                                              const Sculpt *sd,
                                              const Brush *br);
#define SCULPT_get_final_channel(ss, idname, sd, br) \
  SCULPT_get_final_channel_intern(ss, BRUSH_BUILTIN_##idname, sd, br)

/** \} */

float SCULPT_calc_concavity(SculptSession *ss, SculptVertRef vref);

/*
If useAccurateSolver is false, a faster but less accurate
power solver will be used.  If true then BLI_eigen_solve_selfadjoint_m3
will be called.
*/
bool SCULPT_calc_principle_curvatures(SculptSession *ss,
                                      SculptVertRef vertex,
                                      SculptCurvatureData *out,
                                      bool useAccurateSolver);

void SCULPT_curvature_begin(SculptSession *ss, struct PBVHNode *node, bool useAccurateSolver);
void SCULPT_curvature_dir_get(SculptSession *ss,
                              SculptVertRef v,
                              float dir[3],
                              bool useAccurateSolver);

/* -------------------------------------------------------------------- */
/** \name Cotangent API
 * \{ */

bool SCULPT_dyntopo_check_disk_sort(SculptSession *ss, SculptVertRef vertex);
void SCULT_dyntopo_flag_all_disk_sort(SculptSession *ss);

// call SCULPT_cotangents_begin in the main thread before any calls to this function
void SCULPT_dyntopo_get_cotangents(SculptSession *ss,
                                   SculptVertRef vertex,
                                   float *r_ws,
                                   float *r_cot1,
                                   float *r_cot2,
                                   float *r_area,
                                   float *r_totarea);

/** call SCULPT_cotangents_begin in the main thread before any calls to this function */
void SCULPT_get_cotangents(SculptSession *ss,
                           SculptVertRef vertex,
                           float *r_ws,
                           float *r_cot1,
                           float *r_cot2,
                           float *r_area,
                           float *r_totarea);

/** call this in the main thread before any calls to SCULPT_get_cotangents */
void SCULPT_cotangents_begin(struct Object *ob, SculptSession *ss);
/** \} */

void SCULPT_ensure_persistent_layers(SculptSession *ss, struct Object *ob);
void SCULPT_ensure_epmap(SculptSession *ss);
bool SCULPT_dyntopo_automasking_init(const SculptSession *ss,
                                     Sculpt *sd,
                                     const Brush *br,
                                     Object *ob,
                                     DyntopoMaskCB *r_mask_cb,
                                     void **r_mask_cb_data);
void SCULPT_dyntopo_automasking_end(void *mask_data);

#define SCULPT_LAYER_PERS_CO "Persistent Base Co"
#define SCULPT_LAYER_PERS_NO "Persistent Base No"
#define SCULPT_LAYER_PERS_DISP "Persistent Base Height"
#define SCULPT_LAYER_DISP "__temp_layer_disp"

// these tools don't support dynamic pbvh splitting during the stroke
#define DYNTOPO_HAS_DYNAMIC_SPLIT(tool) true

#define SCULPT_TOOL_NEEDS_COLOR(tool) \
  ELEM(tool, SCULPT_TOOL_PBR, SCULPT_TOOL_PAINT, SCULPT_TOOL_SMEAR)

#define SCULPT_stroke_needs_original(brush) \
  ELEM(brush->sculpt_tool, \
       SCULPT_TOOL_DRAW_SHARP, \
       SCULPT_TOOL_GRAB, \
       SCULPT_TOOL_ROTATE, \
       SCULPT_TOOL_THUMB, \
       SCULPT_TOOL_ELASTIC_DEFORM, \
       SCULPT_TOOL_BOUNDARY, \
       SCULPT_TOOL_POSE)

// exponent to make boundary_smooth_factor more user-friendly
#define BOUNDARY_SMOOTH_EXP 2.0
