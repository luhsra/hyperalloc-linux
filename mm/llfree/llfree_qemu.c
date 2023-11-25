#include "llfree.h"
#include "llfree_inner.h"
#include "llfree_qemu.h"
#include <linux/slab.h> 
#include "asm/io.h"

typedef struct llfree_info_buffer{
	llfree_t qemu_llfree;
	_Atomic(int64_t) zone_normal_free_pages;
} llfree_info_buffer_t;

void llfree_create_buffer(void **buffer, size_t *buffer_len) {
	*buffer = (llfree_info_buffer_t *) kzalloc(sizeof(llfree_info_buffer_t), GFP_KERNEL);
	*buffer_len = sizeof(llfree_info_buffer_t);
}

void llfree_translate_to_gpa(llfree_t *qemu_llfree) {
	if (!qemu_llfree) {
		return;
	}

	qemu_llfree->lower.children = (_Atomic(child_t)* ) virt_to_phys(qemu_llfree->lower.children);
	qemu_llfree->lower.fields = (bitfield_t *) virt_to_phys(qemu_llfree->lower.fields);
	qemu_llfree->trees = (_Atomic(tree_t)* ) virt_to_phys(qemu_llfree->trees);
}

void llfree_copy_into_buffer(llfree_info_t *qemu_info, void *buffer) {
	if (!buffer) {
		return;
	}

	llfree_info_buffer_t *qemu_info_buffer = (llfree_info_buffer_t *) buffer;
	qemu_info_buffer->qemu_llfree = *qemu_info->qemu_llfree;
	qemu_info_buffer->qemu_llfree.meta = NULL;
	qemu_info_buffer->qemu_llfree.local = NULL;
	llfree_translate_to_gpa(&qemu_info_buffer->qemu_llfree);
	qemu_info_buffer->zone_normal_free_pages = *qemu_info->zone_normal_free_pages;
}



