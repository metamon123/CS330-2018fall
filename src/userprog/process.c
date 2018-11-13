#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "threads/malloc.h"

#ifdef VM
#include "vm/frame.h"
#include "vm/page.h"
#endif

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
static void set_arguments (void **_esp, char *fn_copy, size_t init_len);

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy, *delim;
  tid_t tid;
  struct semaphore sema_start;
  bool load_success = false;
  size_t init_filename_len;

  void *aux[4]; // share pointers with start_process

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);
  init_filename_len = strlen(fn_copy);

  // printf ("initial argument : %s | init_filename_len : %d\n", fn_copy, init_filename_len);
  delim = strchr (fn_copy, ' ');
  if (delim != NULL)
    *delim = 0; // set first occurance of ' ' -> '\0'

  sema_init (&sema_start, 0);

  aux[0] = fn_copy;
  aux[1] = &sema_start;
  aux[2] = &load_success;
  aux[3] = init_filename_len; // This length doesn't include \0
  
  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (fn_copy, PRI_DEFAULT, start_process, (void *)aux);
  if (tid == TID_ERROR)
  {
    // start_process would not be executed
    palloc_free_page (fn_copy);
    return tid;
  }

  sema_down (&sema_start);
  palloc_free_page (fn_copy); // -> start_process will free it
  if (!load_success)
  {
    return TID_ERROR;
  }

  struct thread *child = tid2thread (tid);
  ASSERT (child != NULL);

  struct thread *cur = thread_current();

  list_push_back (&cur->child_list, &child->child_elem);
  // if above line is on thread_create, it will be problematic if one process tries consequent process_exec.
  return tid;
}

/* A thread function that loads a user process and makes it start
   running. */
static void
start_process (void *_aux)
{
  void **aux = (void **)_aux;
  char *fn_copy = aux[0];
  struct semaphore *sema_startp = aux[1];
  bool *load_success = aux[2];
  size_t init_filename_len = aux[3];

  struct intr_frame if_;
  bool success;

  thread_current ()->is_process = true;
#ifdef VM
  spt_init ();
#endif
  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (fn_copy, &if_.eip, &if_.esp);

  // printf ("load complete\n");
  /* If load failed, quit. */
  //palloc_free_page (file_name);
  if (!success)
  {
    // printf ("load failed, exitting...\n");
    *load_success = success;
    sema_up (sema_startp);
    thread_exit ();
  }

  // printf ("load succeed, setting arguments...\n");
  set_arguments (&if_.esp, fn_copy, init_filename_len);
  // printf ("argument setting finished\n");

  *load_success = success;
  sema_up (sema_startp);

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

static void
set_arguments (void **_esp, char *fn_copy, size_t init_len)
{
  // argument setting
  char *adjusted_argv = fn_copy + init_len + 1;
  char *adjusted_argv_stack, *cursor, *end_ptr;
  size_t adjusted_len = 0;
  int argc = 0;


  cursor = fn_copy;
  end_ptr = fn_copy + init_len + 1;

  while (strchr (" ", *cursor) != NULL) cursor++;
  while (cursor < end_ptr)
  {
    char *token = cursor;
    size_t len;

    while (strchr (" ", *cursor) == NULL) cursor++;

    if (*cursor != '\0') *cursor = '\0';

    len = strlen (token);
    if (len > 0)
    {
      strlcpy (adjusted_argv + adjusted_len, token, len + 1);
      argc++;
      adjusted_len += len + 1;
    }

    cursor++;

    while (strchr (" ", *cursor) != NULL) cursor++;
  }

  // copy adjusted_argv into user stack
  *_esp -= adjusted_len;
  adjusted_argv_stack = *_esp;
  memcpy ((void *)adjusted_argv_stack, (const void *)adjusted_argv, adjusted_len);

  // align to 4byte multiples
  while ((int)(*_esp) % 4 != 0)
  {
    *_esp -= 1;
    *(char *)(*_esp) = 0;
  }

  // set argv[]
  int i;
  *_esp -= 4 * (argc + 1);
  cursor = adjusted_argv_stack;
  for (i = 0; i < argc; ++i)
  {
    // iterate adjusted_argv_stack to get addresses of each argument
    size_t len = strlen (cursor);
    *((void **)(*_esp) + i) = cursor;
    cursor += len + 1;
  }
  *((void **)(*_esp) + argc) = NULL;

  *_esp -= 4;
  *(void **)(*_esp) = *_esp + 4;

  // set argc
  *_esp -= 4;
  *(int *)(*_esp) = argc;

  // later return address will be set here
  *_esp -= 4;
  // TODO: remove hex_dump later
  // hex_dump (*_esp, *_esp, PHYS_BASE - *_esp, true);
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid) 
{
  struct list_elem *e;
  struct thread *t = NULL;
  struct thread *cur = thread_current ();
  int exit_status;

  if (child_tid == TID_ERROR) return -1;

  //printf ("loop starts\n");
  for (e = list_begin (&cur->child_list); e != list_end (&cur->child_list);
       e = list_next (e))
  {
    struct thread *_t = list_entry (e, struct thread, child_elem);
    ASSERT (_t != NULL);

    if (_t->tid == child_tid)
    {
      t = _t;
      break;
    }
  }
  //printf ("loop ends\n");

  if (t == NULL) return -1; // child_tid is not a child of cur.

  // printf ("waiting for child %d\n", child_tid);
  // struct thread *t = tid2thread (child_tid);
  sema_down (&t->sema_wait);
  
  exit_status = t->exit_status; // we can assure that exit_status is completely set
  
  list_remove (&t->child_elem);

  sema_up (&t->sema_destroy);
  return exit_status;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *curr = thread_current ();
  uint32_t *pd;

  struct list_elem *e;

  filesys_lock_acquire ();
  e = list_begin (&curr->file_list);
  while (e != list_end (&curr->file_list))
  {
    struct fd_elem *fdelem = list_entry (e, struct fd_elem, list_elem);

    ASSERT (fdelem != NULL);

    e = list_remove (e);
    file_close (fdelem->file);
    free (fdelem);
  }
  filesys_lock_release ();

  // For orphan processes
  for (e = list_begin (&curr->child_list); e != list_end (&curr->child_list);
       e = list_next (e))
  {
    struct thread *_t = list_entry (e, struct thread, child_elem);
    ASSERT (_t != NULL);

    sema_up (&_t->sema_destroy);
  }

  // TODO: synchronize spt_destroy
  spt_destroy (); // free all spte structure & corresponding frame_entry / frame / swap slot
  // spt structure will not be freed yet,
  // since its on struct thread (not malloc'd)

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = curr->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      curr->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

struct fd_elem *
fd_lookup (int fd)
{
  struct thread *cur = thread_current ();
  // iterate cur->filelist, and return appropriate struct fd_elem *
  struct list_elem *e;
  for (e = list_begin (&cur->file_list); e != list_end (&cur->file_list);
       e = list_next (e))
  {
    struct fd_elem *fdelem = list_entry (e, struct fd_elem, list_elem);
    ASSERT (fdelem != NULL);

    if (fdelem->fd == fd)
      return fdelem;
  }
  return NULL;
}

struct file *
fd2file (int fd)
{
  struct fd_elem *fdelem = fd_lookup (fd);
  if (fdelem != NULL)
    return fdelem->file;
  return NULL;
}

int
allocate_fd (void)
{
  struct thread *cur = thread_current ();
  int return_fd;

  lock_acquire (&cur->fd_lock); // may not be needed due to filesys_lock

  return_fd = cur->next_fd;
  cur->next_fd++;

  lock_release (&cur->fd_lock);

  if (cur->next_fd == 0)
    PANIC ("Too many fds!\n");
  return return_fd;
}
/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  filesys_lock_acquire ();
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  struct fd_elem *fdelem = (struct fd_elem *)malloc (sizeof (struct fd_elem));
  if (fdelem == NULL)
  {
    goto done;
  }
  fdelem->file = file;
  fdelem->fd = allocate_fd ();
  list_push_back (&t->file_list, &fdelem->list_elem);

  success = true;
  file_deny_write (file);

 done:
  /* We arrive here whether the load is successful or not. */
  if (!success) file_close (file);
  filesys_lock_release ();
  return success;
}

/* load() helpers. */

//static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  struct thread *cur = thread_current ();
  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Do calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;
/*
      // Get a page of memory.
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      // Load this page.
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false; 
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      // Add the page to the process's address space.
      if (!install_page (upage, kpage, writable)) 
        {
          palloc_free_page (kpage);
          return false; 
        }
*/
      // TODO on 3-2
      // do special handling for the case where
      // page_zero_bytes == PGSIZE

      // TODO on 3-1

      struct spt_entry *spte = (struct spt_entry *) malloc (sizeof (struct spt_entry));
      spte->spt = cur->spt;
      spte->upage = upage;
      spte->location = NONE;
      spte->fe = NULL;
      spte->swap_slot_idx = -1;
      spte->writable = writable;
      spte->file = file;
      spte->ofs = ofs;
      spte->page_read_bytes = page_read_bytes;

      // spte->hash_elem (contains list_elem) does not need to be initialize. (list_insert does it)

      lock_acquire (&frame_lock);
      struct frame_entry *fe = frame_alloc (PAL_USER, spte);
      // lock_release (&frame_lock);

      if (fe == NULL)
      {
          lock_release (&frame_lock);
          free (spte);
          return false;
      }
      
      // Other process can try to swap out the frame while I'm setting it...
      // Do lock? or Pinning on the frame?
      if (file_read (file, fe->kpage, page_read_bytes) != (int) page_read_bytes)
      {
          frame_free (fe);
          lock_release (&frame_lock);
          free (spte);
          return false;
      }

      memset (fe->kpage + page_read_bytes, 0, page_zero_bytes);

      if (!install_page (upage, fe->kpage, writable))
      {
          frame_free (fe);
          lock_release (&frame_lock);
          free (spte);
          return false;
      }

      ASSERT 

      // frame alloc -> success
      // file read -> success
      // install to pagedir -> success

      spte->location = MEM;
      spte->fe = fe;
      
      lock_acquire (&spte->spt->spt_lock);
      if (!install_spte (spte->spt, spte))
      {
          // it fails if
          // there already exists a spte with same upage
          lock_release (&spte->spt->spt_lock);

          frame_free (fe);
          pagedir_clear_page (cur->pagedir, spte->upage);
          lock_release (&frame_lock);
          free (spte);
          return false;
      }
      lock_release (&spte->spt->spt_lock);

      fe->is_pin = false;

      lock_release (&frame_lock);
      // for debug
      printf ("load_segment - spte info : \nspte->upage = 0x%x\nspte->fe->kpage : 0x%x\nspte->fe = 0x%x\nspte->swap_slot_idx = %d\nspte->ofs = %d\nspte->location = %d\n", spte->upage, spte->fe->kpage, spte->fe, spte->swap_slot_idx, spte->ofs, spte->location);
      //spte = get_spte (cur->spt, upage);
      //printf ("load_segment - get_spte info : \nspte->upage = 0x%x\nspte->fe = 0x%x\nspte->swap_slot_idx = %d\nspte->ofs = %d\nspte->location = %d\n", spte->upage, spte->fe, spte->swap_slot_idx, spte->ofs, spte->location);


      // Advance.
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      ofs += page_read_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
/*
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL) 
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
        palloc_free_page (kpage);
    }
  return success;
*/
  struct thread *cur = thread_current ();
  bool success = false;

  struct spt_entry *spte = (struct spt_entry *) malloc (sizeof (struct spt_entry));
  if (spte == NULL)
      return false;

  spte->spt = cur->spt;
  spte->upage = ((uint8_t *) PHYS_BASE) - PGSIZE;
  spte->location = NONE;
  spte->fe = NULL;
  spte->swap_slot_idx = -1;
  spte->writable = true;
  spte->file = NULL;


  lock_acquire (&frame_lock);
  struct frame_entry *fe = frame_alloc (PAL_USER | PAL_ZERO, spte);
  if (fe == NULL)
  {
      lock_release (&frame_lock);
      free (spte);
      return false;
  }

  success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, fe->kpage, true);
  if (success)
      *esp = PHYS_BASE;
  else
  {
      frame_free (fe);
      lock_release (&frame_lock);
      free (spte);
      return false;
  }

  spte->location = MEM;
  spte->fe = fe;
  lock_acquire (&spte->spt->spt_lock);
  if (!install_spte (spte->spt, spte))
  {
      lock_release (&spte->spt->spt_lock);

      frame_free (fe);
      lock_release (&frame_lock);
      free (spte);
      return false;
  }
  lock_release (&spte->spt->spt_lock);

  fe->is_pin = false;
  lock_release (&frame_lock);

  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
