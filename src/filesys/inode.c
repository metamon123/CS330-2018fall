#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

#ifdef FILESYS
#include "filesys/cache.h"
#endif

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44
#define NONE -1

#define DIRECT_NUM 123
#define SECTOR_PER_SINGLE 128

// max index among direct sectors (122)
#define D_MAX (DIRECT_NUM - 1)

// max index among single-indirect sectors (250)
#define SIND_MAX (D_MAX + SECTOR_PER_SINGLE)

// max index among double-indirect sectors
#define DIND_MAX (SIND_MAX + (SIND_MAX + 1) * SECTOR_PER_SINGLE)

static char zeros[DISK_SECTOR_SIZE];

/* On-disk inode.
   Must be exactly DISK_SECTOR_SIZE bytes long. */
struct inode_disk
{
    uint32_t flag;
    off_t length;
    disk_sector_t direct_sectors[DIRECT_NUM]; // direct block pointers
    disk_sector_t sind_sector;          // single-indirect block pointer
    disk_sector_t dind_sector;          // double-indirect block pointer
    unsigned magic;
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, DISK_SECTOR_SIZE);
}

/* In-memory inode. */
/* On-disk inode + meta data needed in kernel */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    disk_sector_t sector;               /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
  };

/* Returns the disk sector that contains byte offset POS within
   INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static disk_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
  ASSERT (DISK_SECTOR_SIZE == sizeof (struct inode_disk));

  disk_sector_t sector = -1;

  struct inode_disk *disk_inode = (struct inode_disk *) malloc (DISK_SECTOR_SIZE);
  if (disk_inode == NULL)
      PANIC ("[ byte_to_sector ] no memory for malloc");

  cache_read (inode->sector, disk_inode);

  //printf("inode-sector %d / inode-length %d / pos %d\n", inode->sector, disk_inode->length, pos);
  if (pos < disk_inode->length)
  {
      int sector_idx = pos / DISK_SECTOR_SIZE;
      //printf ("sector_idx %d\n", sector_idx);
      if (sector_idx <= D_MAX)
      {
          // direct
          sector = disk_inode->direct_sectors[sector_idx];
      }
      else if (sector_idx <= SIND_MAX)
      {
          // single indirect
          // ex.
          //    sector_idx = 123 (first sector in single_indirect sectors)
          //    idx = 0
          if (disk_inode->sind_sector == NONE)
              goto fast_exit;
          int idx = sector_idx - D_MAX - 1;
          //printf ("idx : %d\n", idx);
          cache_read_at (disk_inode->sind_sector, &sector, idx * sizeof (disk_sector_t), sizeof (sector));
      }
      else if (sector_idx <= DIND_MAX)
      {
          // double indirect
          // ex.
          //    sector_idx = 251 (first sector in double_indirect sectors)
          //    sind_idx = 0
          //    direct_idx = 0
          if (disk_inode->dind_sector == NONE)
              goto fast_exit;
          int sind_idx = (sector_idx - SIND_MAX - 1) / SECTOR_PER_SINGLE;
          int direct_idx = (sector_idx - SIND_MAX - 1) % SECTOR_PER_SINGLE;
          
          disk_sector_t sind_sector;
          cache_read_at (disk_inode->dind_sector, &sind_sector, sind_idx * sizeof (disk_sector_t), sizeof (sind_sector));
          if (sind_sector == NONE)
              goto fast_exit;
          cache_read_at (sind_sector, &sector, direct_idx * sizeof (disk_sector_t), sizeof (sector));
      }
      else
      {
          PANIC ("[ byte_to_sector ] Too big pos %d == index %d sector", pos, sector_idx);
      }
  }

fast_exit:
  free (disk_inode);
  return sector;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

static bool
alloc_single_indirect (disk_sector_t *sector, int *sectors)
{
    if (!free_map_allocate (1, sector))
        return false;

    disk_sector_t *buf = (disk_sector_t *) malloc (DISK_SECTOR_SIZE);
    if (buf == NULL)
        PANIC ("No space for malloc");
    
    int i;
    for (i = 0; i < SECTOR_PER_SINGLE; ++i)
        buf[i] = NONE;
    
    for (i = 0; i < SECTOR_PER_SINGLE; ++i)
    {
        if (*sectors <= 0)
            break;
        if (!free_map_allocate (1, &buf[i]))
        {
            free (buf);
            return false;
        }
        cache_write (buf[i], zeros);
        *sectors -= 1;
    }

    cache_write (*sector, buf);
    free (buf);
    return true;
}

static void
inode_extend (struct inode *inode, off_t new_length)
{
    ASSERT (inode != NULL);

    int i;
    off_t length = inode_length (inode);
    if (length >= new_length) return;

    struct inode_disk *disk_inode = (struct inode_disk *) malloc (DISK_SECTOR_SIZE);
    if (disk_inode == NULL)
        PANIC ("No memory for malloc");
    cache_read (inode->sector, disk_inode);

    disk_inode->length = new_length;

    // sector indexes
    int next_idx = bytes_to_sectors (length);
    int goal_idx = bytes_to_sectors (new_length);

    // how many sectors should be allocated additionally?
    int sectors = goal_idx - next_idx + 1;

    //printf ("next_idx : %d / goal_idx : %d / sectors : %d\n", next_idx, goal_idx, sectors);
    while (next_idx >= 0 && next_idx <= D_MAX && sectors > 0)//next_idx <= goal_idx)//sectors > 0)
    {
        if (!free_map_allocate (1, &disk_inode->direct_sectors[next_idx]))
            PANIC ("free_map_allocate failed");
        cache_write (disk_inode->direct_sectors[next_idx], zeros);
        next_idx += 1;
        sectors -= 1;
    }

    if (next_idx > D_MAX && next_idx <= SIND_MAX && sectors > 0)//next_idx <= goal_idx)//sectors > 0)
    {
        disk_sector_t *buf = (disk_sector_t *) malloc (DISK_SECTOR_SIZE);
        if (buf == NULL)
            PANIC ("No space for malloc");

        // if sind_sector is not set yet, allocate and initialize it.
        // else, load data from sind_sector
        if (disk_inode->sind_sector == NONE)
        {
            if (!free_map_allocate (1, &disk_inode->sind_sector))
                PANIC ("free_map_allocate failed");
            for (i = 0; i < SECTOR_PER_SINGLE; ++i)
                buf[i] = NONE;
        }
        else
            cache_read (disk_inode->sind_sector, buf);
        
        for (i = next_idx - D_MAX - 1; i < SECTOR_PER_SINGLE; ++i)
        {
            if (sectors <= 0)
                break;
            if (!free_map_allocate (1, &buf[i]))
            {
                //free (buf);
                PANIC ("free_map_allocate failed");
            }
            cache_write (buf[i], zeros);
            next_idx += 1;
            sectors -= 1;
        }

        cache_write (disk_inode->sind_sector, buf);
        free (buf);
    }

    if (next_idx > SIND_MAX && next_idx <= DIND_MAX && sectors > 0)//next_idx <= goal_idx)
    {
        disk_sector_t *out_buf = (disk_sector_t *) malloc (DISK_SECTOR_SIZE);
        if (out_buf == NULL)
            PANIC ("No space for malloc");

        if (disk_inode->dind_sector == NONE)
        {
            if (!free_map_allocate (1, &disk_inode->dind_sector))
                PANIC ("free_map_allocate failed");
            for (i = 0; i < SECTOR_PER_SINGLE; ++i)
                out_buf[i] = NONE;
        }
        else
            cache_read (disk_inode->dind_sector, out_buf);

        // Handle initial part
        int sind_idx = (next_idx - SIND_MAX - 1) / SECTOR_PER_SINGLE;
        int direct_idx = (next_idx - SIND_MAX - 1) % SECTOR_PER_SINGLE;

        disk_sector_t *in_buf = (disk_sector_t *) malloc (DISK_SECTOR_SIZE);
        if (in_buf == NULL)
            PANIC ("No space for malloc");

        if (out_buf[sind_idx] == NONE)
        {
            if (!free_map_allocate (1, &out_buf[sind_idx]))
                PANIC ("free_map_allocate failed");
            for (i = 0; i < SECTOR_PER_SINGLE; ++i)
                in_buf[i] = NONE;
        }
        else
            cache_read (out_buf[sind_idx], in_buf);

        for (i = direct_idx; i < SECTOR_PER_SINGLE; ++i)
        {
            if (sectors <= 0)
                break;
            if (!free_map_allocate (1, &in_buf[i]))
                PANIC ("free_map_allocate failed");
            cache_write (in_buf[i], zeros);
            //next_idx += 1; // will not be used anymore
            sectors -= 1;
        }
        cache_write (out_buf[sind_idx], in_buf);
        free (in_buf);

        // Rest part will be handled by alloc_single_indirect
        for (i = sind_idx + 1; i < SECTOR_PER_SINGLE; ++i)
        {
            if (sectors <= 0)
                break;
            if (!alloc_single_indirect (&out_buf[i], &sectors))
                PANIC ("free_map_allocate failed");
        }

        cache_write (disk_inode->dind_sector, out_buf);
        free (out_buf);
    }

    cache_write (inode->sector, disk_inode);
    free (disk_inode);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   disk.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (disk_sector_t sector, off_t length)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == DISK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      int sectors_idx = sectors - 1;

      // Initialization
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;

      int i;
      for (i = 0; i < DIRECT_NUM; ++i)
      {
          disk_inode->direct_sectors[i] = NONE;
      }
      disk_inode->sind_sector = NONE;
      disk_inode->dind_sector = NONE;
      // End of Initialization

      // Set all details of inode_disk, and write it on the given sector
      
      // directs
      for (i = 0; i < DIRECT_NUM; ++i)
      {
          if (sectors <= 0)
              break;
          if (!free_map_allocate (1, &disk_inode->direct_sectors[i]))
          {
              goto rollback;
          }
          cache_write (disk_inode->direct_sectors[i], zeros);
          sectors -= 1;
      }

      // single indirect
      if (sectors > 0)
      {
          if (!alloc_single_indirect (&disk_inode->sind_sector, &sectors))
              goto rollback;
      }

      // double indirect
      if (sectors > 0)
      {
          if (!free_map_allocate (1, &disk_inode->dind_sector))
              goto rollback;

          disk_sector_t *buf = (disk_sector_t *) malloc (DISK_SECTOR_SIZE);
          if (buf == NULL)
              PANIC ("No space for malloc");

          for (i = 0; i < SECTOR_PER_SINGLE; ++i)
              buf[i] = NONE;

          for (i = 0; i < SECTOR_PER_SINGLE; ++i)
          {
              if (sectors <= 0)
                  break;
              if (!alloc_single_indirect (&buf[i], &sectors))
              {
                  free (buf);
                  goto rollback;
              }
          }

          cache_write (disk_inode->dind_sector, buf);
          free (buf);
      }

      success = true;
rollback:
      if (!success)
      {
          // free allocated sectors?
          PANIC ("inode_create failed");
      }
      cache_write (sector, disk_inode);
      free (disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (disk_sector_t sector) 
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
disk_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

static void
inode_disk_release (struct inode_disk *disk_inode)
{
    if (disk_inode == NULL)
        return;

    int i;
    int sectors = bytes_to_sectors (disk_inode->length);

    // direct
    for (i = 0; i < DIRECT_NUM; ++i)
    {
        if (disk_inode->direct_sectors[i] == NONE)
            break;
        free_map_release (disk_inode->direct_sectors[i], 1);
    }

    // single indirect
    if (sectors > D_MAX + 1 && disk_inode->sind_sector != NONE)
    {
        for (i = 0; i < SECTOR_PER_SINGLE; ++i)
        {
            disk_sector_t sector;
            cache_read_at (disk_inode->sind_sector, &sector, i * sizeof (disk_sector_t), sizeof (disk_sector_t));
            if (sector == NONE)
                break;
            free_map_release (sector, 1);
        }
        free_map_release (disk_inode->sind_sector, 1);
    }

    if (sectors > SIND_MAX + 1 && disk_inode->dind_sector != NONE)
    {
        for (i = 0; i < SECTOR_PER_SINGLE; ++i)
        {
            disk_sector_t sind_sector;
            cache_read_at (disk_inode->dind_sector, &sind_sector, i * sizeof (disk_sector_t), sizeof (disk_sector_t));
            if (sind_sector == NONE)
                break;

            int j;
            for (j = 0; j < SECTOR_PER_SINGLE; ++j)
            {
                disk_sector_t sector;
                cache_read_at (sind_sector, &sector, j * sizeof (disk_sector_t), sizeof (disk_sector_t));
                if (sector == NONE)
                    break;
                free_map_release (sector, 1);
            }

            free_map_release (sind_sector, 1);
        }
        free_map_release (disk_inode->dind_sector, 1);
    }
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          struct inode_disk *disk_inode = (struct inode_disk *) malloc (DISK_SECTOR_SIZE);
          cache_read (inode->sector, disk_inode);

          inode_disk_release (disk_inode);
          free (disk_inode);

          free_map_release (inode->sector, 1);
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      disk_sector_t sector_idx = byte_to_sector (inode, offset);
      if (sector_idx == NONE)
          break; // read beyond EOF
      int sector_ofs = offset % DISK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = DISK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE) 
        {
          /* Read full sector directly into caller's buffer. */
          cache_read (sector_idx, buffer + bytes_read);
        }
      else 
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          cache_read_at (sector_idx, buffer + bytes_read, sector_ofs, chunk_size);
        }
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;

  if (inode->deny_write_cnt)
    return 0;

  off_t length = inode_length (inode);
  if (offset + size > length)
  {
      //printf ("length : %d / offset : %d => extending inode\n", length, offset);
      inode_extend (inode, offset + size);
      //printf ("new length : %d\n", inode_length (inode));
  }
  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      disk_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % DISK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = DISK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == DISK_SECTOR_SIZE) 
        {
          cache_write (sector_idx, buffer + bytes_written);
        }
      else 
        {
          cache_write_at (sector_idx, buffer + bytes_written, sector_ofs, chunk_size);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  off_t length;
  cache_read_at (inode->sector, &length, sizeof (uint32_t), sizeof (off_t));
  return length;
}
