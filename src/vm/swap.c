#include "vm/swap.h"
#include "threads/vaddr.h"

#define used true
#define freed false

static size_t swap_get_empty_slot (void);

void swap_init ()
{
    swap_disk = disk_get (1, 1);
    ASSERT (swap_disk != NULL);

    swap_disk_bitmap = bitmap_create (disk_size (swap_disk) / SECTOR_PER_PAGE);
    ASSERT (swap_disk_bitmap != NULL);

    // bitmap_create sets all bits to 0

    lock_init (&swap_lock);
}

size_t swap_out (void *kpage)
{
    lock_acquire (&swap_lock);

    size_t idx = swap_get_empty_slot ();
    if (idx == BITMAP_ERROR)
    {
        PANIC ("[ swap_out () ] no empty swap slot\n");
    }
    
    int i;
    for (i = 0; i < 8; ++i)
    {
        disk_write (swap_disk, (idx << 3) + i, kpage);
        kpage += DISK_SECTOR_SIZE;
    }
    
    lock_release (&swap_lock);
    return idx;
}

static size_t swap_get_empty_slot (void)
{
    return bitmap_scan_and_flip (swap_disk_bitmap, 0, 1, freed);
}

void swap_in (void *kpage, size_t idx)
{
    ASSERT (kpage != NULL);
    ASSERT (idx != -1 && (idx << 3) >= idx);
    ASSERT (idx < bitmap_size (swap_disk_bitmap));
    
    lock_acquire (&swap_lock);

    if (bitmap_test (swap_disk_bitmap, idx) == freed)
    {
        PANIC ("[ swap_in () with idx = %d] tried to swap in with freed slot\n", idx);
    }

    int i;
    for (i = 0; i < 8; ++i)
    {
        disk_read (swap_disk, (idx << 3) + i, kpage);
        kpage += DISK_SECTOR_SIZE;
    }

    swap_slot_free (idx);

    lock_release (&swap_lock);
}

void swap_slot_free (size_t idx)
{
    ASSERT (bitmap_test (swap_disk_bitmap, idx) == used); // initially, bitmap[idx] should be true
    bitmap_reset (swap_disk_bitmap, idx); // atomic
}
