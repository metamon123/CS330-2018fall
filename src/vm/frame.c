#include "vm/frame.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include <list.h>

static struct list frame_list;

void frame_init ()
{
  list_init (&frame_list);
}

void frame_alloc ()
{
    1+1;
}

void frame_free ()
{
    1+1;
}
