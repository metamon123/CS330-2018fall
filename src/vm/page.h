#include "threads/thread.h"
#include <hash.h>
#include <inttypes.h>

struct spt_entry {
    bool is_registered;
    bool writable;
};
