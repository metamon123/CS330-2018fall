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
    // page replacement policy : FIFO
    
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
        // filesys lock?
        file_write_at (spte->file, spte->fe->kpage, spte->page_read_bytes, spte->ofs);
    }

}

static void frame_evict (void)
{
    //struct thread *cur = thread_current (); // for debugging
    //printf ("[ frame_alloc - frame_evict starts in tid %d ]\n", cur->tid);
    struct frame_entry *victim = select_victim ();
    struct spt_entry *victim_spte = victim->spte;

    bool is_sptlock_acquired = lock_held_by_current_thread (&victim->spte->spt->spt_lock);
    if (!is_sptlock_acquired) lock_acquire (&victim_spte->spt->spt_lock);

    ASSERT (lock_held_by_current_thread (&frame_lock));
    ASSERT (victim_spte->location == MEM);
    
    //printf ("[ frame_alloc - frame_evict in tid %d ]\nvictim->kpage : 0x%x\nvictim->spte : 0x%x\nvictim->spte->upage : 0x%x\nvictim->spte->file : 0x%x\n", cur->tid, victim->kpage, victim->spte, victim->spte->upage, victim->spte->file);

    /*
    if (victim_spte->is_mmapped && pagedir_is_dirty (victim_spte->spt->owner->pagedir, victim_spte->upage))
    {
        // TODO: MMAP -> write back when modified
        bool is_fslock_acquired = lock_held_by_current_thread (&filesys_lock);
        if (!is_fslock_acquired) filesys_lock_acquire ();

        file_write_at (victim_spte->file, victim->kpage, victim_spte->page_read_bytes, victim_spte->ofs);

        if (!is_fslock_acquired) filesys_lock_release ();
    }
    */
    write_back (victim_spte);

    if (victim_spte->file != NULL && !victim_spte->writable)
    {
        enum intr_level old_level = intr_disable ();
        frame_free (victim);
        pagedir_clear_page (victim_spte->spt->owner->pagedir, victim_spte->upage);
        intr_set_level (old_level);

        victim_spte->location = FS;
        victim_spte->fe = NULL;
    }
    else
    {
        // file == NULL OR (file != NULL but writable)
        // swap_out : copy data to swap disk (swap_lock will be done in swap_out)
        size_t swap_slot_idx = swap_out (victim->kpage);

        enum intr_level old_level = intr_disable ();
        frame_free (victim);
        pagedir_clear_page (victim_spte->spt->owner->pagedir, victim_spte->upage);
        intr_set_level (old_level);

        victim_spte->location = SWAP;
        victim_spte->fe = NULL;
        victim_spte->swap_slot_idx = swap_slot_idx;
    }

    //printf ("[ frame_alloc - frame_evict in tid %d] victim_spte->location = %d\n", cur->tid, victim_spte->location);
    if (!is_sptlock_acquired) lock_release (&victim_spte->spt->spt_lock);
}

struct frame_entry *frame_alloc (enum palloc_flags flag, struct spt_entry *spte)
{
    //printf ("[ frame_alloc() tid - %d ] started\n", thread_current ()->tid);
    ASSERT (lock_held_by_current_thread (&frame_lock));
    //printf ("here1\n");
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
        //PANIC ("[ frame_alloc() ] User pool is full & frame_evict is not implemented yet\n");
        //free (fe);
        //return NULL;
        frame_evict ();
    }

    fe->is_pin = true;
    fe->kpage = kpage;
    fe->spte = spte;
    //printf ("1\n");
    list_push_back (&frame_list, &fe->elem);
    //printf ("11\n");
    //printf ("[ frame_alloc() tid - %d ] success, fe = 0x%x, kpage = 0x%x, upage = 0x%x\n", thread_current ()->tid, fe, kpage, spte->upage);
    return fe;
}

// Can be called by non fe->spte->owner process -> spt lock needed?
void frame_free (struct frame_entry *fe)
{
    ASSERT (fe != NULL);
    ASSERT (lock_held_by_current_thread (&frame_lock));
    //printf ("[ frame_free() ] success, fe = 0x%x, kpage = 0x%x, upage = 0x%x\n", fe, fe->kpage, fe->spte->upage);
    list_remove (&fe->elem);
    
    // maybe spt lock will be needed
    // pagedir_clear_page (fe->spte->spt->owner->pagedir, fe->spte->upage);
    palloc_free_page (fe->kpage);
    free (fe);
}
