#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "devices/disk.h"

#ifdef FILESYS
#include "threads/thread.h"
#include "filesys/cache.h"
#endif

/* The disk that contains the file system. */
struct disk *filesys_disk;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
#ifdef FILESYS
  cache_init ();
#endif
  filesys_disk = disk_get (0, 1);
  if (filesys_disk == NULL)
    PANIC ("hd0:1 (hdb) not present, file system initialization failed");

  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
#ifdef FILESYS
  cache_flush_all ();
#endif

  free_map_close ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *path, off_t initial_size, ftype type) 
{
  struct thread *cur = thread_current ();
  struct dir *cwd = (cur->cwd != NULL) ? dir_reopen (cur->cwd) : dir_open_root ();
  disk_sector_t inode_sector = 0;
  struct dir *dir = NULL;
  char *name = NULL;

  // if path is not valid && target directory is already removed
  if (!dir_parse (cwd, path, &dir, &name) || dir->inode->removed)
  {
      goto done;
  }

  bool success = (free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, type)
                  && dir_add (dir, name, inode_sector));

  if (!success && inode_sector != 0)
    // should release all sector in inode_sector... (directs, sind, dind, ...)
    free_map_release (inode_sector, 1);

  if (success && type == DIR_T)
  {
      // add . and ..
      struct inode *inode;
      if (!dir_lookup (dir, name, &inode))
        PANIC ("[ filesys_create () ] Directory creation failed\n");
      
      struct dir *sub_dir = dir_open (inode);
      if (!dir_add (sub_dir, ".", inode_sector)
          || !dir_add (sub_dir, "..", inode_get_inumber (dir_get_inode (dir))))
          PANIC ("[ filesys_create () ] creating . and .. failed");
      dir_close (sub_dir);
  }
done:
  if (dir != NULL)
    dir_close (dir);
  dir_close (cwd);
  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *path)
{
  struct thread *cur = thread_current ();
  struct dir *cwd = (cur->cwd != NULL) ? dir_reopen (cur->cwd) : dir_open_root ();
  struct dir *dir = NULL;
  char *name = NULL; 

  if (!dir_parse (cwd, path, &dir, &name))
  {
      // if path is not valid
      dir_close (cwd);
      return NULL;
  }

  if (dir->inode->removed)
  {
      // if target directory is already removed
      dir_close (cwd);
      dir_close (dir);
      return NULL;
  }

  dir_close (cwd);

  ASSERT (dir != NULL);

  struct inode *inode = NULL;
  if (dir != NULL)
    dir_lookup (dir, name, &inode);
  dir_close (dir);

  return file_open (inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *path) 
{
  struct thread *cur = thread_current ();
  struct dir *cwd = (cur->cwd != NULL) ? dir_reopen (cur->cwd) : dir_open_root ();
  struct dir *dir = NULL;
  char *name = NULL; 

  // TODO: parse name => (dir, filename)
  if (!dir_parse (cwd, path, &dir, &name))
  {
      dir_close (cwd);
      return NULL;
  }
  dir_close (cwd);
  ASSERT (dir != NULL);

  // directory => remove only when it's empty => done in dir_remove
 
  bool success = dir_remove (dir, name);
  dir_close (dir);

  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
