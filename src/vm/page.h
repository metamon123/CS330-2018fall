#include "threads/thread.h"
#include "threads/synch.h"
#include <hash.h>
#include <inttypes.h>


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

    //bool is_exist;
    enum location loc;
    bool writable;
};

void spt_init ();
void spt_destroy ();
