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

void
_exit (int status)
{
  struct thread *cur = thread_current ();
  printf ("%s: exit(%d)\n", cur->name, status);

  cur->exit_status = status;
  thread_exit ();
}

static int
_open (const char *filename)
{
  struct file *file = filesys_open (filename);
  if (file == NULL)
    return -1;

  struct fd_elem *fdelem = (struct fd_elem *)malloc (sizeof (struct fd_elem));
  if (fdelem == NULL)
  {
    file_close (file);
    return -1;
  }

  fdelem->file = file;
  fdelem->fd = allocate_fd ();
  list_push_back (&thread_current ()->file_list, &fdelem->list_elem);
  return fdelem->fd;
}

static void
_close (int fd)
{
  struct fd_elem *fdelem = fd_lookup (fd);
  if (fdelem == NULL)
    return;

  ASSERT (fdelem->file != NULL);

  list_remove (&fdelem->list_elem); // remove from process's filelist
  file_close (fdelem->file);
  free (fdelem);
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
    struct file *file = fd2file (fd);
    if (file == NULL)
      return -1;
    return file_read (file, buffer, size);
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
    struct file *file = fd2file (fd);
    if (file == NULL)
      return -1;
    return file_write (file, buffer, size);
  }
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
        filesys_lock_acquire ();
        f->eax = filesys_create (*(const char **)(esp + 4), *(off_t *)(esp + 8));
        filesys_lock_release ();
        bad_exit = false;
        break;
    case SYS_REMOVE:
        if (!check_ubuf (esp + 4))
          break;
        filesys_lock_acquire ();
        f->eax = filesys_remove (*(const char **)(esp + 4));
        filesys_lock_release ();
        bad_exit = false;
        break;
    case SYS_OPEN:
        if (!check_ubuf (esp + 4))
          break;
        filesys_lock_acquire ();
        f->eax = _open (*(const char **)(esp + 4));
        filesys_lock_release ();
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
        filesys_lock_acquire ();
        f->eax = _read (*(int *)(esp + 4), *(const void **)(esp + 8), *(uint32_t *)(esp + 12));
        filesys_lock_release ();
        bad_exit = false;
        break;
    case SYS_WRITE:
        if (!check_uaddr (esp + 4, 4) || !check_uaddr (esp + 8, 4) || !check_uaddr (esp + 12, 4)
            || !check_uaddr (*(void **)(esp + 8), *(uint32_t *)(esp + 12)))
          break;
        filesys_lock_acquire ();
        f->eax = _write (*(int *)(esp + 4), *(const void **)(esp + 8), *(uint32_t *)(esp + 12));
        filesys_lock_release ();
        bad_exit = false;
        break;
    case SYS_SEEK:
        if (!check_uaddr (esp + 4, 4) || !check_uaddr (esp + 8, 4))
          break;
        filesys_lock_acquire ();
        file_seek (fd2file (*(int *)(esp + 4)), *(uint32_t *)(esp + 8));
        filesys_lock_release ();
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
        filesys_lock_acquire ();
        _close (*(int *)(esp + 4));
        filesys_lock_release ();
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
