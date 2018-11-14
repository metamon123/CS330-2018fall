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
  void *last_page = pg_round_down ((void *)address + size);
  for (page = pg_round_down ((void *)address); page <= last_page; page += PGSIZE)
  {
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
                  break;
              case MEM:
                  printf ("syscall spte check -> MEM, wrong situation\n");
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
          return false;
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
        result = filesys_create (filename, size);
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
    if (file == NULL)
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
    if (file == NULL)
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
    file_seek (fd2file (fd), pos);
    filesys_lock_release ();
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
