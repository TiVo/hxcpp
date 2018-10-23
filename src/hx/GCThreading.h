// Implements functionality to allow multi-threaded programs to use the
// garbage collector safely.

#include <setjmp.h>

// This defines the data structure containing information needed for a thread
// during garbage collection.
typedef struct GCThreading_ThreadInfo
{
    void *top; // Stack starts here and grows downwards
    void *bottom; // Stack stops here
    jmp_buf jmpbuf; // Saves registers
} GCThread_ThreadInfo;

// Acquire a lock suitable for preventing multithreaded access to GC
// structures.  This is implemented efficiently for single threaded programs.
void GCThreading_Lock();

// Release the GC threading lock.
void GCThreading_Unlock();

// Prepares for multithreaded operation
void GCThreading_PrepareMultiThreaded();

// Every thread must call this function shortly after creation, before it ever
// touches any heap values.
void GCThreading_InitializeGCForThisThread(void *stack_top);

// Every thread that called InitializeGCForThisThread must call
// DeinitializeGCForThisThread() after the thread has finished touching any
// heap values, right before the thread exits.
void GCThreading_DeinitializeGCForThisThread();

// Every thread should periodically call this function.  This function may
// pause the calling thread if another thread is waiting for all threads to
// pause before it can start GC Mark.
void GCThreading_CheckForGCMark();

// Causes the calling thread to be removed from consideration as a thread
// that could do anything impacting a GC mark
void GCThreading_EnterMarkSafe();

// Causes the calling thread to be re-considered as a thread that could do
// something impacting a GC mark
void GCThreading_LeaveMarkSafe();

// Called by a thread that wants to start the mark phase of garbage
// collection.  This call will ensure that all other threads are paused before
// returning.  The calling thread must call EndGCMark() when done with the
// mark phase.
//
// XXX This could take a function to call?  That way if necessary it could
// arrange for the function to be called from a signal handler?
// Trying to think of a scheme where the SIGUSR2 signal causes the process
// to garbage collect.  When a GC is needed, the program would just send
// SIGUSR2 to itself.  Every thread that receives SIGUSR2 would add its
// stack to the global stack, then if it's the last thread to receive SIGURS2
// (detected via an atomic counter of number of threads in SIGUSR2 compared
// to a global count of number of threads), it would do the mark, including
// marking from all of the collected thread stacks.
// Since it's not possible for a SIGUSR2 handler to know anything about which
// thread it is (can't call pthread_self(), although Boehm GC does, so
// consider allowing it as an option for greater efficiency), it can just
// push its stack bottom onto a global list, then the thread that actually
// walks stacks can create the stack list by matching stack tops and bottoms
// (sorting the list of all stack tops and bottoms should make the stack
// ranges obvious).
// Also could use one of the RT signals instead of SIGUSR2 like Boehm GC
// does.  Search google for "Boehm GC pthread_self" to find its code ...
void GCThreading_BeginGCMark();

// Get the number of threads that have stacks to walk
int GCThreading_GetThreadCount();

// Gets a thread info by index.
void GCThreading_GetThreadInfo(int index, GCThreading_ThreadInfo *&threadInfo);

// Called by the thread that called BeginGCMark() when it is done with its
// mark phase.
void GCThreading_EndGCMark();
