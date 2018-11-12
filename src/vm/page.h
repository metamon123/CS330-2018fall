#ifndef VM_PAGE_H
#define VM_PAGE_H

#include "threads/thread.h"
#include "threads/synch.h"
#include <hash.h>
#include <inttypes.h>
#include "vm/frame.h"
#include "filesys/file.h"

// MEM : page is in physical memory
// SWAP : page is in swap disk
// FS : page is in file system disk
enum location {NONE, MEM, SWAP, FS};

struct spt {
    struct thread *owner; // necessary?
    struct hash spt_hash;
    struct lock spt_lock;
};

struct spt_entry {
    struct spt *spt;
    // spte can access its parent spt hash easily

    void *upage;
    enum location location;

    // In physical memory
    struct frame_entry *fe;

    // In swap disk
    size_t swap_slot_idx; // slot = PGSIZE (8 sectors)

    bool writable;

    // In file system
    struct file *file;
    off_t ofs;
    size_t page_read_bytes;

    struct hash_elem hash_elem;
};

void spt_init (void);
void spt_destroy (void);
bool install_spte (struct spt *spt, struct spt_entry *spte);
struct spt_entry *get_spte (struct spt *spt, void *upage);

bool load_swap (struct spt_entry *spte);
bool load_file (struct spt_entry *spte);

#endif
