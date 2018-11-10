#include "threads/thread.h"
#include "threads/synch.h"
#include <hash.h>
#include <inttypes.h>
#include "vm/frame.h"


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
    // uint32_t(?) swap_slot

    bool writable;

    // In file system
    // something

    struct hash_elem hash_elem;
};

void spt_init ();
void spt_destroy ();
bool install_spte (struct spt *spt, struct spt_entry *spte);
struct spt_entry *get_spte (struct spt *spt, void *upage);
