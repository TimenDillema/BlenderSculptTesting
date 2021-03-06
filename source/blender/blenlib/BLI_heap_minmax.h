/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#pragma once

/** \file
 * \ingroup bli
 * \brief A min-heap / priority queue ADT
 */

#include "BLI_math.h"

#ifdef __cplusplus
extern "C" {
#endif

struct MinMaxHeap;
struct MinMaxHeapNode;
typedef struct MinMaxHeap MinMaxHeap;
typedef struct MinMaxHeapNode MinMaxHeapNode;

typedef void (*MinMaxHeapFreeFP)(void *ptr);

/**
 * Creates a new heap. Removed nodes are recycled, so memory usage will not shrink.
 *
 * \note Use when the size of the heap is known in advance.
 */
MinMaxHeap *BLI_mm_heap_new_ex(unsigned int tot_reserve) ATTR_WARN_UNUSED_RESULT;
MinMaxHeap *BLI_mm_heap_new(void) ATTR_WARN_UNUSED_RESULT;
void BLI_mm_heap_clear(MinMaxHeap *heap, MinMaxHeapFreeFP ptrfreefp) ATTR_NONNULL(1);
void BLI_mm_heap_free(MinMaxHeap *heap, MinMaxHeapFreeFP ptrfreefp) ATTR_NONNULL(1);
/**
 * Insert heap node with a value (often a 'cost') and pointer into the heap,
 * duplicate values are allowed.
 */
MinMaxHeapNode *BLI_mm_heap_insert(MinMaxHeap *heap, float value, void *ptr) ATTR_NONNULL(1);
/**
 * Convenience function since this is a common pattern.
 */
void BLI_mm_heap_insert_or_update(MinMaxHeap *heap,
                                  MinMaxHeapNode **node_p,
                                  float value,
                                  void *ptr) ATTR_NONNULL(1, 2);
bool BLI_mm_heap_is_empty(const MinMaxHeap *heap) ATTR_NONNULL(1);
unsigned int BLI_mm_heap_len(const MinMaxHeap *heap) ATTR_WARN_UNUSED_RESULT ATTR_NONNULL(1);

/**
 * Return the top node of the heap.
 * This is the node with the lowest value.
 */
MinMaxHeapNode *BLI_mm_heap_min(const MinMaxHeap *heap) ATTR_WARN_UNUSED_RESULT ATTR_NONNULL(1);
/**
 * Return the value of top node of the heap.
 * This is the node with the lowest value.
 */
float BLI_mm_heap_min_value(const MinMaxHeap *heap) ATTR_WARN_UNUSED_RESULT ATTR_NONNULL(1);

/**
 * Return the top node of the heap.
 * This is the node with the lowest value.
 */
MinMaxHeapNode *BLI_mm_heap_max(const MinMaxHeap *heap) ATTR_WARN_UNUSED_RESULT ATTR_NONNULL(1);
/**
 * Return the value of top node of the heap.
 * This is the node with the lowest value.
 */
float BLI_mm_heap_max_value(const MinMaxHeap *heap) ATTR_WARN_UNUSED_RESULT ATTR_NONNULL(1);

/**
 * Pop the top node off the heap and return its pointer.
 */
void *BLI_mm_heap_pop_min(MinMaxHeap *heap) ATTR_NONNULL(1);

/**
 * Pop the top node off the heap and return its pointer.
 */
void *BLI_mm_heap_pop_max(MinMaxHeap *heap) ATTR_NONNULL(1);

/**
 * Can be used to avoid #BLI_mm_heap_remove, #BLI_mm_heap_insert calls,
 * balancing the tree still has a performance cost,
 * but is often much less than remove/insert, difference is most noticeable with large heaps.
 */
MinMaxHeapNode *BLI_mm_heap_node_value_update(MinMaxHeap *heap, MinMaxHeapNode *node, float value)
    ATTR_NONNULL(1, 2);

MinMaxHeapNode *BLI_mm_heap_node_value_update_ptr(MinMaxHeap *heap,
                                                  MinMaxHeapNode *node,
                                                  float value,
                                                  void *ptr);

/**
 * Return the value or pointer of a heap node.
 */
float BLI_mm_heap_node_value(const MinMaxHeapNode *heap) ATTR_WARN_UNUSED_RESULT ATTR_NONNULL(1);
void *BLI_mm_heap_node_ptr(const MinMaxHeapNode *heap) ATTR_WARN_UNUSED_RESULT ATTR_NONNULL(1);
/**
 * Only for checking internal errors (gtest).
 */
bool BLI_mm_heap_is_valid(const MinMaxHeap *heap);

#ifdef __cplusplus
}
#endif
