#ifndef VM_SWAP_H
#define VM_SWAP_H

#include "devices/disk.h"
#include "threads/synch.h"
#include <bitmap.h>
#include "vm/page.h"

#define SECTOR_PER_PAGE (PGSIZE / DISK_SECTOR_SIZE)


struct lock swap_lock;
struct bitmap *swap_disk_bitmap;
struct disk *swap_disk;


void swap_init (void);

// swap_out funcs
size_t swap_out (void *kpage);

// swap_in funcs
void swap_in (void *kpage, size_t idx);
void swap_slot_free (size_t idx);

#endif
