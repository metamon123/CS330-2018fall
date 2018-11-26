#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init (void);
void _exit (int status);
void _unmap (int mapid);
#endif /* userprog/syscall.h */
