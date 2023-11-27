#include "child.h"
#include "linux/slab.h"
#include "llfree.h"
#include "llfree_alloc.h"
#include "llfree_qemu.h"
#include "llfree_inner.h"
#include "llfree_qemu_test.h"


llfree_t *llfree_create_test_llfree(void) {
	llfree_t *test_llfree = (llfree_t *) kzalloc(sizeof(llfree_t), GFP_KERNEL);
	llfree_result_t res = llfree_init(test_llfree, 1, 4096, 128*512, LLFREE_INIT_VOLATILE, true);
	if(!llfree_ok(res)) {
		printk("Coult not create test llfree");
		return NULL;
	}

	return test_llfree;
}

void *llfree_inspect_llfree(llfree_t *test_llfree) {
	_Atomic(child_t) *children = test_llfree->lower.children;
	llfree_debug("children addr: %p", children);
	return children;
}
