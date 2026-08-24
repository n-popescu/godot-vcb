/* vcb_vec.c -- growable byte-vector + allocation primitives used throughout the
 * Godot-free compiler/model/sim core. TCVec is a {begin, end, cap} triple that
 * mirrors the std::vector layout the original engine used, so the same push/count
 * idioms map 1:1 onto the recovered algorithm. Elements are appended by raw byte
 * size (esz), letting one vector type hold pixels, nested vectors, or pointers. */
#include "vcb_vec.h"
#include <stdlib.h>
#include <string.h>

/* Append esz bytes, doubling capacity when full (16*esz initial). */
static void vec_push_bytes(TCVec *v, const void *elem, size_t esz) {
	size_t used = (char *)v->end - (char *)v->begin;
	size_t cap = (char *)v->cap - (char *)v->begin;
	if (used + esz > cap) {
		size_t ncap = cap ? cap * 2 : 16 * esz;
		if (ncap < used + esz)
			ncap = used + esz;
		char *nb = (char *)realloc(v->begin, ncap);
		v->begin = nb;
		v->end = nb + used;
		v->cap = nb + ncap;
	}
	memcpy(v->end, elem, esz);
	v->end = (char *)v->end + esz;
}

void TCVecPix_push(TCVec *v, const TCPixel *p) { vec_push_bytes(v, p, sizeof(TCPixel)); }

void TCVecGrp_push(TCVec *v, TCVec *g) {
	vec_push_bytes(v, g, sizeof(TCVec));
	g->begin = g->end = g->cap = 0; /* move: source now empty */
}

void TCVecPtr_push(TCVec *v, void *h) { vec_push_bytes(v, &h, sizeof(void *)); }

size_t TCVec_count(const TCVec *v, size_t esz) {
	return ((char *)v->end - (char *)v->begin) / esz;
}

void TCVec_free(TCVec *v) {
	free(v->begin);
	v->begin = v->end = v->cap = 0;
}

void *TC_op_new(size_t n) { return calloc(1, n); }
void *TC_alloc_bytes(size_t n) { return malloc(n); }
void TC_free_bytes(void *p) { free(p); }
