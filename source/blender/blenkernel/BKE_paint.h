/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2009 by Nicholas Bishop. All rights reserved. */

#pragma once

/** \file
 * \ingroup bke
 */

#include "BKE_attribute.h"
#include "BKE_brush_engine.h"
#include "BKE_pbvh.h"

#include "BLI_bitmap.h"
#include "BLI_utildefines.h"
#include "DNA_brush_enums.h"
#include "DNA_customdata_types.h"
#include "DNA_object_enums.h"

#ifdef __cplusplus
extern "C" {
#endif

struct SculptCustomLayer;
struct MSculptVert;
struct BMFace;
struct BMesh;
struct BlendDataReader;
struct BlendLibReader;
struct BlendWriter;
struct Brush;
struct CurveMapping;
struct Depsgraph;
struct EdgeSet;
struct EnumPropertyItem;
struct GHash;
struct GridPaintMask;
struct ImagePool;
struct ListBase;
struct MLoop;
struct MLoopTri;
struct MVert;
struct Main;
struct Mesh;
struct MeshElemMap;
struct Object;
struct PBVH;
struct Paint;
struct PaintCurve;
struct Palette;
struct PaletteColor;
struct Scene;
struct StrokeCache;
struct SubdivCCG;
struct Tex;
struct SculptCustomLayer;
struct ToolSettings;
struct UnifiedPaintSettings;
struct View3D;
struct ViewLayer;
struct bContext;
struct bToolRef;
struct tPaletteColorHSV;

extern const char PAINT_CURSOR_SCULPT[3];
extern const char PAINT_CURSOR_VERTEX_PAINT[3];
extern const char PAINT_CURSOR_WEIGHT_PAINT[3];
extern const char PAINT_CURSOR_TEXTURE_PAINT[3];

typedef enum ePaintMode {
  PAINT_MODE_SCULPT = 0,
  /** Vertex color. */
  PAINT_MODE_VERTEX = 1,
  PAINT_MODE_WEIGHT = 2,
  /** 3D view (projection painting). */
  PAINT_MODE_TEXTURE_3D = 3,
  /** Image space (2D painting). */
  PAINT_MODE_TEXTURE_2D = 4,
  PAINT_MODE_SCULPT_UV = 5,
  PAINT_MODE_GPENCIL = 6,
  /* Grease Pencil Vertex Paint */
  PAINT_MODE_VERTEX_GPENCIL = 7,
  PAINT_MODE_SCULPT_GPENCIL = 8,
  PAINT_MODE_WEIGHT_GPENCIL = 9,
  /** Curves. */
  PAINT_MODE_SCULPT_CURVES = 10,

  /** Keep last. */
  PAINT_MODE_INVALID = 11,
} ePaintMode;

#define PAINT_MODE_HAS_BRUSH(mode) !ELEM(mode, PAINT_MODE_SCULPT_UV)

/* overlay invalidation */
typedef enum ePaintOverlayControlFlags {
  PAINT_OVERLAY_INVALID_TEXTURE_PRIMARY = 1,
  PAINT_OVERLAY_INVALID_TEXTURE_SECONDARY = (1 << 2),
  PAINT_OVERLAY_INVALID_CURVE = (1 << 3),
  PAINT_OVERLAY_OVERRIDE_CURSOR = (1 << 4),
  PAINT_OVERLAY_OVERRIDE_PRIMARY = (1 << 5),
  PAINT_OVERLAY_OVERRIDE_SECONDARY = (1 << 6),
} ePaintOverlayControlFlags;

#define PAINT_OVERRIDE_MASK \
  (PAINT_OVERLAY_OVERRIDE_SECONDARY | PAINT_OVERLAY_OVERRIDE_PRIMARY | \
   PAINT_OVERLAY_OVERRIDE_CURSOR)

/**
 * Defines 8 areas resulting of splitting the object space by the XYZ axis planes. This is used to
 * flip or mirror transform values depending on where the vertex is and where the transform
 * operation started to support XYZ symmetry on those operations in a predictable way.
 */
#define PAINT_SYMM_AREA_DEFAULT 0

typedef enum ePaintSymmetryAreas {
  PAINT_SYMM_AREA_X = (1 << 0),
  PAINT_SYMM_AREA_Y = (1 << 1),
  PAINT_SYMM_AREA_Z = (1 << 2),
} ePaintSymmetryAreas;

#define PAINT_SYMM_AREAS 8

void BKE_paint_invalidate_overlay_tex(struct Scene *scene,
                                      struct ViewLayer *view_layer,
                                      const struct Tex *tex);
void BKE_paint_invalidate_cursor_overlay(struct Scene *scene,
                                         struct ViewLayer *view_layer,
                                         struct CurveMapping *curve);
void BKE_paint_invalidate_overlay_all(void);
ePaintOverlayControlFlags BKE_paint_get_overlay_flags(void);
void BKE_paint_reset_overlay_invalid(ePaintOverlayControlFlags flag);
void BKE_paint_set_overlay_override(enum eOverlayFlags flag);

/* Palettes. */

struct Palette *BKE_palette_add(struct Main *bmain, const char *name);
struct PaletteColor *BKE_palette_color_add(struct Palette *palette);
bool BKE_palette_is_empty(const struct Palette *palette);
/**
 * Remove color from palette. Must be certain color is inside the palette!
 */
void BKE_palette_color_remove(struct Palette *palette, struct PaletteColor *color);
void BKE_palette_clear(struct Palette *palette);

void BKE_palette_sort_hsv(struct tPaletteColorHSV *color_array, int totcol);
void BKE_palette_sort_svh(struct tPaletteColorHSV *color_array, int totcol);
void BKE_palette_sort_vhs(struct tPaletteColorHSV *color_array, int totcol);
void BKE_palette_sort_luminance(struct tPaletteColorHSV *color_array, int totcol);
bool BKE_palette_from_hash(struct Main *bmain,
                           struct GHash *color_table,
                           const char *name,
                           bool linear);

/* Paint curves. */

struct PaintCurve *BKE_paint_curve_add(struct Main *bmain, const char *name);

/**
 * Call when entering each respective paint mode.
 */
bool BKE_paint_ensure(struct ToolSettings *ts, struct Paint **r_paint);
void BKE_paint_init(struct Main *bmain, struct Scene *sce, ePaintMode mode, const char col[3]);
void BKE_paint_free(struct Paint *p);
/**
 * Called when copying scene settings, so even if 'src' and 'tar' are the same still do a
 * #id_us_plus(), rather than if we were copying between 2 existing scenes where a matching
 * value should decrease the existing user count as with #paint_brush_set()
 */
void BKE_paint_copy(struct Paint *src, struct Paint *tar, int flag);

void BKE_paint_runtime_init(const struct ToolSettings *ts, struct Paint *paint);

void BKE_paint_cavity_curve_preset(struct Paint *p, int preset);

eObjectMode BKE_paint_object_mode_from_paintmode(ePaintMode mode);
bool BKE_paint_ensure_from_paintmode(struct Scene *sce, ePaintMode mode);
struct Paint *BKE_paint_get_active_from_paintmode(struct Scene *sce, ePaintMode mode);
const struct EnumPropertyItem *BKE_paint_get_tool_enum_from_paintmode(ePaintMode mode);
const char *BKE_paint_get_tool_prop_id_from_paintmode(ePaintMode mode);
uint BKE_paint_get_brush_tool_offset_from_paintmode(ePaintMode mode);
struct Paint *BKE_paint_get_active(struct Scene *sce, struct ViewLayer *view_layer);
struct Paint *BKE_paint_get_active_from_context(const struct bContext *C);
ePaintMode BKE_paintmode_get_active_from_context(const struct bContext *C);
ePaintMode BKE_paintmode_get_from_tool(const struct bToolRef *tref);
struct Brush *BKE_paint_brush(struct Paint *paint);
void BKE_paint_brush_set(struct Paint *paint, struct Brush *br);
struct Palette *BKE_paint_palette(struct Paint *paint);
void BKE_paint_palette_set(struct Paint *p, struct Palette *palette);
void BKE_paint_curve_set(struct Brush *br, struct PaintCurve *pc);
void BKE_paint_curve_clamp_endpoint_add_index(struct PaintCurve *pc, int add_index);

/**
 * Return true when in vertex/weight/texture paint + face-select mode?
 */
bool BKE_paint_select_face_test(struct Object *ob);
/**
 * Return true when in vertex/weight paint + vertex-select mode?
 */
bool BKE_paint_select_vert_test(struct Object *ob);
/**
 * used to check if selection is possible
 * (when we don't care if its face or vert)
 */
bool BKE_paint_select_elem_test(struct Object *ob);

/* Partial visibility. */

/**
 * Returns non-zero if any of the face's vertices are hidden, zero otherwise.
 */
bool paint_is_face_hidden(const struct MLoopTri *lt,
                          const struct MVert *mvert,
                          const struct MLoop *mloop);
/**
 * Returns non-zero if any of the corners of the grid
 * face whose inner corner is at (x, y) are hidden, zero otherwise.
 */
bool paint_is_grid_face_hidden(const unsigned int *grid_hidden, int gridsize, int x, int y);
/**
 * Return true if all vertices in the face are visible, false otherwise.
 */
bool paint_is_bmesh_face_hidden(struct BMFace *f);

/* Paint masks. */

float paint_grid_paint_mask(const struct GridPaintMask *gpm, uint level, uint x, uint y);

void BKE_paint_face_set_overlay_color_get(int face_set, int seed, uchar r_color[4]);

/* Stroke related. */

bool paint_calculate_rake_rotation(struct UnifiedPaintSettings *ups,
                                   struct Brush *brush,
                                   const float mouse_pos[2],
                                   const float initial_mouse_pos[2]);
void paint_update_brush_rake_rotation(struct UnifiedPaintSettings *ups,
                                      struct Brush *brush,
                                      float rotation);

void BKE_paint_stroke_get_average(struct Scene *scene, struct Object *ob, float stroke[3]);
bool BKE_paint_uses_channels(ePaintMode mode);

/* Tool slot API. */

void BKE_paint_toolslots_init_from_main(struct Main *bmain);
void BKE_paint_toolslots_len_ensure(struct Paint *paint, int len);
void BKE_paint_toolslots_brush_update_ex(struct Paint *paint, struct Brush *brush);
void BKE_paint_toolslots_brush_update(struct Paint *paint);
/**
 * Run this to ensure brush types are set for each slot on entering modes
 * (for new scenes for example).
 */
void BKE_paint_toolslots_brush_validate(struct Main *bmain, struct Paint *paint);
struct Brush *BKE_paint_toolslots_brush_get(struct Paint *paint, int slot_index);

/* .blend I/O */

void BKE_paint_blend_write(struct BlendWriter *writer, struct Paint *paint);
void BKE_paint_blend_read_data(struct BlendDataReader *reader,
                               const struct Scene *scene,
                               struct Paint *paint);
void BKE_paint_blend_read_lib(struct BlendLibReader *reader,
                              struct Scene *scene,
                              struct Paint *paint);

#define SCULPT_FACE_SET_NONE 0

/** Used for both vertex color and weight paint. */
struct SculptVertexPaintGeomMap {
  int *vert_map_mem;
  struct MeshElemMap *vert_to_loop;
  int *poly_map_mem;
  struct MeshElemMap *vert_to_poly;
};

/** Pose Brush IK Chain. */
typedef struct SculptPoseIKChainSegment {
  float orig[3];
  float head[3];

  float initial_orig[3];
  float initial_head[3];
  float len;
  float scale[3];
  float rot[4];
  float *weights;

  /* Store a 4x4 transform matrix for each of the possible combinations of enabled XYZ symmetry
   * axis. */
  float trans_mat[PAINT_SYMM_AREAS][4][4];
  float pivot_mat[PAINT_SYMM_AREAS][4][4];
  float pivot_mat_inv[PAINT_SYMM_AREAS][4][4];
} SculptPoseIKChainSegment;

typedef struct SculptPoseIKChain {
  SculptPoseIKChainSegment *segments;
  int tot_segments;
  float grab_delta_offset[3];
  float bend_mat[4][4];
  float bend_mat_inv[4][4];
  float bend_factor;
  float bend_limit;
  float bend_upper_limit;
} SculptPoseIKChain;

/* Cloth Brush */

/* Cloth Simulation. */
typedef enum eSculptClothNodeSimState {
  /* Constraints were not built for this node, so it can't be simulated. */
  SCULPT_CLOTH_NODE_UNINITIALIZED,

  /* There are constraints for the geometry in this node, but it should not be simulated. */
  SCULPT_CLOTH_NODE_INACTIVE,

  /* There are constraints for this node and they should be used by the solver. */
  SCULPT_CLOTH_NODE_ACTIVE,
} eSculptClothNodeSimState;

typedef enum eSculptClothConstraintType {
  /* Constraint that creates the structure of the cloth. */
  SCULPT_CLOTH_CONSTRAINT_STRUCTURAL = 0,
  /* Constraint that references the position of a vertex and a position in deformation_pos which
   * can be deformed by the tools. */
  SCULPT_CLOTH_CONSTRAINT_DEFORMATION = 1,
  /* Constraint that references the vertex position and a editable soft-body position for
   * plasticity. */
  SCULPT_CLOTH_CONSTRAINT_SOFTBODY = 2,
  /* Constraint that references the vertex position and its initial position. */
  SCULPT_CLOTH_CONSTRAINT_PIN = 3,
} eSculptClothConstraintType;

#define CLOTH_NO_POS_PTR

typedef struct SculptClothConstraint {
  signed char ctype, thread_nr;

  /* Index in #SculptClothSimulation.node_state of the node from where this constraint was
   * created. This constraints will only be used by the solver if the state is active. */
  short node;

  float strength;

  /* Elements that are affected by the constraint. */
  /* Element a should always be a mesh vertex
   * with the index stored in elem_index_a as
   * it is \
   * always deformed. Element b could be
   * another vertex of the same mesh or any
   * other position \
   * (arbitrary point, position for a previous
   * state). In that case, elem_index_a and \
   * elem_index_b should be the same to avoid
   * affecting two different vertices when
   * solving the \
   * constraints. *elem_position points to the
   * position which is owned by the element. */

  struct {
    int index;
#ifndef CLOTH_NO_POS_PTR
    float *position;
#endif
  } elems[];
} SculptClothConstraint;

#ifndef CLOTH_NO_POS_PTR
#  define MAKE_CONSTRAINT_STRUCT(totelem) \
    signed char ctype, thread_nr; \
    short node; \
    float strength; \
    struct { \
      int index; \
      float *position; \
    } elems[totelem]
#else
#  define MAKE_CONSTRAINT_STRUCT(totelem) \
    signed char ctype, thread_nr; \
    short node; \
    float strength; \
    struct { \
      int index; \
    } elems[totelem]
#endif

typedef struct SculptClothLengthConstraint {
  MAKE_CONSTRAINT_STRUCT(2);

  float length;
  eSculptClothConstraintType type;
} SculptClothLengthConstraint;

typedef struct SculptClothBendConstraint {
  MAKE_CONSTRAINT_STRUCT(4);

  float rest_angle, stiffness;
} SculptClothBendConstraint;

struct SculptClothTaskData;

typedef struct SculptClothSimulation {
  SculptClothConstraint *constraints[2];
  int tot_constraints[2];
  int capacity_constraints[2];

  struct EdgeSet *created_length_constraints;
  struct EdgeSet *created_bend_constraints;
  float *length_constraint_tweak;

  SculptClothBendConstraint *bend_constraints;
  int tot_bend_constraints, capacity_bend_constraints;

  struct SculptClothTaskData *constraint_tasks;

  /* final task always run in main thread, after all the others
   * have completed
   */
  int tot_constraint_tasks;

  float mass;
  float damping;
  float softbody_strength;

  // cache some values here to avoid
  // brush channel lookups inside of inner loops
  float sim_limit;
  int simulation_area_type;
  float sim_falloff;

  float (*acceleration)[3];

  float (*pos)[3];
  float (*init_pos)[3];
  float (*softbody_pos)[3];

  /* Position anchors for deformation brushes. These positions are modified by the brush and the
   * final positions of the simulated vertices are updated with constraints that use these points
   * as targets. */
  float (*deformation_pos)[3];
  float *deformation_strength;

  float (*prev_pos)[3];
  float (*last_iteration_pos)[3];
  float (*init_normal)[3];

  struct ListBase *collider_list;

  int totnode;
  /** #PBVHNode pointer as a key, index in #SculptClothSimulation.node_state as value. */
  struct GHash *node_state_index;
  eSculptClothNodeSimState *node_state;

  // persistent base customdata layer offsets
  int cd_pers_co;
  int cd_pers_no;
  int cd_pers_disp;

  bool use_bending;
  float bend_stiffness;
} SculptClothSimulation;

typedef struct SculptVertexInfo {
  /* Indexed by vertex, stores and ID of its topologically connected component. */
  int *connected_component;

  /* Indexed by base mesh vertex index, stores if that vertex is a boundary. */
  BLI_bitmap *boundary;

  /* Indexed by vertex, stores the symmetrical topology vertex index found by symmetrize. */
  int *symmetrize_map;
} SculptVertexInfo;

typedef struct SculptBoundaryEditInfo {
  /* Vertex index from where the topology propagation reached this vertex. */
  SculptVertRef original_vertex;
  int original_vertex_i;

  /* How many steps were needed to reach this vertex from the boundary. */
  int num_propagation_steps;

  /* Strength that is used to deform this vertex. */
  float strength_factor;
} SculptBoundaryEditInfo;

/* Edge for drawing the boundary preview in the cursor. */
typedef struct SculptBoundaryPreviewEdge {
  SculptVertRef v1;
  SculptVertRef v2;
} SculptBoundaryPreviewEdge;

#define MAX_STORED_COTANGENTW_EDGES 7

typedef struct StoredCotangentW {
  float static_weights[MAX_STORED_COTANGENTW_EDGES];
  float *weights;
  int length;
} StoredCotangentW;

typedef struct SculptBoundary {
  /* Vertex indices of the active boundary. */
  SculptVertRef *vertices;
  int *vertex_indices;

  int vertices_capacity;
  int num_vertices;

  /* Distance from a vertex in the boundary to initial vertex indexed by vertex index, taking into
   * account the length of all edges between them. Any vertex that is not in the boundary will have
   * a distance of 0. */
  float *distance;

  float (*smoothco)[3];
  float *boundary_dist;  // distances from verts to boundary
  float (*boundary_tangents)[3];

  StoredCotangentW *boundary_cotangents;
  SculptVertRef *boundary_closest;
  int sculpt_totvert;

  /* Data for drawing the preview. */
  SculptBoundaryPreviewEdge *edges;
  int edges_capacity;
  int num_edges;

  /* True if the boundary loops into itself. */
  bool forms_loop;

  /* Initial vertex in the boundary which is closest to the current sculpt active vertex. */
  SculptVertRef initial_vertex;

  /* Vertex that at max_propagation_steps from the boundary and closest to the original active
   * vertex that was used to initialize the boundary. This is used as a reference to check how much
   * the deformation will go into the mesh and to calculate the strength of the brushes. */
  SculptVertRef pivot_vertex;

  /* Stores the initial positions of the pivot and boundary initial vertex as they may be deformed
   * during the brush action. This allows to use them as a reference positions and vectors for some
   * brush effects. */
  float initial_vertex_position[3];
  float initial_pivot_position[3];

  /* Maximum number of topology steps that were calculated from the boundary. */
  int max_propagation_steps;

  /* Indexed by vertex index, contains the topology information needed for boundary deformations.
   */
  struct SculptBoundaryEditInfo *edit_info;

  /* Bend Deform type. */
  struct {
    float (*pivot_rotation_axis)[3];
    float (*pivot_positions)[4];
  } bend;

  /* Slide Deform type. */
  struct {
    float (*directions)[3];
  } slide;

  /* Twist Deform type. */
  struct {
    float rotation_axis[3];
    float pivot_position[3];
  } twist;

  /* Cicrle Deform type. */
  struct {
    float (*origin)[3];
    float *radius;
  } circle;

  int deform_target;
} SculptBoundary;

/* Array Brush. */
typedef struct SculptArrayCopy {
  int index;
  int symm_pass;
  float mat[4][4];
  float imat[4][4];
  float origin[3];
} SculptArrayCopy;

typedef struct ScultpArrayPathPoint {
  float length;
  float strength;
  float co[3];
  float orco[3];
  float direction[3];
} ScultpArrayPathPoint;

typedef struct SculptArray {
  SculptArrayCopy *copies[PAINT_SYMM_AREAS];
  int num_copies;

  struct {
    ScultpArrayPathPoint *points;
    int tot_points;
    int capacity;
    float total_length;
  } path;

  int mode;
  float normal[3];
  float direction[3];
  float radial_angle;
  float initial_radial_angle;

  bool source_mat_valid;
  float source_origin[3];
  float source_mat[4][4];
  float source_imat[4][4];
  float (*orco)[3];

  int *copy_index;
  int *symmetry_pass;

  float *smooth_strength;
  struct SculptCustomLayer *scl_inst, *scl_sym;
} SculptArray;

typedef struct SculptFakeNeighbors {
  bool use_fake_neighbors;

  /* Max distance used to calculate neighborhood information. */
  float current_max_distance;

  /* Indexed by vertex, stores the vertex index of its fake neighbor if available. */
  SculptVertRef *fake_neighbor_index;

} SculptFakeNeighbors;

/* Session data (mode-specific) */

/* Custom Temporary Attributes */

typedef struct SculptLayerParams {
  int simple_array : 1;  // cannot be combined with permanent
  int permanent : 1;     // cannot be combined with simple_array
  int nocopy : 1;
  int nointerp : 1;
} SculptLayerParams;

typedef struct SculptCustomLayer {
  AttributeDomain domain;
  int proptype;
  SculptLayerParams params;

  char name[MAX_CUSTOMDATA_LAYER_NAME];

  bool is_cdlayer;  // false for multires data
  void *data;       // only valid for multires and face
  int elemsize;
  int cd_offset;                  // for bmesh
  struct CustomDataLayer *layer;  // not for multires
  bool from_bmesh;  // note that layers can be fixed arrays but still from a bmesh, e.g. filter
                    // laplacian smooth
  bool released;
  bool ready;
} SculptCustomLayer;

/* These custom attributes have references
  (SculptCustomLayer pointers) inside of ss->custom_layers
  that are kept up to date with SCULPT_update_customdata_refs.
  */
typedef enum {
  SCULPT_SCL_FAIRING_MASK,
  SCULPT_SCL_FAIRING_FADE,
  SCULPT_SCL_PREFAIRING_CO,
  SCULPT_SCL_PERS_CO,
  SCULPT_SCL_PERS_NO,
  SCULPT_SCL_PERS_DISP,
  SCULPT_SCL_LAYER_DISP,
  SCULPT_SCL_LAYER_STROKE_ID,
  SCULPT_SCL_ORIG_FSETS,
  SCULPT_SCL_SMOOTH_VEL,
  SCULPT_SCL_SMOOTH_BDIS,
  SCULPT_SCL_AUTOMASKING,
  SCULPT_SCL_LIMIT_SURFACE,
  SCULPT_SCL_LAYER_MAX,
} SculptStandardAttr;

#define SCULPT_SCL_GET_NAME(stdattr) ("__" #stdattr)

typedef struct SculptSession {
  /* Mesh data (not copied) can come either directly from a Mesh, or from a MultiresDM */
  struct { /* Special handling for multires meshes */
    bool active;
    struct MultiresModifierData *modifier;
    int level;
  } multires;

  /* Depsgraph for the Cloth Brush solver to get the colliders. */
  struct Depsgraph *depsgraph;

  /* These are always assigned to base mesh data when using PBVH_FACES and PBVH_GRIDS. */
  struct MVert *mvert;
  struct MEdge *medge;
  struct MLoop *mloop;
  struct MPoly *mpoly;

  const float (*vert_normals)[3];

  // only assigned in PBVH_FACES and PBVH_GRIDS
  CustomData *vdata, *edata, *ldata, *pdata;

  // for grids
  CustomData temp_vdata, temp_pdata;
  int temp_vdata_elems, temp_pdata_elems;

  /* These contain the vertex and poly counts of the final mesh. */
  int totvert, totpoly;

  struct KeyBlock *shapekey_active;
  struct MPropCol *vcol;
  struct MLoopCol *mcol;

  int vcol_domain;
  int vcol_type;

  float *vmask;

  /* Mesh connectivity maps. */
  /* Vertices to adjacent polys. */
  struct MeshElemMap *pmap;
  int *pmap_mem;

  /* Edges to adjacent polys. */
  struct MeshElemMap *epmap;
  int *epmap_mem;

  /* Vertices to adjacent edges. */
  struct MeshElemMap *vemap;
  int *vemap_mem;

  /* Mesh Face Sets */
  /* Total number of polys of the base mesh. */
  int totedges, totloops, totfaces;
  /* Face sets store its visibility in the sign of the integer, using the absolute value as the
   * Face Set ID. Positive IDs are visible, negative IDs are hidden.
   * The 0 ID is not used by the tools or the visibility system, it is just used when creating new
   * geometry (the trim tool, for example) to detect which geometry was just added, so it can be
   * assigned a valid Face Set after creation. Tools are not intended to run with Face Sets IDs set
   * to 0. */
  int *face_sets;

  /* BMesh for dynamic topology sculpting */
  struct BMesh *bm;
  int cd_sculpt_vert;
  int cd_vert_node_offset;
  int cd_face_node_offset;
  int cd_vcol_offset;
  int cd_vert_mask_offset;
  int cd_faceset_offset;
  int cd_face_areas;

  int totuv;

  bool bm_smooth_shading;
  bool ignore_uvs;

  /* Undo/redo log for dynamic topology sculpting */
  struct BMLog *bm_log;

  /* Limit surface/grids. */
  struct SubdivCCG *subdiv_ccg;

  /* PBVH acceleration structure */
  struct PBVH *pbvh;
  bool show_mask;
  bool show_face_sets;

  /* Setting this to true allows a PBVH rebuild when evaluating the object even if the stroke or
   * filter caches are active. */
  bool needs_pbvh_rebuild;

  /* Painting on deformed mesh */
  bool deform_modifiers_active; /* Object is deformed with some modifiers. */
  float (*orig_cos)[3];         /* Coords of un-deformed mesh. */
  float (*deform_cos)[3];       /* Coords of deformed mesh but without stroke displacement. */
  float (*deform_imats)[3][3];  /* Crazy-space deformation matrices. */
  float *face_areas;            /* cached face areas for PBVH_FACES and PBVH_GRIDS */

  /* Used to cache the render of the active texture */
  unsigned int texcache_side, *texcache, texcache_actual;
  struct ImagePool *tex_pool;

  struct StrokeCache *cache;
  struct FilterCache *filter_cache;
  struct ExpandCache *expand_cache;

  /* Cursor data and active vertex for tools */
  SculptVertRef active_vertex_index;
  SculptFaceRef active_face_index;

  int active_grid_index;

  /* When active, the cursor draws with faded colors, indicating that there is an action enabled.
   */
  bool draw_faded_cursor;
  float cursor_radius;
  float cursor_location[3];
  float cursor_normal[3];
  float cursor_sampled_normal[3];
  float cursor_view_normal[3];

  /* For Sculpt trimming gesture tools, initial ray-cast data from the position of the mouse when
   * the gesture starts (intersection with the surface and if they ray hit the surface or not). */
  float gesture_initial_back_location[3];
  float gesture_initial_location[3];
  float gesture_initial_normal[3];
  bool gesture_initial_hit;

  /* TODO(jbakker): Replace rv3d and v3d with ViewContext */
  struct RegionView3D *rv3d;
  struct View3D *v3d;
  struct Scene *scene;
  int cd_origvcol_offset;
  int cd_origco_offset;
  int cd_origno_offset;

  /* Face Sets by topology. */
  int face_set_last_created;
  SculptFaceRef face_set_last_poly;
  SculptEdgeRef face_set_last_edge;

  /* Dynamic mesh preview */
  SculptVertRef *preview_vert_index_list;
  int preview_vert_index_count;

  /* Pose Brush Preview */
  float pose_origin[3];
  SculptPoseIKChain *pose_ik_chain_preview;

  /* Boundary Brush Preview */
  SculptBoundary *boundary_preview;

  SculptVertexInfo vertex_info;
  SculptFakeNeighbors fake_neighbors;

  /* Array. */
  SculptArray *array;

  /* Transform operator */
  float pivot_pos[3];
  float pivot_rot[4];
  float pivot_scale[3];

  float prev_pivot_pos[3];
  float prev_pivot_rot[4];
  float prev_pivot_scale[3];

  float init_pivot_pos[3];
  float init_pivot_rot[4];
  float init_pivot_scale[3];

  union {
    struct {
      struct SculptVertexPaintGeomMap gmap;

      /* For non-airbrush painting to re-apply from the original (MLoop aligned). */
      unsigned int *previous_color;
    } vpaint;

    struct {
      struct SculptVertexPaintGeomMap gmap;
      /* Keep track of how much each vertex has been painted (non-airbrush only). */
      float *alpha_weight;

      /* Needed to continuously re-apply over the same weights (BRUSH_ACCUMULATE disabled).
       * Lazy initialize as needed (flag is set to 1 to tag it as uninitialized). */
      struct MDeformVert *dvert_prev;
    } wpaint;

    /* TODO: identify sculpt-only fields */
    // struct { ... } sculpt;
  } mode;
  eObjectMode mode_type;

  /* This flag prevents PBVH from being freed when creating the vp_handle for texture paint. */
  bool building_vp_handle;

  /**
   * ID data is older than sculpt-mode data.
   * Set #Main.is_memfile_undo_flush_needed when enabling.
   */
  char needs_flush_to_id;

  // id of current stroke, used to detect
  // if vertex original data needs to be updated
  int stroke_id, boundary_symmetry;

  bool fast_draw;  // hides facesets/masks and forces smooth to save GPU bandwidth
  struct MSculptVert *mdyntopo_verts;  // for non-bmesh
  int mdyntopo_verts_size;

  /*list of up to date custom layer references,
    note that entries can be NULL if layer doesn't
    exist.  See SCULPT_SCL_XXX enum above.*/
  struct SculptCustomLayer *custom_layers[SCULPT_SCL_LAYER_MAX];

  /*
  PBVH_GRIDS cannot store customdata layers in real CustomDataLayers,
  so we queue the memory allocated for them to free later
  */
  struct SculptCustomLayer **layers_to_free;
  int tot_layers_to_free;

  bool save_temp_layers;
} SculptSession;

void BKE_sculptsession_free(struct Object *ob);
void BKE_sculptsession_free_deformMats(struct SculptSession *ss);
void BKE_sculptsession_free_vwpaint_data(struct SculptSession *ss);

void BKE_sculptsession_bm_to_me(struct Object *ob, bool reorder);
void BKE_sculptsession_bm_to_me_for_render(struct Object *object);

bool BKE_sculptsession_check_sculptverts(SculptSession *ss, struct PBVH *pbvh, int totvert);

struct BMesh *BKE_sculptsession_empty_bmesh_create(void);
void BKE_sculptsession_bmesh_attr_update_internal(struct Object *ob);

void BKE_sculptsession_sync_attributes(struct Object *ob, struct Mesh *me);

void BKE_sculptsession_bmesh_add_layers(struct Object *ob);
bool BKE_sculptsession_attr_get_layer(struct Object *ob,
                                      AttributeDomain domain,
                                      int proptype,
                                      const char *name,
                                      SculptCustomLayer *scl,
                                      SculptLayerParams *params);
bool BKE_sculptsession_attr_release_layer(struct Object *ob, SculptCustomLayer *scl);
void BKE_sculptsession_update_attr_refs(struct Object *ob);

int BKE_sculptsession_get_totvert(const SculptSession *ss);

void BKE_sculptsession_ignore_uvs_set(struct Object *ob, bool value);

/**
 * Create new color layer on object if it doesn't have one and if experimental feature set has
 * sculpt vertex color enabled. Returns truth if new layer has been added, false otherwise.
 */

void BKE_sculpt_color_layer_create_if_needed(struct Object *object);

/**
 * \warning Expects a fully evaluated depsgraph.
 */
void BKE_sculpt_update_object_for_edit(struct Depsgraph *depsgraph,
                                       struct Object *ob_orig,
                                       bool need_pmap,
                                       bool need_mask,
                                       bool need_colors);
void BKE_sculpt_update_object_before_eval(struct Object *ob_eval);
void BKE_sculpt_update_object_after_eval(struct Depsgraph *depsgraph, struct Object *ob_eval);

/**
 * Sculpt mode handles multi-res differently from regular meshes, but only if
 * it's the last modifier on the stack and it is not on the first level.
 */
struct MultiresModifierData *BKE_sculpt_multires_active(const struct Scene *scene,
                                                        struct Object *ob);
int BKE_sculpt_mask_layers_ensure(struct Object *ob, struct MultiresModifierData *mmd);
void BKE_sculpt_toolsettings_data_ensure(struct Scene *scene);

struct PBVH *BKE_sculpt_object_pbvh_ensure(struct Depsgraph *depsgraph, struct Object *ob);

void BKE_sculpt_bvh_update_from_ccg(struct PBVH *pbvh, struct SubdivCCG *subdiv_ccg);

/**
 * This ensure that all elements in the mesh (both vertices and grids) have their visibility
 * updated according to the face sets.
 */
void BKE_sculpt_sync_face_set_visibility(struct Mesh *mesh, struct SubdivCCG *subdiv_ccg);

/**
 * Individual function to sync the Face Set visibility to mesh and grids.
 */
void BKE_sculpt_sync_face_sets_visibility_to_base_mesh(struct Mesh *mesh);
void BKE_sculpt_sync_face_sets_visibility_to_grids(struct Mesh *mesh,
                                                   struct SubdivCCG *subdiv_ccg);

/**
 * Ensures that a Face Set data-layers exists. If it does not, it creates one respecting the
 * visibility stored in the vertices of the mesh. If it does, it copies the visibility from the
 * mesh to the Face Sets. */
void BKE_sculpt_face_sets_ensure_from_base_mesh_visibility(struct Mesh *mesh);

/**
 * Ensures we do have expected mesh data in original mesh for the sculpt mode.
 *
 * \note IDs are expected to be original ones here, and calling code should ensure it updates its
 * depsgraph properly after calling this function if it needs up-to-date evaluated data.
 */
void BKE_sculpt_ensure_orig_mesh_data(struct Scene *scene, struct Object *object);

/**
 * Test if PBVH can be used directly for drawing, which is faster than
 * drawing the mesh and all updates that come with it.
 */
bool BKE_sculptsession_use_pbvh_draw(const struct Object *ob, const struct View3D *v3d);

char BKE_get_fset_boundary_symflag(struct Object *object);

enum {
  SCULPT_MASK_LAYER_CALC_VERT = (1 << 0),
  SCULPT_MASK_LAYER_CALC_LOOP = (1 << 1),
};

#ifdef __cplusplus
}
#endif
