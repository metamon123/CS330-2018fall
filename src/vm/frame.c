#include "vm/frame.h"
#include "vm/page.h"
#include "vm/swap.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include <list.h>

void frame_init ()
{
  list_init (&frame_list);
  lock_init (&frame_lock);
}

static struct frame_entry *select_victim (void)
{
    // page replacement policy : FIFO
    
    ASSERT (lock_held_by_current_thread (&frame_lock));
    while (1)
    {
        int n = 0;
        struct list_elem *e;
        for (e = list_begin (&frame_list); e != list_end (&frame_list); e = list_next (e))
        {
            struct frame_entry *fe = list_entry (e, struct frame_entry, elem);
            ASSERT (fe != NULL);

            if (!fe->is_pin)
            {
                return fe;
            }
            n++;
        }
        printf ("[ select_victim() ] all %d frames in frame_list are pinned\n", n);
    }
}

struct frame_entry *frame_alloc (enum palloc_flags flag, struct spt_entry *spte)
{
    ASSERT (lock_held_by_current_thread (&frame_lock));
    struct frame_entry *fe = (struct frame_entry *) malloc (sizeof (struct frame_entry));
    if (fe == NULL)
    {
        printf ("[ frame_alloc() ] No kernel space form new frame_entry structure\n");
        return NULL;
    }

    ASSERT (flag & PAL_USER); // frame is only for user pool
    void *kpage;
    while ((kpage = palloc_get_page (flag)) == NULL)
    {
        /*
        printf ("[ frame_alloc() ] User pool is full & frame_evict is not implemented yet\n");
        free (fe);
        return NULL;
        */
        
        // FRAME EVICTION !
        // get a victim frame
        struct frame_entry *victim = select_victim ();

        struct spt_entry *victim_spte = victim->spte;
        
        bool is_sptlock_acquired = lock_held_by_current_thread (&victim->spte->spt->spt_lock);
        if (!is_sptlock_acquired) lock_acquire (&victim_spte->spt->spt_lock);

        ASSERT (victim_spte->location == MEM);

        if (victim_spte->file == NULL)
        {
            // swap_out : copy data to swap disk (swap_lock will be done in swap_out)
            size_t swap_slot_idx = swap_out (victim->kpage);

            // free the victim
            frame_free (victim);
            pagedir_clear_page (victim_spte->spt->owner->pagedir, victim_spte->upage);

            victim_spte->location = SWAP;
            victim_spte->fe = NULL;
            victim_spte->swap_slot_idx = swap_slot_idx;
        }
        else
        {
            if (pagedir_is_dirty (victim_spte->spt->owner->pagedir, victim_spte->upage))
            {
                // if page is modified => write back
                // else => do not write back
                //PANIC ("[ frame_alloc () ] frame with file, and modified. Should not happen yet\n"); // possible (.bss in executable)

                bool is_fslock_acquired = lock_held_by_current_thread (&filesys_lock);
                if (!is_fslock_acquired) filesys_lock_acquire ();

                file_write_at (victim_spte->file, victim->kpage, victim_spte->ofs, victim_spte->page_read_bytes);

                if (!is_fslock_acquired) filesys_lock_release ();
            }

            // not modified file frames do not need to be swapped out
            frame_free (victim);
            pagedir_clear_page (victim_spte->spt->owner->pagedir, victim_spte->upage);

            victim_spte->location = FS;
            victim_spte->fe = NULL; 
        }

        if (!is_sptlock_acquired) lock_release (&victim_spte->spt->spt_lock);
    }

    fe->is_pin = true;
    fe->kpage = kpage;
    fe->spte = spte;
    list_push_back (&frame_list, &fe->elem);
    //printf ("[ frame_alloc() ] success, kpage = 0x%x, upage = 0x%x\n", kpage, spte->upage);
    return fe;
}

// Can be called by non fe->spte->owner process -> spt lock needed?
void frame_free (struct frame_entry *fe)
{
    ASSERT (fe != NULL);
    ASSERT (lock_held_by_current_thread (&frame_lock));
    //printf ("[ frame_free() ] success, kpage = 0x%x, upage = 0x%x\n", fe->kpage, fe->spte->upage);
    list_remove (&fe->elem);
    
    // maybe spt lock will be needed
    // pagedir_clear_page (fe->spte->spt->owner->pagedir, fe->spte->upage);
    palloc_free_page (fe->kpage);
    free (fe);
}
