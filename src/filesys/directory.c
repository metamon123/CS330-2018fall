#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "filesys/cache.h"

/* A single directory entry. */
struct dir_entry 
  {
    disk_sector_t inode_sector;         /* Sector number of header. */
    char name[NAME_MAX + 1];            /* Null terminated file name. */
    bool in_use;                        /* In use or free? */
  };

/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (disk_sector_t sector, size_t entry_cnt) 
{
  bool success = inode_create (sector, entry_cnt * sizeof (struct dir_entry), DIR_T);
  if (success && sector == ROOT_DIR_SECTOR)
  {
      struct dir *root = dir_open_root ();

      // add . & .. in root directory
      dir_add (root, ".", ROOT_DIR_SECTOR);
      dir_add (root, "..", ROOT_DIR_SECTOR);
  }
  return success;
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) 
{
  struct dir *dir = calloc (1, sizeof *dir);
  if (inode != NULL && dir != NULL)
    {
      dir->inode = inode;
      dir->pos = 0;
      return dir;
    }
  else
    {
      inode_close (inode);
      free (dir);
      return NULL; 
    }
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *
dir_open_root (void)
{
  return dir_open (inode_open (ROOT_DIR_SECTOR));
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) 
{
  return dir_open (inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) 
{
  if (dir != NULL)
    {
      inode_close (dir->inode);
      free (dir);
    }
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) 
{
  return dir->inode;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
        struct dir_entry *ep, off_t *ofsp) 
{
  struct dir_entry e;
  size_t ofs;
  
  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (e.in_use && !strcmp (name, e.name)) 
      {
        if (ep != NULL)
          *ep = e;
        if (ofsp != NULL)
          *ofsp = ofs;
        return true;
      }
  return false;
}

/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
            struct inode **inode) 
{
  struct dir_entry e;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);
/*
  bool found = lookup (dir, name, &e, NULL);
  bool success = found;
  if (found && inode != NULL)
  {
      *inode = inode_open (e.inode_sector);
      success = (*inode != NULL);
  }
  else if (!found && inode != NULL)
  {
      *inode = NULL;
      success = false;
  }
*/
  if (!strcmp (name, "/"))
    *inode = inode_open (ROOT_DIR_SECTOR);
  else if (lookup (dir, name, &e, NULL))
    *inode = inode_open (e.inode_sector);
  else
    *inode = NULL;

  return *inode != NULL;
}

/* Parse the given PATH under CWD given by a caller,
 * and set appropriate DIR and FILENAME (among PATH) given by a caller.
 * A caller should close DIR. */
bool
dir_parse (struct dir *cwd, const char *path, struct dir **dir, const char **filename)
{
    //printf ("dir_parse - path : %s\n", path);
    ASSERT (*dir == NULL && *filename == NULL && cwd != NULL);
    if (path == NULL || *path == '\0')
        return false;

    char *cursor = path;
    bool abs_path = false;

    while (*cursor == '/')
    {
        abs_path = true;
        cursor += 1;
    }

    if (*cursor == '\0')
    {
        // path = "" or "/////"
        if (!abs_path)
        {
            // path = ""
            return false;
        }
        else
        {
            // path = "/////"
            *dir = dir_open_root ();
            *filename = ".";
            return true;
        }
    }

    // path = "////asdf~" or "asdf~"

    if (abs_path)
        cwd = dir_open_root ();
    else
        cwd = dir_reopen (cwd);

    // cwd should be closed later

    char *name_path = cursor;
    while (*cursor != '\0' && *cursor != '/')
    {
        cursor += 1;
    }

    if (*cursor == '\0')
    {
        // name_path = "asdf\0"
        *dir = cwd;
        *filename = name_path;
        return true;
    }

    // name_path = "asdf/~" or "asdf////~"
    char *dir_name = (char *) malloc (NAME_MAX + 1);
    strlcpy (dir_name, name_path, NAME_MAX + 1);
    *strchr(dir_name, '/') = '\0';
    
    // dir_name = "asdf\0~" or "asdf\0///~"
    while (*cursor == '/')
    {
        cursor += 1;
    }

    if (*cursor == '\0')
    {
        // name_path = "asdf/\0" or "asdf////\0"
        // => disallow (only allowed is '/')
        free (dir_name);
        dir_close (cwd);
        return false;
    }
    else
    {
        // name_path = "asdf/as~" or "asdf///as~"
        struct inode *inode;
        if (!dir_lookup (cwd, dir_name, &inode))
        {
            // non-exist directory name
            free (dir_name);
            dir_close (cwd);
            return false;
        }

        free (dir_name);
        dir_close (cwd);

        ASSERT (inode != NULL);

        // found inode should be directory inode
        if (!inode_is_dir (inode))
        {
            inode_close (inode);
            return false;
        }

        // found inode is directory!
        struct dir *sub_cwd = dir_open (inode);
        bool success = dir_parse (sub_cwd, cursor, dir, filename);
        dir_close (sub_cwd);
        return success;
    }
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool
dir_add (struct dir *dir, const char *name, disk_sector_t inode_sector) 
{
  //printf ("dir_add - sector : %d\n", inode_sector);
  struct dir_entry e;
  off_t ofs;
  bool success = false;
  
  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Check NAME for validity. */
  if (*name == '\0' || strlen (name) > NAME_MAX || strchr (name, '/') != NULL)
    return false;

  /* Check that NAME is not in use. */
  if (lookup (dir, name, NULL, NULL))
    goto done;

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.
     
     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy (e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

 done:
  return success;
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name) 
{
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Find directory entry. */
  if (!lookup (dir, name, &e, &ofs))
    goto done;

  /* Open inode. */
  inode = inode_open (e.inode_sector);
  if (inode == NULL)
    goto done;

  if (inode_is_dir (inode))
  {
      // TODO: check whether target directory is empty
  }
  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e) 
    goto done;

  /* Remove inode. */
  inode_remove (inode);
  success = true;

 done:
  inode_close (inode);
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
  struct dir_entry e;

  while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) 
    {
      dir->pos += sizeof e;
      if (e.in_use)
        {
          strlcpy (name, e.name, NAME_MAX + 1);
          return true;
        } 
    }
  return false;
}

bool
dir_chdir (const char *dir_path)
{
    struct thread *cur = thread_current ();
    ASSERT (cur->cwd != NULL); // chdir can called only by syscall

    struct dir *dir = NULL;
    char *name = NULL;

    if (!dir_parse (cur->cwd, dir_path, &dir, &name))
    {
        return NULL;
    }

    ASSERT (dir != NULL);

    struct inode *inode = NULL;
    bool success = dir_lookup (dir, name, &inode);
    dir_close (dir);

    if (success)
        cur->cwd = dir_open (inode);

    return success;
}
