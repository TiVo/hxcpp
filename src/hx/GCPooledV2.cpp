/** **************************************************************************
 * GCPooledV2.cpp
 *
 * This is a re-implementation of the hxcpp garbage collector, whose
 * "interface" is partially defined (and partially over-defined with cruft!)
 * in GC.h.
 *
 * This implementation features:
 * - Per-thread small object pooled allocators that are very fast and that are
 *   tuned to common hxcpp object sizes (being per-thread means that
 *   allocations can occur with no locking, even in multithreaded programs)
 * - Per-thread large object allocator directly from malloc (being per-thread
 *   means that large allocations can occur with only malloc locking, even in
 *   multithreaded programs)
 *
 * TODO:
 *
 * - Improve performance of weak hash/weak map by not using C++ STL
 *
 ************************************************************************** **/

// The following compiler values may be set in order to tune the functionality
// of the garbage collector for a particular application (all may be set via
// haxe compiler options ("-D xxx=yyy") or via lime project settings):
//
// HXCPP_SINGLE_THREADED_APP -- define this if the application is known to be
//     single threaded.  This will make the GC very, very slightly more
//     efficient as it will not have to do locking.  If an app tries to create
//     another haxe thread when it has been compiled with
//     HXCPP_SINGLE_THREADED_APP, it will print an error message and exit.
//     The default is that this is not defined.
//
// TIVOCONFIG_GC_BLOCK_SIZE -- this is the size of a "block" used by the pool
//     allocators.  Allocations for small sizes come out of pools, and pools
//     are allocated in chunks of HXCPP_GC_BLOCK_SIZE at a time.  The default
//     is 1 MiB.
//
// TIVOCONFIG_GC_ALLOC_0_SIZE -- size of allocs covered by the first pool.
//     Defaults to 2 * sizeof(uintptr_t).
//
// TIVOCONFIG_GC_ALLOC_1_SIZE -- size of allocs covered by the second
//     pool.  Must be greater than or equal to HXCPP_GC_ALLOC_0_SIZE.
//     Defaults to 12 bytes (unless TIVOCONFIG_GC_ALLOC_0_SIZE is 12 bytes or
//     larger, in which case this pool is not used).
//
// TIVOCONFIG_GC_ALLOC_2_SIZE -- size of allocs covered by the third
//     pool.  Must be greater than or equal to HXCPP_GC_ALLOC_1_SIZE.
//     Defaults to 20 bytes.
//
// TIVOCONFIG_GC_ALLOC_3_SIZE -- size of allocs covered by the fourth
//     pool.  Must be greater than or equal to HXCPP_GC_ALLOC_2_SIZE.
//     Defaults to 64 bytes.
//
// TIVOCONFIG_GC_MIN_COLLECT_SIZE -- number of bytes that can be allocated by
//     a single thread before a system-wide GC is performed.  Note that
//     the larger this is made, the less frequent garbage collects will be,
//     but the longer they will take.  Usually more frequent, shorter duration
//     GCs are preferable for an interactive application.  Defaults to 10 MiB.
//
// TIVOCONFIG_GC_RECENT_ALLOCS_THRESHOLD -- define as a number > 0 to enable
//     the reporting of each time the number of allocations hits a threshold.
//     The number can be overridden by setting the environment variable
//     "TIVOCONFIG_GC_RECENT_ALLOCS_THRESHOLD".  Also, the environment
//     variable "TIVOCONFIG_GC_RECENT_ALLOCS_STACK_FRAME_COUNT" may be set to
//     a number of stack frames to report whenever the alloc threshold is hit
//     (this "samples" allocations and can allow frequently allocating stack
//     traces to be identified).
//
// TIVOCONFIG_GC_SIZE_STATISTICS_THRESHOLD -- define as a number > 0 to
//     enable the reporting of the sizes of allocations after this many
//     allocations have been performed; the number can be overridden by
//     setting the environment variable
//     "TIVOCONFIG_GC_SIZE_STATISTICS_THRESHOLD".
//
// TIVOCONFIG_GC_COLLECT_STACK_TRACES -- define this to enable the collection
//     of stack traces for every allocation.  This slows down the app
//     immensely but allows memory dumps to be examined to extract the stack
//     trace associated with every live allocation, which can be useful in
//     object leak debugging.


#include <algorithm>
#ifndef ANDROID
#include <execinfo.h>
#endif
#include <list>
#include <map>
#ifndef HXCPP_SINGLE_THREADED_APP
#include <pthread.h>
#endif
#include <setjmp.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#if (defined HX_MACOS || defined APPLETV || defined IPHONE)
#include <unistd.h>
#include <vector>

#include "hxcpp.h"
#include "hx/GC.h"
#include "hx/Unordered.h"
#include "Hash.h"

#ifdef TIVO_STB
#include <stx/StxProcessLog.h>
#endif


/**** Forward type declarations ******************************************** */
class GCSizeLogger;
class GCWeakRef;
class ThreadGlobalData;


/**** Static function signature forward declarations *********************** */

// Helper function to run a finalizer for a given object.  It is assumed that
// there is a finalizer for the pointer.
static void RunFinalizer(hx::Object *obj);

// Remove a weak reference that is no longer referenced.
static void RemoveWeakRef(GCWeakRef *wr);

// Helper function to allocate memory, without doing a possible collect
// afterwards
static inline void *InternalNewNoCollect(ThreadGlobalData *gd,
                                         int inSize, bool inIsObject);

#ifdef TIVOCONFIG_GC_RECENT_ALLOCS_THRESHOLD
// Used to emit logging when the number of allocations has exceeded a certain
// threshold, which can help in diagnosing how much excessive churn the
// application is engaging in
static void check_recent_allocs_threshold();
#endif

#ifdef TIVOCONFIG_GC_SIZE_STATISTICS_THRESHOLD
// Used to emit logging when the number of allocations has exceeded a certain
// threshold, which can help in diagnosing how much excessive churn the
// application is engaging in
static void check_size_statistics_threshold();
#endif

#ifdef TIVOCONFIG_GC_COLLECT_STACK_TRACES
// Clears stack trace saved for the given allocated pointer.
static void clear_stacktrace(void *ptr);
#endif


/**** Macro definitions **************************************************** */

// Configurable tunable values
#ifndef TIVOCONFIG_GC_BLOCK_SIZE
#define TIVOCONFIG_GC_BLOCK_SIZE (1024 * 1024)
#endif

#ifndef TIVOCONFIG_GC_ALLOC_0_SIZE
#ifdef HXCPP_M64
#define TIVOCONFIG_GC_ALLOC_0_SIZE 16
#else
#define TIVOCONFIG_GC_ALLOC_0_SIZE 8
#endif
#endif

#ifndef TIVOCONFIG_GC_ALLOC_1_SIZE
#if (TIVOCONFIG_GC_ALLOC_0_SIZE < 12)
#define TIVOCONFIG_GC_ALLOC_1_SIZE 12
#else
#define TIVOCONFIG_GC_ALLOC_1_SIZE TIVOCONFIG_GC_ALLOC_0_SIZE 
#endif
#endif

#ifndef TIVOCONFIG_GC_ALLOC_2_SIZE
#if (TIVOCONFIG_GC_ALLOC_1_SIZE < 20)
#define TIVOCONFIG_GC_ALLOC_2_SIZE 20
#else
#define TIVOCONFIG_GC_ALLOC_2_SIZE TIVOCONFIG_GC_ALLOC_1_SIZE 
#endif
#endif

#ifndef TIVOCONFIG_GC_ALLOC_3_SIZE
#if (TIVOCONFIG_GC_ALLOC_2_SIZE < 64)
#define TIVOCONFIG_GC_ALLOC_3_SIZE 64
#else
#define TIVOCONFIG_GC_ALLOC_3_SIZE TIVOCONFIG_GC_ALLOC_2_SIZE 
#endif
#endif

#ifndef TIVOCONFIG_GC_MIN_COLLECT_SIZE
#define TIVOCONFIG_GC_MIN_COLLECT_SIZE (10 * 1024 * 1024)
#endif


// Sanity checks
#if (TIVOCONFIG_GC_ALLOC_0_SIZE > TIVOCONFIG_GC_ALLOC_1_SIZE)
#error "TIVOCONFIG_GC_ALLOC_0_SIZE > TIVOCONFIG_GC_ALLOC_1_SIZE"
#endif

#if (TIVOCONFIG_GC_ALLOC_1_SIZE > TIVOCONFIG_GC_ALLOC_2_SIZE)
#error "TIVOCONFIG_GC_ALLOC_1_SIZE > TIVOCONFIG_GC_ALLOC_2_SIZE"
#endif

#if (TIVOCONFIG_GC_ALLOC_2_SIZE > TIVOCONFIG_GC_ALLOC_3_SIZE)
#error "TIVOCONFIG_GC_ALLOC_2_SIZE > TIVOCONFIG_GC_ALLOC_3_SIZE"
#endif


// Whether or not to use allocators
#if (TIVOCONFIG_GC_ALLOC_0_SIZE != 0)
#define USE_ALLOCATOR_0 1
#else
#define USE_ALLOCATOR_0 0
#endif

#if (TIVOCONFIG_GC_ALLOC_1_SIZE != TIVOCONFIG_GC_ALLOC_0_SIZE)
#define USE_ALLOCATOR_1 1
#else
#define USE_ALLOCATOR_1 0
#endif

#if (TIVOCONFIG_GC_ALLOC_2_SIZE != TIVOCONFIG_GC_ALLOC_1_SIZE)
#define USE_ALLOCATOR_2 1
#else
#define USE_ALLOCATOR_2 0
#endif

#if (TIVOCONFIG_GC_ALLOC_3_SIZE != TIVOCONFIG_GC_ALLOC_2_SIZE)
#define USE_ALLOCATOR_3 1
#else
#define USE_ALLOCATOR_3 0
#endif


#ifdef ANDROID
#include <android/log.h>
#define GCLOG(...) __android_log_print(ANDROID_LOG_ERROR, "GC", __VA_ARGS__)
#define stacktrace(x, y)
#else
#define GCLOG(...) fprintf(stderr, __VA_ARGS__)
#endif


// Enable process logging only on STB targets
#ifdef TIVO_STB
#define PLOG(type, data_len, data) process_log(type, data_len, data)
#else
#define PLOG(type, data_len, data)
#endif

// This computes the next threshold at which garbage collection will be done,
// given a current number of allocated bytes.
#define NEXT_COLLECT_THRESHOLD(x) ((x) + TIVOCONFIG_GC_MIN_COLLECT_SIZE)

// Flags for use in Entry flags
#define MARK_A_FLAG               (1 << 29)
#define MARK_B_FLAG               (1 << 28)
#define IS_OBJECT_FLAG            (1 << 27)
#define IS_WEAK_REF_FLAG          (1 << 26)
#define HAS_FINALIZER_FLAG        (1 << 25)
#if USE_ALLOCATOR_0
#define IN_ALLOCATOR_0_FLAG       (1 << 24)
#endif
#if USE_ALLOCATOR_1
#define IN_ALLOCATOR_1_FLAG       (1 << 23)
#endif
#if USE_ALLOCATOR_2
#define IN_ALLOCATOR_2_FLAG       (1 << 22)
#endif
#if USE_ALLOCATOR_3
#define IN_ALLOCATOR_3_FLAG       (1 << 21)
#endif

// Mask used to mask out the mask identifier from the flags
#define LAST_MARK_MASK            (MARK_A_FLAG | MARK_B_FLAG)

// Recover an Entry from a pointer
#define ENTRY_FROM_PTR(ptr)                                                  \
    ((Entry *) (((uintptr_t) (ptr)) - offsetof(Entry, bytes)))

// Recover a LargeEntry from a pointer
#define LARGE_ENTRY_FROM_PTR(ptr)                                            \
    ((LargeEntry *) (((uintptr_t) (ptr)) - (offsetof(LargeEntry, entry) +    \
                                            offsetof(Entry, bytes))))

// Get a reference to the flags of an entry
#define FLAGS_OF_ENTRY(entry) (((uint32_t *) ((entry)->bytes))[-1])

// Declares a pool allocator for a particular size allocation
#define PoolAllocatorType(size, flag)                                        \
    PoolAllocator<size,                                                      \
                  ((TIVOCONFIG_GC_BLOCK_SIZE - (2 * sizeof(void *))) /       \
                   (size + sizeof(Entry))),                                  \
                  flag>


/**** Static variables needed by type definitions ************************** */

// This is the Object to id mapping; this is unfortunate stuff but the GC API
// requires it.  Everything in here is protected by g_lock.
#ifdef HXCPP_M64
static hx::UnorderedMap<uintptr_t, int> g_obj_map;
static hx::UnorderedMap<int, uintptr_t> g_id_map;
static int g_current_id;
#endif

// The most recent mark value, used to mark objects during the mark phase;
// toggles between MARK_A_FLAG and MARK_B_FLAG, to match the last mark bits of
// an Entry's flag_and_offset value.  Read concurrently by many threads
// outside of a collect, written only by the collecting thread during a
// collect.
static uint32_t g_mark = MARK_A_FLAG;

// Total number of bytes allocated after the last GC.  Not necessarily the
// same as the current total number of bytes allocated, which is much more
// expensive to determine
static uint32_t g_total_after_last_collect;

// This is the total number of objects "reaped" during a GC, to use for
// plogging purposes.  Not referenced outside of a collect.
static unsigned int g_total_reaped;


/**** Type definitions ***************************************************** */

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


// Type of a finalizer function passed in from the haxe side
typedef void (*HaxeFinalizer)(Dynamic);
    

// Type of finalizer (since hxcpp has several ways to register finalizers,
// this is used to distinguish between those different ways)
typedef enum FinalizerType
{
    FinalizerTypeFinalizer,
    FinalizerTypeInternalFinalizer,
    FinalizerTypeHaxeFinalizer
} FinalizerType;


// Data associated with a finalizer to call
typedef struct FinalizerData
{
    FinalizerType type;
    union {
        hx::finalizer f;
        hx::InternalFinalizer *internal_finalizer;
        HaxeFinalizer haxe_finalizer;
    };
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


// This keeps track of a GC "root", which is a primary object that is at the
// top of the GC reference tree.
typedef struct Root
{
    struct Root *next;
    hx::Object **pptr;
} Root;


// Log the pool sizes, for diagnostic purposes
class GCSizeLogger
{
public:

    GCSizeLogger()
    {
#if USE_ALLOCATOR_0
        GCLOG("TiVo GC using pool size"
#if (USE_ALLOCATOR_1 || USE_ALLOCATOR_2 || USE_ALLOCATOR_3)
                "s "
#else
                " "
#endif
                );
        GCLOG("%d", TIVOCONFIG_GC_ALLOC_0_SIZE);
#if USE_ALLOCATOR_1
        GCLOG(", %d", TIVOCONFIG_GC_ALLOC_1_SIZE);
#endif
#if USE_ALLOCATOR_2
        GCLOG(", %d", TIVOCONFIG_GC_ALLOC_2_SIZE);
#endif
#if USE_ALLOCATOR_3
        GCLOG(", %d", TIVOCONFIG_GC_ALLOC_3_SIZE);
#endif
#else
        GCLOG("TiVo GC not using pools");
#endif
        GCLOG("\n");
    }
};


#ifdef TIVOCONFIG_GC_COLLECT_STACK_TRACES
/**
 * Saves stacktraces associated with a given pointer, in a form amenable to
 * extracting from a memory dump.
 **/
typedef struct StackTrace
{
    struct StackTrace *next;
    void *ptr;
    void *frames[33];
} StackTrace;
#endif


// Allocator where allocations from calloc() are done block-at-a-time,
// and objects can be quickly added to and removed from the block.
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

    PoolAllocator(ThreadGlobalData *gd)
    {
        // Assumes embedded in ThreadGlobalData, which is allocated via
        // calloc() and thus all members of this object are zeroed before
        // construction
        mGd = gd;
        entriesPerBlockM = entriesPerBlockC;
    }

    // Returns the number of bytes allocated by this allocator (not all bytes
    // may be in use, some may be in unused blocks)
    uint32_t GetTotalBytes() const
    {
        uint32_t total = 0;
        Block *block = blocksM;
        do {
            total += sizeof(*block);
        } while (block != blocksM);
        return total + sizeof(*this);
    }
    
    inline uint32_t GetAllocatedBytes() const
    {
        return mAllocatedBytes;
    }

    void *Allocate(bool is_object)
    {
        // If no free entries ...
        if (!freeM) {
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
        FLAGS_OF_ENTRY(entry) = (g_mark | allocatorFlagC |
                                 (is_object ? IS_OBJECT_FLAG : 0));

        // Remove the entry from the free list
        freeM = entry->next;

        // Now entrySizeC more bytes are in use in the thread owning this
        // allocator
        mAllocatedBytes += entrySizeC;

#ifdef TIVOCONFIG_GC_RECENT_ALLOCS_THRESHOLD
        check_recent_allocs_threshold();
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
        void *newptr = InternalNewNoCollect
            (mGd, size, FLAGS_OF_ENTRY(entry) & IS_OBJECT_FLAG);
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
        
        // The new size was already added in InternalNewNoCollect().  Reduce
        // by the size that is no longer being used.
        mAllocatedBytes -= entrySizeC;

        // The old entry is no longer used, it must not have had its
        // HAS_FINALIZER_FLAG set since objects are never re-allocated, so
        // there is no worry about the old memory having a finalizer called
        // incorrectly.
        return newptr;
    }

    // Return true if this Entry is in one of this allocator's blocks
    bool Contains(Entry *entry)
    {
        // If this allocator never allocated anything, then of course nothing
        // is contained
        if (!blocksM) {
            return false;
        }
        
        uintptr_t e = (uintptr_t) entry;

        // Look in each block to see if it could be in there
        Block *block = blocksM;
        do {
            // First check to see if the entry is even within the range of
            // entries of this block, skip if not
            uintptr_t first = (uintptr_t) block->entries;
            uintptr_t last = (uintptr_t) &(block->entries[entriesPerBlockC]);
            if ((e >= first) && (e < last)) {
                // It points within the block, now return whether or not it is
                // a valid entry - i.e. if it's aligned properly to be an
                // Entry.
                return ((((uintptr_t) (e - first)) % sizeof(SizedEntry)) == 0);
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
        // If this allocator never allocated anything, then there is nothing
        // to sweep.
        if (!blocksM) {
            return;
        }

        Block *block = blocksM;

        while (true) {
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
                        RunFinalizer((hx::Object *) (entry->bytes));
                    }
                    // If it's a weak reference object itself, then remove it
                    // from the list of weak references
                    else {
                        RemoveWeakRef((GCWeakRef *) (entry->bytes));
                    }
                }
#ifdef HXCPP_M64
                uintptr_t ptr = (uintptr_t) (entry->bytes);
                if (g_id_map.count(ptr)) {
                    int id = g_id_map[ptr];
                    g_id_map.erase(ptr);
                    g_obj_map.erase(id);
                }
#endif
                mAllocatedBytes -= entrySizeC;
                g_total_reaped += 1;
#ifdef TIVOCONFIG_GC_COLLECT_STACK_TRACES
                clear_stacktrace(entry->bytes);
#endif
                // Clear out its bytes, so that it is ready for re-use
                memset(entry, 0, sizeof(SizedEntry));
                // And put it on the free list
                entry->next = freeM;
                freeM = entry;
            }

            // Stop if the next block that would be done was already done
            Block *next = block->next;
            
            bool stop = (next == blocksM);

            // If all of the entries are free, and there is more than one
            // block, then free the block - first removing its entries from
            // the free list
            if ((free_count == entriesPerBlockC) && (block->next != block)) {
                this->RemoveBlock(block);
            }

            if (stop) {
                break;
            }

            block = next;
        }
    }

    void MergeInto
        (PoolAllocator<entrySizeC, entriesPerBlockC, allocatorFlagC> &other)
    {
        Entry **replace = &freeM;
        while (*replace) {
            replace = &((*replace)->next);
        }
        *replace = other.freeM;

        blocksM->prev->next = other.blocksM;
        other.blocksM->prev->next = blocksM;
        Block *tmp = blocksM->prev;
        blocksM->prev = other.blocksM->prev;
        other.blocksM->prev = tmp;
    }

private:
    
    typedef struct SizedEntry
    {
        Entry entry;
        char sizing[entrySizeC];
    } SizedEntry;

    typedef struct Block
    {
        Block *prev, *next;
        SizedEntry entries[entriesPerBlockC];
    } Block;

    void AddNewBlock()
    {
        Block *block = (Block *) calloc(sizeof(Block), 1);
        if (!block) {
            // Couldn't add a new block, must be out of memory
            return;
        }

        if (blocksM) {
            block->next = blocksM;
            block->prev = blocksM->prev;
            blocksM->prev->next = block;
            blocksM->prev = block;
        }
        else {
            block->next = block;
            block->prev = block;
            blocksM = block;
        }

        this->AddEntriesOfBlock(block);
    }

    void RemoveBlock(Block *block)
    {
        // A block always has every entry on the free entries list when it is
        // removed.
        Entry *entry = freeM, **replace = &freeM;

        Entry *first_entry = (Entry *) block->entries;
        Entry *last_entry = (Entry *) &(block->entries[entriesPerBlockC - 1]);
        while (entry) {
            if ((entry >= first_entry) && (entry <= last_entry)) {
                // It's in the block, so remove it from the free list
                entry = entry->next;
                *replace = entry;
            }
            else {
                replace = &(entry->next);
                entry = entry->next;
            }
        }
        
        if (block == blocksM) {
            blocksM = block->next;
        }
        
        block->next->prev = block->prev;
        block->prev->next = block->next;
        
        free(block);
    }

    void AddEntriesOfBlock(Block *block)
    {
        for (int i = entriesPerBlockC - 2; i >= 0; i--) {
            block->entries[i].entry.next = &(block->entries[i + 1].entry);
        }
        block->entries[entriesPerBlockC - 1].entry.next = freeM;
        freeM = &(block->entries[0].entry);
    }

    uint32_t mAllocatedBytes;

    // ThreadGlobalData of the thread owning this allocator
    ThreadGlobalData *mGd;

    // List of all blocks
    Block *blocksM;

    // List of all free entries
    Entry *freeM;

    // Number of entries in a block (used only by the extractallocs program
    // when examining memory dumps)
    uint32_t entriesPerBlockM;
};


class LargeAllocator
{
public:

    LargeAllocator(ThreadGlobalData *gd)
    {
        // Assumes embedded in ThreadGlobalData, which is allocated via
        // calloc() and thus all members of this object are zeroed before
        // construction
        mGd = gd;
    }

    inline uint32_t GetAllocatedBytes() const
    {
        return mAllocatedBytes;
    }
    
    void *Allocate(int size, bool is_object)
    {
        LargeEntry *entry = (LargeEntry *) calloc(sizeof(LargeEntry) + size, 1);
        
        if (!entry) {
            return 0;
        }

        entry->size = size;
        
        FLAGS_OF_ENTRY(&(entry->entry)) =
            g_mark | (is_object ? IS_OBJECT_FLAG : 0);

        AddLargeEntry(entry);

        mAllocatedBytes += size;

#ifdef TIVOCONFIG_GC_RECENT_ALLOCS_THRESHOLD
        check_recent_allocs_threshold();
#endif
    
        return entry->entry.bytes;
    }

    void *Reallocate(void *old_ptr, int size)
    {
        LargeEntry *entry = LARGE_ENTRY_FROM_PTR(old_ptr);

        uint32_t old_size = entry->size;

        LargeEntry *entry_next = entry->entry.largeNext;
        
        // When moving from a large size down into a small, must copy over,
        // and can immediately reclaim the large
        if (size <= TIVOCONFIG_GC_ALLOC_3_SIZE) {
            void *new_ptr = InternalNewNoCollect
                (mGd, size, FLAGS_OF_ENTRY(&(entry->entry)) & IS_OBJECT_FLAG);
            memcpy(new_ptr, old_ptr, size);
            mAllocatedBytes -= old_size;
            RemoveLargeEntry(entry, entry_next);
            free(entry);
            return new_ptr;
        }

        LargeEntry *new_entry = (LargeEntry *) realloc
            (entry, sizeof(LargeEntry) + size);
        
        if (!new_entry) {
            return 0;
        }

        int size_diff = size - old_size;
        if (size_diff > 0) {
            memset(&(new_entry->entry.bytes[old_size]), 0, size_diff);
        }
        
        if (entry != new_entry) {
            RemoveLargeEntry(entry, entry_next);
            AddLargeEntry(new_entry);
        }
        
        new_entry->size = size;

        mAllocatedBytes += size_diff;

        return new_entry->entry.bytes;
    }

    bool Contains(void *ptr)
    {
        LargeEntry *entry = LARGE_ENTRY_FROM_PTR(ptr);
        
        LargeEntry *mapped = mLargeAllocs[MapLargeEntry(entry)];
    
        while (mapped && (mapped != entry)) {
            mapped = mapped->entry.largeNext;
        }

        return mapped;
    }

    void Sweep()
    {
        // Sweep the large allocations
        for (int i = 0; i < (sizeof(mLargeAllocs) /
                             sizeof(mLargeAllocs[0])); i++) {
            LargeEntry **replace = &(mLargeAllocs[i]);
            LargeEntry *entry = *replace;
            while (entry) {
                // If the entry is marked properly, then it's still live, so
                // do nothing to it
                if ((FLAGS_OF_ENTRY(&(entry->entry)) &
                     LAST_MARK_MASK) == g_mark) {
                    replace = &(entry->entry.largeNext);
                    entry = *replace;
                    continue;
                }
                
                // The entry is now dead, collect it
                if (FLAGS_OF_ENTRY(&(entry->entry)) &
                    (HAS_FINALIZER_FLAG | IS_WEAK_REF_FLAG)) {
                    if (FLAGS_OF_ENTRY(&(entry->entry)) & HAS_FINALIZER_FLAG) {
                        RunFinalizer((hx::Object *) (entry->entry.bytes));
                    }
                    // If it's a weak reference object itself, then remove it
                    // from the list of weak references
                    else {
                        RemoveWeakRef((GCWeakRef *) (entry->entry.bytes));
                    }
                }

                mAllocatedBytes -= entry->size;
                
#ifdef HXCPP_M64
                uintptr_t ptr = (uintptr_t) (entry->entry.bytes);
                if (g_id_map.count(ptr)) {
                    int id = g_id_map[ptr];
                    g_id_map.erase(ptr);
                    g_obj_map.erase(id);
                }
#endif

                LargeEntry *to_free = entry;
                
#ifdef TIVOCONFIG_GC_COLLECT_STACK_TRACES
                clear_stacktrace(to_free->entry.bytes);
#endif
                // Remove entry from mLargeAllocs
                *replace = entry->entry.largeNext;
                entry = *replace;

                // Free it
                free(to_free);
                
                g_total_reaped += 1;

            }
        }
    }

    void MergeInto(LargeAllocator &other)
    {
        for (int i = 0; i < (sizeof(mLargeAllocs) /
                             sizeof(mLargeAllocs[0])); i++) {
            LargeEntry **this_one = &(mLargeAllocs[i]);
            LargeEntry *other_one = other.mLargeAllocs[i];
            if (*this_one) {
                if (other_one) {
                    while (*this_one) {
                        this_one = &((*this_one)->entry.largeNext);
                    }
                    *this_one = other_one;
                }
            }
            else {
                *this_one = other_one;
            }
        }
    }

private:

    inline int MapLargeEntry(LargeEntry *entry)
    {
#ifdef HXCPP_M64
        return ((((uintptr_t) entry) >> 3) %
                (sizeof(mLargeAllocs) / sizeof(mLargeAllocs[0])));
#else
        return ((((uintptr_t) entry) >> 2) %
                (sizeof(mLargeAllocs) / sizeof(mLargeAllocs[0])));
#endif
    }
    
    inline void AddLargeEntry(LargeEntry *entry)
    {
        int idx = MapLargeEntry(entry);

        entry->entry.largeNext = mLargeAllocs[idx];

        mLargeAllocs[idx] = entry;
    }
    
    inline bool HasLargeEntry(void *ptr)
    {
        LargeEntry *entry = LARGE_ENTRY_FROM_PTR(ptr);
        
        int idx = MapLargeEntry(entry);
        
        LargeEntry *mapped = mLargeAllocs[idx];
        
        while (mapped && (mapped != entry)) {
            mapped = mapped->entry.largeNext;
        }

        return mapped;
    }

    // next_entry is passed in because entry may have been realloced, and so
    // shouldn't be dereferenced to figure out what its next entry was
    inline void RemoveLargeEntry(LargeEntry *entry, LargeEntry *next_entry)
    {
        LargeEntry **replace = &(mLargeAllocs[MapLargeEntry(entry)]);

        while (*replace != entry) {
            replace = &((*replace)->entry.largeNext);
        }
        
        *replace = next_entry;
    }

    uint32_t mAllocatedBytes;

    ThreadGlobalData *mGd;
    
    LargeEntry *mLargeAllocs[37337];
};


/**
 * Every thread gets its own ThreadGlobalData structure which is used to
 * store stuff that is specific to the thread
 **/
class ThreadGlobalData
{
public:

    // New uses calloc, because the thing should be initialized to 0
    void *operator new(std::size_t size)
    {
        return calloc(size, 1);
    }

    // New used calloc, so delete uses free
    void operator delete(void *ptr)
    {
        free(ptr);
    }

    ThreadGlobalData()
        :
#if USE_ALLOCATOR_0
          allocator_0(this),
#endif
#if USE_ALLOCATOR_1
          allocator_1(this),
#endif
#if USE_ALLOCATOR_2
          allocator_2(this),
#endif
#if USE_ALLOCATOR_3
          allocator_3(this),
#endif
          allocator_large(this),
          next_collect_threshold(TIVOCONFIG_GC_MIN_COLLECT_SIZE)
    {
        // Since calloc is used to allocate the memory for this, everything
        // starts out zero, so any value not set into the beacon array
        // is zero.  The goal here is to store pointers to the various
        // allocators in the beacon array so that the extractallocs program
        // knows where to find them (and what size allocation they handle).
        // The 'large allocator' uses 0 as its alloc size here to indicate
        // that it's the large allocator.
        mBeacon[1] = (void *) &allocator_large;
#if USE_ALLOCATOR_0
        mBeacon[2] = (void *) TIVOCONFIG_GC_ALLOC_0_SIZE;
        mBeacon[3] = (void *) &allocator_0;
#endif
#if USE_ALLOCATOR_1
        mBeacon[4] = (void *) TIVOCONFIG_GC_ALLOC_1_SIZE;
        mBeacon[5] = (void *) &allocator_1;
#endif
#if USE_ALLOCATOR_2
        mBeacon[6] = (void *) TIVOCONFIG_GC_ALLOC_2_SIZE;
        mBeacon[7] = (void *) &allocator_2;
#endif
#if USE_ALLOCATOR_3
        mBeacon[8] = (void *) TIVOCONFIG_GC_ALLOC_3_SIZE;
        mBeacon[9] = (void *) &allocator_3;
#endif
    }

    inline uint32_t GetAllocatedBytes()
    {
        uint32_t total;
#if USE_ALLOCATOR_0
        total = allocator_0.GetAllocatedBytes();
#else
        total = 0;
#endif
#if USE_ALLOCATOR_1
        total += allocator_1.GetAllocatedBytes();
#endif
#if USE_ALLOCATOR_2
        total += allocator_2.GetAllocatedBytes();
#endif
#if USE_ALLOCATOR_3
        total += allocator_3.GetAllocatedBytes();
#endif
        total += allocator_large.GetAllocatedBytes();
        return total;
    }

    // Beacon to assist memory dump reader -- it uses the known location of
    // this array relative to the beginning of this object, to find the
    // pointers to the various allocators.  This is an interleaved pattern:
    // size-allocator-size-allocator ... large allocator size is 0.
    // Terminated by two zero values.
    void *mBeacon[12];
    
    ThreadGlobalData *next;
    
    // These hold the Block that can be used to acquire an allocation of up to
    // the given size
    #if USE_ALLOCATOR_0
    PoolAllocatorType(TIVOCONFIG_GC_ALLOC_0_SIZE,
                      IN_ALLOCATOR_0_FLAG) allocator_0;
    #endif
    #if USE_ALLOCATOR_1
    PoolAllocatorType(TIVOCONFIG_GC_ALLOC_1_SIZE,
                      IN_ALLOCATOR_1_FLAG) allocator_1;
    #endif
    #if USE_ALLOCATOR_2
    PoolAllocatorType(TIVOCONFIG_GC_ALLOC_2_SIZE,
                      IN_ALLOCATOR_2_FLAG) allocator_2;
    #endif
    #if USE_ALLOCATOR_3
    PoolAllocatorType(TIVOCONFIG_GC_ALLOC_3_SIZE,
                      IN_ALLOCATOR_3_FLAG) allocator_3;
    #endif
    LargeAllocator allocator_large;

    bool in_collect; // True when the owning thread is performing collect

    int next_collect_threshold; // when total_size exceeds this, collect

    void *stack_top; // Stack starts here and grows downwards
    void *stack_bottom; // Stack stops here
    jmp_buf jmpbuf; // Saves registers
    bool done;
};


/**** Global variables  **************************************************** */

// This is used internally by this code to indicate when "quiesce" should
// occur, as well as being used by external code to test for "quiesce"
// needing to occur
int hx::gPauseForCollect;


/**** Static variables  **************************************************** */

// Log the pool sizes
static GCSizeLogger gGCSizeLogger;

// This is used to protect against concurrent access to certain globals
// shared amongst all threads, and to signal when threading events occur.
#ifndef HXCPP_SINGLE_THREADED_APP
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
#endif

// This maps allocations to the weak refs to them.  Protected by g_lock.
static hx::UnorderedMap<void *, std::list<GCWeakRef *> > g_weak_ref_map;

// Garbage collection can be temporarily disabled, although it's not clear why
// this would ever be done.  Start GC disabled until it is ready, this
// prevents static initializers from suffering from unexpected GC.
static bool g_gc_enabled;

// These are explicitly declared root objects; hxcpp doesn't really seem to
// use these except to mark some CFFI stuff.
static Root *g_roots;

// This maps objects to finalizer data - it is expected that very few
// finalizers will be used
static hx::UnorderedMap<hx::Object *, FinalizerData> g_finalizers;

// Hxcpp includes a "weak hash" class that needs GC-side support
static std::list<hx::HashBase<Dynamic> *> g_weak_hash_list;

// Each thread gets one of these
static ThreadGlobalData *g_thread_global_data;
#ifdef HXCPP_SINGLE_THREADED_APP
static pthread_t g_haxe_thread;
#endif

// Number of non-GC-safe threads running
static int g_thread_count;

#ifndef HXCPP_SINGLE_THREADED_APP
static pthread_key_t g_info_key;
#endif

#ifdef TIVOCONFIG_GC_RECENT_ALLOCS_THRESHOLD
// Threshold at which a plog is dumped indicating that this many allocs have
// recently occurred.  Will be taken from the environment variable
// TIVOCONFIG_GC_RECENT_ALLOCS_THRESHOLD if it is set.
static uint32_t g_recent_allocs_threshold =
    TIVOCONFIG_GC_RECENT_ALLOCS_THRESHOLD;
// Number of stack frames to dump at each alloc threshold for the allocation
// that crosses the threshold; collecting stack traces in this way allows
// sampling of allocations.  This can be expensive so the default is 0, you
// have to turn it on via TIVOCONFIG_GC_RECENT_ALLOCS_STACK_FRAME_COUNT.
#ifdef TIVOCONFIG_GC_RECENT_ALLOCS_STACK_FRAME_COUNT
static uint32_t g_recent_allocs_frames =
    TIVOCONFIG_GC_RECENT_ALLOCS_STACK_FRAME_COUNT;
#endif
#endif

#ifdef TIVOCONFIG_GC_SIZE_STATISTICS_THRESHOLD
static uint32_t g_size_statistics_threshold =
    TIVOCONFIG_GC_SIZE_STATISTICS_THRESHOLD;
static hx::UnorderedMap<uint32_t, uint32_t> g_size_statistics;
#if !HXCPP_SINGLE_THREADED_APP
#endif
#endif

#ifdef TIVOCONFIG_GC_COLLECT_STACK_TRACES
static StackTrace *g_stacktrace_map[377737];
#endif


// The following allows the memory dump analyzer program to find values of
// interest -- it scans memory to find the four "fingerprint" values at the
// beginning of this array, and then can find pointers to the other globals of
// interest in the remaining known positions.
static void *gBeacon[] =
    { (void *) 0xdeadcccc, (void *) 0xdeadcc11,
      (void *) 0xdeadaabb, (void *) 0xdeadaabd,
      (void *) &g_mark,
      (void *) &g_roots,
      (void *) &g_thread_global_data,
#ifdef TIVOCONFIG_GC_COLLECT_STACK_TRACES
      (void *) g_stacktrace_map,
      (void *) &(g_stacktrace_map[sizeof(g_stacktrace_map) /
                                  sizeof(g_stacktrace_map[0])])
#else
      (void *) 0,
      (void *) 0
#endif
    };


/**** Static helper functions ********************************************** */


#ifdef HXCPP_SINGLE_THREADED_APP

#define Lock()
#define Unlock()

#else

static inline void Lock()
{
    pthread_mutex_lock(&g_lock);
}


static inline void Unlock()
{
    pthread_mutex_unlock(&g_lock);
}

// GCC built-in atomics seem to be supported only on gcc versions > 4.2.
// For ancient compilers (currently used only by mips), use a simple lock.
// This will reduce performance on multi-threaded applications, but then
// again, none of the programs we build using this ancient compiler are
// multithreaded anyway
#if ((__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR__ > 2)))

static inline bool CompareAndSwapPtr(uintptr_t *where, uintptr_t test,
                                     uintptr_t set)
{
    return __sync_bool_compare_and_swap(where, test, set);
}


static inline bool CompareAndSwapInt(int *where, int test, int set)
{
    return __sync_bool_compare_and_swap(where, test, set);
}

#else

static pthread_mutex_t g_old_compiler_mutex = PTHREAD_MUTEX_INITIALIZER;

static inline bool CompareAndSwapPtr(uintptr_t *where, uintptr_t test,
                                     uintptr_t set)
{
    bool ret;
    pthread_mutex_lock(&g_old_compiler_mutex);
    if (*where == test) {
        *where = set;
        ret = true;
    }
    else {
        ret = false;
    }
    pthread_mutex_unlock(&g_old_compiler_mutex);
    return ret;
}


static inline bool CompareAndSwapInt(int *where, int test, int set)
{
    bool ret;
    pthread_mutex_lock(&g_old_compiler_mutex);
    if (*where == test) {
        *where = set;
        ret = true;
    }
    else {
        ret = false;
    }
    pthread_mutex_unlock(&g_old_compiler_mutex);
    return ret;
}

#endif


static void make_key()
{
    pthread_key_create(&g_info_key, NULL);
}


#endif


#ifdef TIVOCONFIG_GC_RECENT_ALLOCS_THRESHOLD
static void check_recent_allocs_threshold()
{
    // If not supposed to check recent allocs (threshold is 0), then do nothing
    if (g_recent_allocs_threshold == 0) {
        return;
    }

    // Tracks the number of allocs since the last "recent allocs" plog
    static int g_recent_allocs;

    // Increment the number of recent allocs
    int recent_allocs;
    while (true) {
        recent_allocs = g_recent_allocs;
        // If this thread is the one that successfully incremented it ...
        if (CompareAndSwapInt(&g_recent_allocs, recent_allocs,
                              recent_allocs + 1)) {
            recent_allocs += 1;
            break;
        }
    }

    // If didn't hit a multiple of the threshold, nothing to do
    if (recent_allocs % g_recent_allocs_threshold) {
        return;
    }

#ifdef TIVO_STB    
    PLOG(PLOG_TYPE_STX_GC_RECENT_ALLOCS, sizeof(uint32_t),
         &g_recent_allocs);
#else
    GCLOG("Haxe GC recent allocs: %ld\n", g_recent_allocs);
#endif
    
    g_recent_allocs = 0;
    
#ifdef TIVOCONFIG_GC_RECENT_ALLOCS_STACK_FRAME_COUNT
    if (g_recent_allocs_frames > 0) {
        void **frames = (void **) malloc
            (sizeof(void *) * g_recent_allocs_frames);
#ifdef ANDROID
        int cnt = 0;
#else
        int cnt = backtrace(frames, g_recent_allocs_frames);
#endif
#ifdef TIVO_STB
        PLOG(PLOG_TYPE_STX_GC_RECENT_ALLOCS_STACKTRACE,
             (cnt * sizeof(void *)), frames);
#else
        GCLOG("Haxe GC recent allocs stack trace: ");
        for (int i = 0; i < cnt; i++) {
            if (i > 0) {
                GCLOG(", ");
            }
            GCLOG("%p", frames[i]);
        }
        GCLOG("\n");
        free(frames);
#endif
    }
#endif
}
#endif

#ifdef TIVOCONFIG_GC_SIZE_STATISTICS_THRESHOLD
static void check_size_statistics_threshold(uint32_t size)
{
    if (g_size_statistics_threshold == 0) {
        return;
    }

    // Save away the allocation size
    g_size_statistics[size] += 1;

    // Tracks the number of allocs since the last "recent allocs" plog
    static int g_recent_allocs;

    if (g_recent_allocs != g_size_statistics_threshold) {
        return;
    }

    g_recent_allocs = 0;

    uint32_t total = 0;

    // Get a sorted list of keys
    std::vector<uint32_t> keys;
    hx::UnorderedMap<uint32_t, uint32_t>::iterator it = g_size_statistics.begin();
    while (it != g_size_statistics.end()) {
        keys.push_back(it->first);
        total += it->second;
        it++;
    }

    std::sort(keys.begin(), keys.end());

    std::vector<uint32_t>::iterator it2 = keys.begin();

    GCLOG("GCX                   GC size allocation statistics\n");
    GCLOG("GCX Size          Total           Total LE        Percent"
          "    Percent LE\n");
    GCLOG("GCX ------------  --------------  --------------  -------"
          "    ----------\n");

    uint32_t running_total = 0;

    while (it2 != keys.end()) {
        uint32_t value = (unsigned) g_size_statistics[*it2];
        running_total += value;
        float value_pct = (value * 100.0) / total;
        float running_pct = (running_total * 100.0) / total;
        GCLOG("GCX %-12u  %-14u  %-14u  %s%s%0.2f      %s%s%0.2f\n",
              (unsigned) *it2, value, running_total,
              (value_pct < 100) ? " " : "",
              (value_pct < 10) ? " " : "", value_pct,
              (running_pct < 100) ? " " : "",
              (running_pct < 10) ? " ": "", running_pct);
        it2++;
    }
}
#endif


#ifdef TIVOCONFIG_GC_COLLECT_STACK_TRACES
static inline int map_stacktrace(void *ptr)
{
#if HXCPP_M64
    return ((((uintptr_t) ptr) >> 3) % 
            (sizeof(g_stacktrace_map) / sizeof(g_stacktrace_map[0])));
#else
    return ((((uintptr_t) ptr) >> 2) % 
            (sizeof(g_stacktrace_map) / sizeof(g_stacktrace_map[0])));
#endif
}


/**
 * Saves the stacktrace of the current stack and associates it with the given
 * pointer.
 **/
static void save_stacktrace(void *ptr)
{
#ifndef ANDROID
    // Create a StackTrace
    StackTrace *bt = (StackTrace *) malloc(sizeof(StackTrace));
    bt->ptr = ptr;
    // Collect the stacktrace.  Collect at most the number of stacktraces that
    // will fit in the frames array, minus one, to always allow enough room
    // for a zero terminator, and also write the zero terminator (the index of
    // which is returned by the stacktrace function).
    bt->frames[backtrace(bt->frames, 
                         (sizeof(bt->frames) / sizeof(bt->frames[0])) - 1)] = 0;

    // Put the stacktrace in the map
    Lock();
    int idx = map_stacktrace(ptr);
    bt->next = g_stacktrace_map[idx];
    g_stacktrace_map[idx] = bt;
    Unlock();
#endif
}


/**
 * Clears the stacktrace for a given pointer
 **/
static void clear_stacktrace(void *ptr)
{
    Lock();
    StackTrace **replace = &(g_stacktrace_map[map_stacktrace(ptr)]);

    while (*replace && ((*replace)->ptr != ptr)) {
        replace = &((*replace)->next);
    }

    StackTrace *tofree = *replace;

    if (*replace) {
        *replace = (*replace)->next;
    }
    Unlock();

    if (tofree) {
        free(tofree);
    }
}
#endif


// Sets the finalizer for an object to either f, if its set, or
// internal_finalizer, if it's set, or haxe_finalizer if its set, or to
// nothing if none are set
static void SetFinalizer(hx::Object *obj, hx::finalizer f,
                         hx::InternalFinalizer *internal_finalizer,
                         HaxeFinalizer haxe_finalizer)
{
    Lock();
    
    if (f || internal_finalizer || haxe_finalizer) {
        // If there was a pre-existing finalizer, clean it up
        if (g_finalizers.count(obj) > 0) {
            delete g_finalizers[obj].internal_finalizer;
        }
        FinalizerData &fd = g_finalizers[obj];
        if (f) {
            fd.type = FinalizerTypeFinalizer;
            fd.f = f;
        }
        else if (internal_finalizer) {
            fd.type = FinalizerTypeInternalFinalizer;
            fd.internal_finalizer = internal_finalizer;
        }
        else {
            fd.type = FinalizerTypeHaxeFinalizer;
            fd.haxe_finalizer = haxe_finalizer;
        }
        FLAGS_OF_ENTRY(ENTRY_FROM_PTR(obj)) |= HAS_FINALIZER_FLAG;
    }
    else {
        if (g_finalizers.count(obj) > 0) {
            delete g_finalizers[obj].internal_finalizer;
            g_finalizers.erase(obj);
        }
        FLAGS_OF_ENTRY(ENTRY_FROM_PTR(obj)) &= ~HAS_FINALIZER_FLAG;
    }

    Unlock();
}


// Only called during a sweep, when the calling thread has quiesced all other
// threads, so no locking is necessary
static void RunFinalizer(hx::Object *obj)
{
    // Take a copy of finalizer data so that it can be destroyed before
    // actually calling the finalizer in case the finalizer does something
    // crazy like attempt to delete the finalizer itself
    FinalizerData fd = g_finalizers[obj];
    g_finalizers.erase(obj);
    FLAGS_OF_ENTRY(ENTRY_FROM_PTR(obj)) &= ~HAS_FINALIZER_FLAG;

    switch (fd.type) {
    case FinalizerTypeFinalizer:
        (fd.f)(obj);
        break;
    case FinalizerTypeInternalFinalizer:
        if (fd.internal_finalizer) {
            (fd.internal_finalizer->mFinalizer)(obj);
            delete fd.internal_finalizer;
        }
        break;
    default:
        (fd.haxe_finalizer)(obj);
        break;
    }
}


// Only called during a sweep, when the calling thread has quiesced all other
// threads, so no locking is necessary
static void HandleWeakRefs()
{
    hx::UnorderedMap<void *, std::list<GCWeakRef *> >::iterator iter =
        g_weak_ref_map.begin();

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
        iter = g_weak_ref_map.erase(iter);
    }
}


// Only called during a sweep, when the calling thread has quiesced all other
// threads, so no locking is necessary
static void RemoveWeakRef(GCWeakRef *wr)
{
    void *ptr = wr->mRef.mPtr;
    hx::UnorderedMap<void *, std::list<GCWeakRef *> >::iterator iter = 
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


static inline ThreadGlobalData *GetThreadGlobalData()
{
#ifdef HXCPP_SINGLE_THREADED_APP
    return g_thread_global_data;
#else
    return (ThreadGlobalData *) pthread_getspecific(g_info_key);
#endif
}


static void UpdateThreadInfoWithThreadGlobalData(ThreadGlobalData *gd)
{
    void *bottom;
    gd->stack_bottom = &bottom;
    // Use setjmp to (hopefully) save registers
    (void) setjmp(gd->jmpbuf);
}


static inline void UpdateThreadInfo()
{
    UpdateThreadInfoWithThreadGlobalData(GetThreadGlobalData());
}


#ifndef HXCPP_SINGLE_THREADED_APP
static inline void QuiesceUntilGCDone()
{
    // The caller could be the one doing collect.  This happens when a
    // finalizer calls back into the GC during sweep.  In this case, don't
    // pause since we're already in collect.
    ThreadGlobalData *gd = GetThreadGlobalData();
    if (gd->in_collect) {
        return;
    }
    UpdateThreadInfoWithThreadGlobalData(gd);
    Lock();
    g_thread_count -= 1;
    pthread_cond_broadcast(&g_cond);
    while (hx::gPauseForCollect) {
        pthread_cond_wait(&g_cond, &g_lock);
    }
    g_thread_count += 1;
    Unlock();
}
#endif


// To be called at the beginning of GC, after all threads have quiesced.
// Removes all 'done' thread global data, by moving any non-empty pool blocks
// into another thread global data.  Because this is called during a collect,
// all threads except the calling thread are quiesced and so no locking is
// needed.
static inline void Consolidate()
{
#ifndef HXCPP_SINGLE_THREADED_APP
    // Move the head of the thread global data list to the first not-done
    // member.
    if (g_thread_global_data->done) {
        ThreadGlobalData *gd = g_thread_global_data->next;
        while (gd->done) {
            gd = gd->next;
        }
        g_thread_global_data = gd;
    }

    // Remove all 'done' thread global data structures from the list
    ThreadGlobalData **replace = &(g_thread_global_data->next);
    ThreadGlobalData *gd = *replace;

    while (gd) {
        if (gd->done) {
#if USE_ALLOCATOR_0
            gd->allocator_0.MergeInto
                (g_thread_global_data->allocator_0);
#endif
#if USE_ALLOCATOR_1
            gd->allocator_1.MergeInto
                (g_thread_global_data->allocator_1);
#endif
#if USE_ALLOCATOR_2
            gd->allocator_2.MergeInto
                (g_thread_global_data->allocator_2);
#endif
#if USE_ALLOCATOR_3
            gd->allocator_3.MergeInto
                (g_thread_global_data->allocator_3);
#endif
            gd->allocator_large.MergeInto
                (g_thread_global_data->allocator_large);
            *replace = gd->next;
            delete gd;
            gd = *replace;
        }
        else {
            replace = &(gd->next);
            gd = gd->next;
        }
    }
#endif
}


#ifndef HXCPP_SINGLE_THREADED_APP
static inline void CheckForGC()
{
    // Note that no synchronization is done here.  We may have stale data in
    // hx::gPauseForCollect.  However, this is harmless, it just means that
    // the GC will have to wait a little longer until we check again and get
    // the up-to-date value.
    if (hx::gPauseForCollect) {
        QuiesceUntilGCDone();
    }
}
#endif


static inline void MaybeCollect(ThreadGlobalData *gd, int added)
{
    if ((gd->GetAllocatedBytes() > (gd->next_collect_threshold - added)) &&
        g_gc_enabled) {
        hx::InternalCollect(false, false);
    }
}


// Marks all pointers located in the range [start, end] (inclusive) with the
// current g_mark value.  Only called during GC when a single thread is doing
// collect and all other threads are quiesced, so no locking is necessary.
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
        ThreadGlobalData *gd = g_thread_global_data;
        while (gd) {
            Entry *entry = ENTRY_FROM_PTR(ptr);
            if (
#if USE_ALLOCATOR_0
                gd->allocator_0.Contains(entry) ||
#endif
#if USE_ALLOCATOR_1
                gd->allocator_1.Contains(entry) ||
#endif
#if USE_ALLOCATOR_2
                gd->allocator_2.Contains(entry) ||
#endif
#if USE_ALLOCATOR_3
                gd->allocator_3.Contains(entry) ||
#endif
                gd->allocator_large.Contains(ptr)) {
                if (FLAGS_OF_ENTRY(entry) & LAST_MARK_MASK) {
                    if (FLAGS_OF_ENTRY(entry) & IS_OBJECT_FLAG) {
                        hx::MarkObjectAlloc((hx::Object *) ptr, 0);
                        break;
                    }
                    else {
                        hx::MarkAlloc(ptr, 0);
                        break;
                    }
                }
            }
            gd = gd->next;
        }
    }
}


static inline void *InternalNewNoCollect(ThreadGlobalData *gd, int inSize,
                                         bool inIsObject)
{
#ifdef TIVOCONFIG_GC_SIZE_STATISTICS_THRESHOLD
    check_size_statistics_threshold(inSize);
#endif

#if USE_ALLOCATOR_0
    if (inSize <= TIVOCONFIG_GC_ALLOC_0_SIZE) {
        return gd->allocator_0.Allocate(inIsObject);
    }
#else
    if (false) {
    }
#endif
#if USE_ALLOCATOR_1
    else if (inSize <= TIVOCONFIG_GC_ALLOC_1_SIZE) {
        return gd->allocator_1.Allocate(inIsObject);
    }
#endif
#if USE_ALLOCATOR_2
    else if (inSize <= TIVOCONFIG_GC_ALLOC_2_SIZE) {
        return gd->allocator_2.Allocate(inIsObject);
    }
#endif
#if USE_ALLOCATOR_3
    else if (inSize <= TIVOCONFIG_GC_ALLOC_3_SIZE) {
        return gd->allocator_3.Allocate(inIsObject);
    }
#endif
    else {
        return gd->allocator_large.Allocate(inSize, inIsObject);
    }
}


// Must be called only when all other threads are quiesced
static uint32_t GetTotalAllocatedBytes()
{
    uint32_t total = 0;
    
    ThreadGlobalData *gd = g_thread_global_data;
    while (gd) {
        total += gd->GetAllocatedBytes();
        gd = gd->next;
    }

    return total;
}
    


/**** hxcpp hx namespace public functions ********************************** */

void hx::GCSetFinalizer(hx::Object *obj, hx::finalizer f)
{
    SetFinalizer(obj, f, 0, 0);
}


hx::InternalFinalizer::InternalFinalizer(hx::Object *obj, finalizer inFinalizer)
    : mObject(obj), mFinalizer(inFinalizer)
{
    SetFinalizer(obj, 0, this, 0);
}


void hx::InternalFinalizer::Detach()
{
    SetFinalizer(mObject, 0, 0, 0);
}


void hx::GCAddRoot(hx::Object **inRoot)
{
    Root *root = (Root *) malloc(sizeof(Root));
    root->pptr = inRoot;
    Lock();
    root->next = g_roots;
    g_roots = root;
    Unlock();
}


void hx::GCRemoveRoot(hx::Object **inRoot)
{
    Lock();
    Root **root = &g_roots;
    while (*root && ((*root)->pptr != inRoot)) {
        root = &((*root)->next);
    }
    Root *tofree = *root;
    if (*root) {
        *root = (*root)->next;
    }
    Unlock();
    if (tofree) {
        free(tofree);
    }
}


void hx::PauseForCollect()
{
#ifndef HXCPP_SINGLE_THREADED_APP
    // The caller could be the one doing collect.  This happens when a
    // finalizer calls back into the GC during sweep.  In this case, don't
    // pause since we're already in collect.
    ThreadGlobalData *gd = GetThreadGlobalData();
    if (gd->in_collect) {
        return;
    }
    UpdateThreadInfoWithThreadGlobalData(gd);
#endif
}


void hx::EnterGCFreeZone()
{
#ifndef HXCPP_SINGLE_THREADED_APP
    // The caller could be the one doing collect.  This happens when a
    // finalizer calls back into the GC during sweep.  In this case, don't
    // pause since we're already in collect.
    ThreadGlobalData *gd = GetThreadGlobalData();
    if (gd->in_collect) {
        return;
    }
    UpdateThreadInfoWithThreadGlobalData(gd);
    Lock();
    g_thread_count -= 1;
    pthread_cond_broadcast(&g_cond);
    Unlock();
#endif
}


void hx::ExitGCFreeZone()
{
#ifndef HXCPP_SINGLE_THREADED_APP
    // The caller could be the one doing collect.  This happens when a
    // finalizer calls back into the GC during sweep.  In this case, don't
    // pause since we're already in collect.
    ThreadGlobalData *gd = GetThreadGlobalData();
    if (gd->in_collect) {
        return;
    }
    Lock();
    g_thread_count += 1;
    while (hx::gPauseForCollect) {
        pthread_cond_wait(&g_cond, &g_lock);
    }
    Unlock();
#endif
}


void hx::MarkAlloc(void *ptr, hx::MarkContext *)
{
    Entry *entry = ENTRY_FROM_PTR(ptr);

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


// Despite being named 'InternalCollect', this function is directly invoked
// whenever a garbage collection is requested elsewhere in the code base, in
// addition to being invoked whenever an InternalNew or InternalAlloc would
// exceed the current garbage collect threshold
int hx::InternalCollect(bool, bool)
{
#ifndef HXCPP_SINGLE_THREADED_APP
    // If a quiesce was requested, then assume a GC is being attempted, and
    // quiesce until there is no more GC in progress
    if (!CompareAndSwapInt(&hx::gPauseForCollect, 0, 1)) {
        QuiesceUntilGCDone();
        return g_total_after_last_collect;
    }
#endif

    PLOG(PLOG_TYPE_STX_GC_BEGIN, 0, 0);

#ifdef TIVO_STB
    uint32_t initial_total_size;
    if (process_log_available()) {
        initial_total_size = GetTotalAllocatedBytes();
    }
    else {
        initial_total_size = 0;
    }
#endif

#ifndef HXCPP_SINGLE_THREADED_APP
    // Wait until everyone else is quiesced
    Lock();
    while (g_thread_count != 1) {
        pthread_cond_wait(&g_cond, &g_lock);
    }
    Unlock();
#endif

    ThreadGlobalData *my_gd = GetThreadGlobalData();

    // This thread is now doing the collect
    my_gd->in_collect = true;
    
    // Update this thread info, everyone else already did when quiescing
    UpdateThreadInfoWithThreadGlobalData(my_gd);
    
    // Consolidate thread global data now that everyone is quiesced
    Consolidate();

    // Now do the actual GC

    PLOG(PLOG_TYPE_STX_GC_MARK_BEGIN, 0, 0);
    
    // Update the mark, so that instantly everything is considered
    // non-referenced until it is found via mark
    g_mark = (g_mark == MARK_A_FLAG) ? MARK_B_FLAG : MARK_A_FLAG;

    // Performance improvement idea: do the marking multi-threaded.  This
    // could be heavily memory contentious though and may not be worth it.
    // Would require that we push all of the stacks/registers/root
    // pointers/etc onto a global list and then have threads pull pointers
    // off one by one.  If the pointer is not marked, then CompareAndSwap
    // a mark flag into it, and if succeeded, proceed to recursively mark it

    // Mark from stacks and registers
    ThreadGlobalData *gd = g_thread_global_data;
    while (gd) {
        // Stacks
        MarkPointers(gd->stack_bottom, gd->stack_top);
        // Registers
        char *jmp = (char *) (gd->jmpbuf);
        MarkPointers(jmp, jmp + sizeof(gd->jmpbuf));
        gd = gd->next;
    }

    PLOG(PLOG_TYPE_STX_GC_MARK_STACKS_END, 0, 0);
    
    // Mark class statics
    hx::MarkClassStatics(0);
    
    PLOG(PLOG_TYPE_STX_GC_MARK_STATICS_END, 0, 0);
    
    // Mark roots and everything reachable from them
    Root *root = g_roots;
    while (root) {
        hx::Object **obj = root->pptr;
        if (obj && *obj) {
            MarkObjectAlloc(*obj, 0);
        }
        root = root->next;
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

    PLOG(PLOG_TYPE_STX_GC_SWEEP_BEGIN, 0, 0);

    // Reset total reaped, so that the stats may be collected during the
    // sweeps
    g_total_reaped = 0;

    // Sweep all allocators of all threads.  Also, count total used bytes
    // and update next collect thresholds.
    // Performance improvement idea: have each thread at the end of waiting
    // for GC to be over, do the sweep of its own allocators.  And have this
    // thread only sweep its own allocators here.  In this way, sweeping will
    // be multithreaded.  Another idea that could help single threaded
    // programs is to have a set of threads available to do the sweeping and
    // have each one sweep a different block out concurrently ...
    g_total_after_last_collect = 0;
    gd = g_thread_global_data;
    while (gd) {
#if USE_ALLOCATOR_0
        gd->allocator_0.Sweep();
#endif
#if USE_ALLOCATOR_1
        gd->allocator_1.Sweep();
#endif
#if USE_ALLOCATOR_2
        gd->allocator_2.Sweep();
#endif
#if USE_ALLOCATOR_3
        gd->allocator_3.Sweep();
#endif
        gd->allocator_large.Sweep();

        uint32_t bytes = gd->GetAllocatedBytes();
        g_total_after_last_collect += bytes;
        gd->next_collect_threshold = NEXT_COLLECT_THRESHOLD(bytes);
        
        gd = gd->next;
    }

    PLOG(PLOG_TYPE_STX_GC_SWEEP_END, 0, 0);

    // This thread is no longer doing a collect
    my_gd->in_collect = false;

    // GC is no longer in progress
    hx::gPauseForCollect = 0;

#ifndef HXCPP_SINGLE_THREADED_APP
    pthread_cond_broadcast(&g_cond);
#endif

    PLOG(PLOG_TYPE_STX_GC_END, 0, 0);

    static int gc_count;
    gc_count += 1;
    
#ifdef TIVO_STB
    if (initial_total_size) {
        StxGcReaped reaped =
            { g_total_reaped, initial_total_size - g_total_after_last_collect };
        PLOG(PLOG_TYPE_STX_GC_REAPED, sizeof(reaped), &reaped);
        StxPlogGcStatistics stats =
            { g_total_after_last_collect, 0, gc_count, 0 };
        PLOG(PLOG_TYPE_STX_GC_STATISTICS, sizeof(stats), &stats);
    }
#endif

    return g_total_after_last_collect;
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


void hx::MarkConservative(int *begin, int *end, hx::MarkContext *)
{
    MarkPointers(begin, end);
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


// Called to detect whether or not the calling thread is a haxe thread
bool hx::IsHaxeThread()
{
#ifdef HXCPP_SINGLE_THREADED_APP
    return pthread_equal(pthread_self(), g_haxe_thread);
#else
    return (GetThreadGlobalData() != NULL);
#endif
}

// Called with [stack_top] of 0 by the main thread when the main thread has
//   finished
// Called at the beginning of a haxe thread function execution
// Called on Android by the Java thread via JNI native callback into haxe,
//   right before actually making the callback
// Called with [stack_top] of 0 by the Java thread via JNI native callback
//   into haxe, right after actually making the callback
void hx::SetTopOfStack(int *stack_top, bool)
{
#ifdef TIVOCONFIG_GC_RECENT_ALLOCS_THRESHOLD
    static bool gc_recent_initialized = false;
    if (!gc_recent_initialized) {
        gc_recent_initialized = true;
#ifdef TIVO_STB
        process_log_open();
#endif
        const char *env = getenv("TIVOCONFIG_GC_RECENT_ALLOCS_THRESHOLD");
        if (env) {
            g_recent_allocs_threshold = atoi(env);
            if (g_recent_allocs_threshold < 0) {
                g_recent_allocs_threshold = 0;
            }
        }
        GCLOG("*** Haxe GC set recent allocs threshold to %u\n",
              (unsigned) g_recent_allocs_threshold);
#ifdef TIVOCONFIG_GC_RECENT_ALLOCS_STACK_FRAME_COUNT
        env = getenv("TIVOCONFIG_GC_RECENT_ALLOCS_STACK_FRAME_COUNT");
        if (env) {
            g_recent_allocs_frames = atoi(env);
            if (g_recent_allocs_frames < 0) {
                g_recent_allocs_frames = 0;
            }
            if (g_recent_allocs_frames > 24) {
                g_recent_allocs_frames = 24;
            }
        }
        GCLOG("*** Haxe GC set recent alloc stack frame count "
              "to %u\n", (unsigned) g_recent_allocs_frames);
#endif
    }
#endif
    
    ThreadGlobalData *gd;

#ifdef HXCPP_SINGLE_THREADED_APP
    
    if (!g_thread_global_data) {
        g_thread_global_data = new ThreadGlobalData();
        g_haxe_thread = pthread_self();
    }

    gd = g_thread_global_data;
    
    // Just check to make sure this is not called for HXCPP_SINGLE_THREADED_APP
    if (g_thread_count > 0) {
        GCLOG("ERROR: haxe multiple threads created in program compiled with "
              "HXCPP_SINGLE_THREADED_APP\n");
        exit(-1);
    }
    
    g_thread_count += 1;
    
#else

    static pthread_once_t g_info_key_once = PTHREAD_ONCE_INIT;
    pthread_once(&g_info_key_once, make_key);

    // If there is no thread global data for this thread yet, then make one
    gd = GetThreadGlobalData();

    if (gd) {
        if (stack_top) {
            if (!gd->stack_top) {
                // Going from no stack to a stack, this is now an interesting
                // thread
                Lock();
                g_thread_count += 1;
                Unlock();
            }
        }
        else if (gd->stack_top) {
            // Going from stack to no stack, this is not an interesting
            // thread anymore
            Lock();
            g_thread_count -= 1;
            pthread_cond_broadcast(&g_cond);
            Unlock();
        }
    }
    else {
        // Increment the thread count as this is a new thread
        Lock();
        g_thread_count += 1;
        Unlock();
        // Create the thread global data for this new thread
        gd = new ThreadGlobalData();
        // And store it as this thread's global data
        pthread_setspecific(g_info_key, (void *) gd);
        // Loop until the thread global data can be added into the global list
        while (true) {
            ThreadGlobalData *old_gd = g_thread_global_data;
            gd->next = old_gd;
            // Try to set g_thread_global_data to gd
            if (CompareAndSwapPtr((uintptr_t *) &g_thread_global_data,
                                  (uintptr_t) old_gd, (uintptr_t) gd)) {
                // Succeded
                break;
            }
            // Failed, try again to add gd to the list
        }
    }

#endif

    gd->stack_top = stack_top;

    // This is the earliest call that the GC actually gets, in the first
    // thread's 'main', and all static constructors have completed, so it's
    // time to enable GC
    g_gc_enabled = true;
}


void hx::UnregisterCurrentThread()
{
    ThreadGlobalData *gd = GetThreadGlobalData();
    
    // Mark the thread global data as done
    gd->done = true;

#ifndef HXCPP_SINGLE_THREADED_APP
    // This thread no longer has ownership of gd
    pthread_setspecific(g_info_key, 0);
    
    // Decrement the thread count
    Lock();
    g_thread_count -= 1;
    pthread_cond_broadcast(&g_cond);
    Unlock();
#endif
}


void hx::RegisterWeakHash(hx::HashBase<Dynamic> *inHash)
{
    Lock();
    g_weak_hash_list.push_back(inHash);
    Unlock();
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
    ThreadGlobalData *gd = GetThreadGlobalData();
    if (gd->in_collect) {
        return;
    }
#ifndef HXCPP_SINGLE_THREADED_APP
    // Must quiesce all other threads as if a GC were going to happen
    while (!CompareAndSwapInt(&hx::gPauseForCollect, 0, 1)) {
        QuiesceUntilGCDone();
    }
    // Wait until everyone else is quiesced
    Lock();
    while (g_thread_count != 1) {
        pthread_cond_wait(&g_cond, &g_lock);
    }
    Unlock();
#endif
    
    g_gc_enabled = inEnable;

    hx::gPauseForCollect = 0;

#ifndef HXCPP_SINGLE_THREADED_APP
    pthread_cond_broadcast(&g_cond);
#endif    
}


void *hx::InternalNew(int inSize, bool inIsObject)
{
    // Since threads very often allocate objects, this is a great place to
    // check to see if another thread is waiting for everyone to quiesce so
    // that it can complete a GC
#ifndef HXCPP_SINGLE_THREADED_APP
    CheckForGC();
#endif
    
    ThreadGlobalData *gd = GetThreadGlobalData();

    MaybeCollect(gd, inSize);

    void *ret = InternalNewNoCollect(gd, inSize, inIsObject);

#ifdef TIVOCONFIG_GC_COLLECT_STACK_TRACES
    if (ret) {
        save_stacktrace(ret);
    }
#endif

    return ret;
}


void *hx::InternalRealloc(void *ptr, int new_size)
{
    // If the pointer is null, then this is actually a 'new', and definitely
    // not for an object since objects are never realloc'd
    if (!ptr) {
        return InternalNew(new_size, false);
    }
    
    // Since threads very often allocate objects, this is a great place to
    // check to see if another thread is waiting for everyone to quiesce so
    // that it can complete a GC
#ifndef HXCPP_SINGLE_THREADED_APP
    CheckForGC();
#endif

    ThreadGlobalData *gd = GetThreadGlobalData();

    Entry *entry = ENTRY_FROM_PTR(ptr);

    // Just assume that the entirety is new ...
    MaybeCollect(gd, new_size);
    
    void *ret;

#if USE_ALLOCATOR_0
    if (FLAGS_OF_ENTRY(entry) & IN_ALLOCATOR_0_FLAG) {
        ret = gd->allocator_0.Reallocate(entry, new_size);
    }
#else
    if (false) {
    }
#endif
#if USE_ALLOCATOR_1
    else if (FLAGS_OF_ENTRY(entry) & IN_ALLOCATOR_1_FLAG) {
        ret = gd->allocator_1.Reallocate(entry, new_size);
    }
#endif
#if USE_ALLOCATOR_2
    else if (FLAGS_OF_ENTRY(entry) & IN_ALLOCATOR_2_FLAG) {
        ret = gd->allocator_2.Reallocate(entry, new_size);
    }
#endif
#if USE_ALLOCATOR_3
    else if (FLAGS_OF_ENTRY(entry) & IN_ALLOCATOR_3_FLAG) {
        ret = gd->allocator_3.Reallocate(entry, new_size);
    }
#endif
    else {
        ret = gd->allocator_large.Reallocate(ptr, new_size);
    }

#ifdef TIVOCONFIG_GC_COLLECT_STACK_TRACES
    if (ret != ptr) {
        clear_stacktrace(ptr);
        if (ret) {
            save_stacktrace(ret);
        }
    }
#endif
    
    return ret;
}


void *hx::Object::operator new(size_t inSize, bool inContainer, const char *)
{
    return hx::InternalNew(inSize, inContainer);
}


/**** hxcpp public functions *********************************************** */

void __hxcpp_set_finalizer(Dynamic inObject, void *inFinalizer)
{
    SetFinalizer(inObject.mPtr, 0, 0, (HaxeFinalizer) inFinalizer);
}


hx::Object *__hxcpp_weak_ref_create(Dynamic inObject)
{
    GCWeakRef *ret = new GCWeakRef(inObject);

    FLAGS_OF_ENTRY(ENTRY_FROM_PTR(ret)) |= IS_WEAK_REF_FLAG;
 
    if (inObject.mPtr) {
        Lock();
        // Map the object to the the weak ref, so that when the object is
        // finalized, all of the weak refs to it can be cleared
        g_weak_ref_map[inObject.mPtr].push_back(ret);
        Unlock();
    }

    return ret;
}


hx::Object *__hxcpp_weak_ref_get(Dynamic inRef)
{
    Lock();
    hx::Object *ret = ((GCWeakRef *) (inRef.mPtr))->mRef.mPtr;
    Unlock();
    return ret;
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

    Lock();
    
    hx::UnorderedMap<uintptr_t, int>::iterator i = g_obj_map.find(ptr);

    if (i != g_obj_map.end()) {
        Unlock();
        return i->second;
    }

    int id = ++g_current_id;

    g_id_map[ptr] = id;
    g_obj_map[id] = ptr;

    Unlock();

    return id;
#else
    return (int) ptr;
#endif
}


hx::Object *__hxcpp_id_obj(int id)
{
#ifdef HXCPP_M64

    Lock();
    
    hx::UnorderedMap<int, uintptr_t>::iterator i = g_id_map.find(id);

    hx::Object *ret = (i == g_id_map.end()) ? 0 : (hx::Object *) (i->second);

    Unlock();

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


extern "C"
{
void hxcpp_set_top_of_stack()
{
   int i = 0;
   hx::SetTopOfStack(&i, false);
}
}


void __hxcpp_enter_gc_free_zone()
{
    hx::EnterGCFreeZone();
}


void __hxcpp_exit_gc_free_zone()
{
    hx::ExitGCFreeZone();
}


void __hxcpp_gc_safe_point()
{
#ifndef HXCPP_SINGLE_THREADED_APP
    CheckForGC();
#endif
}


int __hxcpp_gc_used_bytes()
{
    return __hxcpp_gc_mem_info(0);
}


int __hxcpp_gc_mem_info(int inWhich)
{
    if ((inWhich == 0) || (inWhich > 3)) {
        //   MEM_INFO_USAGE - estimate of how much is needed by program (at
        //   last collect)
        return g_total_after_last_collect;
    }

#ifndef HXCPP_SINGLE_THREADED_APP    
    // Must quiesce all other threads as if a GC were going to happen
    while (!CompareAndSwapInt(&hx::gPauseForCollect, 0, 1)) {
        QuiesceUntilGCDone();
    }
    // Wait until everyone else is quiesced 
    Lock();
    while (g_thread_count != 1) {
        pthread_cond_wait(&g_cond, &g_lock);
    }
    Unlock();
#endif
    
    int total = 0;
    
    ThreadGlobalData *gd = g_thread_global_data;
    
    switch (inWhich) {
    case 1:
        //   MEM_INFO_RESERVED - memory allocated for possible use
        while (gd) {
#if USE_ALLOCATOR_0
            total += gd->allocator_0.GetTotalBytes();
#endif
#if USE_ALLOCATOR_1
            total += gd->allocator_1.GetTotalBytes();
#endif
#if USE_ALLOCATOR_2
            total += gd->allocator_2.GetTotalBytes();
#endif
#if USE_ALLOCATOR_3
            total += gd->allocator_3.GetTotalBytes();
#endif
            total += gd->allocator_large.GetAllocatedBytes();
            gd = gd->next;
        }
        break;
    case 2:
        //   MEM_INFO_CURRENT - memory in use, includes uncollected garbage.
        //     This will generally saw-tooth between USAGE and RESERVED
        total = GetTotalAllocatedBytes();
        break;
    case 3:
        //   MEM_INFO_LARGE - Size of separate pool used for large allocs.
        //   Included in all the above.
        while (gd) {
            total += gd->allocator_large.GetAllocatedBytes();
            gd = gd->next;
        }
        break;
    }

    hx::gPauseForCollect = 0;

#ifndef HXCPP_SINGLE_THREADED_APP
    pthread_cond_broadcast(&g_cond);
#endif
    
    return total;
}


/**** hxcpp dead functions ************************************************* */

// ----------------------------------------------------------------------------
// NO-OP functions that don't do anything in this implementation but are
// defined in GC.h and are left basically unimplemented here
// ----------------------------------------------------------------------------

// Used only by haxe cpp.vm.Gc class, not implemented
int __hxcpp_gc_trace(hx::Class, bool)
{
    return 0;
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


// Called when a thread is about to be created; allows initialization of any
// GC state necessary to handle support for this thread to occur.
void hx::GCPrepareMultiThreaded()
{
    // This implementation doesn't do anything here; it's always ready for
    // multithreading if HXCPP_SINGLE_THREADED_APP is not defined
}


// Not called by hxcpp at all
void hx::RegisterNewThread(void *)
{
}


// Not called by hxcpp at all
void hx::RegisterCurrentThread(void *)
{
}


// It's not clear what the intent of this function is, as it's not used
// anywhere.
void hx::GCChangeManagedMemory(int, const char *)
{
}
