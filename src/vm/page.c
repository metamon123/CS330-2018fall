#include "vm/page.h"
#include "vm/swap.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include <hash.h>

static unsigned
spte_hash_func (const struct hash_elem *e, void *aux UNUSED);

static bool
spte_less_func (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED);

static void
spte_destroy_func (const struct hash_elem *e, void *aux UNUSED);

void spt_init ()
{
    struct thread *cur = thread_current ();
    cur->spt = (struct spt *) malloc (sizeof (struct spt));
    if (cur->spt == NULL)
    {
        PANIC ("malloc failed, no space for cur->spt\n");
    }

    struct spt *spt = cur->spt;
    spt->owner = cur;
    lock_init (&spt->spt_lock);
    hash_init (&spt->spt_hash, spte_hash_func, spte_less_func, NULL);
}

void spt_destroy ()
{
    struct thread *cur = thread_current ();
    struct spt *spt = cur->spt;
    
    lock_acquire (&frame_lock);
    lock_acquire (&spt->spt_lock);
    hash_destroy (&spt->spt_hash, spte_destroy_func);
    lock_release (&spt->spt_lock);
    lock_release (&frame_lock);
    free (spt);
}

bool install_spte (struct spt *spt, struct spt_entry *spte)
{
    return hash_insert (&spt->spt_hash, &spte->hash_elem) == NULL;
}

struct spt_entry *get_spte (struct spt *spt, void *upage)
{
    struct spt_entry spte;
    struct hash_elem *he;

    spte.upage = pg_round_down (upage);
    he = hash_find (&spt->spt_hash, &spte.hash_elem);
    return he != NULL ? hash_entry (he, struct spt_entry, hash_elem) : NULL;
}

bool load_swap (struct spt_entry *spte)
{
    ASSERT (spte != NULL);
    ASSERT (lock_held_by_current_thread (&frame_lock) && lock_held_by_current_thread (&spte->spt->spt_lock));
    ASSERT (spte->location = SWAP && spte->swap_slot_idx != -1);

    struct frame_entry *fe = frame_alloc (PAL_USER, spte);
    ASSERT (fe != NULL);

    swap_in (fe->kpage, spte->swap_slot_idx);

    if (!install_page (spte->upage, fe->kpage, spte->writable))
    {
        frame_free (fe);
        return false; // frame_lock will be released by caller
    }

    spte->location = MEM;
    spte->fe = fe;
    fe->is_pin = false;
    return true;
}

bool load_file (struct spt_entry *spte)
{
    // ISSUE - Is it possible that caller already acquired filesys_lock?
    ASSERT (spte != NULL);
    ASSERT (lock_held_by_current_thread (&frame_lock) && lock_held_by_current_thread (&spte->spt->spt_lock));
    ASSERT (spte->location = FS && spte->file != NULL);


    struct frame_entry *fe = frame_alloc (PAL_USER, spte);
    ASSERT (fe != NULL);

    ASSERT (spte->page_read_bytes <= PGSIZE);

    // read from file only if page_read_bytes > 0
    if (spte->page_read_bytes > 0)
    {
        // ISSUE - Is it possible that caller already acquired filesys_lock?
        // filesys_acquire by SYS_READ & page_fault while SYS_READ
        // check whether current thread has already acquired filesys_lock.
        bool is_fslock_acquired = lock_held_by_current_thread (&filesys_lock);
        if (!is_fslock_acquired) filesys_lock_acquire ();

        off_t read_bytes = file_read_at (spte->file, fe->kpage, spte->page_read_bytes, spte->ofs);
        if (read_bytes != spte->page_read_bytes)
        {
            frame_free (fe);
            if (!is_fslock_acquired) filesys_lock_release ();
            return false;
        }

        if (!is_fslock_acquired) filesys_lock_release ();
    }
    memset (fe->kpage + spte->page_read_bytes, 0, PGSIZE - spte->page_read_bytes);


    if (!install_page (spte->upage, fe->kpage, spte->writable))
    {
        frame_free (fe);
        return false;
    }

    spte->location = MEM;
    spte->fe = fe;
    fe->is_pin = false;
    return true;
}

// Caller should verify that upage is near the esp
bool grow_stack (void *upage)
{
    struct thread *cur = thread_current ();
    struct spt_entry *spte = (struct spt_entry *) malloc (sizeof (struct spt_entry));
    if (spte == NULL)
        return false;

    spte->spt = cur->spt;
    spte->upage = pg_round_down (upage);
    spte->location = NONE;
    spte->fe = NULL;
    spte->swap_slot_idx = -1;
    spte->writable = true;
    spte->file = NULL;

    lock_acquire (&frame_lock);
    struct frame_entry *fe = frame_alloc (PAL_USER | PAL_ZERO, spte);
    ASSERT (fe != NULL);

    if (!install_page (spte->upage, fe->kpage, true))
    {
        frame_free (fe);
        lock_release (&frame_lock);
        free (spte);
        return false;
    }

    spte->location = MEM;
    spte->fe = fe;
    lock_acquire (&spte->spt->spt_lock);
    if (!install_spte (spte->spt, spte))
    {
        lock_release (&spte->spt->spt_lock);

        frame_free (fe);
        pagedir_clear_page (&spte->spt->owner->pagedir, spte->upage);

        lock_release (&frame_lock);
        free (spte);
        return false;
    }
    lock_release (&spte->spt->spt_lock);

    fe->is_pin = false;
    lock_release (&frame_lock);
    return true;
}
static unsigned
spte_hash_func (const struct hash_elem *e, void *aux UNUSED)
{
    struct spt_entry *spte = hash_entry (e, struct spt_entry, hash_elem);
    return hash_int ((int) spte->upage);
}

static bool
spte_less_func (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED)
{
    struct spt_entry *sa = hash_entry (a, struct spt_entry, hash_elem);
    struct spt_entry *sb = hash_entry (b, struct spt_entry, hash_elem);
    return (sa->upage < sb->upage);
}

static void
spte_destroy_func (const struct hash_elem *e, void *aux UNUSED)
{
    struct spt_entry *spte = hash_entry (e, struct spt_entry, hash_elem);
    ASSERT (spte != NULL);

    //printf ("[ spte_destroy_func ] spte: 0x%x, spte->upage: 0x%x, spte->location: %d, spte->fe: 0x%x\n", spte, spte->upage, spte->location, spte->fe);
    //lock_acquire (&frame_lock);
    //lock_acquire (&spte->spt->spt_lock);
    switch (spte->location) 
    {
        case NONE:
            break;
        case MEM:
            // assuming that spte->MEM was done but spte->fe = fe was not done
            if (spte->fe != NULL)
            {
                //printf ("spte->fe->kpage: 0x%x\n", spte->fe->kpage);
                frame_free (spte->fe);
            }
            // If pagedir is set normally, clear it so that pagedir_destroy can free it appropriately
            uint32_t *pd = spte->spt->owner->pagedir;
            if (pd != NULL)
                pagedir_clear_page (pd, spte->upage);
            break;
        case SWAP:
            // TODO: what should I do more?
            swap_slot_free (spte->swap_slot_idx);
            break;
        case FS:
            // TODO: what should I do??
            break;
        default:
            PANIC ("[ spte_destroy_func() where spte->upage = 0x%x ] Invalid spte location : %d\n", spte->upage, spte->location);
    }
    //lock_release (&spte->spt->spt_lock);
    //lock_release (&frame_lock);
    free (spte);
}
