#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "filesys/file.h"
#include <list.h>

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

struct fd_elem {
  int fd;
  struct file *file;
  struct list_elem list_elem;
};

struct fd_elem *fd_lookup (int);
struct file *fd2file (int);
int allocate_fd (void);

struct mmap_elem {
  int mapid;
  void *start;
  uint32_t len;
  struct list_elem list_elem;
};

struct mmap_elem *mmap_lookup (int);
int allocate_mapid (void);

bool install_page (void *upage, void *kpage, bool writable);
#endif /* userprog/process.h */
