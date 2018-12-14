#include "filesys/cache.h"
#include "threads/synch.h"
#include "devices/disk.h"
#include "filesys/filesys.h"
#include <debug.h>
static struct cache_entry cache[64];
static struct lock cache_lock;

void
cache_init ()
{
    int i;
    for (i = 0; i < 64; ++i)
    {
        struct cache_entry *ce = &cache[i];
        ce->is_valid = false;
    }

    lock_init (&cache_lock);
}

static struct cache_entry *
cache_lookup (disk_sector_t sector)
{
    int i;
    for (i = 0; i < 64; ++i)
    {
        struct cache_entry *ce = &cache[i];
        if (ce->is_valid && ce->sector == sector)
            return ce;
    }
    return NULL;
}


static void
write_back_entry (struct cache_entry *ce)
{
    ASSERT (ce != NULL && ce->is_valid);

    // write_behind
    if (ce->is_dirty)
    {
        disk_write (filesys_disk, ce->sector, ce->data);
        ce->is_dirty = false;
    }
}


// Write back all valid cache entries.
void
cache_flush_all ()
{
    int i;
    for (i = 0; i < 64; ++i)
    {
        struct cache_entry *ce = &cache[i];

        lock_acquire (&cache_lock);
        if (ce->is_valid)
            write_back_entry (ce);
        lock_release (&cache_lock);
    }
}


static void
delete_entry (struct cache_entry *ce)
{
    ASSERT (ce != NULL && ce->is_valid);

    write_back_entry (ce);
    ce->is_valid = false;
}

static struct cache_entry *
cache_evict ()
{
    int i;
    while (1)
    {
        for (i = 0; i < 64; ++i)
        {
            struct cache_entry *ce = &cache[i];
            if (!ce->is_valid)
                PANIC ("cache_evict while cache is not full");

            if (ce->is_second)
            {
                delete_entry (ce);
                return ce;
            }
            else
            {
                ce->is_second = true;
            }
        }
    }
    return NULL;
}

static struct cache_entry *
get_free_entry ()
{
    struct cache_entry *ce;
    int i;
    for (i = 0; i < 64; ++i)
    {
        ce = &cache[i];
        if (!ce->is_valid)
            return ce;
    }

    // if all entries are in use, evict one
    ce = cache_evict ();
    ASSERT (ce != NULL);

    return ce;
}


static struct cache_entry *
cache_load (disk_sector_t sector)
{
    struct cache_entry *free_ce;

    // Get free entry
    free_ce = get_free_entry ();
    ASSERT (free_ce != NULL && !free_ce->is_valid);

    // Read data from disk & Set meta-data
    disk_read (filesys_disk, sector, free_ce->data);
    free_ce->is_dirty = false;
    free_ce->is_second = false;
    free_ce->sector = sector;
    
    free_ce->is_valid = true;
    return free_ce;
}

void cache_read (disk_sector_t sector, void *buf)
{
    cache_read_at (sector, buf, 0, DISK_SECTOR_SIZE);
}

void cache_write (disk_sector_t sector, void *buf)
{
    cache_write_at (sector, buf, 0, DISK_SECTOR_SIZE);
}

void
cache_read_at (disk_sector_t sector, void *buf, off_t ofs, int len)
{
    ASSERT (ofs < DISK_SECTOR_SIZE && ofs >= 0 && ofs + len <= DISK_SECTOR_SIZE);

    lock_acquire (&cache_lock);

    struct cache_entry *ce = cache_lookup (sector);
    if (ce == NULL)
    {
        ce = cache_load (sector);
    }
    ASSERT (ce != NULL);

    ce->is_second = false;
    memcpy (buf, ce->data + ofs, len);

    lock_release (&cache_lock);
}

void
cache_write_at (disk_sector_t sector, void *buf, off_t ofs, int len)
{
    ASSERT (ofs < DISK_SECTOR_SIZE && ofs >= 0 && ofs + len <= DISK_SECTOR_SIZE);

    lock_acquire (&cache_lock);

    struct cache_entry *ce = cache_lookup (sector);
    if (ce == NULL)
    {
        ce = cache_load (sector);
    }
    ASSERT (ce != NULL);

    ce->is_second = false;
    ce->is_dirty = true;
    memcpy (ce->data + ofs, buf, len);
    
    lock_release (&cache_lock);
}


