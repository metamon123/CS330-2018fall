#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <inttypes.h>
#include "threads/synch.h"
#include <list.h>
#include "vm/page.h"
#include "threads/palloc.h"

struct list frame_list;
struct lock frame_lock;

struct frame_entry
{
    bool is_pin; // do not evict the frame if is_pin == true
    void *kpage; // kernel vaddr of physicall addr of the frame
    struct spt_entry *spte;
    
    struct list_elem elem;
    struct list_elem pin_elem;
};

void frame_init (void);
struct frame_entry *frame_alloc (enum palloc_flags, struct spt_entry *);
void write_back (struct spt_entry *);
void frame_free (struct frame_entry *);
#endif
