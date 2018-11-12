#include "vm/page.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"
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

    hash_destroy (&spt->spt_hash, spte_destroy_func);
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

    switch (spte->location) 
    {
        case NONE:
            break;
        case MEM:
            // assuming that spte->MEM was done but spte->fe = fe was not done
            if (spte->fe != NULL)
            {
                lock_acquire (&frame_lock);
                frame_free (spte->fe);
                lock_release (&frame_lock);
            }
            // If pagedir is set normally, clear it so that pagedir_destroy can free it appropriately
            uint32_t *pd = spte->spt->owner->pagedir;
            if (pd != NULL)
                pagedir_clear_page (pd, spte->upage);
            break;
        case SWAP:
            // TODO: free a swap slot which is occupied by the spte
            break;
        case FS:
            // TODO
        default:
            PANIC ("Invalid spte location : %d\n", spte->location);
    }
    free (spte);
}
