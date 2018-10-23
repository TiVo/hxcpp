#ifndef ANDROID
#include <execinfo.h>
#endif
#include <hxcpp.h>
#include <list>

// Mac OS X implementation of std::tr1::unordered_map is buggy?
#ifdef HX_MACOS
#define USE_STD_MAP
#endif

#ifdef USE_STD_MAP
#include <map>
#else
#include <tr1/unordered_map>
#endif

#include <pthread.h>
#include <set>
#include <signal.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "hx/GC.h"
#include "GCThreading.h"
#include "Hash.h"

// Enable process logging on STB targets
#ifdef TIVO_STB
#include <stx/StxProcessLog.h>
#define PLOG(type, data_len, data) process_log(type, data_len, data)
#else
#define PLOG(type, data_len, data)
#endif

// Define TIVOCONFIG_GC_COLLECT_STACK_TRACES to enable the collection of
// stack traces for every GC allocation.
// #define TIVOCONFIG_GC_COLLECT_STACK_TRACES

// This is the minimum amount of allocated memory before any garbage
// collection will be done.  Memory usage less than this will never cause a
// garbage collect (unless someone forces it by calling InternalCollect).
// Whenever an alloc or realloc would result in more heap than this being
// used, a collect is done first to try to free up memory.
#define MIN_COLLECT_SIZE (10 * 1024 * 1024)
// This computes the next threshold at which garbage collection will be done,
// given a current number of allocated bytes.  This is used at the end of a
// collect to set up the threshold for the next collect.
#define NEXT_COLLECT_SIZE(x) ((x) + MIN_COLLECT_SIZE)

// Every still-reachable allocation is marked with the most recent mark on
// each garbage collect.  The marks toggle between these two values,
// conveniently chosen to fit into the flags of an Entry.
#define MARK_A                  (1 << 29)
#define MARK_B                  (1 << 28)

// Flags for use in Entry flags
#define LAST_MARK_MASK          (MARK_A | MARK_B)
#define IS_OBJECT_FLAG          (1 << 27)
#define IS_WEAK_REF_FLAG        (1 << 26)
#define HAS_FINALIZER_FLAG      (1 << 25)
#define IN_8B_ALLOCATOR_FLAG    (1 << 24)
#define IN_32B_ALLOCATOR_FLAG   (1 << 23)
#define IN_128B_ALLOCATOR_FLAG  (1 << 22)

#ifdef TIVO_STB
// Threshold at which a plog is dumped indicating that this many allocs have
// recently occurred.  Will be taken from the environment variable
// TIVOCONFIG_GC_RECENT_ALLOCS_THRESHOLD if it is set, otherwise defaults to
// 1000.
static uint32_t g_recent_allocs_threshold = 1000;
// Number of stack frames to dump at each alloc threshold for the allocation
// that crosses the threshold; collecting stack traces in this way allows
// sampling of allocations.  This can be expensive so the default is 0, you
// have to turn it on via TIVOCONFIG_GC_RECENT_ALLOCS_STACK_FRAME_COUNT.
static uint32_t g_recent_allocs_frames = 0;
// Tracks the number of allocs since the last "recent allocs" plog
static uint32_t g_recent_allocs;

static inline void check_alloc_threshold()
{
    if (process_log_available() && 
        (++g_recent_allocs == g_recent_allocs_threshold)) {
        PLOG(PLOG_TYPE_STX_GC_RECENT_ALLOCS, sizeof(uint32_t),
             &g_recent_allocs);
        g_recent_allocs = 0;
        if (g_recent_allocs_frames > 0) {
            void *frames[24];
            int cnt = backtrace(frames, g_recent_allocs_frames);
            PLOG(PLOG_TYPE_STX_GC_RECENT_ALLOCS_BACKTRACE,
                 (cnt * sizeof(void *)), frames);
        }
    }
}
#endif

// All allocations are immediately preceded by an entry structure:
// - Small allocations go through one of the PoolAllocator instances, which
//   pre-allocate large numbers of fixed size entries
// - Large allocations allocate memory with a LargeEntry structure
// The flags and bytes members are identically offset, which allows recovering
// the flags from any pointer regardless of whether the allocation was from
// a pooled allocator or a large allocation.
typedef struct Entry
{
    // If this Entry is an Entry, then this is the next Entry in the free
    // list.
    // If this Entry is a LargeEntry, then this is the next LargeEntry in the
    // allocated list.
    union {
        Entry *next;
        struct LargeEntry *largeNext;
    };
    // Flags describing the allocation.  Note that the flags are actually
    // stored at (bytes - 4 bytes), to allow the technique that hxcpp uses
    // for strings to work, which is to treat data pointers as int[] and
    // then index by -1 to get a 32 bit flags value.  Therefore this field
    // just reserves space for the flags, but the flags are actually the 4
    // bytes immediately before [bytes], which may or may not be at the
    // address of the flags_spacing field.
    uint32_t flags_spacing;
    // The allocated data
    union {
        // This needs to match the alignment of any data type that would be
        // allocated by the hxcpp runtime.
        double align[0];
        char bytes[0];
    };
} Entry;

// Large allocations use this entry header to hold an additional size value
typedef struct LargeEntry
{
    // Size of the large allocation
    uint32_t size;
    // Common Entry structure
    Entry entry;
} LargeEntry;


// Recover an Entry from a pointer
#define ENTRY_FROM_PTR(ptr)                                                  \
    ((Entry *) (((uintptr_t) (ptr)) - offsetof(Entry, bytes)))

// Recover a LargeEntry from a pointer
#define LARGE_ENTRY_FROM_PTR(ptr)                                            \
    ((LargeEntry *) (((uintptr_t) (ptr)) - (offsetof(LargeEntry, entry) +    \
                                            offsetof(Entry, bytes))))

// Get a reference to the flags of an entry
#define FLAGS_OF_ENTRY(entry) (((uint32_t *) ((entry)->bytes))[-1])


// Helper function that allocates (from the system -- i.e. malloc)
// a chunk of memory of the given size
static void *SystemMallocLocked(int size);

// Helper function that allocates (from the system -- i.e. calloc) and clears
// a chunk of memory of the given size and clearing it out
static void *SystemCallocLocked(int size);

// Helper function that re-allocates (from the system - i.e. realloc) and
// clears any additional memory of the chunk of memory of the given size and
// clearing it out
static void *SystemReallocLocked(void *ptr, int size, int old_size);

// OK this doesn't really clear anything, but the names are being kept
// consistent here
static void SystemFreeLocked(void *ptr);

// Helper function that does a garbage collect if adding [size] bytes of
// memory would go over the garbage collection threshold, and returns true if
// a garbage collect was done and false if not
static bool GarbageCollectIfNecessaryLocked(int size);

// Clears the weak reference pointer for all allocations that were weakly
// referenced and are no longer reachable
static void ClearWeakRefsLocked();

// Sets the finalizer for an object to either f, if its set, or
// internal_finalizer, of it's set, or to nothing if neither is set
static void SetFinalizerLocked(hx::Object *obj, hx::finalizer f,
                               hx::InternalFinalizer *internal_finalizer);

// Helper function to run a finalizer for a given object.  It is assumed that
// there is a finalizer for the pointer.
static void RunFinalizerLocked(hx::Object *obj);

// Perform an a "large" allocate, i.e. one that doesn't fit into one of the
// static small allocators.  Note that this is considerably less efficient
// than using the small allocators, but then again, large allocates should be
// the minority of allocations and generally the work of allocation should be
// well overshadowed by the actual work done using the allocated block.
static void *LargeAllocLocked(int size, uint32_t is_object_flag);

// Realloc from an entry to an allocation done using LargeAllocLocked
static void *LargeReallocLocked(LargeEntry *entry, int size);

// Just like InternalNew, but called with GC lock held, and always allocates
// memory that will be GC'd when no longer referenced.
static void *InternalNewLocked(int size, bool isObject);

// Just like InternalCollect, but called with GC lock held
static void InternalCollectLocked();

// Marks all pointers located in the range [start, end] (inclusive) with the
// current g_mark value.  Thread concurrency is managed by the GCThreading
// API.
static void MarkPointers(void *start, void *end);


/**
 * Saves backtraces associated with a given pointer, in a form amenable to
 * extracting from a memory dump.
 **/

typedef struct Backtrace
{
    struct Backtrace *next;
    void *ptr;
    void *frames[33];
} Backtrace;


// For ease of detecting the backtrace in a memory dump, and to ensure that
// these globals end up in the heap and not the bss, this has to be done this
// way
class Backtrace_Globals
{
public:

    Backtrace_Globals()
    {
#ifdef TIVOCONFIG_GC_COLLECT_STACK_TRACES
        fingerprint1 = 0xdeadcccc;
        fingerprint2 = 0xdeadcc11;
        memset(map, 0, sizeof(map));
#endif
    }

    // These make the backtrace array easy to find
    uint32_t fingerprint1, fingerprint2;

    // Simple hash table like thing.  Never resized.
#ifdef TIVOCONFIG_GC_COLLECT_STACK_TRACES
    Backtrace *map[377737];
#else
    // Don't waste heap space if stack traces are not going to be collected
    Backtrace *map[1];
#endif
};
static Backtrace_Globals backtrace_globals;


static inline int map_backtrace(void *ptr)
{
    return (((uintptr_t) ptr) % 
            (sizeof(backtrace_globals.map) / sizeof(backtrace_globals.map[0])));
}


/**
 * Saves the backtrace of the current stack and associates it with the given
 * pointer.
 **/
static void save_backtrace(void *ptr)
{
#ifndef ANDROID
    // Create a Backtrace
    Backtrace *bt = (Backtrace *) malloc(sizeof(Backtrace));
    bt->ptr = ptr;
    // Collect the backtrace.  Collect at most the number of backtraces that
    // will fit in the frames array, minus one, to always allow enough room
    // for a zero terminator, and also write the zero terminator (the index of
    // which is returned by the backtrace function).
    bt->frames[backtrace(bt->frames, 
                         (sizeof(bt->frames) / sizeof(bt->frames[0])) - 1)] = 0;

    // Put the backtrace in the map
    int idx = map_backtrace(ptr);
    bt->next = backtrace_globals.map[idx];
    backtrace_globals.map[idx] = bt;
#endif
}


/**
 * Clears the backtrace for a given pointer
 **/
static void clear_backtrace(void *ptr)
{
    int idx = map_backtrace(ptr);
    Backtrace *mapped = backtrace_globals.map[idx];
    Backtrace *mapped_prev = 0;

    while (mapped && (mapped->ptr != ptr)) {
        mapped_prev = mapped;
        mapped = mapped->next;
    }

    if (!mapped) {
        return;
    }

    if (mapped_prev) {
        mapped_prev->next = mapped->next;
    }
    else {
        backtrace_globals.map[idx] = mapped->next;
    }

    free(mapped);
}


// Finalizer bookkeeping
typedef struct FinalizerData
{
    hx::finalizer f;
    hx::InternalFinalizer *internal_finalizer;
} FinalizerData;


// Weak reference -- this is an Object that just references another object,
// and doesn't factor into garbage collection marking (and thus lets the
// pointed-to object go away if it's not otherwise referenced), but that has
// its object pointer zeroed when the pointed-to object is garbage collected.
class GCWeakRef : public hx::Object
{
public:

    GCWeakRef(Dynamic inRef) : mRef(inRef) { }

    Dynamic mRef;
};


// This is the Object to id mapping; this is unfortunate stuff but the GC API
// requires it.  This static data is protected by GCThreading_Lock().
#ifdef HXCPP_M64

#ifdef USE_STD_MAP
static std::map<uintptr_t, int> gObjMap;
static std::map<int, uintptr_t> gIdMap;
#else
static std::tr1::unordered_map<uintptr_t, int> gObjMap;
static std::tr1::unordered_map<int, uintptr_t> gIdMap;
#endif

static int gCurrentId;
#endif


typedef struct Root
{
    struct Root *prev, *next;
    hx::Object **pptr;
} Root;


// Global variables - collected in a class so that they will be located on the
// heap (not in bss), which assists in finding them when doing memory
// forensics.   This static data is protected by GCThreading_Lock().
class GCPooled_Globals
{
public:

    GCPooled_Globals()
    {
        g_fingerprint1 = 0xdeadaabb;
        g_fingerprint2 = 0xdeadaabd;
        g_mark = MARK_A;
        g_gc_disabled = true;
        g_next_collect_size = MIN_COLLECT_SIZE;
    }

    // Fingerprint, to allow GCPooled_Globals singleton to be located in a
    // memory dump
    uint32_t g_fingerprint1, g_fingerprint2;

    // The most recent mark value, used to mark objects during the mark phase;
    // toggles between MARK_A and MARK_B, to match the last mark bits of an
    // Entry's flag_and_offset value.  This value is protected from concurrent
    // multithreaded access by virtue of only being used during GC Mark, and
    // GC Mark is performed only when all threads besides the thread running
    // GC Mark having been stopped via the GCThreading API.
    uint32_t g_mark;

    // This is the total number of bytes currently allocated via the the
    // garbage collector allocation APIs.
    unsigned int g_total_size;

    // This is the total number of objects "reaped" during a GC, to use for
    // plogging purposes
    unsigned int g_total_reaped;

    // This maps allocations to the weak refs to them.  Protected by
    // GCThreading_Lock().
#ifdef USE_STD_MAP
    std::map<void *, std::list<GCWeakRef *> > g_weak_ref_map;
#else
    std::tr1::unordered_map<void *, std::list<GCWeakRef *> > g_weak_ref_map;
#endif

    // Garbage collection can be temporarily disabled, although it's not clear
    // why this would ever be done.  Start GC disabled until it is ready, this
    // prevents static initializers from suffering from unexpected GC.
    bool g_gc_disabled;

    // This is the number of bytes at which the next garbage collection will
    // be invoked on a large malloc (this occurs right before any alloc that
    // would push g_total_size above this value)
    unsigned int g_next_collect_size;

    // These are explicitly declared root objects; hxcpp doesn't really seem to
    // use these except to mark some CFFI stuff
    Root *g_roots;

    // This maps objects to finalizer data - it is expected that very few
    // finalizers will be used
#ifdef USE_STD_MAP
    std::map<hx::Object *, FinalizerData> g_finalizers;
#else
    std::tr1::unordered_map<hx::Object *, FinalizerData> g_finalizers;
#endif

    std::list<hx::HashBase<Dynamic> *> g_weak_hash_list;
};
static GCPooled_Globals globals;

// These #define statements allow members of globals to be used by name which
// makes the rest of the code easier to read
#define g_mark globals.g_mark
#define g_total_size globals.g_total_size
#define g_total_reaped globals.g_total_reaped
#define g_weak_ref_map globals.g_weak_ref_map
#define g_gc_disabled globals.g_gc_disabled
#define g_next_collect_size globals.g_next_collect_size
#define g_roots globals.g_roots
#define g_finalizers globals.g_finalizers
#define g_weak_hash_list globals.g_weak_hash_list


static void RemoveWeakRef(GCWeakRef *wr)
{
    void *ptr = wr->mRef.mPtr;
#ifdef USE_STD_MAP
    std::map<void *, std::list<GCWeakRef *> >::iterator iter = 
#else
    std::tr1::unordered_map<void *, std::list<GCWeakRef *> >::iterator iter = 
#endif
        g_weak_ref_map.find(ptr);
    if (iter == g_weak_ref_map.end()) {
        return;
    }
    std::list<GCWeakRef *> &refs = iter->second;
    refs.remove(wr);
    if (refs.empty()) {
        g_weak_ref_map.erase(iter);
    }
}


// Allocator where allocations from malloc() are done block-at-a-time,
// and objects can be quickly added to and removed from the block.
//
// Note that instances of this template will always have their methods called
// with the global GC lock held, so no locking is done internally within the
// methods of this template.
//
// entrySizeC is the (maximum) size of blocks of memory that can be allocated
// from this allocator.  Ideally allocations from the allocator would be as
// close to this value as possible, without going over, to result in the least
// amount of wasted space
// entriesPerBlockC is the number of entries per block; the allocator starts
// out with a static block that size and then adds blocks when new entries are
// needed and frees blocks when all entries are free.
template<int entrySizeC, int entriesPerBlockC, int allocatorFlagC>
class PoolAllocator
{
public:

    // Assumed to live in BSS (thus all values start out auto initialized
    // to zero)
    PoolAllocator()
    {
        fingerprint1 = 0xdeadaabb;
        fingerprint2 = 0xdeada000 + entrySizeC;
        // Start the block list with the fixed block
        blocksM = &fixedBlockM;
        fixedBlockM.fingerprint1 = 0xdeadaabb;
        fixedBlockM.fingerprint2 = 0xdeadb000 + entrySizeC;
        fixedBlockM.prev = blocksM;
        fixedBlockM.next = blocksM;
        this->AddEntriesOfBlock(blocksM);
    }

    // If [grow] is true, a new block will be added to satisfy the allocate if
    // no free entries are available; if [grow] is false, 0 will be returned if
    // no free entries are available.
    void *Allocate(uint32_t is_object_flag, bool grow)
    {
        // If no free entries ...
        if (!freeM) {
            // If not allowed to grow, then can't acquire new lines
            if (!grow) {
                return 0;
            }
            // Try to add a new block
            this->AddNewBlock();
            // If still no free entries, then couldn't even grow - really out
            // of memory!
            if (!freeM) {
                return 0;
            }
        }

        // Get the free entry.
        Entry *entry = freeM;

        // Set its flags to indicate what block it was allocated from and
        // whether or not it is an object
        FLAGS_OF_ENTRY(entry) = (g_mark | allocatorFlagC | is_object_flag);

        // Remove the entry from the free list
        freeM = entry->next;

        // Now entrySizeC bytes are in use
        g_total_size += entrySizeC;

#ifdef TIVO_STB
        check_alloc_threshold();
#endif

        return entry->bytes;
    }

    // It is assumed that [entry] was allocated from this allocator.
    void *Reallocate(Entry *entry, int size)
    {
        // It's possible that it will fit, if so just update it in place
        if (size <= entrySizeC) {
            // memset the new part (if any)
            memset(&(entry->bytes[size]), 0, entrySizeC - size);
            return entry->bytes;
        }

        // Didn't fit, so must acquire a new value.
        void *newptr = InternalNewLocked
            (size, FLAGS_OF_ENTRY(entry) & IS_OBJECT_FLAG);
        if (!newptr) {
            return 0;
        }

        // Copy the old memory over
        memcpy(newptr, entry->bytes, entrySizeC);

        // Move the prior entry to the free list since it's no longer
        // allocated, having been replaced by the new entry, clearing its
        // bytes first so that it's ready for re-use.
        memset(entry, 0, sizeof(SizedEntry));
        entry->next = freeM;
        freeM = entry;

        // The new size was already added to g_total_size in InternalNewLocked
        // above.  Reduce by the size that is no longer being used.
        g_total_size -= entrySizeC;

        // The old entry is no longer used, it must not have had its
        // HAS_FINALIZER_FLAG set since objects are never re-allocated, so
        // there is no worry about the old memory having a finalizer called
        // incorrectly.
        return newptr;
    }

    // Return true if this Entry is in one of this allocator's blocks
    bool Contains(Entry *entry)
    {
        uintptr_t e = (uintptr_t) entry;

        // Look in each block to see if it could be in there
        Block *block = blocksM;
        do {
            // First check to see if the entry is even within the range of
            // entries of this block, skip if not
            uintptr_t first = (uintptr_t) block->entries;
            uintptr_t last = (uintptr_t) &(block->entries[entriesPerBlockC]);
            if ((e >= first) && (e < last)) {
                // It points within the block, now return whether or not it
                // is an allocated entry - i.e. if it's both aligned properly
                // to be an Entry, and the Entry is allocated.
                return (((((uintptr_t) (e - first)) %
                          sizeof(SizedEntry)) == 0) &&
                        (FLAGS_OF_ENTRY(entry) & LAST_MARK_MASK));
            }
            block = block->next;
        } while (block != blocksM);

        return false;
    }

    // Puts dead entries onto the free list (calling finalizers as necessary),
    // freeing empty blocks.  Any entry with a last_mark not equal to g_mark
    // is considered dead, if the last_mark is zero then it was already dead,
    // but if it's nonzero but not equal to the current mark then it is newly
    // dead and should be finalized.
    void Sweep()
    {
        Block *block = blocksM;

        do {
            // Search just for newly-freed entries, and count the number of
            // free entries in the block
            int free_count = 0;
            for (int i = 0; i < entriesPerBlockC; i++) {
                Entry *entry = (Entry *) &(block->entries[i]);
                int entry_mark = FLAGS_OF_ENTRY(entry) & LAST_MARK_MASK;
                if (entry_mark == g_mark) {
                    // Still used
                    continue;
                }
                // The entry is free
                free_count += 1;
                // Is it newly free?
                if (entry_mark == 0) {
                    // No, was already free, so has already been processed
                    continue;
                }
                if (FLAGS_OF_ENTRY(entry) &
                    (HAS_FINALIZER_FLAG | IS_WEAK_REF_FLAG)) {
                    if (FLAGS_OF_ENTRY(entry) & HAS_FINALIZER_FLAG) {
                        RunFinalizerLocked((hx::Object *) (entry->bytes));
                    }
                    // If it's a weak reference object itself, then remove it
                    // from the list of weak references
                    else {
                        RemoveWeakRef((GCWeakRef *) (entry->bytes));
                    }
                }
#ifdef HXCPP_M64
                uintptr_t ptr = (uintptr_t) (entry->bytes);
                if (gIdMap.count(ptr)) {
                    int id = gIdMap[ptr];
                    gIdMap.erase(ptr);
                    gObjMap.erase(id);
                }
#endif
                g_total_size -= entrySizeC;
                g_total_reaped += 1;
#ifdef TIVOCONFIG_GC_COLLECT_STACK_TRACES
                clear_backtrace(entry->bytes);
#endif
                // Clear out its bytes, so that it is ready for re-use
                memset(entry, 0, sizeof(SizedEntry));
                // And put it on the free list
                entry->next = freeM;
                freeM = entry;
            }

            Block *next = block->next;

            // If the block is not the fixed block, and all of the entries are
            // free, then free the block - first removing its entries from the
            // free list
            if ((block != &fixedBlockM) && (free_count == entriesPerBlockC)) {
                this->RemoveBlock(block);
            }

            block = next;
        } while (block != blocksM);
    }

    // Returns the number of bytes allocated by this allocator (not all bytes
    // may be in use, some may be in unused blocks)
    int GetAllocatedSizeLocked() const
    {
        int total = 0;
        Block *block = blocksM;
        do {
            total += sizeof(*block);
        } while (block != blocksM);
        return total + sizeof(*this);
    }

private:
    
    typedef struct SizedEntry
    {
        Entry entry;
        char sizing[entrySizeC];
    } SizedEntry;

    typedef struct Block
    {
        // Help locate blocks in memory
        uint32_t fingerprint1, fingerprint2;
        Block *prev, *next;
        SizedEntry entries[entriesPerBlockC];
    } Block;

    void AddNewBlock()
    {
        Block *block = (Block *) SystemCallocLocked(sizeof(Block));
        if (!block) {
            // Try a collect first to see if that frees up enough system
            // memory to allow allocating the block
            InternalCollectLocked();
            block = (Block *) SystemCallocLocked(sizeof(Block));
            // If still couldn't get the block, the definitely out of memory
            if (!block) {
                return;
            }
        }

        block->fingerprint1 = 0xdeadaabb;
        block->fingerprint2 = 0xdeadc000 + entrySizeC;
        block->next = blocksM;
        block->prev = blocksM->prev;
        blocksM->prev->next = block;
        blocksM->prev = block;

        this->AddEntriesOfBlock(block);
    }

    void RemoveBlock(Block *block)
    {
        // A block always has every entry on the free entries list when it is
        // removed.
        Entry *entry = freeM, *prev = 0;

        Entry *first_entry = (Entry *) block->entries;
        Entry *last_entry = (Entry *) &(block->entries[entriesPerBlockC - 1]);
        while (entry) {
            if ((entry >= first_entry) && (entry <= last_entry)) {
                // It's in the block, so remove it from the free list
                entry = entry->next;
                if (prev) {
                    prev->next = entry;
                }
                else {
                    freeM = entry;
                }
            }
            else {
                prev = entry;
                entry = entry->next;
            }
        }
        
        // The block cannot be the head since the head is the fixed block and
        // the fixed block is never removed
        block->next->prev = block->prev;
        block->prev->next = block->next;
        
        SystemFreeLocked(block);
    }

    void AddEntriesOfBlock(Block *block)
    {
        for (int i = 0; i < entriesPerBlockC; i++) {
            Entry *entry = (Entry *) &(block->entries[i]);
            entry->next = freeM;
            freeM = entry;
        }
    }

    // Fingerprinting to help locate garbage collected blocks in memory
    uint32_t fingerprint1, fingerprint2;

    // List of all blocks - never empty because the fixed block is always at
    // the head
    Block *blocksM;

    // List of all free entries
    Entry *freeM;

    // The initial fixed block, never freed (obviously!)
    Block fixedBlockM;
};


// Global variables - collected in a class so that they will be located on the
// heap (not in bss), which assists in finding them when doing memory
// forensics.   This static data is protected by GCThreading_Lock().
class GCPooled_Allocators
{
public:

    GCPooled_Allocators()
    {
        g_fingerprint1 = 0xdeadaabb;
        g_fingerprint2 = 0xdeadbabd;
        g_fingerprint3 = 0xdeadaabb;
        g_fingerprint4 = 0xdeaddfff;
        memset(g_large_allocs, 0, sizeof(g_large_allocs));
    }

    // Fingerprint, to allow GCPooled_Allocators singleton to be located in a
    // memory dump
    uint32_t g_fingerprint1, g_fingerprint2;

    // These are the pool allocators.  Each uses blocks of around 1 MB in
    // size.  The number of entries per block is the number that will fit in 1
    // MB, assuming that Entries have 8 bytes of overhead above and beyond the
    // allocator size.
    PoolAllocator<8, (((1024 * 1024) - 16) / (8 + 8)),
                  IN_8B_ALLOCATOR_FLAG> g_8b_allocator;
    PoolAllocator<32, (((1024 * 1024) - 16) / (32 + 8)),
                  IN_32B_ALLOCATOR_FLAG> g_32b_allocator;
    PoolAllocator<128, (((1024 * 1024) - 16) / (128 + 8)),
                  IN_128B_ALLOCATOR_FLAG> g_128b_allocator;

    // Fingerprint, to allow large allocs to be located in a memory dump
    uint32_t g_fingerprint3, g_fingerprint4;

    // Large alloc map
    LargeEntry *g_large_allocs[37337];
};
static GCPooled_Allocators allocators;

// These #define statements allow members of allocators to be used by name
// which makes the rest of the code easier to read
#define g_8b_allocator allocators.g_8b_allocator
#define g_32b_allocator allocators.g_32b_allocator
#define g_128b_allocator allocators.g_128b_allocator
#define g_large_allocs allocators.g_large_allocs


static void HandleWeakRefs()
{
#ifdef USE_STD_MAP
    std::map<void *, std::list<GCWeakRef *> >::iterator iter =
        g_weak_ref_map.begin();
    // Must accumulate elements to remove since the iterator cannot be
    // disturbed in pre-C++11 (such as Mac OS X)
    std::list<void *> removeList;
#else
    std::tr1::unordered_map<void *, std::list<GCWeakRef *> >::iterator iter =
        g_weak_ref_map.begin();
#endif

    while (iter != g_weak_ref_map.end()) {
        Entry *entry = ENTRY_FROM_PTR(iter->first);
        if ((FLAGS_OF_ENTRY(entry) & LAST_MARK_MASK) == g_mark) {
            iter++;
            continue;
        }
        // Special handling of weak refs to function objects: if the 'this'
        // pointer is still alive, then mark the function object.
        if (FLAGS_OF_ENTRY(entry) & IS_OBJECT_FLAG) {
            hx::Object *obj = (hx::Object *) iter->first;
            // It's an object, but is it a function object?
            if (obj->__GetType() == vtFunction) {
                hx::Object *thiz = (hx::Object *) obj->__GetHandle();
                // It's a function object, but does it have a "this"?  And
                // if so, is the "this" marked?
                if (thiz && ((FLAGS_OF_ENTRY(ENTRY_FROM_PTR(thiz)) &
                              LAST_MARK_MASK) == g_mark)) {
                    // So it is a weakly ref'd function object with a this
                    // pointer that is live - mark the function object as live
                    // as well
                    hx::MarkAlloc(obj, 0);
                    iter++;
                    continue;
                }
            }
        }
        std::list<GCWeakRef *> &refs = iter->second;
        std::list<GCWeakRef *>::iterator iter2 = refs.begin();
        while (iter2 != refs.end()) {
            (*iter2++)->mRef.mPtr = 0;
        }
#ifdef USE_STD_MAP
        removeList.push_back(iter->first);
        iter++;
#else
        iter = g_weak_ref_map.erase(iter);
#endif
    }

#ifdef USE_STD_MAP
    std::list<void *>::iterator removeIter = removeList.begin();
    while (removeIter != removeList.end()) {
        g_weak_ref_map.erase(*removeIter);
        removeIter++;
    }
#endif
}


static void *SystemMallocLocked(int size)
{
    // Garbage collect if necessary before allocating
    bool collected = GarbageCollectIfNecessaryLocked(size);

    void *ptr = malloc(size);
    
    if (!ptr) {
        if (collected) {
            return 0;
        }
        InternalCollectLocked();
        ptr = malloc(size);
        if (!ptr) {
            return 0;
        }
    }

    return ptr;
}


static void *SystemCallocLocked(int size)
{
    // Garbage collect if necessary before allocating
    bool collected = GarbageCollectIfNecessaryLocked(size);

    void *ptr = calloc(size, 1);
    
    if (!ptr) {
        if (collected) {
            return 0;
        }
        InternalCollectLocked();
        ptr = calloc(size, 1);
        if (!ptr) {
            return 0;
        }
    }

    return ptr;
}


static void *SystemReallocLocked(void *ptr, int size, int old_size)
{
    int size_diff = size - old_size;

    bool collected;
    if (size_diff > 0) {
        collected = GarbageCollectIfNecessaryLocked(size_diff);
    }
    else {
        collected = false;
    }

    void *newptr = realloc(ptr, size);

    if (!newptr) {
        if (collected) {
            return 0;
        }
        InternalCollectLocked();
        newptr = realloc(ptr, size);
        if (!newptr) {
            return 0;
        }
    }

    if (size_diff > 0) {
        memset((void *) (((uintptr_t) newptr) + old_size), 0, size_diff);
    }

    return newptr;
}


static void SystemFreeLocked(void *ptr)
{
    free(ptr);
}


static bool GarbageCollectIfNecessaryLocked(int size)
{
    if (g_gc_disabled || 
        ((g_total_size < g_next_collect_size) &&
         (size < (g_next_collect_size - g_total_size)))) {
        return false;
    }

    InternalCollectLocked();

    return true;
}


static void SetFinalizerLocked(hx::Object *obj, hx::finalizer f,
                               hx::InternalFinalizer *internal_finalizer)
{
    if (f || internal_finalizer) {
        // If there was a pre-existing finalizer, clean it up
        if (g_finalizers.count(obj) > 0) {
            delete g_finalizers[obj].internal_finalizer;
        }
        FinalizerData &fd = g_finalizers[obj];
        fd.f = f;
        fd.internal_finalizer = internal_finalizer;
        FLAGS_OF_ENTRY(ENTRY_FROM_PTR(obj)) |= HAS_FINALIZER_FLAG;
    }
    else {
        if (g_finalizers.count(obj) > 0) {
            delete g_finalizers[obj].internal_finalizer;
            g_finalizers.erase(obj);
        }
        FLAGS_OF_ENTRY(ENTRY_FROM_PTR(obj)) &= ~HAS_FINALIZER_FLAG;
    }
}


static void RunFinalizerLocked(hx::Object *obj)
{
    // Take a copy of finalizer data so that it can be destroyed before
    // actually calling the finalizer in case the finalizer does something
    // crazy like attempt to delete the finalizer itself
    FinalizerData fd = g_finalizers[obj];
    g_finalizers.erase(obj);
    FLAGS_OF_ENTRY(ENTRY_FROM_PTR(obj)) &= ~HAS_FINALIZER_FLAG;

    // Do not hold the global GC lock while calling the finalizer
    GCThreading_Unlock();
    if (fd.internal_finalizer) {
        (fd.internal_finalizer->mFinalizer)(obj);
        delete fd.internal_finalizer;
    }
    else {
        (fd.f)(obj);
    }
    GCThreading_Lock();
}


static inline int MapLargeEntry(LargeEntry *entry)
{
    return (((uintptr_t) entry) % 
            (sizeof(g_large_allocs) / sizeof(g_large_allocs[0])));
}


static inline void AddLargeEntry(LargeEntry *entry)
{
    int idx = MapLargeEntry(entry);

    entry->entry.largeNext = g_large_allocs[idx];

    g_large_allocs[idx] = entry;
}


static inline bool HasLargeEntry(void *ptr)
{
    LargeEntry *entry = LARGE_ENTRY_FROM_PTR(ptr);

    int idx = MapLargeEntry(entry);

    LargeEntry *mapped = g_large_allocs[idx];
    
    while (mapped && (mapped != entry)) {
        mapped = mapped->entry.largeNext;
    }

    return mapped;
}


// next_entry is passed in because entry may have been realloced, and so
// shouldn't be dereferenced to figure out what its next entry was
static inline void RemoveLargeEntry(LargeEntry *entry, LargeEntry *next_entry)
{
    int idx = MapLargeEntry(entry);

    LargeEntry *mapped = g_large_allocs[idx];
    LargeEntry *mapped_prev = 0;

    while (mapped && (mapped != entry)) {
        mapped_prev = mapped;
        mapped = mapped->entry.largeNext;
    }

    if (!mapped) {
        return;
    }

    if (mapped_prev) {
        mapped_prev->entry.largeNext = next_entry;
    }
    else {
        g_large_allocs[idx] = next_entry;
    }
}


static void *LargeAllocLocked(int size, uint32_t is_object_flag)
{
    LargeEntry *entry =
        (LargeEntry *) SystemCallocLocked(sizeof(LargeEntry) + size);

    if (!entry) {
        return 0;
    }

    entry->size = size;
    FLAGS_OF_ENTRY(&(entry->entry)) = g_mark | is_object_flag;

    void *ptr = entry->entry.bytes;

    AddLargeEntry(entry);

    g_total_size += size;

#ifdef TIVO_STB
    check_alloc_threshold();
#endif

    return ptr;
}


static void *LargeReallocLocked(LargeEntry *entry, int size)
{
    uint32_t old_size = entry->size;
    void *old_ptr = entry->entry.bytes;

    // When moving from a large size down into a small, must copy over, and
    // can immediately reclaim the large
    if (size <= 128) {
        void *new_ptr = InternalNewLocked
            (size, FLAGS_OF_ENTRY(&(entry->entry)) & IS_OBJECT_FLAG);
        memcpy(new_ptr, old_ptr, size);
        g_total_size -= old_size;
        RemoveLargeEntry(entry, entry->entry.largeNext);
        SystemFreeLocked(entry);
        return new_ptr;
    }

    LargeEntry *new_entry = (LargeEntry *) SystemReallocLocked
        (entry, sizeof(LargeEntry) + size, sizeof(LargeEntry) + old_size);

    if (!new_entry) {
        return 0;
    }

    if (entry != new_entry) {
        RemoveLargeEntry(entry, new_entry->entry.largeNext);
        AddLargeEntry(new_entry);
    }
    
    void *new_ptr = new_entry->entry.bytes;

    new_entry->size = size;

    g_total_size += (size - old_size);

    return new_ptr;
}


static void MarkPointers(void *start, void *end)
{
    uintptr_t start_ptr = (uintptr_t) start;
    uintptr_t end_ptr = (uintptr_t) end;
    while (start_ptr < end_ptr) {
        void *ptr = * (void **) start_ptr;
        start_ptr += sizeof(uintptr_t);
        // Cannot possibly be a valid pointer if it doesn't have enough room
        // for an entry
        if (((uintptr_t) ptr) <= sizeof(Entry)) {
            continue;
        }
        // See if it was an allocated pointer
        Entry *entry = ENTRY_FROM_PTR(ptr);
        if (g_8b_allocator.Contains(entry) ||
            g_32b_allocator.Contains(entry) ||
            g_128b_allocator.Contains(entry) ||
            HasLargeEntry(ptr)) {
            if (FLAGS_OF_ENTRY(entry) & IS_OBJECT_FLAG) {
                hx::MarkObjectAlloc((hx::Object *) ptr, 0);
            }
            else {
                hx::MarkAlloc(ptr, 0);
            }
        }
    }
}


void hx::MarkObjectArray(hx::Object **inPtr, int inLength, hx::MarkContext *)
{
    for (int i = 0; i < inLength; i++) {
        if (inPtr[i]) {
            MarkObjectAlloc(inPtr[i], 0);
        }
    }
}


void hx::MarkStringArray(String *inPtr, int inLength, hx::MarkContext *__inCtx)
{
    for (int i = 0; i < inLength; i++) {
        const char *str = inPtr[i].__s;
        HX_MARK_STRING(str);
    }
}


hx::Object *__hxcpp_weak_ref_create(Dynamic inObject)
{
    GCWeakRef *ret = new GCWeakRef(inObject);

    FLAGS_OF_ENTRY(ENTRY_FROM_PTR(ret)) |= IS_WEAK_REF_FLAG;
 
    if (inObject.mPtr) {
        GCThreading_Lock();
        // Map the object to the the weak ref, so that when the object is
        // finalized, all of the weak refs to it can be cleared
        g_weak_ref_map[inObject.mPtr].push_back(ret);
        GCThreading_Unlock();
    }

    return ret;
}


hx::Object *__hxcpp_weak_ref_get(Dynamic inRef)
{
    GCThreading_Lock();
    hx::Object *ret = ((GCWeakRef *) (inRef.mPtr))->mRef.mPtr;
    GCThreading_Unlock();
    return ret;
}


// This should really be defined in GC.cpp as it's not specific to this
// implementation.  hxcpp expects this method to allocate memory and to set
// the byte just before the returned pointer to HX_GC_CONST_STRING.
void *hx::InternalCreateConstBuffer(const void *inData, int inSize,
                                    bool inAddStringHash)
{
    bool addHash = inAddStringHash && inData && (inSize > 0);

    // Note that this does not allocate GC memory and thus does not count
    // against the GC memory threshold
    int *result = (int *) malloc
        (inSize + (addHash ? (2 * sizeof(int)) : sizeof(int)));
    if (!result) {
        return 0;
    }

    if (addHash) {
        unsigned int hash = 0;
        for(int i = 0; i < (inSize -1); i++) {
            hash = (hash * 223) + ((unsigned char *) inData)[i];
        }

        *result++ = hash;
        *result++ = HX_GC_CONST_ALLOC_BIT;
    }
    else {
        *result++ = HX_GC_CONST_ALLOC_BIT | HX_GC_NO_STRING_HASH;
    }

    if (inData) {
        memcpy(result, inData, inSize);
    }
   
    return result;
}


#if 0
void hx::GCInit()
{
}
#endif


void hx::SetTopOfStack(int *top, bool)
{
#ifdef TIVO_STB
    process_log_open();
    const char *env = getenv("TIVOCONFIG_GC_RECENT_ALLOCS_THRESHOLD");
    if (env) {
        uint32_t thresh = atoi(env);
        if (thresh > 0) {
            g_recent_allocs_threshold = thresh;
            fprintf(stderr, "*** Haxe GC set recent allocs threshold to %u\n",
                    (unsigned) thresh);
        }
        else {
            fprintf(stderr, "*** Haxe GC failed to set recent allocs threshold "
                    "to %s, falling back to %u\n", env,
                    (unsigned) g_recent_allocs_threshold);
        }
    }
    env = getenv("TIVOCONFIG_GC_RECENT_ALLOCS_STACK_FRAME_COUNT");
    if (env) {
        uint32_t count = atoi(env);
        if (count < 0) {
            count = 0;
        }
        if (count > 24) {
            count = 24;
        }
        g_recent_allocs_frames = count;
        fprintf(stderr, "*** Haxe GC set recent alloc stack frame count "
                "to %u\n", (unsigned) g_recent_allocs_frames);
    }
#endif
    GCThreading_InitializeGCForThisThread(top);
    // Enable the GC now that everything is ready for it (static initializers
    // have finished).
    g_gc_disabled = false;
}


void hx::RegisterWeakHash(HashBase<Dynamic> *inHash)
{
    GCThreading_Lock();
    g_weak_hash_list.push_back(inHash);
    GCThreading_Unlock();
}


bool hx::IsWeakRefValid(hx::Object *inPtr)
{
    Entry *entry = ENTRY_FROM_PTR(inPtr);

    if ((FLAGS_OF_ENTRY(entry) & LAST_MARK_MASK) == g_mark) {
        return true;
    }

    // Not marked, but handle special case of member closure - check if the
    // 'this' pointer is still alive
    if ((FLAGS_OF_ENTRY(entry) & IS_OBJECT_FLAG) &&
        (inPtr->__GetType() == vtFunction)) {
        hx::Object *thiz = (hx::Object *) inPtr->__GetHandle();
        // It's a function object, but does it have a "this"?  And
        // if so, is the "this" marked?
        if (thiz && ((FLAGS_OF_ENTRY(ENTRY_FROM_PTR(thiz)) &
                      LAST_MARK_MASK) == g_mark)) {
            // So it is a weakly ref'd function object with a this pointer
            // that is live - mark it as live as well
            hx::MarkAlloc(inPtr, 0);
            return true;
        }
    }

    return false;
}


void hx::InternalEnableGC(bool inEnable)
{
    GCThreading_EnterMarkSafe();
    g_gc_disabled = !inEnable;
    GCThreading_LeaveMarkSafe();
}


void hx::GCAddRoot(hx::Object **inRoot)
{
    Root *root = (Root *) malloc(sizeof(Root));
    root->pptr = inRoot;
    GCThreading_Lock();
    if (g_roots) {
        root->next = g_roots;
        root->prev = g_roots->prev;
        g_roots->prev->next = root;
        g_roots->prev = root;
    }
    else {
        root->next = root;
        root->prev = root;
        g_roots = root;
    }
    GCThreading_Unlock();
}


void hx::GCRemoveRoot(hx::Object **inRoot)
{
    GCThreading_Lock();
    Root *root = g_roots;
    do {
        if (root->pptr == inRoot) {
            root->prev->next = root->next;
            root->next->prev = root->prev;
            if (root == g_roots) {
                if (root->next == root) {
                    g_roots = 0;
                }
                else {
                    g_roots = root->next;
                }
            }
            free(root);
            break;
        }
        root = root->next;
    } while (root != g_roots);
    GCThreading_Unlock();
}


void hx::GCSetFinalizer(hx::Object *obj, hx::finalizer f)
{
    GCThreading_Lock();
    SetFinalizerLocked(obj, f, 0);
    GCThreading_Unlock();
}


static void *InternalNewLocked(int size, bool isObject)
{
    void *ret;

    uint32_t is_object_flag = isObject ? IS_OBJECT_FLAG : 0;

#define TRY_ALLOC(first, second)                                   \
    {                                                              \
        ret = (first).Allocate(is_object_flag, false);             \
        if (!ret) {                                                \
            ret = (second).Allocate(is_object_flag, false);        \
            if (!ret) {                                            \
                ret = (first).Allocate(is_object_flag, true);      \
            }                                                      \
        }                                                          \
    }

    if (size <= 8) {
        TRY_ALLOC(g_8b_allocator, g_32b_allocator);
    }
    else if (size <= 32) {
        TRY_ALLOC(g_32b_allocator, g_128b_allocator);
    }
    else if (size <= 128) {
        ret = g_128b_allocator.Allocate(is_object_flag, true);
    }
    else {
        ret = LargeAllocLocked(size, is_object_flag);
    }

    return ret;
}


// This is the allocation method that hxcpp uses to allocate objects and other
// garbage collected memory.  Despite being named "InternalNew" it is not
// internal to anything - it is used throughout the hxcpp code base.
void *hx::InternalNew(int size, bool isObject)
{
    GCThreading_CheckForGCMark();
    GCThreading_Lock();
    void *ret = InternalNewLocked(size, isObject);
    GCThreading_Unlock();
#ifdef TIVOCONFIG_GC_COLLECT_STACK_TRACES
    if (ret) {
        save_backtrace(ret);
    }
#endif
    return ret;
}


// This is the allocation method that hxcpp uses to re-allocate garbage
// collected memory.  Despite being named "InternalRealloc" it is not internal
// to anything - it is used throughout the hxcpp code base.
void *hx::InternalRealloc(void *ptr, int size)
{
    GCThreading_CheckForGCMark();
    // If the pointer is null, then this is actually a 'new', and definitely
    // not for an object since objects are never realloc'd
    if (!ptr) {
        return InternalNew(size, false);
    }

    Entry *entry = ENTRY_FROM_PTR(ptr);

    void *ret;

    GCThreading_Lock();

    if (FLAGS_OF_ENTRY(entry) & IN_8B_ALLOCATOR_FLAG) {
        ret = g_8b_allocator.Reallocate(entry, size);
    }
    else if (FLAGS_OF_ENTRY(entry) & IN_32B_ALLOCATOR_FLAG) {
        ret = g_32b_allocator.Reallocate(entry, size);
    }
    else if (FLAGS_OF_ENTRY(entry) & IN_128B_ALLOCATOR_FLAG) {
        ret = g_128b_allocator.Reallocate(entry, size);
    }
    else {
        ret = LargeReallocLocked(LARGE_ENTRY_FROM_PTR(ptr), size);
    }
    
#ifdef TIVOCONFIG_GC_COLLECT_STACK_TRACES
    if (ret != ptr) {
        clear_backtrace(ptr);
        if (ret) {
            save_backtrace(ret);
        }
    }
#endif
 
    GCThreading_Unlock();

    return ret;
}


static void InternalCollectLocked()
{
    // Release lock so that mark can proceed
    GCThreading_Unlock();

    // Stop all other threads so that the mark phase can proceed
    GCThreading_BeginGCMark();

    PLOG(PLOG_TYPE_STX_GC_BEGIN, 0, 0);

#ifdef TIVO_STB
    int initial_total_size = g_total_size;
#endif

    PLOG(PLOG_TYPE_STX_GC_MARK_BEGIN, 0, 0);

    g_mark = (g_mark == MARK_A) ? MARK_B : MARK_A;

    // Mark from stacks and registers
    int count = GCThreading_GetThreadCount();
    GCThreading_ThreadInfo *gci;
    for (int i = 0; i < count; i++) {
        GCThreading_GetThreadInfo(i, gci);
        // Stacks
        MarkPointers(gci->bottom, gci->top);
        // Registers
        char *jmp = (char *) (gci->jmpbuf);
        MarkPointers(jmp, jmp + sizeof(gci->jmpbuf));
    }

    PLOG(PLOG_TYPE_STX_GC_MARK_STACKS_END, 0, 0);

    // Mark class statics
    hx::MarkClassStatics(0);

    PLOG(PLOG_TYPE_STX_GC_MARK_STATICS_END, 0, 0);

    // Mark roots and everything reachable from them
    Root *root = g_roots;
    if (root) {
        do {
            hx::Object **obj = root->pptr;
            if (obj && *obj) {
                MarkObjectAlloc(*obj, 0);
            }
            root = root->next;
        } while (root != g_roots);
    }

    PLOG(PLOG_TYPE_STX_GC_MARK_ROOTS_END, 0, 0);

    // Mark weak hashes
    std::list<hx::HashBase<Dynamic> *>::iterator iter =
        g_weak_hash_list.begin();
    while (iter != g_weak_hash_list.end()) {
        Entry *entry = ENTRY_FROM_PTR(*iter);
        if ((FLAGS_OF_ENTRY(entry) & LAST_MARK_MASK) == g_mark) {
            hx::HashBase<Dynamic> *ref = *iter++;
            ref->updateAfterGc();
        }
        else {
            iter = g_weak_hash_list.erase(iter);
        }
    }

    // Clear weak refs for those weakly referenced pointers that are no longer
    // reachable, at the same time ensuring that function objects that have
    // marked 'this' objects are also marked
    HandleWeakRefs();

    PLOG(PLOG_TYPE_STX_GC_MARK_END, 0, 0);

    // Allow other threads to proceed now that mark is done, but acquire the
    // GC lock first so that no other thread can mess with GC structures until
    // this collect is done
    GCThreading_Lock();
    GCThreading_EndGCMark();

    PLOG(PLOG_TYPE_STX_GC_SWEEP_BEGIN, 0, 0);

    // Reset total reaped, so that the stats may be collected during the
    // sweeps
    g_total_reaped = 0;

    // Sweep the small allocators
    g_8b_allocator.Sweep();
    g_32b_allocator.Sweep();
    g_128b_allocator.Sweep();

    // Sweep the large allocations
    for (int i = 0; i < (sizeof(g_large_allocs) /
                         sizeof(g_large_allocs[0])); i++) {
        LargeEntry *entry = g_large_allocs[i];
        while (entry) {
            LargeEntry *next_entry = entry->entry.largeNext;

            // If the entry is marked properly, then it's still live, so do
            // nothing to it
            if ((FLAGS_OF_ENTRY(&(entry->entry)) & LAST_MARK_MASK) == g_mark) {
                entry = next_entry;
                continue;
            }

            // The entry is now dead, collect it

            if (FLAGS_OF_ENTRY(&(entry->entry)) &
                (HAS_FINALIZER_FLAG | IS_WEAK_REF_FLAG)) {
                if (FLAGS_OF_ENTRY(&(entry->entry)) & HAS_FINALIZER_FLAG) {
                    RunFinalizerLocked((hx::Object *) (entry->entry.bytes));
                }
                // If it's a weak reference object itself, then remove it
                // from the list of weak references
                else {
                    RemoveWeakRef((GCWeakRef *) (entry->entry.bytes));
                }
            }

            // Remove entry from g_large_allocs
            RemoveLargeEntry(entry, next_entry);

            g_total_size -= entry->size;
            g_total_reaped += 1;
#ifdef HXCPP_M64
            uintptr_t ptr = (uintptr_t) (entry->entry.bytes);
            if (gIdMap.count(ptr)) {
                int id = gIdMap[ptr];
                gIdMap.erase(ptr);
                gObjMap.erase(id);
            }
#endif
#ifdef TIVOCONFIG_GC_COLLECT_STACK_TRACES
            clear_backtrace(entry->entry.bytes);
#endif
            SystemFreeLocked((void *) entry);

            entry = next_entry;
        }
    }

    PLOG(PLOG_TYPE_STX_GC_SWEEP_END, 0, 0);

    // Calculate the threshold for invocation of the next GC
    g_next_collect_size = NEXT_COLLECT_SIZE(g_total_size);
    if (g_next_collect_size < g_total_size) {
        // This happens on overflow of g_next_collect_size.  In this case,
        // memory is nearly all used up (2/3 of a 4 GB vitual address space is
        // allocated); now collect more aggressively as memory fills up
        g_next_collect_size = 
            g_total_size + ((0xFFFFFFFF - g_total_size) / 2);
    }

#ifdef TIVO_STB
    PLOG(PLOG_TYPE_STX_GC_END, 0, 0);
    StxGcReaped reaped = { g_total_reaped, initial_total_size - g_total_size };
    PLOG(PLOG_TYPE_STX_GC_REAPED, sizeof(reaped), &reaped);
    static int gc_count;
    gc_count += 1;
    StxPlogGcStatistics stats = { g_total_size, 0, gc_count, 0 };
    PLOG(PLOG_TYPE_STX_GC_STATISTICS, sizeof(stats), &stats);
#endif
}


// It's not clear what the intent of this function is, as it's not used
// anywhere.
void hx::GCChangeManagedMemory(int, const char *)
{
}


// Despite being named 'InternalCollect', this function is directly invoked
// whenever a garbage collection is requested elsewhere in the code base, in
// addition to being invoked whenever an InternalNew or InternalAlloc would
// exceed the current garbage collect threshold
int hx::InternalCollect(bool, bool)
{
    GCThreading_Lock();
    InternalCollectLocked();
    GCThreading_Unlock();
    return g_total_size;
}
        

// Despite being named InternalAllocID, this function is called from other
// parts of the hxcpp code base whenever a unique identifier for a pointer is
// required.
int __hxcpp_obj_id(Dynamic inObj)
{
    uintptr_t ptr = (uintptr_t) inObj->__GetRealObject();
    if (ptr == 0) {
        return 0;
    }

#ifdef HXCPP_M64
    // Cheesiness: on 64-bit systems, just use ids in increasing order.  ids
    // are never reclaimed, so if too many are allocated we wrap and this
    // could lead to serious problems.  But even if allocating a constant 1000
    // ids per second, it would take 49 days to run out.  The best solution to
    // this issue is to make InternalAllocID return a uintptr_t and just use
    // the pointer itself, as is done for 32 bit systems.  But this would
    // require plumbing 64 bit values up through to Haxe ...

    GCThreading_Lock();
#ifdef USE_STD_MAP
    std::map<uintptr_t, int>::iterator i = gObjMap.find(ptr);
#else
    std::tr1::unordered_map<uintptr_t, int>::iterator i = gObjMap.find(ptr);
#endif

    if (i != gObjMap.end()) {
        GCThreading_Unlock();
        return i->second;
    }

    int id = ++gCurrentId;

    gIdMap[ptr] = id;
    gObjMap[id] = ptr;

    GCThreading_Unlock();

    return id;
#else
    return (int) ptr;
#endif
}


hx::Object *__hxcpp_id_obj(int id)
{
#ifdef HXCPP_M64

    GCThreading_Lock();
    
#ifdef USE_STD_MAP
    std::map<int, uintptr_t>::iterator i = gIdMap.find(id);
#else
    std::tr1::unordered_map<int, uintptr_t>::iterator i = gIdMap.find(id);
#endif

    hx::Object *ret = (i == gIdMap.end()) ? 0 : (hx::Object *) (i->second);

    GCThreading_Unlock();

    return ret;
#else
    return (hx::Object *) id;
#endif
}


unsigned int __hxcpp_obj_hash(Dynamic inObj)
{
    if (!inObj.mPtr) {
        return 0;
    }

    uintptr_t ptr = (uintptr_t) inObj->__GetRealObject();

    if (sizeof(ptr) > 4) {
        return (ptr >> 2) ^ (ptr >> 31);
    }
    else {
        return ptr;
    }
}


// This is called whenever a thread will block.  It indicates that the calling
// thread will not be able to participate in multithreaded garbage collection
// until ExitGCFreeZone() is called.
void hx::EnterGCFreeZone()
{
    GCThreading_EnterMarkSafe();
}


// This is called whenever a thread is no longer going to block.  It indicates
// that the calling thread is now be able to participate in multithreaded
// garbage collection again.
void hx::ExitGCFreeZone()
{
    GCThreading_LeaveMarkSafe();
}


void hx::MarkAlloc(void *ptr, hx::MarkContext *)
{
    Entry *entry = ENTRY_FROM_PTR(ptr);

#ifdef DEBUG
    if (!(g_8b_allocator.Contains(entry) ||
          g_32b_allocator.Contains(entry) ||
          g_128b_allocator.Contains(entry) ||
          HasLargeEntry(ptr))) {
        fprintf(stderr, "*** Haxe GC detected bad MarkAlloc for %p\n", ptr);
        pthread_kill(pthread_self(), SIGABRT);
    }
#endif

    FLAGS_OF_ENTRY(entry) =
        ((FLAGS_OF_ENTRY(entry) & ~LAST_MARK_MASK) | g_mark);
}


void hx::MarkObjectAlloc(hx::Object *obj, hx::MarkContext *)
{
    // If the object has been marked and its last mark matches the global
    // mark, then nothing to do
    Entry *entry = ENTRY_FROM_PTR(obj);
    if ((FLAGS_OF_ENTRY(entry) & LAST_MARK_MASK) == g_mark) {
        return;
    }

    // Set the object as being marked and with the current mark
    FLAGS_OF_ENTRY(entry) =
        ((FLAGS_OF_ENTRY(entry) & ~LAST_MARK_MASK) | g_mark);

    // Recursively mark its contained objects
    obj->__Mark(0);
}


hx::InternalFinalizer::InternalFinalizer(hx::Object *obj, finalizer inFinalizer)
    : mObject(obj), mFinalizer(inFinalizer)
{
    GCThreading_Lock();
    SetFinalizerLocked(obj, 0, this);
    GCThreading_Unlock();
}


void hx::InternalFinalizer::Detach()
{
    GCThreading_Lock();
    SetFinalizerLocked(mObject, 0, 0);
    GCThreading_Unlock();
}


// Called when a thread function is entered, which establishes the beginning
// of a thread execution.
void hx::RegisterCurrentThread(void *inTopOfStack)
{
    GCThreading_InitializeGCForThisThread(inTopOfStack);
}


// Called when a thread function is exited, which establishes the end of a
// thread execution.
void hx::UnregisterCurrentThread()
{
    GCThreading_DeinitializeGCForThisThread();
}


// Called when a thread is about to be created; allows initialization of any
// GC state necessary to handle multithreaded support to occur only when
// a program uses multiple threads
void hx::GCPrepareMultiThreaded()
{
    GCThreading_PrepareMultiThreaded();
}


// Used only by the SAFE_POINT macro.  This implementation doesn't care about
// its value.
int hx::gPauseForCollect;


void __hxcpp_enter_gc_free_zone()
{
    GCThreading_EnterMarkSafe();
}


void __hxcpp_exit_gc_free_zone()
{
    GCThreading_LeaveMarkSafe();
}


void __hxcpp_gc_safe_point()
{
    GCThreading_CheckForGCMark();
}


static int GetLargeAllocatedSizeLocked()
{
    int total = 0;

    for (int i = 0; i < (sizeof(g_large_allocs) /
                         sizeof(g_large_allocs[0])); i++) {
        LargeEntry *entry = g_large_allocs[i];
        while (entry) {
            total += entry->size;
            entry = entry->entry.largeNext;
        }
    }

    return total;
}


int __hxcpp_gc_mem_info(int inWhich)
{
    switch (inWhich) {
    case 0:
        //   MEM_INFO_USAGE - estimate of how much is needed by program (at
        //   last collect)
        return (int) g_total_size;
    case 1: {
        //   MEM_INFO_RESERVED - memory allocated for possible use
        int total = 0;
        // Collect this info by iterating over allocations
        GCThreading_Lock();
        total += g_8b_allocator.GetAllocatedSizeLocked();
        total += g_32b_allocator.GetAllocatedSizeLocked();
        total += g_128b_allocator.GetAllocatedSizeLocked();
        total += GetLargeAllocatedSizeLocked();
        // Only want to know the allocation in excess of what was really
        // allocated
        total -= g_total_size;
        GCThreading_Unlock();
        return total;
    }
    case 2:
        //   MEM_INFO_CURRENT - memory in use, includes uncollected garbage.
        //     This will generally saw-tooth between USAGE and RESERVED
        return (int) g_total_size;
    case 3: {
        //   MEM_INFO_LARGE - Size of separate pool used for large allocs.
        //   Included in all the above.
        int total = 0;
        GCThreading_Lock();
        total = GetLargeAllocatedSizeLocked();
        GCThreading_Unlock();
        return total;
    }
    }

    return 0;
}


namespace hx
{

void MarkConservative(int *begin, int *end, hx::MarkContext *)
{
    MarkPointers(begin, end);
}


void *Object::operator new(size_t inSize, bool inContainer, const char *)
{
    return InternalNew(inSize, inContainer);
}

}


// ----------------------------------------------------------------------------
// NO-OP methods that don't do anything in this implementation but are defined
// in GC.h and are left basically unimplemented here:
// ----------------------------------------------------------------------------

// Used only by haxe cpp.vm.Gc class, not implemented
int __hxcpp_gc_trace(hx::Class, bool)
{
    return 0;
}


// Not called by any code anywhere
int __hxcpp_gc_used_bytes()
{
    return g_total_size;
}


// Used only by haxe cpp.vm.Gc class, not implemented
void __hxcpp_gc_do_not_kill(Dynamic)
{
}


// Used only by haxe cpp.vm.Gc class, not implemented
hx::Object *__hxcpp_get_next_zombie()
{
    return 0;
}


#ifdef HXCPP_DEBUG
// Used just by some debugging fuctions to do who knows what, doesn't seem
// particularly important
void hx::MarkSetMember(const char *, hx::MarkContext *)
{
}


// Used just by some debugging fuctions to do who knows what, doesn't seem
// particularly important
void hx::MarkPushClass(const char *, hx::MarkContext *)
{
}


// Used just by some debugging fuctions to do who knows what, doesn't seem
// particularly important
void hx::MarkPopClass(hx::MarkContext *)
{
}
#endif


// Not called by any code anywhere
void hx::PauseForCollect()
{
}
