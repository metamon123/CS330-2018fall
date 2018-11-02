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

//void frame_alloc (struct spt_entry *spte)
void *frame_alloc ()
{
    // TODO: Acquire frame_lock in somewhere
    void *frame = palloc_get_page (PAL_USER);
    if (frame == NULL)
    {
        // TODO: frame evict and alloc
        printf ("[fail in frame_alloc()] User pool is full & frame_evict is not implemented yet\n");
        return NULL;
    }

    struct frame_entry *fe = (struct frame_entry *) malloc (sizeof struct frame_entry);
    fe->frame = frame;
    list_push_back (&frame_list, fe->elem);

    return frame;
}

void frame_free ()
{
    1+1;
}
