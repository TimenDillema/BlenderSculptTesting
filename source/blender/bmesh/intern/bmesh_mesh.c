/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bmesh
 *
 * BM mesh level functions.
 */

#include "MEM_guardedalloc.h"

#include "DNA_listBase.h"
#include "DNA_scene_types.h"

#include "BLI_alloca.h"
#include "BLI_array.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_rand.h"
#include "BLI_utildefines.h"

#include "BKE_customdata.h"
#include "BKE_mesh.h"

#include "DNA_meshdata_types.h"

#include "bmesh.h"
#include "bmesh_private.h"
#include "range_tree.h"

const BMAllocTemplate bm_mesh_allocsize_default = {512, 1024, 2048, 512};
const BMAllocTemplate bm_mesh_chunksize_default = {512, 1024, 2048, 512};

static void bm_alloc_toolflags(BMesh *bm);

static void bm_mempool_init_ex(const BMAllocTemplate *allocsize,
                               const bool use_toolflags,
                               BLI_mempool **r_vpool,
                               BLI_mempool **r_epool,
                               BLI_mempool **r_lpool,
                               BLI_mempool **r_fpool)
{
  size_t vert_size, edge_size, loop_size, face_size;

  if (use_toolflags == true) {
    vert_size = sizeof(BMVert_OFlag);
    edge_size = sizeof(BMEdge_OFlag);
    loop_size = sizeof(BMLoop);
    face_size = sizeof(BMFace_OFlag);
  }
  else {
    vert_size = sizeof(BMVert);
    edge_size = sizeof(BMEdge);
    loop_size = sizeof(BMLoop);
    face_size = sizeof(BMFace);
  }

  if (r_vpool) {
    *r_vpool = BLI_mempool_create(
        vert_size, allocsize->totvert, bm_mesh_chunksize_default.totvert, BLI_MEMPOOL_ALLOW_ITER);
  }
  if (r_epool) {
    *r_epool = BLI_mempool_create(
        edge_size, allocsize->totedge, bm_mesh_chunksize_default.totedge, BLI_MEMPOOL_ALLOW_ITER);
  }
  if (r_lpool) {
    *r_lpool = BLI_mempool_create(
        loop_size, allocsize->totloop, bm_mesh_chunksize_default.totloop, BLI_MEMPOOL_ALLOW_ITER);
  }
  if (r_fpool) {
    *r_fpool = BLI_mempool_create(
        face_size, allocsize->totface, bm_mesh_chunksize_default.totface, BLI_MEMPOOL_ALLOW_ITER);
  }
}

static void bm_mempool_init(BMesh *bm, const BMAllocTemplate *allocsize, const bool use_toolflags)
{
  bm_mempool_init_ex(allocsize, use_toolflags, &bm->vpool, &bm->epool, &bm->lpool, &bm->fpool);

#ifdef USE_BMESH_HOLES
  bm->looplistpool = BLI_mempool_create(sizeof(BMLoopList), 512, 512, BLI_MEMPOOL_NOP);
#endif
}

void BM_mesh_elem_toolflags_ensure(BMesh *bm)
{
  BLI_assert(bm->use_toolflags);

  if (bm->vtoolflagpool && bm->etoolflagpool && bm->ftoolflagpool) {
    return;
  }

  bm->vtoolflagpool = BLI_mempool_create(sizeof(BMFlagLayer), bm->totvert, 512, BLI_MEMPOOL_NOP);
  bm->etoolflagpool = BLI_mempool_create(sizeof(BMFlagLayer), bm->totedge, 512, BLI_MEMPOOL_NOP);
  bm->ftoolflagpool = BLI_mempool_create(sizeof(BMFlagLayer), bm->totface, 512, BLI_MEMPOOL_NOP);

  bm_alloc_toolflags(bm);

  bm->totflags = 1;
}

void BM_mesh_elem_toolflags_clear(BMesh *bm)
{
  bool haveflags = bm->vtoolflagpool || bm->etoolflagpool || bm->ftoolflagpool;

  if (bm->vtoolflagpool) {
    BLI_mempool_destroy(bm->vtoolflagpool);
    bm->vtoolflagpool = NULL;
  }
  if (bm->etoolflagpool) {
    BLI_mempool_destroy(bm->etoolflagpool);
    bm->etoolflagpool = NULL;
  }
  if (bm->ftoolflagpool) {
    BLI_mempool_destroy(bm->ftoolflagpool);
    bm->ftoolflagpool = NULL;
  }

  if (haveflags) {
    BM_data_layer_free(bm, &bm->vdata, CD_TOOLFLAGS);
    BM_data_layer_free(bm, &bm->edata, CD_TOOLFLAGS);
    BM_data_layer_free(bm, &bm->pdata, CD_TOOLFLAGS);
  }
}

// int cdmap[8] = {0, 1, -1, -1, 2, -1, -1, -1, 3};

static void bm_swap_cd_data(int htype, BMesh *bm, CustomData *cd, void *a, void *b)
{
  int tot = cd->totsize;
  // int cd_id = bm->idmap.cd_id_off[htype];

  char *sa = (char *)a;
  char *sb = (char *)b;

  for (int i = 0; i < tot; i++, sa++, sb++) {
    char tmp = *sa;
    *sa = *sb;
    *sb = tmp;
  }
}

BMesh *BM_mesh_create(const BMAllocTemplate *allocsize, const struct BMeshCreateParams *params)
{
  /* allocate the structure */
  BMesh *bm = MEM_callocN(sizeof(BMesh), __func__);

  /* allocate the memory pools for the mesh elements */
  bm_mempool_init(bm, allocsize, params->use_toolflags);

  bm->idmap.flag = 0;

  if (!params->temporary_ids) {
    bm->idmap.flag |= BM_PERMANENT_IDS;
  }

  if (params->id_map) {
    bm->idmap.flag |= BM_HAS_ID_MAP;
  }

  if (params->no_reuse_ids) {
    bm->idmap.flag |= BM_NO_REUSE_IDS;
  }

  if (params->create_unique_ids) {
    bm->idmap.flag |= BM_HAS_IDS;

    bm->idmap.flag |= params->id_elem_mask;

#ifndef WITH_BM_ID_FREELIST
    bm->idmap.idtree = range_tree_uint_alloc(0, (uint)-1);
#endif
  }

  if (bm->idmap.flag & BM_HAS_ID_MAP) {
    if (bm->idmap.flag & BM_NO_REUSE_IDS) {
      bm->idmap.ghash = BLI_ghash_ptr_new("idmap.ghash");
    }
    else {
      bm->idmap.map_size = BM_DEFAULT_IDMAP_SIZE;
      bm->idmap.map = MEM_callocN(sizeof(void *) * bm->idmap.map_size, "bmesh idmap");
      bm->idmap.ghash = NULL;
    }
  }
  else {
    bm->idmap.map = NULL;
    bm->idmap.ghash = NULL;
  }

  /* allocate one flag pool that we don't get rid of. */
  bm->use_toolflags = params->use_toolflags;
  bm->toolflag_index = 0;
  bm->totflags = 0;

  CustomData_reset(&bm->vdata);
  CustomData_reset(&bm->edata);
  CustomData_reset(&bm->ldata);
  CustomData_reset(&bm->pdata);

  bool init_cdata_pools = false;

  if (bm->use_toolflags) {
    init_cdata_pools = true;
    bm_alloc_toolflags_cdlayers(bm, false);
  }

  if (params->create_unique_ids) {
    bm_init_idmap_cdlayers(bm);
    init_cdata_pools = true;
  }

  if (init_cdata_pools) {
    if (bm->vdata.totlayer) {
      CustomData_bmesh_init_pool_ex(&bm->vdata, 0, BM_VERT, __func__);
    }
    if (bm->edata.totlayer) {
      CustomData_bmesh_init_pool_ex(&bm->edata, 0, BM_EDGE, __func__);
    }
    if (bm->ldata.totlayer) {
      CustomData_bmesh_init_pool_ex(&bm->ldata, 0, BM_LOOP, __func__);
    }
    if (bm->pdata.totlayer) {
      CustomData_bmesh_init_pool_ex(&bm->pdata, 0, BM_FACE, __func__);
    }
  }

#ifdef USE_BMESH_PAGE_CUSTOMDATA
  bmesh_update_attr_refs(bm);
  BMAttr_init(bm);
#endif

  return bm;
}

void BM_mesh_data_free(BMesh *bm)
{
  BMVert *v;
  BMEdge *e;
  BMLoop *l;
  BMFace *f;

  BMIter iter;
  BMIter itersub;

#ifndef WITH_BM_ID_FREELIST
  if (bm->idmap.idtree) {
    range_tree_uint_free(bm->idmap.idtree);
  }
#else
  MEM_SAFE_FREE(bm->idmap.free_ids);
  MEM_SAFE_FREE(bm->idmap.freelist);
  bm->idmap.freelist = NULL;
  bm->idmap.free_ids = NULL;
#endif

  MEM_SAFE_FREE(bm->idmap.map);

  if (bm->idmap.ghash) {
    BLI_ghash_free(bm->idmap.ghash, NULL, NULL);
  }

#ifdef WITH_BM_ID_FREELIST
  if (bm->idmap.free_idx_map) {
    BLI_ghash_free(bm->idmap.free_idx_map, NULL, NULL);
    bm->idmap.free_idx_map = NULL;
  }
#endif

  const bool is_ldata_free = CustomData_bmesh_has_free(&bm->ldata);
  const bool is_pdata_free = CustomData_bmesh_has_free(&bm->pdata);

  /* Check if we have to call free, if not we can avoid a lot of looping */
  if (CustomData_bmesh_has_free(&(bm->vdata))) {
    BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
      CustomData_bmesh_free_block(&(bm->vdata), &(v->head.data));
    }
  }
  if (CustomData_bmesh_has_free(&(bm->edata))) {
    BM_ITER_MESH (e, &iter, bm, BM_EDGES_OF_MESH) {
      CustomData_bmesh_free_block(&(bm->edata), &(e->head.data));
    }
  }

  if (is_ldata_free || is_pdata_free) {
    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (is_pdata_free) {
        CustomData_bmesh_free_block(&(bm->pdata), &(f->head.data));
      }
      if (is_ldata_free) {
        BM_ITER_ELEM (l, &itersub, f, BM_LOOPS_OF_FACE) {
          CustomData_bmesh_free_block(&(bm->ldata), &(l->head.data));
        }
      }
    }
  }

  /* Free custom data pools, This should probably go in CustomData_free? */
  if (bm->vdata.totlayer) {
    BLI_mempool_destroy(bm->vdata.pool);
  }
  if (bm->edata.totlayer) {
    BLI_mempool_destroy(bm->edata.pool);
  }
  if (bm->ldata.totlayer) {
    BLI_mempool_destroy(bm->ldata.pool);
  }
  if (bm->pdata.totlayer) {
    BLI_mempool_destroy(bm->pdata.pool);
  }

  /* free custom data */
  CustomData_free(&bm->vdata, 0);
  CustomData_free(&bm->edata, 0);
  CustomData_free(&bm->ldata, 0);
  CustomData_free(&bm->pdata, 0);

  /* destroy element pools */
  BLI_mempool_destroy(bm->vpool);
  BLI_mempool_destroy(bm->epool);
  BLI_mempool_destroy(bm->lpool);
  BLI_mempool_destroy(bm->fpool);

  if (bm->vtable) {
    MEM_freeN(bm->vtable);
  }
  if (bm->etable) {
    MEM_freeN(bm->etable);
  }
  if (bm->ftable) {
    MEM_freeN(bm->ftable);
  }

  /* destroy flag pools */

  if (bm->vtoolflagpool) {
    BLI_mempool_destroy(bm->vtoolflagpool);
    bm->vtoolflagpool = NULL;
  }
  if (bm->etoolflagpool) {
    BLI_mempool_destroy(bm->etoolflagpool);
    bm->etoolflagpool = NULL;
  }
  if (bm->ftoolflagpool) {
    BLI_mempool_destroy(bm->ftoolflagpool);
    bm->ftoolflagpool = NULL;
  }

#ifdef USE_BMESH_HOLES
  BLI_mempool_destroy(bm->looplistpool);
#endif

  BLI_freelistN(&bm->selected);

  if (bm->lnor_spacearr) {
    BKE_lnor_spacearr_free(bm->lnor_spacearr);
    MEM_freeN(bm->lnor_spacearr);
  }

  BMO_error_clear(bm);

#ifdef USE_BMESH_PAGE_CUSTOMDATA
  BMAttr_free(bm->attr_list);
  bm->attr_list = NULL;
#endif
}

void BM_mesh_clear(BMesh *bm)
{
  const bool use_toolflags = bm->use_toolflags;
  const int idmap_flags = bm->idmap.flag;

  /* free old mesh */
  BM_mesh_data_free(bm);
  memset(bm, 0, sizeof(BMesh));

  /* allocate the memory pools for the mesh elements */
  bm_mempool_init(bm, &bm_mesh_allocsize_default, use_toolflags);

  bm->use_toolflags = use_toolflags;
  bm->toolflag_index = 0;
  bm->totflags = 0;

  CustomData_reset(&bm->vdata);
  CustomData_reset(&bm->edata);
  CustomData_reset(&bm->ldata);
  CustomData_reset(&bm->pdata);

  bm->idmap.flag = idmap_flags;

  if (bm->idmap.flag & BM_HAS_IDS) {
    bm->idmap.map = NULL;
    bm->idmap.ghash = NULL;
    bm->idmap.map_size = 0;

#ifndef WITH_BM_ID_FREELIST
    bm->idmap.idtree = range_tree_uint_alloc(0, (uint)-1);
#else
    MEM_SAFE_FREE(bm->idmap.free_ids);
    MEM_SAFE_FREE(bm->idmap.freelist);

    bm->idmap.freelist_len = bm->idmap.freelist_size = 0;
    bm->idmap.free_ids = NULL;
    bm->idmap.freelist = NULL;
#endif
    bm_init_idmap_cdlayers(bm);
  }

#ifdef USE_BMESH_PAGE_CUSTOMDATA
  if (!bm->attr_list) {
    bm->attr_list = BMAttr_new();
  }
  else {
    BMAttr_reset(bm->attr_list);
  }

  BMAttr_init(bm);
#endif
}

void BM_mesh_free(BMesh *bm)
{
  BM_mesh_data_free(bm);

  if (bm->py_handle) {
    /* keep this out of 'BM_mesh_data_free' because we want python
     * to be able to clear the mesh and maintain access. */
    bpy_bm_generic_invalidate(bm->py_handle);
    bm->py_handle = NULL;
  }

  MEM_freeN(bm);
}

/**
 * \brief BMesh Begin Edit
 *
 * Functions for setting up a mesh for editing and cleaning up after
 * the editing operations are done. These are called by the tools/operator
 * API for each time a tool is executed.
 */
void bmesh_edit_begin(BMesh *bm, BMOpTypeFlag type_flag)
{
  /* switch multires data out of tangent space */
  if ((type_flag & BMO_OPTYPE_FLAG_UNTAN_MULTIRES) &&
      CustomData_has_layer(&bm->ldata, CD_MDISPS)) {
    BM_enter_multires_space(NULL, bm, MULTIRES_SPACE_ABSOLUTE);
    /* ensure correct normals, if possible */
    // bmesh_rationalize_normals(bm, 0);
    // BM_mesh_normals_update(bm);
  }
}

void bmesh_edit_end(BMesh *bm, BMOpTypeFlag type_flag)
{
  ListBase select_history;

  /* switch multires data into tangent space */
  if ((type_flag & BMO_OPTYPE_FLAG_UNTAN_MULTIRES) &&
      CustomData_has_layer(&bm->ldata, CD_MDISPS)) {
    BM_enter_multires_space(NULL, bm, MULTIRES_SPACE_TANGENT);
  }

  /* compute normals, clear temp flags and flush selections */
  if (type_flag & BMO_OPTYPE_FLAG_NORMALS_CALC) {
    bm->spacearr_dirty |= BM_SPACEARR_DIRTY_ALL;
    BM_mesh_normals_update(bm);
  }

  if ((type_flag & BMO_OPTYPE_FLAG_SELECT_VALIDATE) == 0) {
    select_history = bm->selected;
    BLI_listbase_clear(&bm->selected);
  }

  if (type_flag & BMO_OPTYPE_FLAG_SELECT_FLUSH) {
    BM_mesh_select_mode_flush(bm);
  }

  if ((type_flag & BMO_OPTYPE_FLAG_SELECT_VALIDATE) == 0) {
    bm->selected = select_history;
  }
  if (type_flag & BMO_OPTYPE_FLAG_INVALIDATE_CLNOR_ALL) {
    bm->spacearr_dirty |= BM_SPACEARR_DIRTY_ALL;
  }
}

void BM_mesh_elem_index_ensure_ex(BMesh *bm, const char htype, int elem_offset[4])
{

#ifdef DEBUG
  BM_ELEM_INDEX_VALIDATE(bm, "Should Never Fail!", __func__);
#endif

  if (elem_offset == NULL) {
    /* Simple case. */
    const char htype_needed = bm->elem_index_dirty & htype;
    if (htype_needed == 0) {
      goto finally;
    }
  }

  if (htype & BM_VERT) {
    if ((bm->elem_index_dirty & BM_VERT) || (elem_offset && elem_offset[0])) {
      BMIter iter;
      BMElem *ele;

      int index = elem_offset ? elem_offset[0] : 0;
      BM_ITER_MESH (ele, &iter, bm, BM_VERTS_OF_MESH) {
        BM_elem_index_set(ele, index++); /* set_ok */
      }
      BLI_assert(elem_offset || index == bm->totvert);
    }
    else {
      // printf("%s: skipping vert index calc!\n", __func__);
    }
  }

  if (htype & BM_EDGE) {
    if ((bm->elem_index_dirty & BM_EDGE) || (elem_offset && elem_offset[1])) {
      BMIter iter;
      BMElem *ele;

      int index = elem_offset ? elem_offset[1] : 0;
      BM_ITER_MESH (ele, &iter, bm, BM_EDGES_OF_MESH) {
        BM_elem_index_set(ele, index++); /* set_ok */
      }
      BLI_assert(elem_offset || index == bm->totedge);
    }
    else {
      // printf("%s: skipping edge index calc!\n", __func__);
    }
  }

  if (htype & (BM_FACE | BM_LOOP)) {
    if ((bm->elem_index_dirty & (BM_FACE | BM_LOOP)) ||
        (elem_offset && (elem_offset[2] || elem_offset[3]))) {
      BMIter iter;
      BMElem *ele;

      const bool update_face = (htype & BM_FACE) && (bm->elem_index_dirty & BM_FACE);
      const bool update_loop = (htype & BM_LOOP) && (bm->elem_index_dirty & BM_LOOP);

      int index_loop = elem_offset ? elem_offset[2] : 0;
      int index = elem_offset ? elem_offset[3] : 0;

      BM_ITER_MESH (ele, &iter, bm, BM_FACES_OF_MESH) {
        if (update_face) {
          BM_elem_index_set(ele, index++); /* set_ok */
        }

        if (update_loop) {
          BMLoop *l_iter, *l_first;

          l_iter = l_first = BM_FACE_FIRST_LOOP((BMFace *)ele);
          do {
            BM_elem_index_set(l_iter, index_loop++); /* set_ok */
          } while ((l_iter = l_iter->next) != l_first);
        }
      }

      BLI_assert(elem_offset || !update_face || index == bm->totface);
      if (update_loop) {
        BLI_assert(elem_offset || !update_loop || index_loop == bm->totloop);
      }
    }
    else {
      // printf("%s: skipping face/loop index calc!\n", __func__);
    }
  }

finally:
  bm->elem_index_dirty &= ~htype;
  if (elem_offset) {
    if (htype & BM_VERT) {
      elem_offset[0] += bm->totvert;
      if (elem_offset[0] != bm->totvert) {
        bm->elem_index_dirty |= BM_VERT;
      }
    }
    if (htype & BM_EDGE) {
      elem_offset[1] += bm->totedge;
      if (elem_offset[1] != bm->totedge) {
        bm->elem_index_dirty |= BM_EDGE;
      }
    }
    if (htype & BM_LOOP) {
      elem_offset[2] += bm->totloop;
      if (elem_offset[2] != bm->totloop) {
        bm->elem_index_dirty |= BM_LOOP;
      }
    }
    if (htype & BM_FACE) {
      elem_offset[3] += bm->totface;
      if (elem_offset[3] != bm->totface) {
        bm->elem_index_dirty |= BM_FACE;
      }
    }
  }
}

void BM_mesh_elem_index_ensure(BMesh *bm, const char htype)
{
  BM_mesh_elem_index_ensure_ex(bm, htype, NULL);
}

void BM_mesh_elem_index_validate(
    BMesh *bm, const char *location, const char *func, const char *msg_a, const char *msg_b)
{
  const char iter_types[3] = {BM_VERTS_OF_MESH, BM_EDGES_OF_MESH, BM_FACES_OF_MESH};

  const char flag_types[3] = {BM_VERT, BM_EDGE, BM_FACE};
  const char *type_names[3] = {"vert", "edge", "face"};

  BMIter iter;
  BMElem *ele;
  int i;
  bool is_any_error = 0;

  for (i = 0; i < 3; i++) {
    const bool is_dirty = (flag_types[i] & bm->elem_index_dirty) != 0;
    int index = 0;
    bool is_error = false;
    int err_val = 0;
    int err_idx = 0;

    BM_ITER_MESH (ele, &iter, bm, iter_types[i]) {
      if (!is_dirty) {
        if (BM_elem_index_get(ele) != index) {
          err_val = BM_elem_index_get(ele);
          err_idx = index;
          is_error = true;
          break;
        }
      }
      index++;
    }

    if ((is_error == true) && (is_dirty == false)) {
      is_any_error = true;
      fprintf(stderr,
              "Invalid Index: at %s, %s, %s[%d] invalid index %d, '%s', '%s'\n",
              location,
              func,
              type_names[i],
              err_idx,
              err_val,
              msg_a,
              msg_b);
    }
    else if ((is_error == false) && (is_dirty == true)) {

#if 0 /* mostly annoying */

      /* dirty may have been incorrectly set */
      fprintf(stderr,
              "Invalid Dirty: at %s, %s (%s), dirty flag was set but all index values are "
              "correct, '%s', '%s'\n",
              location,
              func,
              type_names[i],
              msg_a,
              msg_b);
#endif
    }
  }

#if 0 /* mostly annoying, even in debug mode */
#  ifdef DEBUG
  if (is_any_error == 0) {
    fprintf(stderr, "Valid Index Success: at %s, %s, '%s', '%s'\n", location, func, msg_a, msg_b);
  }
#  endif
#endif
  (void)is_any_error; /* shut up the compiler */
}

/* debug check only - no need to optimize */
#ifndef NDEBUG
bool BM_mesh_elem_table_check(BMesh *bm)
{
  BMIter iter;
  BMElem *ele;
  int i;

  if (bm->vtable && ((bm->elem_table_dirty & BM_VERT) == 0)) {
    BM_ITER_MESH_INDEX (ele, &iter, bm, BM_VERTS_OF_MESH, i) {
      if (ele != (BMElem *)bm->vtable[i]) {
        return false;
      }
    }
  }

  if (bm->etable && ((bm->elem_table_dirty & BM_EDGE) == 0)) {
    BM_ITER_MESH_INDEX (ele, &iter, bm, BM_EDGES_OF_MESH, i) {
      if (ele != (BMElem *)bm->etable[i]) {
        return false;
      }
    }
  }

  if (bm->ftable && ((bm->elem_table_dirty & BM_FACE) == 0)) {
    BM_ITER_MESH_INDEX (ele, &iter, bm, BM_FACES_OF_MESH, i) {
      if (ele != (BMElem *)bm->ftable[i]) {
        return false;
      }
    }
  }

  return true;
}
#endif

void BM_mesh_elem_table_ensure(BMesh *bm, const char htype)
{
  /* assume if the array is non-null then its valid and no need to recalc */
  const char htype_needed =
      (((bm->vtable && ((bm->elem_table_dirty & BM_VERT) == 0)) ? 0 : BM_VERT) |
       ((bm->etable && ((bm->elem_table_dirty & BM_EDGE) == 0)) ? 0 : BM_EDGE) |
       ((bm->ftable && ((bm->elem_table_dirty & BM_FACE) == 0)) ? 0 : BM_FACE)) &
      htype;

  BLI_assert((htype & ~BM_ALL_NOLOOP) == 0);

  /* in debug mode double check we didn't need to recalculate */
  BLI_assert(BM_mesh_elem_table_check(bm) == true);

  if (htype_needed == 0) {
    goto finally;
  }

  if (htype_needed & BM_VERT) {
    if (bm->vtable && bm->totvert <= bm->vtable_tot && bm->totvert * 2 >= bm->vtable_tot) {
      /* pass (re-use the array) */
    }
    else {
      if (bm->vtable) {
        MEM_freeN(bm->vtable);
      }
      bm->vtable = MEM_mallocN(sizeof(void **) * bm->totvert, "bm->vtable");
      bm->vtable_tot = bm->totvert;
    }
  }
  if (htype_needed & BM_EDGE) {
    if (bm->etable && bm->totedge <= bm->etable_tot && bm->totedge * 2 >= bm->etable_tot) {
      /* pass (re-use the array) */
    }
    else {
      if (bm->etable) {
        MEM_freeN(bm->etable);
      }
      bm->etable = MEM_mallocN(sizeof(void **) * bm->totedge, "bm->etable");
      bm->etable_tot = bm->totedge;
    }
  }
  if (htype_needed & BM_FACE) {
    if (bm->ftable && bm->totface <= bm->ftable_tot && bm->totface * 2 >= bm->ftable_tot) {
      /* pass (re-use the array) */
    }
    else {
      if (bm->ftable) {
        MEM_freeN(bm->ftable);
      }
      bm->ftable = MEM_mallocN(sizeof(void **) * bm->totface, "bm->ftable");
      bm->ftable_tot = bm->totface;
    }
  }

  if (htype_needed & BM_VERT) {
    BM_iter_as_array(bm, BM_VERTS_OF_MESH, NULL, (void **)bm->vtable, bm->totvert);
  }

  if (htype_needed & BM_EDGE) {
    BM_iter_as_array(bm, BM_EDGES_OF_MESH, NULL, (void **)bm->etable, bm->totedge);
  }

  if (htype_needed & BM_FACE) {
    BM_iter_as_array(bm, BM_FACES_OF_MESH, NULL, (void **)bm->ftable, bm->totface);
  }

finally:
  /* Only clear dirty flags when all the pointers and data are actually valid.
   * This prevents possible threading issues when dirty flag check failed but
   * data wasn't ready still.
   */
  bm->elem_table_dirty &= ~htype_needed;
}

void BM_mesh_elem_table_init(BMesh *bm, const char htype)
{
  BLI_assert((htype & ~BM_ALL_NOLOOP) == 0);

  /* force recalc */
  BM_mesh_elem_table_free(bm, BM_ALL_NOLOOP);
  BM_mesh_elem_table_ensure(bm, htype);
}

void BM_mesh_elem_table_free(BMesh *bm, const char htype)
{
  if (htype & BM_VERT) {
    MEM_SAFE_FREE(bm->vtable);
  }

  if (htype & BM_EDGE) {
    MEM_SAFE_FREE(bm->etable);
  }

  if (htype & BM_FACE) {
    MEM_SAFE_FREE(bm->ftable);
  }
}

BMVert *BM_vert_at_index_find(BMesh *bm, const int index)
{
  return BLI_mempool_findelem(bm->vpool, index);
}

BMEdge *BM_edge_at_index_find(BMesh *bm, const int index)
{
  return BLI_mempool_findelem(bm->epool, index);
}

BMFace *BM_face_at_index_find(BMesh *bm, const int index)
{
  return BLI_mempool_findelem(bm->fpool, index);
}

BMLoop *BM_loop_at_index_find(BMesh *bm, const int index)
{
  BMIter iter;
  BMFace *f;
  int i = index;
  BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
    if (i < f->len) {
      BMLoop *l_first, *l_iter;
      l_iter = l_first = BM_FACE_FIRST_LOOP(f);
      do {
        if (i == 0) {
          return l_iter;
        }
        i -= 1;
      } while ((l_iter = l_iter->next) != l_first);
    }
    i -= f->len;
  }
  return NULL;
}

BMVert *BM_vert_at_index_find_or_table(BMesh *bm, const int index)
{
  if ((bm->elem_table_dirty & BM_VERT) == 0) {
    return (index < bm->totvert) ? bm->vtable[index] : NULL;
  }
  return BM_vert_at_index_find(bm, index);
}

BMEdge *BM_edge_at_index_find_or_table(BMesh *bm, const int index)
{
  if ((bm->elem_table_dirty & BM_EDGE) == 0) {
    return (index < bm->totedge) ? bm->etable[index] : NULL;
  }
  return BM_edge_at_index_find(bm, index);
}

BMFace *BM_face_at_index_find_or_table(BMesh *bm, const int index)
{
  if ((bm->elem_table_dirty & BM_FACE) == 0) {
    return (index < bm->totface) ? bm->ftable[index] : NULL;
  }
  return BM_face_at_index_find(bm, index);
}

int BM_mesh_elem_count(BMesh *bm, const char htype)
{
  BLI_assert((htype & ~BM_ALL_NOLOOP) == 0);

  switch (htype) {
    case BM_VERT:
      return bm->totvert;
    case BM_EDGE:
      return bm->totedge;
    case BM_FACE:
      return bm->totface;
    default: {
      BLI_assert(0);
      return 0;
    }
  }
}

/**
 * Remaps the vertices, edges and/or faces of the bmesh as indicated by vert/edge/face_idx arrays
 * (xxx_idx[org_index] = new_index).
 *
 * A NULL array means no changes.
 *
 * \note
 * - Does not mess with indices, just sets elem_index_dirty flag.
 * - For verts/edges/faces only (as loops must remain "ordered" and "aligned"
 *   on a per-face basis...).
 *
 * \warning Be careful if you keep pointers to affected BM elements,
 * or arrays, when using this func!
 */
void BM_mesh_remap(BMesh *bm,
                   const uint *vert_idx,
                   const uint *edge_idx,
                   const uint *face_idx,
                   const uint *loop_idx)
{
  /* Mapping old to new pointers. */
  GHash *vptr_map = NULL, *eptr_map = NULL, *fptr_map = NULL;
  BMIter iter, iterl;
  BMVert *ve;
  BMEdge *ed;
  BMFace *fa;
  BMLoop *lo;

  if (!(vert_idx || edge_idx || face_idx)) {
    return;
  }

  BM_mesh_elem_table_ensure(
      bm, (vert_idx ? BM_VERT : 0) | (edge_idx ? BM_EDGE : 0) | (face_idx ? BM_FACE : 0));

  CustomData *cdatas[4] = {&bm->vdata, &bm->edata, &bm->ldata, &bm->pdata};

#define DO_SWAP(ci, cdata, v, vp) *(v) = *(vp);

// NOT WORKING
/* unswaps customdata blocks*/
#define DO_SWAP2(ci, cdata, v, vp) \
  void *cdold = (v)->head.data; \
  void *cdnew = (vp)->head.data; \
  *(v) = *(vp); \
  if (cdold) { \
    (v)->head.data = cdold; \
    memcpy(cdold, cdnew, bm->cdata.totsize); \
  }

  /* Remap Verts */
  if (vert_idx) {
    BMVert **verts_pool, *verts_copy, **vep;
    int i, totvert = bm->totvert;
    const uint *new_idx;
    /* Special case: Python uses custom data layers to hold PyObject references.
     * These have to be kept in place, else the PyObjects we point to, won't point back to us. */
    const int cd_vert_pyptr = CustomData_get_offset(&bm->vdata, CD_BM_ELEM_PYPTR);

    /* Init the old-to-new vert pointers mapping */
    vptr_map = BLI_ghash_ptr_new_ex("BM_mesh_remap vert pointers mapping", bm->totvert);

    /* Make a copy of all vertices. */
    verts_pool = bm->vtable;
    verts_copy = MEM_mallocN(sizeof(BMVert) * totvert, "BM_mesh_remap verts copy");
    void **pyptrs = (cd_vert_pyptr != -1) ? MEM_mallocN(sizeof(void *) * totvert, __func__) : NULL;
    for (i = totvert, ve = verts_copy + totvert - 1, vep = verts_pool + totvert - 1; i--;
         ve--, vep--) {

      *ve = **vep;

      // printf("*vep: %p, verts_pool[%d]: %p\n", *vep, i, verts_pool[i]);
      if (cd_vert_pyptr != -1) {
        void **pyptr = BM_ELEM_CD_GET_VOID_P(((BMElem *)ve), cd_vert_pyptr);
        pyptrs[i] = *pyptr;
      }
    }

    /* Copy back verts to their new place, and update old2new pointers mapping. */
    new_idx = vert_idx + totvert - 1;
    ve = verts_copy + totvert - 1;
    vep = verts_pool + totvert - 1; /* old, org pointer */
    for (i = totvert; i--; new_idx--, ve--, vep--) {
      BMVert *new_vep = verts_pool[*new_idx];

      DO_SWAP(0, vdata, new_vep, ve);

      BLI_ghash_insert(vptr_map, *vep, new_vep);
#if 0
      printf(
          "mapping vert from %d to %d (%p/%p to %p)\n", i, *new_idx, *vep, verts_pool[i], new_vep);
#endif

      if (cd_vert_pyptr != -1) {
        void **pyptr = BM_ELEM_CD_GET_VOID_P(((BMElem *)new_vep), cd_vert_pyptr);
        *pyptr = pyptrs[*new_idx];
      }
    }
    bm->elem_index_dirty |= BM_VERT;
    bm->elem_table_dirty |= BM_VERT;

    MEM_freeN(verts_copy);
    if (pyptrs) {
      MEM_freeN(pyptrs);
    }
  }

  GHash *lptr_map = NULL;

  /* Remap Loops */
  if (loop_idx) {
    BMLoop **ltable = MEM_malloc_arrayN(bm->totloop, sizeof(*ltable), "ltable");

    BMLoop *ed;
    BLI_mempool_iter liter;
    BLI_mempool_iternew(bm->lpool, &liter);
    BMLoop *l = (BMLoop *)BLI_mempool_iterstep(&liter);

    int i = 0;
    for (; l; l = (BMLoop *)BLI_mempool_iterstep(&liter), i++) {
      l->head.index = i;
      ltable[i] = l;
    }

    BMLoop **loops_pool, *loops_copy, **edl;
    int totloop = bm->totloop;
    const uint *new_idx;
    /* Special case: Python uses custom data layers to hold PyObject references.
     * These have to be kept in place, else the PyObjects we point to, won't point back to us. */
    const int cd_loop_pyptr = CustomData_get_offset(&bm->ldata, CD_BM_ELEM_PYPTR);

    /* Init the old-to-new vert pointers mapping */
    lptr_map = BLI_ghash_ptr_new_ex("BM_mesh_remap loop pointers mapping", bm->totloop);

    /* Make a copy of all vertices. */
    loops_pool = ltable;
    loops_copy = MEM_mallocN(sizeof(BMLoop) * totloop, "BM_mesh_remap loops copy");

    void **pyptrs = (cd_loop_pyptr != -1) ? MEM_mallocN(sizeof(void *) * totloop, __func__) : NULL;
    for (i = totloop, ed = loops_copy + totloop - 1, edl = loops_pool + totloop - 1; i--;
         ed--, edl--) {

      *ed = **edl;

      if (cd_loop_pyptr != -1) {
        void **pyptr = BM_ELEM_CD_GET_VOID_P(((BMElem *)ed), cd_loop_pyptr);
        pyptrs[i] = *pyptr;
      }
    }

    /* Copy back verts to their new place, and update old2new pointers mapping. */
    new_idx = loop_idx + totloop - 1;
    ed = loops_copy + totloop - 1;
    edl = loops_pool + totloop - 1; /* old, org pointer */
    for (i = totloop; i--; new_idx--, ed--, edl--) {
      BMLoop *new_edl = loops_pool[*new_idx];
      *new_edl = *ed;

      DO_SWAP(2, ldata, new_edl, ed);

      BLI_ghash_insert(lptr_map, *edl, new_edl);
#if 0
      printf(
          "mapping loop from %d to %d (%p/%p to %p)\n", i, *new_idx, *edl, loops_pool[i], new_edl);
#endif
      if (cd_loop_pyptr != -1) {
        void **pyptr = BM_ELEM_CD_GET_VOID_P(((BMElem *)new_edl), cd_loop_pyptr);
        *pyptr = pyptrs[*new_idx];
      }
    }

    MEM_SAFE_FREE(ltable);
    MEM_SAFE_FREE(loops_copy);
    MEM_SAFE_FREE(pyptrs);
  }

  /* Remap Edges */
  if (edge_idx) {
    BMEdge **edges_pool, *edges_copy, **edp;
    int i, totedge = bm->totedge;
    const uint *new_idx;
    /* Special case: Python uses custom data layers to hold PyObject references.
     * These have to be kept in place, else the PyObjects we point to, won't point back to us. */
    const int cd_edge_pyptr = CustomData_get_offset(&bm->edata, CD_BM_ELEM_PYPTR);

    /* Init the old-to-new vert pointers mapping */
    eptr_map = BLI_ghash_ptr_new_ex("BM_mesh_remap edge pointers mapping", bm->totedge);

    /* Make a copy of all vertices. */
    edges_pool = bm->etable;
    edges_copy = MEM_mallocN(sizeof(BMEdge) * totedge, "BM_mesh_remap edges copy");
    void **pyptrs = (cd_edge_pyptr != -1) ? MEM_mallocN(sizeof(void *) * totedge, __func__) : NULL;
    for (i = totedge, ed = edges_copy + totedge - 1, edp = edges_pool + totedge - 1; i--;
         ed--, edp--) {
      *ed = **edp;
      if (cd_edge_pyptr != -1) {
        void **pyptr = BM_ELEM_CD_GET_VOID_P(((BMElem *)ed), cd_edge_pyptr);
        pyptrs[i] = *pyptr;
      }
    }

    /* Copy back verts to their new place, and update old2new pointers mapping. */
    new_idx = edge_idx + totedge - 1;
    ed = edges_copy + totedge - 1;
    edp = edges_pool + totedge - 1; /* old, org pointer */
    for (i = totedge; i--; new_idx--, ed--, edp--) {
      BMEdge *new_edp = edges_pool[*new_idx];

      DO_SWAP(1, edata, new_edp, ed);

      if (new_edp->l && lptr_map) {
        new_edp->l = BLI_ghash_lookup(lptr_map, (BMLoop *)new_edp->l);
      }

      BLI_ghash_insert(eptr_map, *edp, new_edp);
#if 0
      printf(
          "mapping edge from %d to %d (%p/%p to %p)\n", i, *new_idx, *edp, edges_pool[i], new_edp);
#endif
      if (cd_edge_pyptr != -1) {
        void **pyptr = BM_ELEM_CD_GET_VOID_P(((BMElem *)new_edp), cd_edge_pyptr);
        *pyptr = pyptrs[*new_idx];
      }
    }
    bm->elem_index_dirty |= BM_EDGE;
    bm->elem_table_dirty |= BM_EDGE;

    MEM_freeN(edges_copy);
    if (pyptrs) {
      MEM_freeN(pyptrs);
    }
  }

  /* Remap Faces */
  if (face_idx) {
    BMFace **faces_pool, *faces_copy, **fap;
    int i, totface = bm->totface;
    const uint *new_idx;
    /* Special case: Python uses custom data layers to hold PyObject references.
     * These have to be kept in place, else the PyObjects we point to, won't point back to us. */
    const int cd_poly_pyptr = CustomData_get_offset(&bm->pdata, CD_BM_ELEM_PYPTR);

    /* Init the old-to-new vert pointers mapping */
    fptr_map = BLI_ghash_ptr_new_ex("BM_mesh_remap face pointers mapping", bm->totface);

    /* Make a copy of all vertices. */
    faces_pool = bm->ftable;
    faces_copy = MEM_mallocN(sizeof(BMFace) * totface, "BM_mesh_remap faces copy");
    void **pyptrs = (cd_poly_pyptr != -1) ? MEM_mallocN(sizeof(void *) * totface, __func__) : NULL;
    for (i = totface, fa = faces_copy + totface - 1, fap = faces_pool + totface - 1; i--;
         fa--, fap--) {
      *fa = **fap;
      if (cd_poly_pyptr != -1) {
        void **pyptr = BM_ELEM_CD_GET_VOID_P(((BMElem *)fa), cd_poly_pyptr);
        pyptrs[i] = *pyptr;
      }
    }

    /* Copy back verts to their new place, and update old2new pointers mapping. */
    new_idx = face_idx + totface - 1;
    fa = faces_copy + totface - 1;
    fap = faces_pool + totface - 1; /* old, org pointer */
    for (i = totface; i--; new_idx--, fa--, fap--) {
      BMFace *new_fap = faces_pool[*new_idx];
      *new_fap = *fa;
      BLI_ghash_insert(fptr_map, *fap, new_fap);

      DO_SWAP(3, pdata, new_fap, fa);

      if (lptr_map) {
        new_fap->l_first = BLI_ghash_lookup(lptr_map, (void *)new_fap->l_first);

        BMLoop *l = new_fap->l_first;

        do {
          l->next = BLI_ghash_lookup(lptr_map, (void *)l->next);
          l->prev = BLI_ghash_lookup(lptr_map, (void *)l->prev);
          l->radial_next = BLI_ghash_lookup(lptr_map, (void *)l->radial_next);
          l->radial_prev = BLI_ghash_lookup(lptr_map, (void *)l->radial_prev);

          l = l->next;
        } while (l != new_fap->l_first);
      }

      if (cd_poly_pyptr != -1) {
        void **pyptr = BM_ELEM_CD_GET_VOID_P(((BMElem *)new_fap), cd_poly_pyptr);
        *pyptr = pyptrs[*new_idx];
      }
    }

    bm->elem_index_dirty |= BM_FACE | BM_LOOP;
    bm->elem_table_dirty |= BM_FACE;

    MEM_freeN(faces_copy);
    if (pyptrs) {
      MEM_freeN(pyptrs);
    }
  }

  /* And now, fix all vertices/edges/faces/loops pointers! */
  /* Verts' pointers, only edge pointers... */
  if (eptr_map) {
    BM_ITER_MESH (ve, &iter, bm, BM_VERTS_OF_MESH) {
      // printf("Vert e: %p -> %p\n", ve->e, BLI_ghash_lookup(eptr_map, ve->e));
      if (ve->e) {
        ve->e = BLI_ghash_lookup(eptr_map, ve->e);
        BLI_assert(ve->e);
      }
    }
  }

  /* Edges' pointers, only vert pointers (as we don't mess with loops!),
   * and - ack! - edge pointers,
   * as we have to handle disk-links. */
  if (vptr_map || eptr_map) {
    BM_ITER_MESH (ed, &iter, bm, BM_EDGES_OF_MESH) {
      if (vptr_map) {
#if 0
        printf("Edge v1: %p -> %p\n", ed->v1, BLI_ghash_lookup(vptr_map, ed->v1));
        printf("Edge v2: %p -> %p\n", ed->v2, BLI_ghash_lookup(vptr_map, ed->v2));
#endif
        ed->v1 = BLI_ghash_lookup(vptr_map, ed->v1);
        ed->v2 = BLI_ghash_lookup(vptr_map, ed->v2);
        BLI_assert(ed->v1);
        BLI_assert(ed->v2);
      }
      if (eptr_map) {
#if 0
        printf("Edge v1_disk_link prev: %p -> %p\n",
               ed->v1_disk_link.prev,
               BLI_ghash_lookup(eptr_map, ed->v1_disk_link.prev));
        printf("Edge v1_disk_link next: %p -> %p\n",
               ed->v1_disk_link.next,
               BLI_ghash_lookup(eptr_map, ed->v1_disk_link.next));
        printf("Edge v2_disk_link prev: %p -> %p\n",
               ed->v2_disk_link.prev,
               BLI_ghash_lookup(eptr_map, ed->v2_disk_link.prev));
        printf("Edge v2_disk_link next: %p -> %p\n",
               ed->v2_disk_link.next,
               BLI_ghash_lookup(eptr_map, ed->v2_disk_link.next));
#endif
        ed->v1_disk_link.prev = BLI_ghash_lookup(eptr_map, ed->v1_disk_link.prev);
        ed->v1_disk_link.next = BLI_ghash_lookup(eptr_map, ed->v1_disk_link.next);
        ed->v2_disk_link.prev = BLI_ghash_lookup(eptr_map, ed->v2_disk_link.prev);
        ed->v2_disk_link.next = BLI_ghash_lookup(eptr_map, ed->v2_disk_link.next);
        BLI_assert(ed->v1_disk_link.prev);
        BLI_assert(ed->v1_disk_link.next);
        BLI_assert(ed->v2_disk_link.prev);
        BLI_assert(ed->v2_disk_link.next);
      }
    }
  }

  /* Faces' pointers (loops, in fact), always needed... */
  BM_ITER_MESH (fa, &iter, bm, BM_FACES_OF_MESH) {
    BM_ITER_ELEM (lo, &iterl, fa, BM_LOOPS_OF_FACE) {
      if (vptr_map) {
        // printf("Loop v: %p -> %p\n", lo->v, BLI_ghash_lookup(vptr_map, lo->v));
        lo->v = BLI_ghash_lookup(vptr_map, lo->v);
        BLI_assert(lo->v);
      }
      if (eptr_map) {
        // printf("Loop e: %p -> %p\n", lo->e, BLI_ghash_lookup(eptr_map, lo->e));
        lo->e = BLI_ghash_lookup(eptr_map, lo->e);
        BLI_assert(lo->e);
      }
      if (fptr_map) {
        // printf("Loop f: %p -> %p\n", lo->f, BLI_ghash_lookup(fptr_map, lo->f));
        lo->f = BLI_ghash_lookup(fptr_map, lo->f);
        BLI_assert(lo->f);
      }
    }
  }

  /* Selection history */
  {
    BMEditSelection *ese;
    for (ese = bm->selected.first; ese; ese = ese->next) {
      switch (ese->htype) {
        case BM_VERT:
          if (vptr_map) {
            ese->ele = BLI_ghash_lookup(vptr_map, ese->ele);
            BLI_assert(ese->ele);
          }
          break;
        case BM_EDGE:
          if (eptr_map) {
            ese->ele = BLI_ghash_lookup(eptr_map, ese->ele);
            BLI_assert(ese->ele);
          }
          break;
        case BM_FACE:
          if (fptr_map) {
            ese->ele = BLI_ghash_lookup(fptr_map, ese->ele);
            BLI_assert(ese->ele);
          }
          break;
      }
    }
  }

  if (fptr_map) {
    if (bm->act_face) {
      bm->act_face = BLI_ghash_lookup(fptr_map, bm->act_face);
      BLI_assert(bm->act_face);
    }
  }

  if (vptr_map) {
    BLI_ghash_free(vptr_map, NULL, NULL);
  }
  if (eptr_map) {
    BLI_ghash_free(eptr_map, NULL, NULL);
  }
  if (fptr_map) {
    BLI_ghash_free(fptr_map, NULL, NULL);
  }

  // regenerate idmap
  if ((bm->idmap.flag & BM_HAS_IDS) && (bm->idmap.flag & BM_HAS_ID_MAP) && bm->idmap.map) {
    memset(bm->idmap.map, 0, sizeof(void *) * bm->idmap.map_size);

    char iters[4] = {BM_VERTS_OF_MESH, BM_EDGES_OF_MESH, 0, BM_FACES_OF_MESH};
    const bool have_loop = bm->idmap.flag & BM_LOOP;

    for (int i = 0; i < 4; i++) {
      int type = 1 << i;

      if (type == BM_LOOP) {  // handle loops with faces
        continue;
      }

      int cd_id = CustomData_get_offset(cdatas[i], CD_MESH_ID);
      int cd_loop_id = CustomData_get_offset(&bm->ldata, CD_MESH_ID);

      BMIter iter;
      BMElem *elem;

      if (cd_id < 0 && !(type == BM_FACE && have_loop)) {
        continue;
      }

      BM_ITER_MESH (elem, &iter, bm, iters[i]) {
        if (type == BM_FACE && have_loop) {
          BMFace *f = (BMFace *)elem;
          BMLoop *l = f->l_first;

          do {
            int id_loop = BM_ELEM_CD_GET_INT(l, cd_loop_id);

            if (bm->idmap.ghash) {
              void **l_val;

              BLI_ghash_ensure_p(bm->idmap.ghash, POINTER_FROM_INT(id_loop), &l_val);
              *l_val = (void *)l;
            }
            else {
              bm->idmap.map[id_loop] = (BMElem *)l;
            }
          } while ((l = l->next) != f->l_first);
        }

        if (cd_id < 0) {
          continue;
        }

        int id = BM_ELEM_CD_GET_INT(elem, cd_id);

        if (bm->idmap.ghash) {
          void **val;

          BLI_ghash_ensure_p(bm->idmap.ghash, POINTER_FROM_INT(id), &val);
          *val = (void *)elem;
        }
        else {
          bm->idmap.map[id] = elem;
        }
      }
    }
  }
}

void BM_mesh_rebuild(BMesh *bm,
                     const struct BMeshCreateParams *params,
                     BLI_mempool *vpool_dst,
                     BLI_mempool *epool_dst,
                     BLI_mempool *lpool_dst,
                     BLI_mempool *fpool_dst)
{
  const char remap = (vpool_dst ? BM_VERT : 0) | (epool_dst ? BM_EDGE : 0) |
                     (lpool_dst ? BM_LOOP : 0) | (fpool_dst ? BM_FACE : 0);

  BMVert **vtable_dst = (remap & BM_VERT) ? MEM_mallocN(bm->totvert * sizeof(BMVert *), __func__) :
                                            NULL;
  BMEdge **etable_dst = (remap & BM_EDGE) ? MEM_mallocN(bm->totedge * sizeof(BMEdge *), __func__) :
                                            NULL;
  BMLoop **ltable_dst = (remap & BM_LOOP) ? MEM_mallocN(bm->totloop * sizeof(BMLoop *), __func__) :
                                            NULL;
  BMFace **ftable_dst = (remap & BM_FACE) ? MEM_mallocN(bm->totface * sizeof(BMFace *), __func__) :
                                            NULL;

  const bool use_toolflags = params->use_toolflags;

  if (remap & BM_VERT) {
    BMIter iter;
    int index;
    BMVert *v_src;
    BM_ITER_MESH_INDEX (v_src, &iter, bm, BM_VERTS_OF_MESH, index) {
      BMVert *v_dst = BLI_mempool_alloc(vpool_dst);
      memcpy(v_dst, v_src, sizeof(BMVert));
      if (use_toolflags) {
        MToolFlags *flags = (MToolFlags *)BM_ELEM_CD_GET_VOID_P(
            v_dst, bm->vdata.layers[bm->vdata.typemap[CD_TOOLFLAGS]].offset);

        flags->flag = bm->vtoolflagpool ? BLI_mempool_calloc(bm->vtoolflagpool) : NULL;
      }

      vtable_dst[index] = v_dst;
      BM_elem_index_set(v_src, index); /* set_ok */
    }
  }

  if (remap & BM_EDGE) {
    BMIter iter;
    int index;
    BMEdge *e_src;
    BM_ITER_MESH_INDEX (e_src, &iter, bm, BM_EDGES_OF_MESH, index) {
      BMEdge *e_dst = BLI_mempool_alloc(epool_dst);
      memcpy(e_dst, e_src, sizeof(BMEdge));
      if (use_toolflags) {
        MToolFlags *flags = (MToolFlags *)BM_ELEM_CD_GET_VOID_P(
            e_dst, bm->edata.layers[bm->edata.typemap[CD_TOOLFLAGS]].offset);

        flags->flag = bm->etoolflagpool ? BLI_mempool_calloc(bm->etoolflagpool) : NULL;
      }

      etable_dst[index] = e_dst;
      BM_elem_index_set(e_src, index); /* set_ok */
    }
  }

  if (remap & (BM_LOOP | BM_FACE)) {
    BMIter iter;
    int index, index_loop = 0;
    BMFace *f_src;
    BM_ITER_MESH_INDEX (f_src, &iter, bm, BM_FACES_OF_MESH, index) {

      if (remap & BM_FACE) {
        BMFace *f_dst = BLI_mempool_alloc(fpool_dst);
        memcpy(f_dst, f_src, sizeof(BMFace));

        if (use_toolflags) {
          MToolFlags *flags = (MToolFlags *)BM_ELEM_CD_GET_VOID_P(
              f_dst, bm->pdata.layers[bm->pdata.typemap[CD_TOOLFLAGS]].offset);

          flags->flag = bm->ftoolflagpool ? BLI_mempool_calloc(bm->ftoolflagpool) : NULL;
        }

        ftable_dst[index] = f_dst;
        BM_elem_index_set(f_src, index); /* set_ok */
      }

      /* handle loops */
      if (remap & BM_LOOP) {
        BMLoop *l_iter_src, *l_first_src;
        l_iter_src = l_first_src = BM_FACE_FIRST_LOOP((BMFace *)f_src);
        do {
          BMLoop *l_dst = BLI_mempool_alloc(lpool_dst);
          memcpy(l_dst, l_iter_src, sizeof(BMLoop));
          ltable_dst[index_loop] = l_dst;
          BM_elem_index_set(l_iter_src, index_loop++); /* set_ok */
        } while ((l_iter_src = l_iter_src->next) != l_first_src);
      }
    }
  }

#define MAP_VERT(ele) vtable_dst[BM_elem_index_get(ele)]
#define MAP_EDGE(ele) etable_dst[BM_elem_index_get(ele)]
#define MAP_LOOP(ele) ltable_dst[BM_elem_index_get(ele)]
#define MAP_FACE(ele) ftable_dst[BM_elem_index_get(ele)]

#define REMAP_VERT(ele) \
  { \
    if (remap & BM_VERT) { \
      ele = MAP_VERT(ele); \
    } \
  } \
  ((void)0)
#define REMAP_EDGE(ele) \
  { \
    if (remap & BM_EDGE) { \
      ele = MAP_EDGE(ele); \
    } \
  } \
  ((void)0)
#define REMAP_LOOP(ele) \
  { \
    if (remap & BM_LOOP) { \
      ele = MAP_LOOP(ele); \
    } \
  } \
  ((void)0)
#define REMAP_FACE(ele) \
  { \
    if (remap & BM_FACE) { \
      ele = MAP_FACE(ele); \
    } \
  } \
  ((void)0)

  /* verts */
  {
    for (int i = 0; i < bm->totvert; i++) {
      BMVert *v = vtable_dst[i];
      if (v->e) {
        REMAP_EDGE(v->e);
      }
    }
  }

  /* edges */
  {
    for (int i = 0; i < bm->totedge; i++) {
      BMEdge *e = etable_dst[i];
      REMAP_VERT(e->v1);
      REMAP_VERT(e->v2);
      REMAP_EDGE(e->v1_disk_link.next);
      REMAP_EDGE(e->v1_disk_link.prev);
      REMAP_EDGE(e->v2_disk_link.next);
      REMAP_EDGE(e->v2_disk_link.prev);
      if (e->l) {
        REMAP_LOOP(e->l);
      }
    }
  }

  /* faces */
  {
    for (int i = 0; i < bm->totface; i++) {
      BMFace *f = ftable_dst[i];
      REMAP_LOOP(f->l_first);

      {
        BMLoop *l_iter, *l_first;
        l_iter = l_first = BM_FACE_FIRST_LOOP((BMFace *)f);
        do {
          REMAP_VERT(l_iter->v);
          REMAP_EDGE(l_iter->e);
          REMAP_FACE(l_iter->f);

          REMAP_LOOP(l_iter->radial_next);
          REMAP_LOOP(l_iter->radial_prev);
          REMAP_LOOP(l_iter->next);
          REMAP_LOOP(l_iter->prev);
        } while ((l_iter = l_iter->next) != l_first);
      }
    }
  }

  LISTBASE_FOREACH (BMEditSelection *, ese, &bm->selected) {
    switch (ese->htype) {
      case BM_VERT:
        if (remap & BM_VERT) {
          ese->ele = (BMElem *)MAP_VERT(ese->ele);
        }
        break;
      case BM_EDGE:
        if (remap & BM_EDGE) {
          ese->ele = (BMElem *)MAP_EDGE(ese->ele);
        }
        break;
      case BM_FACE:
        if (remap & BM_FACE) {
          ese->ele = (BMElem *)MAP_FACE(ese->ele);
        }
        break;
    }
  }

  if (bm->act_face) {
    REMAP_FACE(bm->act_face);
  }

#undef MAP_VERT
#undef MAP_EDGE
#undef MAP_LOOP
#undef MAP_EDGE

#undef REMAP_VERT
#undef REMAP_EDGE
#undef REMAP_LOOP
#undef REMAP_EDGE

  /* Cleanup, re-use local tables if the current mesh had tables allocated.
   * could use irrespective but it may use more memory than the caller wants
   * (and not be needed). */
  if (remap & BM_VERT) {
    if (bm->vtable) {
      SWAP(BMVert **, vtable_dst, bm->vtable);
      bm->vtable_tot = bm->totvert;
      bm->elem_table_dirty &= ~BM_VERT;
    }
    MEM_freeN(vtable_dst);
    BLI_mempool_destroy(bm->vpool);
    bm->vpool = vpool_dst;
  }

  if (remap & BM_EDGE) {
    if (bm->etable) {
      SWAP(BMEdge **, etable_dst, bm->etable);
      bm->etable_tot = bm->totedge;
      bm->elem_table_dirty &= ~BM_EDGE;
    }
    MEM_freeN(etable_dst);
    BLI_mempool_destroy(bm->epool);
    bm->epool = epool_dst;
  }

  if (remap & BM_LOOP) {
    /* no loop table */
    MEM_freeN(ltable_dst);
    BLI_mempool_destroy(bm->lpool);
    bm->lpool = lpool_dst;
  }

  if (remap & BM_FACE) {
    if (bm->ftable) {
      SWAP(BMFace **, ftable_dst, bm->ftable);
      bm->ftable_tot = bm->totface;
      bm->elem_table_dirty &= ~BM_FACE;
    }
    MEM_freeN(ftable_dst);
    BLI_mempool_destroy(bm->fpool);
    bm->fpool = fpool_dst;
  }

  bm_rebuild_idmap(bm);
}

void bm_alloc_toolflags_cdlayers(BMesh *bm, bool set_elems)
{
  CustomData *cdatas[3] = {&bm->vdata, &bm->edata, &bm->pdata};
  int iters[3] = {BM_VERTS_OF_MESH, BM_EDGES_OF_MESH, BM_FACES_OF_MESH};

  for (int i = 0; i < 3; i++) {
    CustomData *cdata = cdatas[i];
    int cd_tflags = CustomData_get_offset(cdata, CD_TOOLFLAGS);

    if (cd_tflags == -1) {
      if (set_elems) {
        BM_data_layer_add(bm, cdata, CD_TOOLFLAGS);
      }
      else {
        CustomData_add_layer(cdata, CD_TOOLFLAGS, CD_ASSIGN, NULL, 0);
      }

      int idx = CustomData_get_layer_index(cdata, CD_TOOLFLAGS);

      cdata->layers[idx].flag |= CD_FLAG_TEMPORARY | CD_FLAG_NOCOPY | CD_FLAG_ELEM_NOCOPY;
      cd_tflags = cdata->layers[idx].offset;

      if (set_elems) {
        BMIter iter;
        BMElem *elem;

        BM_ITER_MESH (elem, &iter, bm, iters[i]) {
          MToolFlags *flags = (MToolFlags *)BM_ELEM_CD_GET_VOID_P(elem, cd_tflags);

          flags->flag = NULL;
        }
      }
    }
  }
}

static void bm_alloc_toolflags(BMesh *bm)
{
  bm_alloc_toolflags_cdlayers(bm, true);

  CustomData *cdatas[3] = {&bm->vdata, &bm->edata, &bm->pdata};
  BLI_mempool *flagpools[3] = {bm->vtoolflagpool, bm->etoolflagpool, bm->ftoolflagpool};
  BLI_mempool *elempools[3] = {bm->vpool, bm->epool, bm->fpool};

  for (int i = 0; i < 3; i++) {
    CustomData *cdata = cdatas[i];
    int cd_tflags = CustomData_get_offset(cdata, CD_TOOLFLAGS);

    BLI_mempool_iter iter;
    BLI_mempool_iternew(elempools[i], &iter);
    BMElem *elem = (BMElem *)BLI_mempool_iterstep(&iter);

    for (; elem; elem = (BMElem *)BLI_mempool_iterstep(&iter)) {
      MToolFlags *flags = (MToolFlags *)BM_ELEM_CD_GET_VOID_P(elem, cd_tflags);

      flags->flag = BLI_mempool_calloc(flagpools[i]);
    }
  }
}

void BM_mesh_toolflags_set(BMesh *bm, bool use_toolflags)
{
  if (bm->use_toolflags == use_toolflags) {
    return;
  }

  if (use_toolflags == false) {
    BLI_mempool_destroy(bm->vtoolflagpool);
    BLI_mempool_destroy(bm->etoolflagpool);
    BLI_mempool_destroy(bm->ftoolflagpool);

    bm->vtoolflagpool = NULL;
    bm->etoolflagpool = NULL;
    bm->ftoolflagpool = NULL;

    BM_data_layer_free(bm, &bm->vdata, CD_TOOLFLAGS);
    BM_data_layer_free(bm, &bm->edata, CD_TOOLFLAGS);
    BM_data_layer_free(bm, &bm->pdata, CD_TOOLFLAGS);
  }
  else {
    bm_alloc_toolflags_cdlayers(bm, true);
  }

  bm->use_toolflags = use_toolflags;

  if (use_toolflags) {
    BM_mesh_elem_toolflags_ensure(bm);
  }
}

/* -------------------------------------------------------------------- */
/** \name BMesh Coordinate Access
 * \{ */

void BM_mesh_vert_coords_get(BMesh *bm, float (*vert_coords)[3])
{
  BMIter iter;
  BMVert *v;
  int i;
  BM_ITER_MESH_INDEX (v, &iter, bm, BM_VERTS_OF_MESH, i) {
    copy_v3_v3(vert_coords[i], v->co);
  }
}

float (*BM_mesh_vert_coords_alloc(BMesh *bm, int *r_vert_len))[3]
{
  float(*vert_coords)[3] = MEM_mallocN(bm->totvert * sizeof(*vert_coords), __func__);
  BM_mesh_vert_coords_get(bm, vert_coords);
  *r_vert_len = bm->totvert;
  return vert_coords;
}

void BM_mesh_vert_coords_apply(BMesh *bm, const float (*vert_coords)[3])
{
  BMIter iter;
  BMVert *v;
  int i;
  BM_ITER_MESH_INDEX (v, &iter, bm, BM_VERTS_OF_MESH, i) {
    copy_v3_v3(v->co, vert_coords[i]);
  }
}

void BM_mesh_vert_coords_apply_with_mat4(BMesh *bm,
                                         const float (*vert_coords)[3],
                                         const float mat[4][4])
{
  BMIter iter;
  BMVert *v;
  int i;
  BM_ITER_MESH_INDEX (v, &iter, bm, BM_VERTS_OF_MESH, i) {
    mul_v3_m4v3(v->co, mat, vert_coords[i]);
  }
}

void bm_swap_ids(BMesh *bm, BMElem *e1, BMElem *e2)
{
  int cd_id = bm->idmap.cd_id_off[e1->head.htype];

  if (cd_id < 0) {
    return;
  }

  int id1 = BM_ELEM_CD_GET_INT(e1, cd_id);
  int id2 = BM_ELEM_CD_GET_INT(e2, cd_id);

  if (bm->idmap.map) {
    SWAP(BMElem *, bm->idmap.map[id1], bm->idmap.map[id2]);
  }
  else if (bm->idmap.ghash) {
    void **val1, **val2;

    BLI_ghash_ensure_p(bm->idmap.ghash, POINTER_FROM_INT(id1), &val1);
    BLI_ghash_ensure_p(bm->idmap.ghash, POINTER_FROM_INT(id2), &val2);

    *val1 = (void *)e2;
    *val2 = (void *)e1;
  }
}
static void bm_swap_elements_post(BMesh *bm, CustomData *cdata, BMElem *e1, BMElem *e2)
{
  // unswap customdata pointers
  SWAP(void *, e1->head.data, e2->head.data);

  // swap contents of customdata instead
  bm_swap_cd_data(e1->head.htype, bm, cdata, e1->head.data, e2->head.data);

  // unswap index
  SWAP(int, e1->head.index, e2->head.index);

  bm_swap_ids(bm, e1, e2);
}

void BM_swap_verts(BMesh *bm, BMVert *v1, BMVert *v2)
{
  if (v1 == v2) {
    return;
  }

  BMLoop **ls1 = NULL;
  BLI_array_staticdeclare(ls1, 64);
  BMLoop **ls2 = NULL;
  BLI_array_staticdeclare(ls2, 64);

  BMEdge **es1 = NULL;
  int *sides1 = NULL;

  BLI_array_staticdeclare(es1, 32);
  BLI_array_staticdeclare(sides1, 32);

  BMEdge **es2 = NULL;
  int *sides2 = NULL;

  BLI_array_staticdeclare(es2, 32);
  BLI_array_staticdeclare(sides2, 32);

  for (int i = 0; i < 2; i++) {
    BMVert *v = i ? v2 : v1;

    BMEdge *e = v->e, *starte = e;

    if (!e) {
      continue;
    }

    // int count = 0;

    do {
      // if (count++ > 10000) {
      // printf("error!\n");
      //        break;
      //    }

      int side = 0;
      if (e->v1 == v) {
        side |= 1;
      }

      if (e->v2 == v) {
        side |= 2;
      }

      if (i) {
        BLI_array_append(es2, e);
        BLI_array_append(sides2, side);
      }
      else {
        BLI_array_append(es1, e);
        BLI_array_append(sides1, side);
      }
    } while ((e = BM_DISK_EDGE_NEXT(e, v)) != starte);
  }

  for (int i = 0; i < 2; i++) {
    BMVert *v = i ? v2 : v1;
    BMVert *v_2 = i ? v1 : v2;

    BMEdge **es = i ? es2 : es1;
    int elen = i ? BLI_array_len(es2) : BLI_array_len(es1);
    int *sides = i ? sides2 : sides1;

    for (int j = 0; j < elen; j++) {
      BMEdge *e = es[j];
      int side = sides[j];

      // if (side == 3) {
      // printf("edge had duplicate verts!\n");
      //}

      if (side & 1) {
        e->v1 = v_2;
      }

      if (side & 2) {
        e->v2 = v_2;
      }

#if 1
      BMLoop *l = e->l;
      if (l) {

        do {
          BMLoop *l2 = l;

          do {
            if (l2->v == v) {
              if (i) {
                BLI_array_append(ls2, l2);
              }
              else {
                BLI_array_append(ls1, l2);
              }
            }
          } while ((l2 = l2->next) != l);
        } while ((l = l->radial_next) != e->l);
      }
#endif
      // e = enext;
    }  // while (e != starte);
  }

  for (int i = 0; i < 2; i++) {
    BMVert *v_2 = i ? v1 : v2;

    BMLoop **ls = i ? ls2 : ls1;

    int llen = i ? BLI_array_len(ls2) : BLI_array_len(ls1);

    for (int j = 0; j < llen; j++) {
      ls[j]->v = v_2;
    }
  }

  BLI_array_free(ls1);
  BLI_array_free(ls2);
  // BMVert tmp = *v1;
  //*v1 = *v2;
  //*v2 = tmp;

  SWAP(BMVert, (*v1), (*v2));
  // swap contents of customdata, don't swap pointers
  bm_swap_elements_post(bm, &bm->vdata, (BMElem *)v1, (BMElem *)v2);

  bm->elem_table_dirty |= BM_VERT;
  bm->elem_index_dirty |= BM_VERT;

  BLI_array_free(es1);
  BLI_array_free(sides1);
  BLI_array_free(es2);
  BLI_array_free(sides2);
}

void BM_swap_edges(BMesh *bm, BMEdge *e1, BMEdge *e2)
{
  for (int i = 0; i < 2; i++) {
    BMEdge *e = i ? e2 : e1;
    BMEdge *e_2 = i ? e1 : e2;

    for (int j = 0; j < 2; j++) {
      BMVert *v = j ? e->v2 : e->v1;

      if (v->e == e) {
        v->e = e_2;
      }
    }

    BMLoop *l = e->l;
    if (l) {
      do {
        l->e = e_2;
      } while ((l = l->radial_next) != e->l);
    }
  }

  SWAP(BMEdge, *e1, *e2);
  // swap contents of customdata, don't swap pointers
  bm_swap_elements_post(bm, &bm->edata, (BMElem *)e1, (BMElem *)e2);
}

void BM_swap_loops(BMesh *bm, BMLoop *l1, BMLoop *l2)
{
  for (int i = 0; i < 2; i++) {
    BMLoop *l = i ? l2 : l1;

    l->prev->next = l2;
    l->next->prev = l2;

    if (l != l->radial_next) {
      l->radial_next->radial_prev = l2;
      l->radial_prev->radial_next = l2;
    }

    if (l == l->e->l) {
      l->e->l = l2;
    }

    if (l == l->f->l_first) {
      l->f->l_first = l2;
    }
  }

  // swap contents of customdata, don't swap pointers
  SWAP(BMLoop, *l1, *l2);
  // swap contents of customdata, don't swap pointers
  bm_swap_elements_post(bm, &bm->ldata, (BMElem *)l1, (BMElem *)l2);
}

// memory coherence defragmentation

#ifndef ABSLL
#  define ABSLL(a) ((a) < 0LL ? -(a) : (a))
#endif

#define DEFRAG_FLAG BM_ELEM_TAG_ALT

bool BM_defragment_vertex(BMesh *bm,
                          BMVert *v,
                          RNG *rand,
                          void (*on_vert_swap)(BMVert *a, BMVert *b, void *userdata),
                          void *userdata)
{
  BMEdge *e = v->e;

#if 1
  int cd_vcol = CustomData_get_offset(&bm->vdata, CD_PROP_COLOR);

  if (cd_vcol >= 0) {
    float *color = BM_ELEM_CD_GET_VOID_P(v, cd_vcol);
    int idx = BLI_mempool_find_real_index(bm->vpool, (void *)v);
    int size = BLI_mempool_get_size(bm->vpool);

    float f = (float)idx / (float)size / 2.0f;

    color[0] = color[1] = color[2] = f;
    color[3] = 1.0f;
  }
#endif

  // return false;

  // return false;

  // BM_mesh_elem_table_ensure(bm, BM_VERT|BM_EDGE|BM_FACE);
  if (!e) {
    return false;
  }

  bool bad = false;
  int limit = 128;

  int vlimit = sizeof(BMVert *) * limit;
  int elimit = sizeof(BMEdge *) * limit;
  int llimit = sizeof(BMLoop *) * limit;
  // int flimit = sizeof(BMFace *) * limit;

  intptr_t iv = (intptr_t)v;

  BMEdge *laste = NULL;
  do {
    BMVert *v2 = BM_edge_other_vert(e, v);
    intptr_t iv2 = (intptr_t)v2;
    intptr_t ie = (intptr_t)e;

    v2->head.hflag &= DEFRAG_FLAG;
    e->head.hflag &= ~DEFRAG_FLAG;

    if (ABSLL(iv2 - iv) > vlimit) {
      bad = true;
      break;
    }

    if (laste) {
      intptr_t ilaste = (intptr_t)laste;
      if (ABSLL(ilaste - ie) > elimit) {
        bad = true;
        break;
      }
    }

    BMLoop *l = e->l;
    if (l) {
      do {
        intptr_t il = (intptr_t)l;
        intptr_t ilnext = (intptr_t)l->next;

        if (ABSLL(il - ilnext) > llimit) {
          bad = true;
          break;
        }

        BMLoop *l2 = l->f->l_first;
        do {
          l2->head.hflag &= ~DEFRAG_FLAG;
        } while ((l2 = l2->next) != l->f->l_first);

        l2->f->head.hflag &= ~DEFRAG_FLAG;

        l = l->radial_next;
      } while (l != e->l);
    }
    laste = e;
  } while (!bad && (e = BM_DISK_EDGE_NEXT(e, v)) != v->e);

  float prob = 1.0;

  if (!bad || BLI_rng_get_float(rand) > prob) {
    return false;
  }

  // find sort candidates
  // BLI_mempool_find_elems_fuzzy

  int vidx = BLI_mempool_find_real_index(bm->vpool, (void *)v);
  const int count = 5;
  BMVert **elems = BLI_array_alloca(elems, count);

  do {
    BMVert *v2 = BM_edge_other_vert(e, v);
    int totelem = BLI_mempool_find_elems_fuzzy(bm->vpool, vidx, 4, (void **)elems, count);

    for (int i = 0; i < totelem; i++) {
      if (elems[i] == v2 || elems[i] == v) {
        continue;
      }

      elems[i]->head.hflag &= ~DEFRAG_FLAG;
    }

    bool ok = false;

    for (int i = 0; i < totelem; i++) {
      if (elems[i] == v2 || elems[i] == v || (elems[i]->head.hflag & DEFRAG_FLAG)) {
        continue;
      }

      if (elems[i]->head.htype != BM_VERT) {
        printf("ERROR!\n");
      }
      // found one
      v2->head.hflag |= DEFRAG_FLAG;
      elems[i]->head.hflag |= DEFRAG_FLAG;

      on_vert_swap(v2, elems[i], userdata);
      BM_swap_verts(bm, v2, elems[i]);

#if 0
      BMIter iter;
      BMEdge *et;
      int f = 0;
      BM_ITER_ELEM (et, &iter, v2, BM_EDGES_OF_VERT) {
        printf("an edge %d\n", f++);
      }

      f = 0;
      BM_ITER_ELEM (et, &iter, v, BM_EDGES_OF_VERT) {
        printf("an 1edge %d\n", f++);
      }
#endif

      // BM_swap_verts(bm, v2, elems[i]);

      ok = true;
      break;
    }

    if (ok) {
      break;
    }
  } while ((e = BM_DISK_EDGE_NEXT(e, v)) != v->e);

  return true;
}

static void on_vert_kill(BMesh *bm, BMVert *v, void *userdata)
{
}
static void on_edge_kill(BMesh *bm, BMEdge *e, void *userdata)
{
}
static void on_face_kill(BMesh *bm, BMFace *f, void *userdata)
{
}

static void on_vert_create(BMesh *bm, BMVert *v, void *userdata)
{
}
static void on_edge_create(BMesh *bm, BMEdge *v, void *userdata)
{
}
static void on_face_create(BMesh *bm, BMFace *v, void *userdata)
{
}

void BM_empty_tracer(BMTracer *tracer, void *userdata)
{
  tracer->userdata = userdata;

  tracer->on_vert_create = on_vert_create;
  tracer->on_edge_create = on_edge_create;
  tracer->on_face_create = on_face_create;

  tracer->on_vert_kill = on_vert_kill;
  tracer->on_edge_kill = on_edge_kill;
  tracer->on_face_kill = on_face_kill;
}
/** \} */
