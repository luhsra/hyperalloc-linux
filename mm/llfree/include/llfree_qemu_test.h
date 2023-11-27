
#ifndef _LLFREE_QEMU_TEST
#define _LLFREE_QEMU_TEST

#ifdef CONFIG_LLFREE

#include <llfree.h>
#include <llfree_types.h>

llfree_t *llfree_create_test_llfree(void);
void *llfree_inspect_llfree(llfree_t *test_llfree);

#endif // CONFIG_LLFREE
#endif // _LLFREE_QEMU_TEST
