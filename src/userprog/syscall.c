#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

#include "threads/vaddr.h"
#include "threads/init.h" // power_off

static void syscall_handler (struct intr_frame *);

/* Check whether user's address (address ~ address + size - 1) is valid.
 * - Less than PHYS_BASE (0xc0000000)
 * - Larger than min(text area base address?)
 *   */
bool verify_user_memory_access (uint32_t address, uint32_t size)
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

static void
_exit (int status)
{
  // TODO
  // - convey exit_status to parent in some way
  // - etc 
  thread_exit ();
}

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  printf ("system call!\n");
  uint32_t esp = (uint32_t)f->esp;
  int syscall_num; // assuming that sizeof(int) == 4

  // esp check is necessary since user can control it
  if (!verify_user_memory_access (esp, 4))
  {
    // TODO: notify bad exit
    thread_exit ();
  }

  syscall_num = *(int *)esp;

  switch(syscall_num)
  {
    case SYS_HALT:
        power_off ();
        break;
    case SYS_EXIT:
        if (verify_user_memory_access (esp + 4, 4))
        {
          _exit (*(int *)(esp + 4));
        }
        else
        {
          // TODO: notify bad exit
          thread_exit ();
        }
        break;
    case SYS_EXEC:
        bool bad_exit = true;
        if (verify_user_memory_access (esp + 4, 4))
        {
          const char *cmd_line = *(const char **)(esp + 4);
          if (verify_user_memory_access ((uint32_t)cmd_line,
                                (uint32_t)strlen (cmd_line) + 1))
          {
            f->eax = process_execute (cmd_line);
            bad_exit = false;
          }
        }

        if (bad_exit)
        {
          // TODO: notify bad exit
          thread_exit ();
        }
        break;
    case SYS_WAIT:
        break;
    case SYS_CREATE:
        break;
    case SYS_REMOVE:
        break;
    case SYS_OPEN:
        break;
    case SYS_FILESIZE:
        break;
    case SYS_READ:
        break;
    case SYS_WRITE:
        break;
    case SYS_SEEK:
        break;
    case SYS_TELL:
        break;
    case SYS_CLOSE:
        break;
    default:
        printf ("Invalid syscall number\n");
        // TODO: notify bad exit
        thread_exit ();
  }
  // thread_exit ();
}
