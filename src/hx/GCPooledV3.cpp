/** **************************************************************************
 * GCPooledV3.cpp
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
 * - Multiple threads can be used during GC to speed up mark and sweep
 *
 * TODO:
 *
 * - Improve performance of weak hash/weak map by not using C++ STL
 * - Implement an Immix style allocator for allocations that are larger than
 *   the largest block allocator, but smaller than some reasonable large
 *   allocation; like 1024 bytes or something like that.
 *
 ************************************************************************** **/

// The following compiler values may be set in order to tune the functionality
// of the garbage collector for a particular application (all may be set via
// haxe compiler options ("-D xxx=yyy") or via lime project settings):
//
// TIVOCONFIG_GC_BLOCK_SIZE -- this is the size of a "block" used by the pool
//     allocators.  Allocations for small sizes come out of pools, and pools
//     are allocated in chunks of HXCPP_GC_BLOCK_SIZE at a time.  The default
//     is 512 KiB.
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
// TIVOCONFIG_GC_THREAD_COUNT -- this number of threads will be used to
//     perform the GC.  Defaults to 2.
//
// TIVOCONFIG_GC_COLLECT_SIZE -- number of bytes that can be allocated before
//     a system-wide GC is performed.  Note that the larger this is made, the
//     less frequent garbage collects will be, but the longer they will take.
//     Usually more frequent, shorter duration GCs are preferable for an
//     interactive application.  Defaults to 10 MiB.
//
// TIVOCONFIG_GC_ENABLE_DEBUGGING -- define to enable GC debugging.  If
//     this is enabled, then the following environment variables may be
//     defined at runtime to enable GC debugging:
//     "TIVOCONFIG_GC_DEBUGGING_HOST" - host name
//     "TIVOCONFIG_GC_DEBUGGING_PORT" - port (default 2826)
//     (on Android, these are files of the same name in /sdcard whose contents
//      are the value to use)
//     If a host:port is defined in runtime variables, then the garbage
//     collector will connect to a peer at this location.  See the code
//     for details of what events are sent out.  In general, the 'debugclient'
//     program should be used to capture and store GC debugger output.

#include <algorithm>
#ifndef ANDROID
#include <execinfo.h>
#endif
#include <list>
#include <map>
#include <pthread.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include "hxcpp.h"
#include "hx/GC.h"
#include "hx/Unordered.h"
#include "Hash.h"


#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unwind.h>
#include <string>
#endif

#ifdef TIVO_STB
#include <stx/StxProcessLog.h>
#endif


/**** Forward type declarations ******************************************** */
class GCWeakRef;


/**** Static function signature forward declarations *********************** */

// Helper function to run a finalizer for a given object.  It is assumed that
// there is a finalizer for the pointer.
static void RunFinalizer(hx::Object *obj);

// Clears weak refs for those weakly referenced pointers that are no longer
// reachable
static void HandleWeakRefs();

// Remove a weak reference that is no longer referenced.
static void RemoveWeakRef(GCWeakRef *wr);

// Run finalizers for all newly free objects with finalizers.  Note that
// the finalizers themselves may make GC allocations.
static void HandleFinalizers();

// Mark roots
static void MarkRoots();

// Mark weak hashes
static void MarkWeakHashes();

// Allocate zeroed memory, running a GC to acquire more memory if necessary.
// May only be called from a haxe thread.
static void *Calloc(size_t nmemb, size_t size);

#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
// Clears stack trace saved for the given allocated pointer.
static void debug_connect();
static void debug_alloc(void *ptr, uint32_t size);
static void debug_mark(void *ptr, void *ref);
static void debug_sweep(void *ptr);
static void debug_live(void *ptr);
static void debug_before_gc();
static void debug_after_gc(uint32_t mark_us, uint32_t sweep_us,
                           uint32_t total_us);


/**** Static helper functions ********************************************** */

static uint64_t nowUs()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ret = ts.tv_nsec / 1000;
    ret += ts.tv_sec * 1000 * 1000;
    return ret;
}

#endif


/**** Macro definitions **************************************************** */

// Configurable tunable values
#ifndef TIVOCONFIG_GC_BLOCK_SIZE
#define TIVOCONFIG_GC_BLOCK_SIZE (512 * 1024)
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

#ifndef TIVOCONFIG_GC_THREAD_COUNT
#define TIVOCONFIG_GC_THREAD_COUNT 4
#endif

#ifndef TIVOCONFIG_GC_MIN_COLLECT_SIZE
#define TIVOCONFIG_GC_MIN_COLLECT_SIZE (12 * 1024 * 1024)
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
#else
// Enable this to see GC plog messages on stdout on dev-host
#define GCLOG(...) fprintf(stderr, __VA_ARGS__)
//#define GCLOG(...)
#endif


// Enable process logging only on STB targets
#ifdef TIVO_STB
#define PLOG(type, data_len, data) process_log(type, data_len, data)
#else
#define PLOG(type, data_len, data)
#endif

// Flags for use in Entry flags
#define MARK_A_FLAG               (1 << 29)
#define MARK_B_FLAG               (1 << 28)
#define IS_OBJECT_FLAG            (1 << 27)
#define IS_WEAK_REF_FLAG          (1 << 26)
#if USE_ALLOCATOR_0
#define IN_ALLOCATOR_0_FLAG       (1 << 25)
#endif
#if USE_ALLOCATOR_1
#define IN_ALLOCATOR_1_FLAG       (1 << 24)
#endif
#if USE_ALLOCATOR_2
#define IN_ALLOCATOR_2_FLAG       (1 << 23)
#endif
#if USE_ALLOCATOR_3
#define IN_ALLOCATOR_3_FLAG       (1 << 22)
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

// Get a reference to the flags of an entry.  Must be done this way because
// hxcpp requires the flags to be 4 bytes before the data.
#define FLAGS_OF_ENTRY(entry) \
    (*((uint32_t *) (((uint32_t) ((entry)->bytes)) - 4)))

// Declares a block manager for a particular size allocation
#define BlockManagerType(size, flag)                                         \
    BlockManager<size,                                                       \
                 ((TIVOCONFIG_GC_BLOCK_SIZE - sizeof(void *)) /              \
                  (size + sizeof(Entry))),                                   \
                 flag>

// Declares a block for a particular size allocation
#define BlockType(size, flag) BlockManagerType(size, flag)::SizedBlock


/**** Static variables needed by type definitions ************************** */

#ifdef HXCPP_M64
// This is the next id to assign to objects.  It is protected by g_lock.
static int g_next_id = 1;
// This is the mapping of ids to objects.  It is protected by g_lock.
static hx::UnorderedMap<int, uintptr_t> g_id_map;
#endif

// These are explicitly declared root objects; hxcpp doesn't really seem to
// use these except to mark some CFFI stuff.
static struct Root *g_roots;

// The most recent mark value, used to mark objects during the mark phase;
// toggles between MARK_A_FLAG and MARK_B_FLAG, to match the last mark bits of
// an Entry's flag_and_offset value.  Read concurrently by many threads
// outside of a collect, written only by the collecting thread during a
// collect.
static uint32_t g_mark = MARK_A_FLAG;

// Size of allocs from this all threads since last GC was requested -- may not
// be accurate
static uint32_t gRecentAllocs;

// Garbage collection can be temporarily disabled, although it's not clear why
// this would ever be done.  Start GC disabled until it is ready, this
// prevents static initializers from suffering from unexpected GC.
static bool g_gc_enabled = true;

// Track when garbage collection is currently in progress, basically to
// prevent finalizers from interacting with the GC in a bad way
static bool g_gc_in_progress;

#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
static int g_alloc_count;
static hx::UnorderedMap<uint32_t, uint32_t> g_size_statistics;
// Debugging timestamps are reported relative to the first instant that the
// GC initializes, in order to keep the value small (< 32 bits)
static uint64_t g_zero_time_us;
// If this code is running in a shared object library (which is true on some
// platforms), then the vtable addresses will be offset from wherever the
// shared object library is mapped into in memory
static uintptr_t g_vtable_offset;
#endif


/**** Type definitions ***************************************************** */

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


// All allocations are immediately preceded by an entry structure:
// - Small allocations go through one of the PoolAllocator instances, which
//   pre-allocate large numbers of fixed size entries
// - Large allocations allocate memory with a LargeEntry structure
// The flags and bytes members are identically offset, which allows recovering
// the flags from any pointer regardless of whether the allocation was from
// a pooled allocator or a large allocation.
struct Entry
{
    // If this Entry is an Entry, then this is the next Entry in the free
    // list.
    // If this Entry is a LargeEntry, then this is the next LargeEntry in the
    // allocated list.
    union {
        Entry *next;
        struct LargeEntry *largeNext;
    };
    // hxcpp requires a 32 bit integer id for every object.  32 bit systems
    // will just use the pointer itself; but 64 bit systems need to assign a
    // unique 32 bit id.
#ifdef HXCPP_M64
    int id;
#endif
    // Flags describing the allocation.  Note that the flags are actually
    // stored at (bytes - 4 bytes), to allow the technique that hxcpp uses for
    // strings to work, which is to treat data pointers as int[] and then
    // index by -1 to get a 32 bit flags value.  Therefore this field just
    // reserves space for the flags, but the flags are actually the 4 bytes
    // immediately before [bytes], which may or may not be at the address of
    // the flags_spacing field.
    uint32_t flags_spacing;
    // The allocated data
    union {
        // This needs to match the alignment of any data type that would be
        // allocated by the hxcpp runtime.
        double align[0];
        char bytes[0];
    };
};


// Large allocations use this entry header to hold an additional size value
struct LargeEntry
{
    // Size of the large allocation
    uint32_t size;
    // Common Entry structure
    Entry entry;
};


template<int entrySizeC, int entriesPerBlockC, int allocatorFlagC>
class Block
{
public:

    void *operator new(std::size_t size)
    {
        return Calloc(size, 1);
    }

    void operator delete(void *ptr)
    {
        free(ptr);
    }

    Block()
    {
        for (int i = 0; i < (entriesPerBlockC - 1); i++) {
            mEntries[i].entry.next = &(mEntries[i + 1].entry);
        }
        mEntries[entriesPerBlockC - 1].entry.next = 0;
        mFree = &(mEntries[0].entry);
    }

    // Called only by the owning thread, so no need to do any locking
    inline void *Allocate(bool is_object)
    {
        if (mFree) {
            Entry *entry = mFree;
            mFree = entry->next;

            // Set its flags to indicate what block it was allocated from and
            // whether or not it is an object
            FLAGS_OF_ENTRY(entry) = (g_mark | allocatorFlagC |
                                     (is_object ? IS_OBJECT_FLAG : 0));

            return entry->bytes;
        }

        return 0;
    }

    // Called whan all threads are quiesced, so no need to do any locking
    inline bool Contains(Entry *entry)
    {
        uintptr_t e = (uintptr_t) entry;        
        uintptr_t begin = (uintptr_t) mEntries;

        // If it's outside of the range of block entries, it's not in the block
        if ((e < begin) ||
            (e > ((uintptr_t) &(mEntries[entriesPerBlockC - 1])))) {
            return false;
        }
        
        // It points within the block, now return whether or not it is
        // a valid entry - i.e. if it's aligned properly to be an
        // Entry.
        return ((((uintptr_t) (e - begin)) % sizeof(SizedEntry)) == 0);
    }

    // Called whan all threads are quiesced, so no need to do any locking
    // Returns the number of entries still used after the sweep
    uint32_t Sweep()
    {
        uint32_t used_count = 0;
        for (int i = 0; i < entriesPerBlockC; i++) {
            Entry *entry = (Entry *) &(mEntries[i]);
            uint32_t entry_mark = (FLAGS_OF_ENTRY(entry) & LAST_MARK_MASK);
            if (entry_mark == g_mark) {
                // Still in use
                used_count++;
#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
                debug_live(entry->bytes);
#endif
                continue;
            }
            if (entry_mark == 0) {
                // The entry is not allocated
                continue;
            }
            
            // It's newly freed, so clean it up
            if (FLAGS_OF_ENTRY(entry) & IS_WEAK_REF_FLAG) {
                // If it's a weak reference object itself, then remove it from
                // the list of weak references
                RemoveWeakRef((GCWeakRef *) (entry->bytes));
            }
            
#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
            debug_sweep(entry->bytes);
#endif
            
#ifdef HXCPP_M64
            if (entry->id) {
                g_id_map.erase(entry->id);
            }
#endif
            
            // Clear out its bytes, so that it is ready for re-use
            memset(entry, 0, sizeof(SizedEntry));
            // And put it on the free list
            entry->next = mFree;
            mFree = entry;
        }

        return used_count;
    }

    inline Block *GetNext()
    {
        return mNext;
    }

    inline void SetNext(Block *block)
    {
        mNext = block;
    }

private:

    struct SizedEntry
    {
        Entry entry;
        char sizing[entrySizeC];
    };

    Block *mNext;

    SizedEntry mEntries[entriesPerBlockC];
    Entry *mFree;
};


template<int entrySizeC, int entriesPerBlockC, int allocatorFlagC>
class BlockManager
{
public:

    typedef Block<entrySizeC, entriesPerBlockC, allocatorFlagC> SizedBlock;

    BlockManager()
    {
        pthread_mutex_init(&mMutex, 0);
    }

    ~BlockManager()
    {
        pthread_mutex_destroy(&mMutex);
    }

    // Potentially called by multiple threads at once, so needs to do locking
    SizedBlock *Recycle(SizedBlock *old)
    {
        pthread_mutex_lock(&mMutex);
        
        if (old) {
            old->SetNext(mFull);
            mFull = old;
        }

        if (mPartial) {
            SizedBlock *ret = mPartial;
            mPartial = mPartial->GetNext();
            pthread_mutex_unlock(&mMutex);
            return ret;
        }
        else {
            pthread_mutex_unlock(&mMutex);
            return new SizedBlock();
        }
    }

    // Potentially called while other threads re in Recycle, so needs
    // to do locking
    void Return(SizedBlock *old)
    {
        // Assume partial
        pthread_mutex_lock(&mMutex);
        old->SetNext(mPartial);
        mPartial = old;
        pthread_mutex_unlock(&mMutex);
    }

    // Called whan all threads are quiesced, so no need to do any locking
    inline bool Contains(Entry *entry)
    {
        return (Contains(mFull, entry) || Contains(mPartial, entry));
    }

    // Called whan all threads are quiesced, so no need to do any locking
    inline void Sweep()
    {
        // Sweep partial
        SizedBlock *block = mPartial;
        SizedBlock *prev = 0;

        while (block) {
            uint32_t used_count = block->Sweep();
            if (used_count == 0) {
                // Empty block, delete
                SizedBlock *next = block->GetNext();
                if (prev) {
                    prev->SetNext(next);
                }
                else {
                    mPartial = next;
                }
                delete block;
                block = next;
            }
            else {
                // Still partial, retain on partial list
                prev = block;
                block = block->GetNext();
            }
        }

        // Now sweep full
        block = mFull;
        prev = 0;

        while (block) {
            uint32_t used_count = block->Sweep();
            if (used_count == 0) {
                // Empty block
                SizedBlock *next = block->GetNext();
                if (prev) {
                    prev->SetNext(next);
                }
                else {
                    mFull = next;
                }
                delete block;
                block = next;
            }
            else if (used_count < entriesPerBlockC) {
                // Partial now, so move to partial list
                SizedBlock *next = block->GetNext();
                if (prev) {
                    prev->SetNext(next);
                }
                else {
                    mFull = next;
                }
                block->SetNext(mPartial);
                mPartial = block;
                block = next;
            }
            else {
                // Still full, retain on full list
                prev = block;
                block = block->GetNext();
            }
        }
    }

private:

    // Called whan all threads are quiesced, so no need to do any locking
    static inline bool Contains(SizedBlock *head, Entry *entry)
    {
        // Look in each block to see if it could be in there
        SizedBlock *block = head;
        while (block) {
            if (block->Contains(entry)) {
                return true;
            }
            block = block->GetNext();
        }
        return false;
    }

    pthread_mutex_t mMutex;
    
    SizedBlock *mFull;
    SizedBlock *mPartial;
};


class LargeAllocator
{
public:

    LargeAllocator()
    {
    }

    // Called only by the owning thread, so no locking is needed
    void *Allocate(int size, bool is_object)
    {
        LargeEntry *entry = (LargeEntry *) Calloc(sizeof(LargeEntry) + size, 1);
        
        if (!entry) {
            return 0;
        }

        entry->size = size;
        
        FLAGS_OF_ENTRY(&(entry->entry)) =
            g_mark | (is_object ? IS_OBJECT_FLAG : 0);

        AddLargeEntry(entry);

        return entry->entry.bytes;
    }

    // Called only when all threads are quiesced so no locking is needed
    bool Contains(LargeEntry *largeEntry)
    {
        LargeEntry *mapped = mLargeAllocs[MapLargeEntry(largeEntry)];
    
        while (mapped && (mapped != largeEntry)) {
            mapped = mapped->entry.largeNext;
        }

        return mapped;
    }

    // Called only when all threads are quiesced so no locking is needed
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
#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
                    debug_live(entry->entry.bytes);
#endif
                    replace = &(entry->entry.largeNext);
                    entry = *replace;
                    continue;
                }

                // It's newly freed, so clean it up
                if (FLAGS_OF_ENTRY(&(entry->entry)) & IS_WEAK_REF_FLAG) {
                    // If it's a weak reference object itself, then
                    // remove it from the list of weak references
                    RemoveWeakRef((GCWeakRef *) (entry->entry.bytes));
                }
            
#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
                debug_sweep(entry->entry.bytes);
#endif
                
#ifdef HXCPP_M64
                if (entry->entry.id) {
                    g_id_map.erase(entry->entry.id);
                }
#endif
                
                LargeEntry *to_free = entry;
                // Remove entry from mLargeAllocs
                *replace = entry->entry.largeNext;
                entry = *replace;
                
                // Free it
                free(to_free);
            }            
        }
    }

    void MergeInto(LargeAllocator &other)
    {
        for (int i = 0; i < (sizeof(mLargeAllocs) /
                             sizeof(mLargeAllocs[0])); i++) {
            if (mLargeAllocs[i]) {
                if (other.mLargeAllocs[i]) {
                    LargeEntry *tail = other.mLargeAllocs[i];
                    while (tail->entry.largeNext) {
                        tail = tail->entry.largeNext;
                    }
                    tail->entry.largeNext = mLargeAllocs[i];
                }
                else {
                    other.mLargeAllocs[i] = mLargeAllocs[i];
                }
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

    // Called only by the owning thread, so no locking is needed
    inline void AddLargeEntry(LargeEntry *entry)
    {
        int idx = MapLargeEntry(entry);

        entry->entry.largeNext = mLargeAllocs[idx];

        mLargeAllocs[idx] = entry;
    }

    LargeEntry *mLargeAllocs[37337];
};


#if USE_ALLOCATOR_0
static BlockManagerType(TIVOCONFIG_GC_ALLOC_0_SIZE,
                        IN_ALLOCATOR_0_FLAG) gBlockManager0;
#endif
#if USE_ALLOCATOR_1
static BlockManagerType(TIVOCONFIG_GC_ALLOC_1_SIZE,
                        IN_ALLOCATOR_1_FLAG) gBlockManager1;
#endif
#if USE_ALLOCATOR_2
static BlockManagerType(TIVOCONFIG_GC_ALLOC_2_SIZE,
                        IN_ALLOCATOR_2_FLAG) gBlockManager2;
#endif
#if USE_ALLOCATOR_3
static BlockManagerType(TIVOCONFIG_GC_ALLOC_3_SIZE,
                        IN_ALLOCATOR_3_FLAG) gBlockManager3;
#endif


class HaxeThread
{
public:

    void *operator new(std::size_t size)
    {
        return calloc(size, 1);
    }

    void operator delete(void *ptr)
    {
        free(ptr);
    }
    
    // Gets the HaxeThread instance for the current thread.  If
    // [createIfNecessary] is true, will create a new HaxeThread instance if
    // one does not exist for this thread.
    static inline HaxeThread *Get(bool createIfNecessary)
    {
        HaxeThread *ret = (HaxeThread *) pthread_getspecific(gInfoKey);

        if (!ret && createIfNecessary) {
            ret = new HaxeThread();
        }

        return ret;
    }
    
    HaxeThread()
    {
        pthread_mutex_lock(&gHeadMutex);
        
#if USE_ALLOCATOR_0
        mBlock0 = gBlockManager0.Recycle(0);
#endif
#if USE_ALLOCATOR_1
        mBlock1 = gBlockManager1.Recycle(0);
#endif
#if USE_ALLOCATOR_2
        mBlock2 = gBlockManager2.Recycle(0);
#endif
#if USE_ALLOCATOR_3
        mBlock3 = gBlockManager3.Recycle(0);
#endif

        if (!gHead) {
            GCLOG("Using GCPooledV3 ...\n");
#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
            g_zero_time_us = nowUs();
            debug_connect();
#endif
            // This is the first HaxeThread ... need to have the key
            pthread_key_create(&gInfoKey, 0);
            // And create the GC threads if necessary.  Create one less than
            // the total GC thread count because the haxe thread initiating GC
            // counts too.
#if (TIVOCONFIG_GC_THREAD_COUNT > 2)
            for (int i = 1; i < TIVOCONFIG_GC_THREAD_COUNT; i++) {
                pthread_t junk;
                pthread_create(&junk, 0, &GCTaskThreadMain, 0);
            }
#endif
        }

        // Store this as thread local data
        pthread_setspecific(gInfoKey, (void *) this);

        mNext = gHead;
        gHead = this;

        pthread_mutex_unlock(&gHeadMutex);
    }

    inline void Unregister()
    {
        mDead = true;

        this->SetTopOfStack(0);
    }

    // Returns true if the HaxeStack represents a haxe thread that is
    // currently running in haxe
    inline bool IsRunningInHaxe()
    {
        // Running in haxe if mStackTop is nonzero
        return mStackTop;
    }
    
    // If zero, this thread is no longer expected to use the GC until the
    // stack is re-set to a nonzero value
    inline void SetTopOfStack(int *top)
    {
        mStackTop = top;

        if (mStackTop) {
            this->Unquiesce();
        }
        else {
            this->Quiesce();
        }
    }

    inline void MaybePauseForCollect()
    {
        if (!g_gc_in_progress && g_gc_enabled && hx::gPauseForCollect) {
            // Quiesce the current thread, which will allow the garbage
            // collect to start
            this->Quiesce();
            // Now unquiesce, since this thread will continue running as a
            // haxe thread
            this->Unquiesce();
        }
    }

    // Takes this HaxeThread into a quiesced state, where it will not call
    // interact with the garbage collector at all, *except* to possibly create
    // a new haxe thread.
    inline void Quiesce()
    {
        // Update this thread's stack info in its global thread data so that
        // if the garbage collect thread actually starts a garbage collect
        // while we are off doing our thing, it will have our stack data to
        // mark.
        void *bottom;
        mStackBottom = &bottom;
        // Use setjmp to (hopefully) save registers
        (void) setjmp(mJmpbuf);

        // This thread is not an active thread until it calls Unquiesce
        pthread_mutex_lock(&gGcMutex);
        // If there are now zero live haxe threads, and if a GC is requested,
        // do it
        if ((--gActiveThreadCount == 0) && hx::gPauseForCollect) {
            PerformGC();
        }
        pthread_mutex_unlock(&gGcMutex);
    }

    // Takes a HaxeThread out of the quiesced state, and allows it to interact
    // with the garbage collector again.
    inline void Unquiesce()
    {
        // Cannot unquiesce while gPauseForCollect is true
        pthread_mutex_lock(&gGcMutex);
        while (hx::gPauseForCollect) {
            pthread_cond_wait(&gCollectDoneCond, &gGcMutex);
        }

        // Update the number of active threads.
        gActiveThreadCount++;
        
        pthread_mutex_unlock(&gGcMutex);
    }

    void *Allocate(size_t size, bool isObject)
    {
#if USE_ALLOCATOR_0
        if (size <= TIVOCONFIG_GC_ALLOC_0_SIZE) {
            void *ret = mBlock0->Allocate(isObject);
            if (!ret) {
                mBlock0 = gBlockManager0.Recycle(mBlock0);
                ret = mBlock0->Allocate(isObject);
            }
            return ret;
        }
#else
        if (false) {
        }
#endif
#if USE_ALLOCATOR_1
        else if (size <= TIVOCONFIG_GC_ALLOC_1_SIZE) {
            void *ret = mBlock1->Allocate(isObject);
            if (!ret) {
                mBlock1 = gBlockManager1.Recycle(mBlock1);
                ret = mBlock1->Allocate(isObject);
            }
            return ret;
        }
#endif
#if USE_ALLOCATOR_2
        else if (size <= TIVOCONFIG_GC_ALLOC_2_SIZE) {
            void *ret = mBlock2->Allocate(isObject);
            if (!ret) {
                mBlock2 = gBlockManager2.Recycle(mBlock2);
                ret = mBlock2->Allocate(isObject);
            }
            return ret;
        }
#endif
#if USE_ALLOCATOR_3
        else if (size <= TIVOCONFIG_GC_ALLOC_3_SIZE) {
            void *ret = mBlock3->Allocate(isObject);
            if (!ret) {
                mBlock3 = gBlockManager3.Recycle(mBlock3);
                ret = mBlock3->Allocate(isObject);
            }
            return ret;
        }
#endif
        else {
            return mLargeAllocator.Allocate(size, isObject);
        }
    }

    void *Reallocate(void *ptr, size_t new_size)
    {
        if (ptr == 0) {
            return hx::InternalNew(new_size, false);
        }
        
        // Get entry size
        uint32_t flags = FLAGS_OF_ENTRY(ENTRY_FROM_PTR(ptr));
#if USE_ALLOCATOR_0
        if (flags & IN_ALLOCATOR_0_FLAG) {
            return Reallocate(ptr, TIVOCONFIG_GC_ALLOC_0_SIZE, new_size);
        }
#else
        if (false) {
        }
#endif
#if USE_ALLOCATOR_1
        else if (flags & IN_ALLOCATOR_1_FLAG) {
            return Reallocate(ptr, TIVOCONFIG_GC_ALLOC_1_SIZE, new_size);
        }
#endif
#if USE_ALLOCATOR_2
        else if (flags & IN_ALLOCATOR_2_FLAG) {
            return Reallocate(ptr, TIVOCONFIG_GC_ALLOC_2_SIZE, new_size);
        }
#endif
#if USE_ALLOCATOR_3
        else if (flags & IN_ALLOCATOR_3_FLAG) {
            return Reallocate(ptr, TIVOCONFIG_GC_ALLOC_3_SIZE, new_size);
        }
#endif
        else {
            return Reallocate(ptr, LARGE_ENTRY_FROM_PTR(ptr)->size, new_size);
        }
    }

    void Mark()
    {
        // Mark stacks
        MarkPointers(mStackBottom, mStackTop);

        // Mark registers
        char *jmp = (char *) mJmpbuf;
        MarkPointers(jmp, jmp + sizeof(jmp_buf));
    }

    // Marks all pointers located in the range [start, end] (inclusive) with
    // the current g_mark value.  Only called during GC when a single thread
    // is doing collect and all other threads are quiesced, so no locking is
    // necessary.
    static void MarkPointers(void *start, void *end)
    {
        uintptr_t start_ptr = (uintptr_t) start;
        uintptr_t end_ptr = (uintptr_t) end;
        while (start_ptr < end_ptr) {
            void *ptr = * (void **) start_ptr;
            start_ptr += sizeof(uintptr_t);
            // Cannot possibly be a valid pointer if it doesn't have enough
            // room for an entry
            if (((uintptr_t) ptr) <= sizeof(Entry)) {
                continue;
            }
            Entry *entry = ENTRY_FROM_PTR(ptr);
            if (IsValidEntry(entry) &&
                (FLAGS_OF_ENTRY(entry) & LAST_MARK_MASK)) {
                if (FLAGS_OF_ENTRY(entry) & IS_OBJECT_FLAG) {
                    hx::MarkObjectAlloc((hx::Object *) ptr, 0);
                }
                else {
                    hx::MarkAlloc(ptr, 0);
                }
            }
        }
    }
    
    // Seep the thread's blocks.  Even empty ones are left owned by the Thread,
    // they are not deleted
    inline void Sweep()
    {
#if USE_ALLOCATOR_0
        mBlock0->Sweep();
#endif
#if USE_ALLOCATOR_1
        mBlock1->Sweep();
#endif
#if USE_ALLOCATOR_2
        mBlock2->Sweep();
#endif
#if USE_ALLOCATOR_3
        mBlock3->Sweep();
#endif
        mLargeAllocator.Sweep();
    }

private:

    static inline void *Reallocate(void *ptr, size_t old_size, size_t new_size)
    {
        // Attempt to re-use current entry
        if (new_size <= old_size) {
            // memset the no-longer-used part, in case we reallocate back into
            // it later, we want to see zeroed bytes there
            memset(&(((uint8_t *) ptr)[new_size]), 0, old_size - new_size);
            return ptr;
        }

        // Else, make a new allocation (cannot be an object as objects are
        // never re-allocated)
        void *new_ptr = hx::InternalNew(new_size, false);
        memcpy(new_ptr, ptr, old_size);
        return new_ptr;
    }

    static inline bool IsValidEntry(Entry *entry)
    {
#if USE_ALLOCATOR_0
        if (gBlockManager0.Contains(entry)) {
            return true;
        }
#endif
#if USE_ALLOCATOR_1
        if (gBlockManager1.Contains(entry)) {
            return true;
        }
#endif
#if USE_ALLOCATOR_2
        if (gBlockManager2.Contains(entry)) {
            return true;
        }
#endif
#if USE_ALLOCATOR_3
        if (gBlockManager3.Contains(entry)) {
            return true;
        }
#endif
        // Now look in individual thread blocks/allocators
        LargeEntry *largeEntry = LARGE_ENTRY_FROM_PTR(entry->bytes);
        
        HaxeThread *ht = gHead;
        while (ht) {
#if USE_ALLOCATOR_0
            if (ht->mBlock0->Contains(entry)) {
                return true;
            }
#endif
#if USE_ALLOCATOR_1
            if (ht->mBlock1->Contains(entry)) {
                return true;
            }
#endif
#if USE_ALLOCATOR_2
            if (ht->mBlock2->Contains(entry)) {
                return true;
            }
#endif
#if USE_ALLOCATOR_3
            if (ht->mBlock3->Contains(entry)) {
                return true;
            }
#endif
            if (ht->mLargeAllocator.Contains(largeEntry)) {
                return true;
            }
            ht = ht->mNext;
        }

        return false;
    }

    // Called when all haxe threads are quiesced and thus no new threads can
    // be added to the global thread list.  However, threads may become dead
    // while this function is running; however, if we miss dead threads
    // this time, that's OK because we'll get them next time
    static inline void CleanupDeadThreads()
    {
        HaxeThread *ht = gHead;
        HaxeThread *prev = 0;
        HaxeThread *live = 0;
            
        while (ht) {
            if (ht->mDead) {
                // - Clean up dead thread by merging it into the first
                //   available live thread
                if (!live) {
                    if (prev) {
                        live = prev;
                    }
                    else {
                        live = ht->mNext;
                        while (live && live->mDead) {
                            live = live->mNext;
                        }
                    }
                }
                if (live) {
                    ht->Cleanup(live);
                    if (prev) {
                        prev->mNext = ht->mNext;
                    }
                    else {
                        gHead = ht->mNext;
                    }
                    HaxeThread *next = ht->mNext;
                    delete ht;
                    ht = next;
                }
                else {
                    // Weird, all threads are dead
                    break;
                }
            }
            else {
                prev = ht;
                ht = ht->mNext;
            }
        }
    }

    void Cleanup(HaxeThread *live)
    {
#if USE_ALLOCATOR_0
        gBlockManager0.Return(mBlock0);
#endif
#if USE_ALLOCATOR_1
        gBlockManager1.Return(mBlock1);
#endif
#if USE_ALLOCATOR_2
        gBlockManager2.Return(mBlock2);
#endif
#if USE_ALLOCATOR_3
        gBlockManager3.Return(mBlock3);
#endif
        mLargeAllocator.MergeInto(live->mLargeAllocator);
    }

#if (TIVOCONFIG_GC_THREAD_COUNT == 1)
    
    static void PerformGC()
    {
        PLOG(PLOG_TYPE_STX_GC_BEGIN, 0, 0);
    
#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
        debug_before_gc();
        uint64_t start_us = nowUs();
#endif
        
        // Don't let any threads get created until the GC is done
        pthread_mutex_lock(&gHeadMutex);

        CleanupDeadThreads();

#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
        uint64_t sub_start_us = nowUs();
#endif

        PLOG(PLOG_TYPE_STX_GC_MARK_BEGIN, 0, 0);
    
        // Begin the garbage collect by updating the mark, so that
        // instantly everything is considered non-referenced until it is
        // found via mark
        g_mark = (g_mark == MARK_A_FLAG) ? MARK_B_FLAG : MARK_A_FLAG;

        // Mark the stacks and registers of each quiesced thread (the only
        // not-quiesced threads would be new threads that were created but
        // haven't even had a chance to do a quiesce yet, and should be
        // ignored for this run of the garbage collector).
        HaxeThread *ht = gHead;
        
        while (ht) {
            // Only if mStackBottom is set has this thread ever done any
            // quiesces; so if it's not set, it's a new thread that has
            // been created and hasn't even quiesced yet.  If it has a
            // stack top, then it is a live haxe thread and can be marked.
            if (ht->mStackBottom && ht->mStackTop) {
                ht->Mark();
            }
            ht = ht->mNext;
        }
        
        PLOG(PLOG_TYPE_STX_GC_MARK_STACKS_END, 0, 0);
    
        // Mark class statics
        hx::MarkClassStatics(0);

        PLOG(PLOG_TYPE_STX_GC_MARK_STATICS_END, 0, 0);
    
        // Mark roots
        MarkRoots();

        PLOG(PLOG_TYPE_STX_GC_MARK_ROOTS_END, 0, 0);
    
        // Mark weak hashes
        MarkWeakHashes();

#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
        uint32_t mark_us = nowUs() - sub_start_us;
#endif

        // Clear weak refs for those weakly referenced pointers that are no
        // longer reachable
        HandleWeakRefs();

        PLOG(PLOG_TYPE_STX_GC_MARK_END, 0, 0);

        // Run finalizers for all no longer referenced objects that have
        // finalizers.  Must set g_gc_in_progress to true while this is
        // occurring so that we don't recursively start a GC if a finalizer
        // tries to allocate memory.
        g_gc_in_progress = true;
        HandleFinalizers();
        g_gc_in_progress = false;

#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
        sub_start_us = nowUs();
#endif

        PLOG(PLOG_TYPE_STX_GC_SWEEP_BEGIN, 0, 0);
    
        // Sweep the block managers
#if USE_ALLOCATOR_0
        gBlockManager0.Sweep();
#endif
#if USE_ALLOCATOR_1
        gBlockManager1.Sweep();
#endif
#if USE_ALLOCATOR_2
        gBlockManager2.Sweep();
#endif
#if USE_ALLOCATOR_3
        gBlockManager3.Sweep();
#endif

        // Sweep HaxeThreads
        ht = gHead;
        while (ht) {
            ht->Sweep();
            ht = ht->mNext;
        }

        PLOG(PLOG_TYPE_STX_GC_SWEEP_END, 0, 0);

        PLOG(PLOG_TYPE_STX_GC_END, 0, 0);
    
#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
        uint32_t sweep_us += nowUs() - sub_start_us;
#endif

        // No longer touching the HaxeThread list so can release the lock now
        pthread_mutex_unlock(&gHeadMutex);
        
        // Update the global count of recent allocs, since there are now
        // globally zero allocs since this garbage collect.
        gRecentAllocs = 0;

#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
        debug_after_gc(mark_us, sweep_us, nowUs() - start_us);
#endif

        // Allow all haxe threads to wake up now that the mark is done.
        // They will do their own sweep after waking up.
        hx::gPauseForCollect = false;
        pthread_cond_broadcast(&gCollectDoneCond);
    }

#else // TIVOCONFIG_GC_THREAD_COUNT > 1

    // Types of mark task to perform
    enum MarkTaskType
    {
        MarkHaxeThreadTask,
        MarkClassStaticsTask,
        MarkRootTask
    };

    // Mark task to perform
    struct MarkTask
    {
        MarkTaskType type;
        union {
            // If type is MarkHaxeThread, this is the HaxeThread to mark
            HaxeThread *ht;
            // If type is MarkRoot, this is the root to mark
            hx::Object *obj;
        };
    };

    // Type of sweep task to perform
    enum SweepTaskType
    {
        SweepHaxeThreadTask,
        SweepBlockManagerTask
    };

    // Sweep task to perform
    struct SweepTask
    {
        SweepTaskType type;
        union {
            // If type is SweepHaxeThread, this is the HaxeThread to sweep
            HaxeThread *ht;
            // If type is SweepBlockManager, this is the number of the block
            // manager to sweep
            int bmn;
        };
    };

    static inline void PushMarkTask(MarkTask &mt)
    {
        pthread_mutex_lock(&gTaskMutex);
        gMarkTasks.push_back(mt);
        gOutstandingTaskCount++;
        pthread_cond_signal(&gTaskCond);
        pthread_mutex_unlock(&gTaskMutex);
    }

    static inline void PushSweepTask(SweepTask &st)
    {
        pthread_mutex_lock(&gTaskMutex);
        gSweepTasks.push_back(st);
        gOutstandingTaskCount++;
        pthread_cond_signal(&gTaskCond);
        pthread_mutex_unlock(&gTaskMutex);
    }

    static inline void RunMarkTask(MarkTask &mt)
    {
        switch (mt.type) {
        case MarkHaxeThreadTask:
            mt.ht->Mark();
            break;
        case MarkClassStaticsTask:
            hx::MarkClassStatics(0);
            break;
        case MarkRootTask:
            MarkObjectAlloc(mt.obj, 0);
            break;
        }
    }

    static inline void RunSweepTask(SweepTask &st)
    {
        switch (st.type) {
        case SweepHaxeThreadTask:
            st.ht->Sweep();
            break;
        case SweepBlockManagerTask:
            switch (st.bmn) {
#if USE_ALLOCATOR_0
            case 0:
                gBlockManager0.Sweep();
                break;
#endif
#if USE_ALLOCATOR_1
            case 1:
                gBlockManager1.Sweep();
                break;
#endif
#if USE_ALLOCATOR_2
            case 2:
                gBlockManager2.Sweep();
                break;
#endif
#if USE_ALLOCATOR_3
            case 3:
                gBlockManager3.Sweep();
                break;
#endif
            default:
                // Not possible, but avoid compiler error
                break;
            }
            break;
        }
    }

    static void PerformGC()
    {
        PLOG(PLOG_TYPE_STX_GC_BEGIN, 0, 0);
    
#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
        debug_before_gc();
        uint64_t start_us = nowUs();
#endif
        
        // Don't let any threads get created until the GC is done
        pthread_mutex_lock(&gHeadMutex);

        CleanupDeadThreads();

#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
        uint64_t sub_start_us = nowUs();
#endif
        
        PLOG(PLOG_TYPE_STX_GC_MARK_BEGIN, 0, 0);
    
        // Begin the garbage collect by updating the mark, so that
        // instantly everything is considered non-referenced until it is
        // found via mark
        g_mark = (g_mark == MARK_A_FLAG) ? MARK_B_FLAG : MARK_A_FLAG;

        // Build a list of mark and sweep tasks to do
        HaxeThread *ht = gHead;

        while (ht) {
            // Only if mStackBottom is set has this thread ever done any
            // quiesces; so if it's not set, it's a new thread that has
            // been created and hasn't even quiesced yet.  If it has a
            // stack top, then it is a live haxe thread and can be marked.
            if (ht->mStackBottom && ht->mStackTop) {
                MarkTask mt;
                mt.type = MarkHaxeThreadTask;
                mt.ht = ht;
                PushMarkTask(mt);
            }
            ht = ht->mNext;
        }

        // Also need to mark class statics
        {
            MarkTask mt;
            mt.type = MarkClassStaticsTask;
            PushMarkTask(mt);
        }

        // Also need to mark all roots
        Root *root = g_roots;
        while (root) {
            hx::Object **obj = root->pptr;
            if (obj && *obj) {
                MarkTask mt;
                mt.type = MarkRootTask;
                mt.obj = *obj;
                PushMarkTask(mt);
            }
            root = root->next;
        }

        // Now help perform mark tasks until they are done
        pthread_mutex_lock(&gTaskMutex);
        while (!gMarkTasks.empty()) {
            MarkTask mt = gMarkTasks.back();
            gMarkTasks.pop_back();
            pthread_mutex_unlock(&gTaskMutex);
            RunMarkTask(mt);
            pthread_mutex_lock(&gTaskMutex);
            gOutstandingTaskCount--;
        }
        // Now wait to ensure that all outstanding tasks are done
        while (gOutstandingTaskCount) {
            pthread_cond_wait(&gOutstandingTaskCountCond, &gTaskMutex);
        }
        pthread_mutex_unlock(&gTaskMutex);

        PLOG(PLOG_TYPE_STX_GC_MARK_STACKS_END, 0, 0);
        PLOG(PLOG_TYPE_STX_GC_MARK_STATICS_END, 0, 0);

        // Also need to mark weak hashes, must be done after all other
        // marks complete
        MarkWeakHashes();

        PLOG(PLOG_TYPE_STX_GC_MARK_ROOTS_END, 0, 0);

#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
        uint32_t mark_us = nowUs() - sub_start_us;
#endif
        
        // Clear weak refs for those weakly referenced pointers that are no
        // longer reachable
        HandleWeakRefs();

        PLOG(PLOG_TYPE_STX_GC_MARK_END, 0, 0);

        // Run finalizers for all no longer referenced objects that have
        // finalizers.  Must set g_gc_in_progress to true while this is
        // occurring so that we don't recursively start a GC if a finalizer
        // tries to allocate memory.
        g_gc_in_progress = true;
        HandleFinalizers();
        g_gc_in_progress = false;

#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
        sub_start_us = nowUs();
#endif

        PLOG(PLOG_TYPE_STX_GC_SWEEP_BEGIN, 0, 0);
        
        // All threads should be swept
        ht = gHead;
        while (ht) {
            SweepTask st;
            st.type = SweepHaxeThreadTask;
            st.ht = ht;
            ht = ht->mNext;
            PushSweepTask(st);
        }

        // And so should all block managers
#if USE_ALLOCATOR_0
        {
            SweepTask st;
            st.type = SweepBlockManagerTask;
            st.bmn = 0;
            PushSweepTask(st);
        }
#endif
#if USE_ALLOCATOR_1
        {
            SweepTask st;
            st.type = SweepBlockManagerTask;
            st.bmn = 1;
            PushSweepTask(st);
        }
#endif
#if USE_ALLOCATOR_2
        {
            SweepTask st;
            st.type = SweepBlockManagerTask;
            st.bmn = 2;
            PushSweepTask(st);
        }
#endif
#if USE_ALLOCATOR_3
        {
            SweepTask st;
            st.type = SweepBlockManagerTask;
            st.bmn = 3;
            PushSweepTask(st);
        }
#endif

        // Now help perform sweep tasks until they are done
        pthread_mutex_lock(&gTaskMutex);
        while (!gSweepTasks.empty()) {
            SweepTask st = gSweepTasks.back();
            gSweepTasks.pop_back();
            pthread_mutex_unlock(&gTaskMutex);
            RunSweepTask(st);
            pthread_mutex_lock(&gTaskMutex);
            gOutstandingTaskCount--;
        }
        // Now wait to ensure that all outstanding tasks are done
        while (gOutstandingTaskCount) {
            pthread_cond_wait(&gOutstandingTaskCountCond, &gTaskMutex);
        }
        pthread_mutex_unlock(&gTaskMutex);
        
        PLOG(PLOG_TYPE_STX_GC_SWEEP_END, 0, 0);
    
        PLOG(PLOG_TYPE_STX_GC_END, 0, 0);
    
#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
        uint32_t sweep_us = nowUs() - sub_start_us;
#endif
    
        // No longer touching the HaxeThread list so can release the lock now
        pthread_mutex_unlock(&gHeadMutex);
        
        // Update the global count of recent allocs, since there are now
        // globally zero allocs since this garbage collect.
        gRecentAllocs = 0;

#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
        debug_after_gc(mark_us, sweep_us, nowUs() - start_us);
#endif
        
        // Allow all haxe threads to wake up now that the mark is done.
        // They will do their own sweep after waking up.
        hx::gPauseForCollect = false;
        pthread_cond_broadcast(&gCollectDoneCond);
    }

    static void *GCTaskThreadMain(void *)
    {
        pthread_mutex_lock(&gTaskMutex);

        while (true) {
            if (!gMarkTasks.empty()) {
                MarkTask mt = gMarkTasks.back();
                gMarkTasks.pop_back();
                pthread_mutex_unlock(&gTaskMutex);
                RunMarkTask(mt);
                pthread_mutex_lock(&gTaskMutex);
                if (--gOutstandingTaskCount == 0) {
                    pthread_cond_signal(&gOutstandingTaskCountCond);
                }
            }
            else if (!gSweepTasks.empty()) {
                SweepTask st = gSweepTasks.back();
                gSweepTasks.pop_back();
                pthread_mutex_unlock(&gTaskMutex);
                RunSweepTask(st);
                pthread_mutex_lock(&gTaskMutex);
                if (--gOutstandingTaskCount == 0) {
                    pthread_cond_signal(&gOutstandingTaskCountCond);
                }
            }
            else {
                pthread_cond_wait(&gTaskCond, &gTaskMutex);
            }
        }
    }
    
#endif

    // Puts this HaxeThread on the global list of all HaxeThreads
    HaxeThread *mNext;

    // True only when this thread is dead
    bool mDead;

    // Each thread sets these before quiescing
    void *mStackTop; // Stack starts here and grows downwards
    void *mStackBottom; // Stack stops here
    jmp_buf mJmpbuf; // Saves registers

#if USE_ALLOCATOR_0
    BlockType(TIVOCONFIG_GC_ALLOC_0_SIZE, IN_ALLOCATOR_0_FLAG) *mBlock0;
#endif
#if USE_ALLOCATOR_1
    BlockType(TIVOCONFIG_GC_ALLOC_1_SIZE, IN_ALLOCATOR_1_FLAG) *mBlock1;
#endif
#if USE_ALLOCATOR_2
    BlockType(TIVOCONFIG_GC_ALLOC_2_SIZE, IN_ALLOCATOR_2_FLAG) *mBlock2;
#endif
#if USE_ALLOCATOR_3
    BlockType(TIVOCONFIG_GC_ALLOC_3_SIZE, IN_ALLOCATOR_3_FLAG) *mBlock3;
#endif
    LargeAllocator mLargeAllocator;

    // The pthread key that is used to look up an instance of ThreadGlobalData
    // for each thread
    static pthread_key_t gInfoKey;

    // The head of the list of thread global data; this list encompasses all
    // haxe threads and thus has all haxe allocations in it
    static HaxeThread *gHead;
    
    // Count of active threads that are currenty not quiesced
    static uint32_t gActiveThreadCount;

    // This mutex protects gHead as multiple threads may add/remove thread
    // global data concurrently
    static pthread_mutex_t gHeadMutex;
    static pthread_mutex_t gGcMutex;
    static pthread_cond_t gCollectDoneCond;
    
#if (TIVOCONFIG_GC_THREAD_COUNT > 1)
    // Stack of mark tasks to perform
    static std::list<MarkTask> gMarkTasks;
    // Stack of sweep tasks to perform
    static std::list<SweepTask> gSweepTasks;
    // Number of outstanding tasks
    static uint32_t gOutstandingTaskCount;
    // Mutex protecting these
    static pthread_mutex_t gTaskMutex;
    // Condition variable signalling when there is a task to run
    static pthread_cond_t gTaskCond;
    // Condition variable signalling when gOutstandingTaskCount is set to 0
    // by a GC task thread
    static pthread_cond_t gOutstandingTaskCountCond;
#endif
};


/**** Global variables  **************************************************** */

// This is used internally by this code to indicate when "quiesce" should
// occur, as well as being used by external code to test for "quiesce" needing
// to occur
int hx::gPauseForCollect;


/**** Static variables  **************************************************** */

// This is used to protect against concurrent access to certain globals
// shared amongst all threads, and to signal when threading events occur.
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;

// This maps allocations to the weak refs to them.  Protected by g_lock.
static hx::UnorderedMap<void *, std::list<GCWeakRef *> > g_weak_ref_map;

// This maps objects to finalizer data - it is expected that very few
// finalizers will be used
static hx::UnorderedMap<hx::Object *, FinalizerData> g_finalizers;

// Hxcpp includes a "weak hash" class that needs GC-side support
static std::list<hx::HashBase<Dynamic> *> g_weak_hash_list;

// Statics of HaxeThread
pthread_key_t HaxeThread::gInfoKey;
HaxeThread *HaxeThread::gHead;
uint32_t HaxeThread::gActiveThreadCount;
pthread_mutex_t HaxeThread::gHeadMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t HaxeThread::gGcMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t HaxeThread::gCollectDoneCond = PTHREAD_COND_INITIALIZER;
#if (TIVOCONFIG_GC_THREAD_COUNT > 1)
std::list<HaxeThread::MarkTask> HaxeThread::gMarkTasks;
std::list<HaxeThread::SweepTask> HaxeThread::gSweepTasks;
uint32_t HaxeThread::gOutstandingTaskCount;
pthread_mutex_t HaxeThread::gTaskMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t HaxeThread::gTaskCond = PTHREAD_COND_INITIALIZER;
pthread_cond_t HaxeThread::gOutstandingTaskCountCond = PTHREAD_COND_INITIALIZER;
#endif



/**** Static helper functions ********************************************** */

static inline void Lock()
{
    pthread_mutex_lock(&g_lock);
}


static inline void Unlock()
{
    pthread_mutex_unlock(&g_lock);
}


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
        bool alreadyExists = (g_finalizers.count(obj) > 0);
        FinalizerData &fd = g_finalizers[obj];
        if (alreadyExists && (fd.type == FinalizerTypeInternalFinalizer)) {
            delete g_finalizers[obj].internal_finalizer;
        }
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
    }
    else {
        if (g_finalizers.count(obj) > 0) {
            FinalizerData &fd = g_finalizers[obj];
            if (fd.type == FinalizerTypeInternalFinalizer) {
                delete g_finalizers[obj].internal_finalizer;
            }
            g_finalizers.erase(obj);
        }
    }

    Unlock();
}


// Removes the finalizer from the set of globally tracked finalizers and
// runs it
static void RunFinalizer(hx::Object *obj)
{
    // Take a copy of finalizer data so that it can be destroyed before
    // actually calling the finalizer in case the finalizer does something
    // crazy like attempt to delete the finalizer itself
    FinalizerData fd = g_finalizers[obj];
    g_finalizers.erase(obj);
    
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


// Only called during a mark, when the calling thread has quiesced all other
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

        std::list<GCWeakRef *> &refs = iter->second;
        std::list<GCWeakRef *>::iterator iter2 = refs.begin();
        while (iter2 != refs.end()) {
            (*iter2++)->mRef.mPtr = 0;
        }
        iter = g_weak_ref_map.erase(iter);
    }
}


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


static void HandleFinalizers()
{
    // First collect all finalizers to run.  Necessary because a finalizer
    // callback may itself modify the g_finalizers list.
    std::list<hx::Object *> torun;

    {
        hx::UnorderedMap<hx::Object *, FinalizerData>::iterator it =
            g_finalizers.begin();
        while (it != g_finalizers.end()) {
            if ((FLAGS_OF_ENTRY(ENTRY_FROM_PTR(it->first)) & LAST_MARK_MASK)
                != g_mark) {
                // It's not used anymore, so run its finalizer.
                torun.push_back(it->first);
            }
            it++;
        }
    }

    {
        std::list<hx::Object *>::iterator it = torun.begin();
        while (it != torun.end()) {
            RunFinalizer(*it); // Removes *it from g_finalizers
            it++;
        }
    }
}


static void MarkRoots()
{
    // Mark roots and everything reachable from them
    Root *root = g_roots;
    while (root) {
        hx::Object **obj = root->pptr;
        if (obj && *obj) {
            MarkObjectAlloc(*obj, 0);
        }
        root = root->next;
    }
}


static void MarkWeakHashes()
{
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
}


static void *Calloc(size_t nmemb, uint32_t size)
{
    void *ret = calloc(nmemb, size);

    if (!ret && !g_gc_in_progress && g_gc_enabled) {
        hx::InternalCollect(true, true);
        ret = calloc(nmemb, size);
    }

    return ret;
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


// Haxe threads call this periodically to possibly pause to wait for a GC to
// occur
void hx::PauseForCollect()
{
    HaxeThread::Get(true)->MaybePauseForCollect();
}


// Called when a haxe thread is entering an area of code that will not
// interact with the Haxe GC and may block indefinitely
void hx::EnterGCFreeZone()
{
    HaxeThread::Get(true)->Quiesce();
}


// Called when a haxe thread previously in an inactive area (promising not to
// call any GC functions and possibly blocking) is now going to start being
// active (using the GC) again
void hx::ExitGCFreeZone()
{
    HaxeThread::Get(true)->Unquiesce();
}


void hx::MarkAlloc(void *ptr, hx::MarkContext *ref)
{
#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
    debug_mark(ptr, (void *) ref);
#endif
    
    Entry *entry = ENTRY_FROM_PTR(ptr);

    FLAGS_OF_ENTRY(entry) =
        ((FLAGS_OF_ENTRY(entry) & ~LAST_MARK_MASK) | g_mark);
}


void hx::MarkObjectAlloc(hx::Object *obj, hx::MarkContext *ref)
{
#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
    debug_mark((void *) obj, (void *) ref);
#endif
        
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
#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
    obj->__Mark((hx::MarkContext *) obj);
#else
    obj->__Mark(0);
#endif
}


void hx::MarkObjectArray(hx::Object **inPtr, int inLength, hx::MarkContext *ref)
{
    for (int i = 0; i < inLength; i++) {
        if (inPtr[i]) {
#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
            MarkObjectAlloc(inPtr[i], ref);
#else
            MarkObjectAlloc(inPtr[i], 0);
#endif
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
    HaxeThread::MarkPointers(begin, end);
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
    HaxeThread *ht = HaxeThread::Get(false);

    // Is a haxe thread only if its stack top is nonzero ... because
    // if it is zero, then it has previously been a haxe thread in the
    // past, but is not one now (i.e. it was something like a Java thread
    // that became a haxe thread, thus accumulating a HaxeThread instance
    // in its pthread local storage, but later left Haxe and returned to
    // Java, retaining the HaxeThread instance but setting its top of
    // stack to zero in the process)
    return (ht && ht->IsRunningInHaxe());
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
    HaxeThread::Get(true)->SetTopOfStack(stack_top);
}


void hx::UnregisterCurrentThread()
{
    HaxeThread::Get(false)->Unregister();
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

    return false;
}


int hx::InternalCollect(bool, bool)
{
    hx::gPauseForCollect = true;
    HaxeThread::Get(true)->MaybePauseForCollect();
}


void hx::InternalEnableGC(bool inEnable)
{
    g_gc_enabled = inEnable;
}


void *hx::InternalNew(int inSize, bool inIsObject)
{
    gRecentAllocs += inSize;
    if (!g_gc_in_progress && g_gc_enabled &&
        (gRecentAllocs >= TIVOCONFIG_GC_MIN_COLLECT_SIZE)) {
        hx::gPauseForCollect = true;
        hx::PauseForCollect();
    }

    void *ret = (HaxeThread::Get(true))->Allocate(inSize, inIsObject);
    
#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
    if (ret) {
        debug_alloc(ret, inSize);
    }
#endif
    
    return ret;
}


void *hx::InternalRealloc(void *ptr, int new_size)
{
    void *ret = (HaxeThread::Get(true))->Reallocate(ptr, new_size);

#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
    debug_alloc(ret, new_size);
#endif
    
    return ret;
}


void *hx::Object::operator new(size_t inSize, bool inIsObject, const char *)
{
    void *ret = (HaxeThread::Get(true))->Allocate(inSize, inIsObject);
    
#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING
    if (ret) {
        debug_alloc(ret, inSize);
    }
#endif
    
    return ret;
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


// This function is called from other parts of the hxcpp code base whenever a
// unique identifier for a pointer is required.
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
    // this issue is to make __hxcpp_obj_id return a uintptr_t and just use
    // the pointer itself, as is done for 32 bit systems.  But this would
    // require plumbing 64 bit values up through to Haxe ...
    // Another solution would be to set a flag after wrapping g_next_id back
    // to 1, and if that flag is set, assume that the id might already have
    // been assigned so keep iterating on g_next_id until a value not found in
    // g_id_map is identified.  Things would get slower after wrapping the
    // ids, but at least they would still work.
    Entry *entry = ENTRY_FROM_PTR(ptr);
    if (entry->id == 0) {
        Lock();
        entry->id = g_next_id++;
        g_id_map[entry->id] = ptr;
        Unlock();
    }
    return entry->id;
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


extern "C" void hxcpp_set_top_of_stack()
{
   int i = 0;
   hx::SetTopOfStack(&i, false);
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
    HaxeThread::Get(true)->MaybePauseForCollect();
}


int __hxcpp_gc_used_bytes()
{
    return __hxcpp_gc_mem_info(0);
}


int __hxcpp_gc_mem_info(int which)
{
    switch (which) {
    case 1:
        //   MEM_INFO_RESERVED - memory allocated for possible use
        return 0;
    case 2:
        //   MEM_INFO_CURRENT - memory in use, includes uncollected garbage.
        //     This will generally saw-tooth between USAGE and RESERVED
        return 0;
    case 3:
        //   MEM_INFO_LARGE - Size of separate pool used for large allocs.
        //   Included in all the above.
        return 0;
    default:
        //   MEM_INFO_USAGE - estimate of how much is needed by program (at
        //   last collect)
        return 0;
    }
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
    // This implementation doesn't do anything here
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


#ifdef TIVOCONFIG_GC_ENABLE_DEBUGGING

static bool debug_write(char *buf, uint32_t size);
    
#if (TIVOCONFIG_GC_THREAD_COUNT > 1)
static pthread_mutex_t g_write_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

struct AllocDetails
{
    // Offset in ms from zero time of the allocation
    uint32_t timeOffsetMs;

    // Size of the allocation
    uint32_t size;

    // Backtrace of the allocation
    int frame_count;
    uintptr_t frames[33];

    void dump(void *ptr, int socket)
    {
        // Look up the vtable
        uintptr_t vtable = (uintptr_t) (((void **) ptr)[0]);
        if (vtable != 0) {
            vtable -= g_vtable_offset;
        }
        char buf[1 + sizeof(void *) + 4 + 4 + sizeof(void *) +
                 1 + (33 * sizeof(void *))];
        // buf[0] = Message Type = 3 (Alloc Record)
        // buf[next] = ptr;
        // buf[next] = timeOffsetMs
        // buf[next] = size
        // buf[next] = vtable
        // buf[next_char] = frame_count
        // buf[next...] = frames
        // buf[next] = ref_count
        // buf[next...] = refs
        int index = 0;
        buf[index++] = 3;
        if (sizeof(void *) == 8) {
            buf[index++] = (((uint64_t) ptr) >> 56) & 0xFF;
            buf[index++] = (((uint64_t) ptr) >> 48) & 0xFF;
            buf[index++] = (((uint64_t) ptr) >> 40) & 0xFF;
            buf[index++] = (((uint64_t) ptr) >> 32) & 0xFF;
            buf[index++] = (((uint64_t) ptr) >> 24) & 0xFF;
            buf[index++] = (((uint64_t) ptr) >> 16) & 0xFF;
            buf[index++] = (((uint64_t) ptr) >>  8) & 0xFF;
            buf[index++] = (((uint64_t) ptr) >>  0) & 0xFF;
        }
        else {
            buf[index++] = (((uint32_t) ptr) >> 24) & 0xFF;
            buf[index++] = (((uint32_t) ptr) >> 16) & 0xFF;
            buf[index++] = (((uint32_t) ptr) >>  8) & 0xFF;
            buf[index++] = (((uint32_t) ptr) >>  0) & 0xFF;
        }
        buf[index++] = (timeOffsetMs >> 24) & 0xFF;
        buf[index++] = (timeOffsetMs >> 16) & 0xFF;
        buf[index++] = (timeOffsetMs >>  8) & 0xFF;
        buf[index++] = (timeOffsetMs >>  0) & 0xFF;
        buf[index++] = (size >> 24) & 0xFF;
        buf[index++] = (size >> 16) & 0xFF;
        buf[index++] = (size >>  8) & 0xFF;
        buf[index++] = (size >>  0) & 0xFF;
        if (sizeof(void *) == 8) {
            buf[index++] = (((uint64_t) vtable) >> 56) & 0xFF;
            buf[index++] = (((uint64_t) vtable) >> 48) & 0xFF;
            buf[index++] = (((uint64_t) vtable) >> 40) & 0xFF;
            buf[index++] = (((uint64_t) vtable) >> 32) & 0xFF;
            buf[index++] = (((uint64_t) vtable) >> 24) & 0xFF;
            buf[index++] = (((uint64_t) vtable) >> 16) & 0xFF;
            buf[index++] = (((uint64_t) vtable) >>  8) & 0xFF;
            buf[index++] = (((uint64_t) vtable) >>  0) & 0xFF;
        }
        else {
            buf[index++] = (((uint32_t) vtable) >> 24) & 0xFF;
            buf[index++] = (((uint32_t) vtable) >> 16) & 0xFF;
            buf[index++] = (((uint32_t) vtable) >>  8) & 0xFF;
            buf[index++] = (((uint32_t) vtable) >>  0) & 0xFF;
        }
        uint8_t fc = (frame_count > 2) ? (frame_count - 2) : 0;
        buf[index++] = fc;
        for (int i = 2; i < frame_count; i++) {
            uintptr_t f = frames[i] - g_vtable_offset;
            if (sizeof(void *) == 8) {
                buf[index++] = (((uint64_t) f) >> 56) & 0xFF;
                buf[index++] = (((uint64_t) f) >> 48) & 0xFF;
                buf[index++] = (((uint64_t) f) >> 40) & 0xFF;
                buf[index++] = (((uint64_t) f) >> 32) & 0xFF;
                buf[index++] = (((uint64_t) f) >> 24) & 0xFF;
                buf[index++] = (((uint64_t) f) >> 16) & 0xFF;
                buf[index++] = (((uint64_t) f) >>  8) & 0xFF;
                buf[index++] = (((uint64_t) f) >>  0) & 0xFF;
            }
            else {
                buf[index++] = (((uint32_t) f) >> 24) & 0xFF;
                buf[index++] = (((uint32_t) f) >> 16) & 0xFF;
                buf[index++] = (((uint32_t) f) >>  8) & 0xFF;
                buf[index++] = (((uint32_t) f) >>  0) & 0xFF;
            }
        }

        // Dump is only called while already locked so don't lock
        debug_write(buf, index);
    }
};

static bool g_dump_on_gc;
static int g_debug_socket = -1;
static hx::UnorderedMap<void *, AllocDetails> g_details;

// These are loaded once.  They are just all of the ranges of mapped memory
// regions of the process.
struct Mapping
{
    uintptr_t begin, end;
};

static bool debug_write(char *buf, uint32_t size)
{
    int written = send(g_debug_socket, buf, size, MSG_NOSIGNAL);
    if ((written < 0) || (written < size)) {
        GCLOG("Short write to GC debug socket, terminating GC debugging\n");
        close(g_debug_socket);
        g_debug_socket = -1;
        return false;
    }
    return true;
}

static uintptr_t readAddress(const char *&c)
{
    uintptr_t ret = 0;
    
    while (true) {
        if ((*c >= 'a') && (*c <= 'f')) {
            ret *= 16;
            ret += (*c++ - 'a') + 10;
        }
        else if ((*c >= 'A') && (*c <= 'F')) {
            ret *= 16;
            ret += (*c++ - 'A') + 10;
        }
        else if ((*c >= '0') && (*c <= '9')) {
            ret *= 16;
            ret += *c++ - '0';
        }
        else {
            return ret;
        }
    }
}


// Used to detect vtable offset
class VtableOffsetCalculator
{
public:

    VtableOffsetCalculator()
    {
    }

    virtual void virtual_method()
    {
    }
};


static void debug_connect()
{
    int port = 2826;

    FILE *f;
    
#ifdef ANDROID
    char host[1024];
    f = fopen("/sdcard/TIVOCONFIG_GC_DEBUGGING_HOST", "r");
    if (f) {
        fgets(host, sizeof(host), f);
        fclose(f);
        int len = strlen(host);
        if (host[len - 1] == '\n') {
            host[len - 1] = 0;
        }
    }
    else {
        host[0] = 0;
    }
    char portstr[1024];
    f = fopen("/sdcard/TIVOCONFIG_GC_DEBUGGING_PORT", "r");
    if (f) {
        fgets(portstr, sizeof(portstr), f);
        fclose(f);
        int len = strlen(portstr);
        if (portstr[len - 1] == '\n') {
            portstr[len - 1] = 0;
        }
    }
    else {
        portstr[0] = 0;
    }
#else
    const char *host = getenv("TIVOCONFIG_GC_DEBUGGING_HOST");
    const char *portstr = getenv("TIVOCONFIG_GC_DEBUGGING_PORT");
#endif
    
    if (!host || !host[0]) {
        return;
    }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    
    struct hostent *hent = gethostbyname(host);
    if (hent) {
        memcpy(&(sin.sin_addr.s_addr), hent->h_addr, hent->h_length);
    }
    else {
        // Well, Android apparently doesn't really support gethostbyname, so
        // maybe we're lucky and it's a dot-quad
        in_addr inaddr;
        if (!inet_aton(host, &inaddr)) {
            GCLOG("Failed to look up GC debug host %s\n", host);
            g_debug_socket = -1;
            return;
        }
        memcpy(&(sin.sin_addr.s_addr), &inaddr, sizeof(inaddr));
    }
        
    if (portstr && portstr[0]) {
        port = atoi(portstr);
    }
    
    sin.sin_port = htons(port);
    
    g_debug_socket = socket(AF_INET, SOCK_STREAM, 0);

    if (g_debug_socket < 0) {
        GCLOG("Failed to create GC debug socket\n");
        return;
    }

    GCLOG("Connecting GC debug socket to %s:%d\n", host, port);
    if (connect(g_debug_socket, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
        GCLOG("Failed to connect GC debug socket\n");
        close(g_debug_socket);
        g_debug_socket = -1;
        return;
    }
    GCLOG("Connected GC debug socket\n");

    // Get the maps
    f = fopen("/proc/self/maps", "r");
    if (f) {
        std::map<std::string, Mapping> mapped;
        unsigned int begin, end;
        char binary[1024];
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "%x-%x %*s %*s %*s %*s %1023s",
                       &begin, &end, binary) != 3) {
                continue;
            }
            std::string bin = binary;
            if (mapped.count(bin) == 0) {
                Mapping &map = mapped[bin];
                map.begin = begin;
                map.end = end;
            }
            else {
                Mapping &map = mapped[bin];
                if (begin < map.begin) {
                    map.begin = begin;
                }
                if (end > map.end) {
                    map.end = end;
                }
            }
        }
        // Now detect where vtables are coming from ... a shared object
        // library or not?
        VtableOffsetCalculator calc;
        uintptr_t vtable = (uintptr_t) (((void **) &calc)[0]);
        std::map<std::string, Mapping>::iterator it = mapped.begin();
        while (it != mapped.end()) {
            if ((vtable >= it->second.begin) && (vtable < it->second.end)) {
                const char *val = it->first.c_str();
                int len = it->first.length();
                // Hokey!
                if ((val[len - 1] == 'o') && (val[len - 2] == 's') &&
                    (val[len - 3] == '.')) {
                    g_vtable_offset = it->second.begin;
                }
                break;
            }
            it++;
        }
        fclose(f);
    }

    // Now send some details
    // buf[0] = Message Type = 0 (GC Descriptor)
    // buf[1] = 0 for 32-bit, 1 for 64-bit
    char buf[2];
    int index = 0;
    buf[index++] = 0;
    buf[index++] = (sizeof(void *) == 8) ? 1 : 0;

    debug_write(buf, index);
}


#ifdef ANDROID
typedef struct BacktraceState
{
    uintptr_t *current;
    uintptr_t *end;
} BacktraceState;


static _Unwind_Reason_Code unwind_callback(struct _Unwind_Context *context,
                                           void *data)
{
    BacktraceState *state = (BacktraceState *) data;
    
    uintptr_t pc = _Unwind_GetIP(context);
    
    if (pc) {
        if (state->current >= state->end) {
            return _URC_END_OF_STACK;
        }
        else {
            *state->current++ = pc;
        }
    }
    
    return _URC_NO_REASON;
}

static uint32_t backtrace(void *vbuffer, int max)
{
    uintptr_t *buffer = (uintptr_t *) vbuffer;
    
    BacktraceState state = { buffer, buffer + max };
    
    _Unwind_Backtrace(unwind_callback, &state);

    return (state.current - buffer);
}
#endif


static void debug_alloc(void *ptr, uint32_t size)
{
    uint32_t timeOffsetMs = (nowUs() - g_zero_time_us) / 1000;

#if (TIVOCONFIG_GC_THREAD_COUNT > 1)
    pthread_mutex_lock(&g_write_lock);
#endif
    
    AllocDetails &ad = g_details[ptr];

#if (TIVOCONFIG_GC_THREAD_COUNT > 1)
    pthread_mutex_unlock(&g_write_lock);
#endif
    
    ad.timeOffsetMs = timeOffsetMs;

    ad.size = size;

    ad.frame_count = backtrace
        (ad.frames, (sizeof(ad.frames) / sizeof(ad.frames[0])));

#if (TIVOCONFIG_GC_THREAD_COUNT > 1)
    pthread_mutex_lock(&g_write_lock);
#endif
    
    if (++g_alloc_count == 100) {
        if (g_debug_socket != -1) {
            char msg = 1; // Message Type = 1 (GC Allocs)
            debug_write(&msg, 1);
        }
        g_alloc_count = 0;
    }
    
#if (TIVOCONFIG_GC_THREAD_COUNT > 1)
    pthread_mutex_unlock(&g_write_lock);
#endif
}


static void debug_before_gc()
{
    g_dump_on_gc = false;
    
    if (g_debug_socket == -1) {
        debug_connect();
        if (g_debug_socket == -1) {
            return;
        }
    }

    // Try to read a byte (nonblocking)
    char buf;
    int recvd = recv(g_debug_socket, &buf, 1, MSG_DONTWAIT);
    if (recvd < 0) {
        if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
            recvd = 0;
        }
        else {
            GCLOG("Failed recv on GC debug socket, terminating GC debugging");
            close(g_debug_socket);
            g_debug_socket = -1;
            return;
        }
    }

    g_dump_on_gc = (recvd != 0);
}


static void debug_mark(void *ptr, void *ref)
{
    if (!g_dump_on_gc) {
        return;
    }

    // report the ref
    char buf[1 + (2 * 8)];

    // buf[0] = Message Type = 2 (Mark)
    // buf[sizeof(void *)] = ptr
    // buf[sizeof(void *)] = ref
    int index = 0;
    buf[index++] = 2;
    if (sizeof(void *) == 8) {
        buf[index++] = (((uint64_t) ptr) >> 56) & 0xFF;
        buf[index++] = (((uint64_t) ptr) >> 48) & 0xFF;
        buf[index++] = (((uint64_t) ptr) >> 40) & 0xFF;
        buf[index++] = (((uint64_t) ptr) >> 32) & 0xFF;
        buf[index++] = (((uint64_t) ptr) >> 24) & 0xFF;
        buf[index++] = (((uint64_t) ptr) >> 16) & 0xFF;
        buf[index++] = (((uint64_t) ptr) >>  8) & 0xFF;
        buf[index++] = (((uint64_t) ptr) >>  0) & 0xFF;
        buf[index++] = (((uint64_t) ref) >> 56) & 0xFF;
        buf[index++] = (((uint64_t) ref) >> 48) & 0xFF;
        buf[index++] = (((uint64_t) ref) >> 40) & 0xFF;
        buf[index++] = (((uint64_t) ref) >> 32) & 0xFF;
        buf[index++] = (((uint64_t) ref) >> 24) & 0xFF;
        buf[index++] = (((uint64_t) ref) >> 16) & 0xFF;
        buf[index++] = (((uint64_t) ref) >>  8) & 0xFF;
        buf[index++] = (((uint64_t) ref) >>  0) & 0xFF;
        
    }
    else {
        buf[index++] = (((uint32_t) ptr) >> 24) & 0xFF;
        buf[index++] = (((uint32_t) ptr) >> 16) & 0xFF;
        buf[index++] = (((uint32_t) ptr) >>  8) & 0xFF;
        buf[index++] = (((uint32_t) ptr) >>  0) & 0xFF;
        buf[index++] = (((uint32_t) ref) >> 24) & 0xFF;
        buf[index++] = (((uint32_t) ref) >> 16) & 0xFF;
        buf[index++] = (((uint32_t) ref) >>  8) & 0xFF;
        buf[index++] = (((uint32_t) ref) >>  0) & 0xFF;
    }
    
#if (TIVOCONFIG_GC_THREAD_COUNT > 1)
    pthread_mutex_lock(&g_write_lock);
#endif
    
    debug_write(buf, index);

#if (TIVOCONFIG_GC_THREAD_COUNT > 1)
    pthread_mutex_unlock(&g_write_lock);
#endif
}


static void debug_sweep(void *ptr)
{
#if (TIVOCONFIG_GC_THREAD_COUNT > 1)
    pthread_mutex_lock(&g_write_lock);
#endif
    
    g_details.erase(ptr);
    
#if (TIVOCONFIG_GC_THREAD_COUNT > 1)
    pthread_mutex_unlock(&g_write_lock);
#endif
}


static void debug_live(void *ptr)
{
    if (!g_dump_on_gc) {
        return;
    }
    
    // Send an allocation record to the debug client
#if (TIVOCONFIG_GC_THREAD_COUNT > 1)
    pthread_mutex_lock(&g_write_lock);
#endif
    
    g_details[ptr].dump(ptr, g_debug_socket);
    
#if (TIVOCONFIG_GC_THREAD_COUNT > 1)
    pthread_mutex_unlock(&g_write_lock);
#endif
}


static void debug_after_gc(uint32_t mark_us, uint32_t sweep_us,
                           uint32_t total_us)
{
    if (g_debug_socket == -1) {
        return;
    }

    // Send an "end GC" message
    char buf[1 + (3 * 4)];
    // buf[0] = Message Type = 4 (End GC)
    // buf[1-4] = mark_us
    // buf[5-8] = sweep_us
    // buf[9-12] = total_us
    int index = 0;
    buf[index++] = 4;
    buf[index++] = ((mark_us >> 24) & 0xFF);
    buf[index++] = ((mark_us >> 16) & 0xFF);
    buf[index++] = ((mark_us >>  8) & 0xFF);
    buf[index++] = ((mark_us >>  0) & 0xFF);
    buf[index++] = ((sweep_us >> 24) & 0xFF);
    buf[index++] = ((sweep_us >> 16) & 0xFF);
    buf[index++] = ((sweep_us >>  8) & 0xFF);
    buf[index++] = ((sweep_us >>  0) & 0xFF);
    buf[index++] = ((total_us >> 24) & 0xFF);
    buf[index++] = ((total_us >> 16) & 0xFF);
    buf[index++] = ((total_us >>  8) & 0xFF);
    buf[index++] = ((total_us >>  0) & 0xFF);

#if (TIVOCONFIG_GC_THREAD_COUNT > 1)
    pthread_mutex_lock(&g_write_lock);
#endif
    
    debug_write(buf, index);

#if (TIVOCONFIG_GC_THREAD_COUNT > 1)
    pthread_mutex_unlock(&g_write_lock);
#endif
}

#endif
