#include "vm/frame.h"
#include "vm/page.h"
#include "vm/swap.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/interrupt.h"
#include <list.h>

void frame_init ()
{
  list_init (&frame_list);
  lock_init (&frame_lock);
}

static struct frame_entry *select_victim (void)
{
    // page replacement policy : Second chance
    
    ASSERT (lock_held_by_current_thread (&frame_lock));
    while (1)
    {
        struct list_elem *e;
        for (e = list_begin (&frame_list); e != list_end (&frame_list); e = list_next (e))
        {
            struct frame_entry *fe = list_entry (e, struct frame_entry, elem);
            ASSERT (fe != NULL);

            if (fe->is_pin)
            {
                continue;
            }
            
            struct spt *spt = fe->spte->spt;
            bool is_sptlock_acquired = lock_held_by_current_thread (&spt->spt_lock);
            if (!is_sptlock_acquired) lock_acquire (&spt->spt_lock);

            if (pagedir_is_accessed (spt->owner->pagedir, fe->spte->upage))
            {
                pagedir_set_accessed (spt->owner->pagedir, fe->spte->upage, false);
                if (!is_sptlock_acquired) lock_release (&fe->spte->spt->spt_lock);
                continue;
            }
            if (!is_sptlock_acquired) lock_release (&fe->spte->spt->spt_lock);

            return fe;
        }
    }
}

void write_back (struct spt_entry *spte)
{
    if (spte != NULL && spte->is_mmap && spte->location == MEM && spte->fe != NULL && pagedir_is_dirty (spte->spt->owner->pagedir, spte->upage))
    {
        ASSERT (spte->file != NULL);
        // filesys lock? (acquired and released by caller)
        file_write_at (spte->file, spte->fe->kpage, spte->page_read_bytes, spte->ofs);
    }

}

static void frame_evict (void)
{
    struct frame_entry *victim = select_victim ();
    struct spt_entry *victim_spte = victim->spte;

    bool is_sptlock_acquired = lock_held_by_current_thread (&victim->spte->spt->spt_lock);
    if (!is_sptlock_acquired) lock_acquire (&victim_spte->spt->spt_lock);

    ASSERT (lock_held_by_current_thread (&frame_lock));
    ASSERT (victim_spte->location == MEM);
    

    bool is_fslock_acquired = lock_held_by_current_thread (&filesys_lock);
    if (!is_fslock_acquired) filesys_lock_acquire ();
    write_back (victim_spte);
    if (!is_fslock_acquired) filesys_lock_release ();

    // non-writable OR mmapped region originated from file system
    // => No need for swap out. They can be recovered from file system
    if (victim_spte->file != NULL && (!victim_spte->writable || victim_spte->is_mmap))
    {
        //enum intr_level old_level = intr_disable ();
        pagedir_clear_page (victim_spte->spt->owner->pagedir, victim_spte->upage);
        frame_free (victim);
        //intr_set_level (old_level);

        victim_spte->location = FS;
        victim_spte->fe = NULL;
    }
    else
    {
        // file == NULL OR (file != NULL but (writable and not mmapped region))
        // swap_out : copy data to swap disk (swap_lock will be done in swap_out)
        size_t swap_slot_idx = swap_out (victim->kpage);

        //enum intr_level old_level = intr_disable ();
        pagedir_clear_page (victim_spte->spt->owner->pagedir, victim_spte->upage);
        frame_free (victim);
        //intr_set_level (old_level);

        victim_spte->location = SWAP;
        victim_spte->fe = NULL;
        victim_spte->swap_slot_idx = swap_slot_idx;
    }

    if (!is_sptlock_acquired) lock_release (&victim_spte->spt->spt_lock);
}

struct frame_entry *frame_alloc (enum palloc_flags flag, struct spt_entry *spte)
{
    ASSERT (lock_held_by_current_thread (&frame_lock));
    struct frame_entry *fe = (struct frame_entry *) malloc (sizeof (struct frame_entry));
    if (fe == NULL)
    {
        PANIC ("[ frame_alloc() ] No kernel space form new frame_entry structure\n");
        return NULL;
    }

    ASSERT (flag & PAL_USER); // frame is only for user pool
    void *kpage;
    while ((kpage = palloc_get_page (flag)) == NULL)
    {
        frame_evict ();
    }

    fe->is_pin = true;
    fe->kpage = kpage;
    fe->spte = spte;
    list_push_back (&frame_list, &fe->elem);
    return fe;
}

// Can be called by non fe->spte->owner process -> spt lock needed?
void frame_free (struct frame_entry *fe)
{
    ASSERT (fe != NULL);
    ASSERT (lock_held_by_current_thread (&frame_lock));
    list_remove (&fe->elem);
    
    palloc_free_page (fe->kpage);
    free (fe);
}
