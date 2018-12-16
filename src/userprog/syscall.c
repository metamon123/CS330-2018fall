#include "userprog/syscall.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

#include "threads/vaddr.h"
#include "threads/init.h" // power_off
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/malloc.h"
#include "devices/input.h" // input_getc

#ifdef VM
#include "vm/page.h"
#endif

#ifdef FILESYS
#include "filesys/inode.h"
#endif

static void syscall_handler (struct intr_frame *);

/* Check whether user's address (address ~ address + size - 1) is valid.
 * - Less than PHYS_BASE (0xc0000000)
 * - Larger than min(text area base address?)
 *   */
static bool
check_uaddr (uint32_t address, uint32_t size)
{
  if (!is_user_vaddr((const void *)address))
    return false;

  // {address < PHYS_BASE}

  if ((uint32_t)PHYS_BASE - address < size) // PHYS_BASE = address + size is yet okay
    return false;

  // {address < PHYS_BASE && address + size <= PHYS_BASE (don't have to worry about overflow)}

/*
  Codes below are not used. Instead, user will give strlen + 1 to verify string pointer.
  if (null_term_chk)
  {
    // check the case where
    // - 0xbfffffff = 'c'; (not null)
    // - user try puts(0xbfffffff);
    // -> I'll not allow even the above case since
    //    it lets user know that *0xc0000000 is NULL.
    //    User should not get any information about kernel address space.
    uint32_t end_address = address + size - 1;
    return !(strchr ((const char *)end_address, 0) >= PHYS_BASE);
  }
*/
  return true;
}

static bool
check_ubuf (uint32_t address)
{
  const char *buf;

  if (!check_uaddr (address, 4))
    return false;

  buf = *(const char **)address;

  return check_uaddr (buf, strlen (buf) + 1);
}

// preload() : preload the pages to be used and pin them
// purpose : it prevents dead-lock due to the page_fault in file-related syscalls
// caution : it should be called after check_addr
static bool
preload (uint32_t address, uint32_t size)
{
  struct thread *cur = thread_current ();
  void *page;
  void *last_page = pg_round_down ((void *)address + size - 1);
  for (page = pg_round_down ((void *)address); page <= last_page; page += PGSIZE)
  {
      //printf ("in preload : page 0x%x\n", page);
      lock_acquire (&cur->spt->spt_lock);
      struct spt_entry *spte = get_spte (cur->spt, page);
      lock_release (&cur->spt->spt_lock);

      bool success = false;
      if (spte == NULL)
      {
          if (page >= cur->sc_esp - 32 && page >= 0xbf000000)
          {
              lock_acquire (&frame_lock);
              lock_acquire (&cur->spt->spt_lock);

              success = grow_stack (page);
              if (success)
              {
                  struct spt_entry *spte = get_spte (cur->spt, page);
                  spte->fe->is_pin = true;
                  list_push_back (&cur->pin_list, &spte->fe->pin_elem);
              }
              lock_release (&cur->spt->spt_lock);
              lock_release (&frame_lock);
          }
      }
      else
      {
          lock_acquire (&frame_lock);
          lock_acquire (&cur->spt->spt_lock);
          switch (spte->location)
          {
              case NONE:
                  success = false;
                  break;
              case MEM:
                  success = true;
                  break;
              case SWAP:
                  success = load_swap (spte);
                  if (success)
                  {
                      spte->fe->is_pin = true;
                      list_push_back (&cur->pin_list, &spte->fe->pin_elem);
                  }
                  break;
              case FS:
                  success = load_file (spte);
                  if (success)
                  {
                      spte->fe->is_pin = true;
                      list_push_back (&cur->pin_list, &spte->fe->pin_elem);
                  }
                  break;
              default:
                  PANIC ("[ preload tid %d ] wrong spte->location %d\n", cur->tid, spte->location);
          }
          lock_release (&cur->spt->spt_lock);
          lock_release (&frame_lock);
      }
      if (!success)
      {
          //printf ("page 0x%x failed\n", page);
          return false;
      }
      //printf ("page 0x%x success\n", page);
  }
  return true;
}

void
unpin_all ()
{
    struct list_elem *e;
    struct thread *cur = thread_current ();
    lock_acquire (&frame_lock);
    for (e = list_begin (&cur->pin_list); e != list_end (&cur->pin_list);
         e = list_remove (e))
    {
        struct frame_entry *fe = list_entry (e, struct frame_entry, pin_elem);
        ASSERT (fe != NULL && fe->is_pin);
        fe->is_pin = false;
    }
    lock_release (&frame_lock);
}

void
_exit (int status)
{
  struct thread *cur = thread_current ();
  printf ("%s: exit(%d)\n", cur->name, status);

  // Do below process only if status == -1?
  if (status == -1)
  {
      // unpin the frames pinned by the process
      unpin_all ();

      // unlock all the locks held by the process
      struct list_elem *e;
      while ((e = list_begin (&cur->lock_list)) != list_end (&cur->lock_list))
      {
          struct lock *lock = list_entry (e, struct lock, elem);
          ASSERT (lock != NULL);
          lock_release (lock);
      }
  }

  cur->exit_status = status;
  thread_exit ();
}

static bool
_create (const char *filename, uint32_t size)
{
    bool result;
    bool success = preload (filename, strlen (filename) + 1);
    if (success)
    {
        filesys_lock_acquire ();
        result = filesys_create (filename, size, FILE_T);
        filesys_lock_release ();

        unpin_all ();
        return result;
    }
    else
    {
        struct thread *cur = thread_current ();
        cur->in_syscall = false;
        cur->sc_esp = NULL;
        _exit (-1);
    }
}

static bool
_remove (const char *filename)
{
    bool result;
    bool success = preload (filename, strlen (filename) + 1);
    if (success)
    {
        filesys_lock_acquire ();
        result = filesys_remove (filename);
        filesys_lock_release ();

        unpin_all ();
        return result;
    }
    else
    {
        struct thread *cur = thread_current ();
        cur->in_syscall = false;
        cur->sc_esp = NULL;
        _exit (-1);
    }
}


static int
_open (const char *filename)
{
  struct thread *cur = thread_current ();
  if (!preload (filename, strlen (filename) + 1))
  {
    cur->in_syscall = false;
    cur->sc_esp = NULL;
    _exit (-1);
  }
  
  filesys_lock_acquire ();
  struct file *file = filesys_open (filename);
  if (file == NULL)
  {
    filesys_lock_release ();
    unpin_all ();
    return -1;
  }

  struct fd_elem *fdelem = (struct fd_elem *)malloc (sizeof (struct fd_elem));
  if (fdelem == NULL)
  {
    file_close (file);
    filesys_lock_release ();
    unpin_all ();
    return -1;
  }

  fdelem->file = file;
  fdelem->fd = allocate_fd ();
  list_push_back (&cur->file_list, &fdelem->list_elem);

  filesys_lock_release ();
  unpin_all ();
  return fdelem->fd;
}

static void
_close (int fd)
{
  filesys_lock_acquire ();
  struct fd_elem *fdelem = fd_lookup (fd);
  if (fdelem == NULL)
  {
    filesys_lock_release ();
    return;
  }

  ASSERT (fdelem->file != NULL);

  list_remove (&fdelem->list_elem); // remove from process's filelist
  file_close (fdelem->file);
  free (fdelem);
  filesys_lock_release ();
}

static int
_read (int fd, const void *buffer, uint32_t size)
{
  if (fd == 0)
  {
    int i;
    for (i = 0; i < size; ++i)
    {
      *(char *)(buffer + size) = input_getc ();
    }
    return size;
  }
  else
  {
    struct thread *cur = thread_current ();
    if (!preload (buffer, size))
    {
      cur->in_syscall = false;
      cur->sc_esp = NULL;
      _exit (-1);
    }

    filesys_lock_acquire ();
    struct file *file = fd2file (fd);
    if (file == NULL || file_is_dir (file))
    {
      filesys_lock_release ();
      unpin_all ();
      return -1;
    }

    int result = file_read (file, buffer, size);
    filesys_lock_release ();
    unpin_all ();
    return result;
  }
}

static int
_write (int fd, const void *buffer, uint32_t size)
{
  if (fd == 1)
  {
    putbuf ((const char *)buffer, (size_t)size);
    return size;
  }
  else
  {
    struct thread *cur = thread_current ();
    if (!preload (buffer, size))
    {
      cur->in_syscall = false;
      cur->sc_esp = NULL;
      _exit (-1);
    }

    filesys_lock_acquire ();
    struct file *file = fd2file (fd);
    if (file == NULL || file_is_dir (file))
    {
      filesys_lock_release ();
      unpin_all ();
      return -1;
    }
    int result = file_write (file, buffer, size);
    filesys_lock_release ();
    unpin_all ();
    return result;
  }
}

static void
_seek (int fd, uint32_t pos)
{
    // no need for preloading
    filesys_lock_acquire ();
    struct file *file = fd2file (fd);
    if (!file_is_dir (file))
        file_seek (file, pos);
    filesys_lock_release ();
}

static int
_mmap (int fd, void *addr)
{
    // if addr == NULL or != PGSIZE * k => error (return -1)
    if (addr == NULL || ((uint32_t) addr) % PGSIZE != 0)
        return -1;

    // get a file object with fd
    // * not found => error (return -1)
    // * fd = 0, 1 will be handled with error automatically
    struct fd_elem *fdelem = fd_lookup (fd);
    if (fdelem == NULL)
        return -1;

    filesys_lock_acquire ();
    struct file *file = file_reopen (fdelem->file);
    filesys_lock_release ();

    uint32_t len = file_length (file);

    // file length == 0 => error
    if (len == 0)
    {
        filesys_lock_acquire ();
        file_close (file);
        filesys_lock_release ();
        return -1;
    }

    //printf ("here1\n");
    struct thread *cur = thread_current ();
    //printf ("here2\n");
    uint32_t read_bytes = len;
    off_t ofs = 0;
    //uint32_t zero_bytes = ROUND_UP (read_bytes, PGSIZE) - read_bytes;

    // Check overlapping mappings, and register spt entries if there is no overwrapping.
    lock_acquire (&cur->spt->spt_lock);
    uint32_t upage;
    for (upage = (uint32_t) addr; upage < (uint32_t) addr + len; upage += PGSIZE)
    {
        struct spt_entry *spte = get_spte (cur->spt, (void *)upage);
        if (spte != NULL && spte->location != NONE)
        {
            lock_release (&cur->spt->spt_lock);
            filesys_lock_acquire ();
            file_close (file);
            filesys_lock_release ();
            return -1;
        }
    }

    struct mmap_elem *mmelem = (struct mmap_elem *) malloc (sizeof (struct mmap_elem));
    mmelem->mapid = allocate_mapid ();
    mmelem->file = file;
    mmelem->start = addr;
    mmelem->len = len;
    list_push_back (&cur->mmap_list, &mmelem->list_elem);

    while (read_bytes > 0)
    {
        size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
        size_t page_zero_bytes = PGSIZE - page_read_bytes;

        struct spt_entry *spte = (struct spt_entry *) malloc (sizeof (struct spt_entry));
        spte->spt = cur->spt;
        spte->upage = addr;
        spte->location = FS;
        spte->fe = NULL;
        spte->swap_slot_idx = -1;
        spte->writable = true;
        spte->file = file;
        spte->ofs = ofs;
        spte->page_read_bytes = page_read_bytes;
        spte->is_mmap = true;

        if(!install_spte (spte->spt, spte))
        {
            lock_release (&spte->spt->spt_lock);
            free (spte);
            list_remove (&mmelem->list_elem);
            free (mmelem);
            // should remove all spte previously set.
            // but it's tired. just panic
            PANIC ("MMAP : install_spte failed");
            return -1;
        }

        // Advance.
        read_bytes -= page_read_bytes;
        ofs += page_read_bytes;
        addr += PGSIZE;
    }
    lock_release (&cur->spt->spt_lock);

    return mmelem->mapid;
}

void
_unmap (int mapid)
{
    struct mmap_elem *mmelem = mmap_lookup (mapid);
    if (mmelem == NULL)
    {
        return;
    }

    ASSERT ((uint32_t) mmelem->start % PGSIZE == 0);

    struct thread *cur = thread_current ();
    uint32_t page;
    for (page = (uint32_t) mmelem->start; page < (uint32_t) mmelem->start + mmelem->len; page += PGSIZE)
    {
        //printf ("unmapping all region for mapid %d\n", mapid);
        //printf ("here3\n");
        lock_acquire (&cur->spt->spt_lock);
        struct spt_entry *spte = spte_delete (cur->spt, page);
        lock_release (&cur->spt->spt_lock);
        //printf ("here4\n");
        if (spte == NULL)
        {
            // TODO: more specific error msg is needed
            printf ("no spte for upage %d\n", page);
            return;
        }
        
        ASSERT (spte->is_mmap);
        
        lock_acquire (&frame_lock);
        lock_acquire (&cur->spt->spt_lock);
        switch (spte->location)
        {
            case NONE:
                printf ("mmap spte->location == NONE\n");
                break;
            case MEM:
                ASSERT (spte->fe != NULL);
                
                write_back (spte);
                frame_free (spte->fe);

                uint32_t *pd = spte->spt->owner->pagedir;
                if (pd != NULL)
                    pagedir_clear_page (pd, spte->upage);
                break;
            case SWAP:
                swap_slot_free (spte->swap_slot_idx);
                break;
            case FS:
                break;
            default:
                PANIC ("[ _unmap() where spte->upage == 0x%x ] Invalid spte->loction = %d", spte->upage, spte->location);
        }
        lock_release (&cur->spt->spt_lock);
        lock_release (&frame_lock);
        free (spte);
    }

    list_remove (&mmelem->list_elem);
    filesys_lock_acquire ();
    file_close (mmelem->file);
    filesys_lock_release ();
    free (mmelem);
}

bool
_chdir (const char *dir)
{
    // cur->cwd =
    return false; 
}

bool
_mkdir (const char *dir)
{
    bool result;
    bool success = preload (dir, strlen (dir) + 1);
    if (success)
    {
        filesys_lock_acquire ();
        result = filesys_create (dir, size, DIR_T);
        filesys_lock_release ();

        unpin_all ();
        return result;
    }
    else
    {
        struct thread *cur = thread_current ();
        cur->in_syscall = false;
        cur->sc_esp = NULL;
        _exit (-1);
    }
}

bool
_readdir (int fd, char *name)
{
    return false;
}

bool
_isdir (int fd)
{
    return false;
}

int
_inumber (int fd)
{
    return -1;
}

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  // printf ("system call!\n");
  uint32_t esp = (uint32_t)f->esp;
  struct thread *cur = thread_current ();
  cur->sc_esp = esp;
  cur->in_syscall = true;

  int syscall_num; // assuming that sizeof(int) == 4
  bool bad_exit = true;
  // esp check is necessary since user can control it
  if (!check_uaddr (esp, 4))
  {
    // TODO: notify bad exit
    //thread_exit ();
    _exit (-1);
  }

  syscall_num = *(int *)esp;

  switch(syscall_num)
  {
    case SYS_HALT:
        power_off ();
        break;
    case SYS_EXIT:
        if (check_uaddr (esp + 4, 4))
        {
          _exit (*(int *)(esp + 4));
          bad_exit = false;
        }
        break;
    case SYS_EXEC:
        if (!check_ubuf (esp + 4))
          break;
        f->eax = process_execute (*(const char **)(esp + 4));
        bad_exit = false;
        break;
    case SYS_WAIT:
        if (!check_uaddr (esp + 4, 4))
          break;
        f->eax = process_wait (*(tid_t *)(esp + 4));
        bad_exit = false;
        break;
    case SYS_CREATE:
        if (!check_ubuf (esp + 4) || !check_uaddr (esp + 8, 4))
          break;

        f->eax = _create (*(char **)(esp + 4), *(off_t *)(esp + 8));
        bad_exit = false;
        break;
    case SYS_REMOVE:
        if (!check_ubuf (esp + 4))
          break;

        f->eax = _remove (*(char **)(esp + 4));
        bad_exit = false;
        break;
    case SYS_OPEN:
        if (!check_ubuf (esp + 4))
          break;

        f->eax = _open (*(char **)(esp + 4));
        bad_exit = false;
        break;
    case SYS_FILESIZE:
        if (!check_uaddr (esp + 4, 4))
          break;
        f->eax = file_length (fd2file (*(int *)(esp + 4)));
        bad_exit = false;
        break;
    case SYS_READ:
        if (!check_uaddr (esp + 4, 4) || !check_uaddr (esp + 8, 4) || !check_uaddr (esp + 12, 4)
            || !check_uaddr (*(void **)(esp + 8), *(uint32_t *)(esp + 12)))
          break;

        f->eax = _read (*(int *)(esp + 4), *(void **)(esp + 8), *(uint32_t *)(esp + 12));
        bad_exit = false;
        break;
    case SYS_WRITE:
        if (!check_uaddr (esp + 4, 4) || !check_uaddr (esp + 8, 4) || !check_uaddr (esp + 12, 4)
            || !check_uaddr (*(void **)(esp + 8), *(uint32_t *)(esp + 12)))
          break;

        f->eax = _write (*(int *)(esp + 4), *(void **)(esp + 8), *(uint32_t *)(esp + 12));
        bad_exit = false;
        break;
    case SYS_SEEK:
        if (!check_uaddr (esp + 4, 4) || !check_uaddr (esp + 8, 4))
          break;

        // no need for preloading (no dereference of user address)
        _seek (*(int *)(esp + 4), *(uint32_t *)(esp + 8));
        bad_exit = false;
        break;
    case SYS_TELL:
        if (!check_uaddr (esp + 4, 4))
          break;
        f->eax = file_tell (fd2file (*(int *)(esp + 4)));
        bad_exit = false;
        break;
    case SYS_CLOSE:
        if (!check_uaddr (esp + 4, 4))
          break;

        _close (*(int *)(esp + 4));
        bad_exit = false;
        break;
    case SYS_MMAP:
        if (!check_uaddr (esp + 4, 4) || !check_uaddr (esp + 8, 4))
          break;
        f->eax = _mmap (*(int *)(esp + 4), *(void **)(esp + 8));
        bad_exit = false;
        break;
    case SYS_MUNMAP:
        if (!check_uaddr (esp + 4, 4))
          break;
        _unmap (*(int *)(esp + 4));
        bad_exit = false;
        break;
    case SYS_CHDIR:
        if (!check_ubuf (esp + 4))
          break;
        f->eax = _chdir (*(char **)(esp + 4));
        bad_exit = false;
        break;
    case SYS_MKDIR:
        if (!check_ubuf (esp + 4))
          break;
        f->eax = _mkdir (*(char **)(esp + 4));
        bad_exit = false;
        break;
    case SYS_READDIR:
        break;
    case SYS_ISDIR:
        if (!check_uaddr (esp + 4, 4))
          break;
        f->eax = _isdir (*(int *)(esp + 4));
        bad_exit = false;
        break;
    case SYS_INUMBER:
        if (!check_uaddr (esp + 4, 4))
          break;
        f->eax = _inumber (*(int *)(esp + 4));
        bad_exit = false;
        break;
    default:
        printf ("Invalid syscall number\n");
  }
  cur->in_syscall = false;
  cur->sc_esp = NULL;
  if (bad_exit)
  {
    // TODO: notify bad exit
    //thread_exit ();
    _exit (-1);
  }
}
