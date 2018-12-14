#include "filesys/cache.h"

void
cache_init ()
{
    int i;
    for (i = 0; i < 64; ++i)
    {
        struct cache_entry *ce = &cache[i];
        ce->is_valid = false;
    }
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

void
cache_read_at (disk_sector_t sector, void *buf, off_t ofs, int len)
{
    ASSERT (ofs < DISK_SECTOR_SIZE && ofs >= 0 && ofs + len <= DISK_SECTOR_SIZE);

    struct cache_entry *ce = cache_lookup (sector);
    if (ce == NULL)
    {
    }
    
    ce->is_second = false;
    memcpy (buf, ce->data + ofs, len);
}

void
cache_write_at (disk_sector_t sector, void *buf, off_t ofs, int len)
{
    struct cache_entry *ce = cache_lookup (sector);
    if (ce == NULL)
    {
    }

    ce->is_second = false;
    memcpy (ce->data + ofs, buf, len);
}


