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

#endif /* userprog/process.h */
