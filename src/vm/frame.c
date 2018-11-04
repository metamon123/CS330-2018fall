#include "vm/frame.h"
#include "vm/page.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include <list.h>

void frame_init ()
{
  list_init (&frame_list);
  lock_init (&frame_lock);
}

// TODO: void frame_alloc (enum palloc_flags flag, struct spt_entry *spte)
struct frame_entry *frame_alloc (enum palloc_flags flag)
{
    // TODO: Acquire frame_lock in somewhere

    struct frame_entry *fe = (struct frame_entry *) malloc (sizeof struct frame_entry);
    if (fe == NULL)
    {
        printf ("[ frame_alloc() ] No kernel space form new frame_entry structure\n");
        return NULL;
    }

    ASSERT (flag & PAL_USER); // frame is only for user pool
    void *kpage = palloc_get_page (flag);
    if (kpage == NULL)
    {
        // TODO: frame evict and alloc
        printf ("[ frame_alloc() ] User pool is full & frame_evict is not implemented yet\n");
        free (fe);
        return NULL;
    }

    fe->kpage = kpage;
    // TODO: set spte
    list_push_back (&frame_list, fe->elem);

    return fe;
}

// Can be called by non fe->spte->owner process -> spt lock needed?
void frame_free (struct frame_entry *fe)
{
    ASSERT (fe != NULL);
    list_remove (&fe->elem);
    // maybe spt lock will be needed
    // pagedir_clear_page (fe->spte->spt->owner->pagedir, fe->spte->upage);
    palloc_free_page (fe->kpage);
    free (fe);
}
