#ifndef _LLFREE_QEMU
#define _LLFREE_QEMU

#ifdef CONFIG_LLFREE

#include <llfree.h>
#include <llfree_types.h>

typedef struct llfree_info {
	llfree_t *qemu_llfree;
	_Atomic(int64_t) *zone_normal_free_pages;
	_Atomic(int64_t) *num_pagecache_reclaimable_pages;
} llfree_info_t;

void llfree_create_buffer(void **buffer, size_t *buffer_len);
void llfree_copy_into_buffer(llfree_info_t *qemu_info, void *buffer);

#endif // CONFIG_LLFREE
#endif // _LLFREE_QEMU
