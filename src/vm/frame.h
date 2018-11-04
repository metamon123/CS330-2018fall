#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <inttypes.h>
#include "threads/synch.h"
#include <list.h>
#include "vm/page.h"

struct list frame_list;
struct lock frame_lock;

struct frame_entry
{
    struct list_elem elem;
    void *kpage; // kernel vaddr of physicall addr of the frame
    struct spt_entry *spte;
    //bool writable;
    // don't check writable from spte
    // since it will not be changed easily
}
#endif
