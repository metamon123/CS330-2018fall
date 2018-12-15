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

#define DIRECT_NUM 124
#define SECTOR_PER_SINGLE 128

// max index among direct sectors (123)
#define D_MAX DIRECT_NUM - 1

// max index among single-indirect sectors (251)
#define SIND_MAX D_MAX + SECTOR_PER_SINGLE

// max index among double-indirect sectors (32507)
#define DIND_MAX SIND_MAX + (SIND_MAX + 1) * SECTOR_PER_SINGLE

static char zeros[DISK_SECTOR_SIZE];

/* On-disk inode.
   Must be exactly DISK_SECTOR_SIZE bytes long. */
struct _inode_disk
  {
    disk_sector_t start;                /* First data sector. */
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    uint32_t unused[125];               /* Not used. */
  };

struct inode_disk
{
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
    //struct inode_disk data;             /* Inode content. */
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
  printf ("sizeof (disk_sector_t sector) == %d\n", sizeof (sector));

  struct inode_disk *disk_inode = (struct inode_disk *) malloc (DISK_SECTOR_SIZE);
  if (disk_inode == NULL)
      PANIC ("[ byte_to_sector ] no memory for malloc");

  cache_read (inode->sector, disk_inode);

  printf("inode-sector %d / inode-length %d / pos %d\n", inode->sector, disk_inode->length, pos);
  if (pos < disk_inode->length)
  {
      int sector_idx = pos / DISK_SECTOR_SIZE;

      if (sector_idx <= D_MAX)
      {
          // direct
          sector = disk_inode->direct_sectors[sector_idx];
      }
      else if (sector_idx <= SIND_MAX)
      {
          // single indirect
          // ex.
          //    sector_idx = 124 (first sector in single_indirect sectors)
          //    idx = 0
          int idx = sector_idx - D_MAX - 1;
          cache_read_at (disk_inode->sind_sector, &sector, idx * sizeof (disk_sector_t), sizeof (sector));
      }
      else if (sector_idx <= DIND_MAX)
      {
          // double indirect
          // ex.
          //    sector_idx = 252 (first sector in double_indirect sectors)
          //    sind_idx = 0
          //    direct_idx = 0
          int sind_idx = (sector_idx - SIND_MAX - 1) / SECTOR_PER_SINGLE;
          int direct_idx = (sector_idx - SIND_MAX - 1) % SECTOR_PER_SINGLE;
          
          disk_sector_t sind_sector;
          cache_read_at (disk_inode->dind_sector, &sind_sector, sind_idx * sizeof (disk_sector_t), sizeof (sind_sector));
          cache_read_at (sind_sector, &sector, direct_idx * sizeof (disk_sector_t), sizeof (sector));
      }
      else
      {
          PANIC ("[ byte_to_sector ] Too big pos %d == index %d sector", pos, sector_idx);
      }
  }

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

bool
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
    
    while (next_
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
      for (i = 0; i < DIRECT_NUM, ++i)
      {
          disk_inode->direct_sectors[i] = NONE;
      }
      disk_inode->sind_sector = NONE;
      disk_inode->dind_sector = NONE;
      // End of Initialization

      // TODO: set all details of inode_disk, and write it on the given sector
      int next_sidx = 0; // next sector index
      
      // directs
      while (next_sidx <= D_MAX)
      {
          if (sectors > 0)
          {
              if (!free_map_allocate (1, &disk_inode->direct_sectors[next_sidx]))
              {
                  goto rollback;
              }
              cache_write (disk_inode->direct_sectors[next_sidx], zeros);
              sectors -= 1;
          }
          next_sidx += 1;
      }

      // single indirect
      if (sectors > 0)
      {
          if (!free_map_allocate (1, &disk_inode->sind_sector))
              goto rollback;

          disk_sector_t *buf = (disk_sector_t *) malloc (DISK_SECTOR_SIZE);
          if (buf == NULL)
              PANIC ("No space for malloc");
          for (i = 0; i < SECTOR_PER_SINGLE; ++i)
              buf[i] = NONE;

          //while (next_sidx <= SIND_MAX)
          for (i = 0; i < SECTOR_PER_SINGLE; ++i)
          {
              //int single_idx = next_sidx - D_MAX - 1;
              if (sectors > 0)
              {
                  //if (!free_map_allocate (1, &buf[single_idx]))
                  if (!free_map_allocate (1, &buf[i]))
                  {
                      free (buf);
                      goto rollback;
                  }
                  //cache_write (buf[single_idx], zeros);
                  cache_write (buf[i], zeros);
                  sectors -= 1;
              }
              next_sidx += 1;
          }
          cache_write (disk_inode->sind_sector, buf);
          free (buf);
      }

      // double indirect
      if (sectors > 0)
      {
          asdf
      }



rollback:
      if (!success)
      {
          // free allocated sectors
      }
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
  //disk_read (filesys_disk, inode->sector, &inode->data);
  cache_read (inode->sector, &inode->data);
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
          free_map_release (inode->sector, 1);
          free_map_release (inode->data.start,
                            bytes_to_sectors (inode->data.length)); 
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
  //uint8_t *bounce = NULL;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      disk_sector_t sector_idx = byte_to_sector (inode, offset);
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
          //disk_read (filesys_disk, sector_idx, buffer + bytes_read); 
          cache_read (sector_idx, buffer + bytes_read);
        }
      else 
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          cache_read_at (sector_idx, buffer + bytes_read, sector_ofs, chunk_size);
          /*
          if (bounce == NULL) 
            {
              bounce = malloc (DISK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          disk_read (filesys_disk, sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
          */
        }
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  //free (bounce);

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
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

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
          /* Write full sector directly to disk. */
          //disk_write (filesys_disk, sector_idx, buffer + bytes_written); 
          cache_write (sector_idx, buffer + bytes_written);
        }
      else 
        {
          /* We need a bounce buffer. */
          /*
          if (bounce == NULL) 
            {
              bounce = malloc (DISK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          */

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */

          /*
          if (sector_ofs > 0 || chunk_size < sector_left) 
            disk_read (filesys_disk, sector_idx, bounce);
          else
            memset (bounce, 0, DISK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          disk_write (filesys_disk, sector_idx, bounce); 
          */
          cache_write_at (sector_idx, buffer + bytes_written, sector_ofs, chunk_size);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  //free (bounce);

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
  return inode->data.length;
}
