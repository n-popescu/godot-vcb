/* vcb_vec.h -- growable-vector + entity primitives for the pipeline.
 * Real implementations (in vcb_vec.c) of what the binary emitted as inlined
 * std::vector operations / allocator calls. Standalone; no Godot dependency. */
#ifndef VCB_VEC_H
#define VCB_VEC_H

#include "vcb_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void   TCVecPix_push(TCVec *v, const TCPixel *p);   /* push a 12-byte pixel */
void   TCVecGrp_push(TCVec *v, TCVec *moved_group); /* move a 0x18 group header */
void   TCVecPtr_push(TCVec *v, void *handle);       /* push an 8-byte handle */
size_t TCVec_count(const TCVec *v, size_t elem_size);
void   TCVec_free(TCVec *v);                        /* free storage, reset to empty */

void  *TC_op_new(size_t n);      /* entity alloc (calloc) */
void  *TC_alloc_bytes(size_t n); /* scratch alloc (malloc) */
void   TC_free_bytes(void *p);

#ifdef __cplusplus
}
#endif

#endif /* VCB_VEC_H */
