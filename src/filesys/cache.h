#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/disk.h"
#include "filesys/off_t.h"
#include <stdbool.h>
#include <debug.h>

struct cache_entry {
    bool is_valid;
    bool is_dirty;
    bool is_second;
    disk_sector_t sector;
    int8_t data[DISK_SECTOR_SIZE];
};

void cache_init (void);
void cache_read (disk_sector_t sector, void *buf);
void cache_write (disk_sector_t sector, void *buf);
void cache_read_at (disk_sector_t sector, void *buf, off_t ofs, int len);
void cache_write_at (disk_sector_t sector, void *buf, off_t ofs, int len);
void cache_flush_all (void);
void cache_periodic_flush (void *aux UNUSED);

#endif
