#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include <list.h>
#include "devices/disk.h"
#include "filesys/off_t.h"

struct cache_entry {
    bool is_valid;
    bool is_dirty;
    bool is_second;
    disk_sector_t sector;
    //void *data;
    int8_t data[DISK_SECTOR_SIZE];
};

// struct cache_entry cache[64];

void cache_init (void);
void cache_read (disk_sector_t sector, void *buf);
void cache_write (disk_sector_t sector, void *buf);
void cache_read_at (disk_sector_t sector, void *buf, off_t ofs, int len);
void cache_write_at (disk_sector_t sector, void *buf, off_t ofs, int len);
void cache_flush_all (void);

#endif
