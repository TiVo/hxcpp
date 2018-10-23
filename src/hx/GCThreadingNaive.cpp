#include <hxcpp.h>
#include <stdio.h>
#include <map>
#include <pthread.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "GCThreading.h"

// This implementation of the functions defined in GCThreading.h is 'naive' -
// it requires that every thread call CheckForGCMark() periodically, and
// any thread doing garbage collection will block until all other threads
// have called this function.
//
// A major disadvantage of this naivety is that EnterMarkSafe() is costly,
// because the current register set of the calling thread must be saved
// away every time.
//
// This is equivalent to the pre-existing GCInternal.cpp's implementation,
// which was also naive and presumed that all threads would check in
// periodically.
//
// A more sophisticated implementation would use signals to force all threads
// other than the thread doing the GC to capture their stack and wait until
// the GC is done, ala the Boehm GC.

// This is the lock that protects all global GC data from GCPooled.cpp
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
// This is the lock that protects all global GC data from this file
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_stop_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_resume_cond = PTHREAD_COND_INITIALIZER;
static pthread_key_t g_info_key;
static pthread_once_t g_info_key_once = PTHREAD_ONCE_INIT;
static volatile bool g_gc_in_progress;
static volatile int g_thread_count;
static volatile int g_is_multithreaded;
static int g_thread_stopped_count;
static std::vector<GCThreading_ThreadInfo *> g_thread_infos;


// This function plays dirty games with cache coherency, and is represented
// here as inline functions just so that this can be fully documented
static inline bool IsGCInProgress()
{
    // Because this value could be set or cleared by a thread other than the
    // calling thread, the bare access here could give the wrong result.  But
    // if the value is out of date:
    // - If some other thread has set it to true, but this thread hasn't seen
    //   that update yet, then the worst that will happen is that this
    //   function will return false incorrectly.  Only CheckForGCMark() calls
    //   this function, and if it gets a false value when another thread has
    //   already set g_gc_in_progress, then the worst that happens is that the
    //   other thread has to wait longer to start its GC - until the calling
    //   thread finally sees the update to g_gc_in_progress.  This is expected
    //   to happen really quicky as cache coherency should happen in
    //   microseconds.
    // - If some other thread has set it to false, but this thread hasn't seen
    //   that update yet, then the call in CheckForGCMark() will unnecessarily
    //   grab the GC lock and enter WaitUntilGCDoneLocked() - which will
    //   immediately see that there is no GC in progress, and exit.  No harm
    //   done.
    return g_gc_in_progress;
}


static void make_key()
{
    pthread_key_create(&g_info_key, NULL);
}


static void InitializeThreadInfoLocked(void *top_of_stack)
{
    pthread_once(&g_info_key_once, make_key);
    GCThreading_ThreadInfo *si =
        (GCThreading_ThreadInfo *) malloc(sizeof(GCThreading_ThreadInfo));
    si->top = top_of_stack;
    si->bottom = 0;
    memset(si->jmpbuf, 0, sizeof(si->jmpbuf));
    g_thread_infos.push_back(si);
    pthread_setspecific(g_info_key, (void *) si);
    g_thread_count += 1;
}


// Use setjmp to save state to stack.  It is assumed that setjmp saves all
// pointers into the jmp_buf in an unmangled form, such that the mark will
// find any pointers to garbage collected allocated entries that were stored
// in registers on entry to InternalCollect in the jmpbuf field if the
// calling thread's TLS data.
static void UpdateThreadInfoLocked()
{
    void *bottom;
    ((GCThreading_ThreadInfo *) pthread_getspecific(g_info_key))->bottom =
        &bottom;
    // Use setjmp to (hopefully) save registers
    (void) setjmp(((GCThreading_ThreadInfo *)
                   pthread_getspecific(g_info_key))->jmpbuf);
}
    

static void DeleteThreadInfoLocked()
{
    GCThreading_ThreadInfo *gi =
        (GCThreading_ThreadInfo *) pthread_getspecific(g_info_key);
    int count = g_thread_infos.size();
    for (int i = 0; i < (count - 1); i++) {
        if (g_thread_infos[i] == gi) {
            g_thread_infos[i] = g_thread_infos[count - 1];
            break;
        }
    }
    g_thread_infos.resize(count - 1);
    free(gi);
    pthread_setspecific(g_info_key, 0);
}


static void WaitUntilGCDoneLocked()
{
    g_thread_stopped_count += 1;
    UpdateThreadInfoLocked();
    pthread_cond_signal(&g_stop_cond);
    do {
        pthread_cond_wait(&g_resume_cond, &g_mutex);
    } while (g_gc_in_progress);
    g_thread_stopped_count -= 1;
}


void GCThreading_Lock()
{
    if (g_is_multithreaded) {
        pthread_mutex_lock(&g_lock);
    }
}


void GCThreading_Unlock()
{
    if (g_is_multithreaded) {
        pthread_mutex_unlock(&g_lock);
    }
}


void GCThreading_PrepareMultiThreaded()
{
    pthread_mutex_lock(&g_mutex);
    g_is_multithreaded = true;
    pthread_mutex_unlock(&g_mutex);
}


void GCThreading_InitializeGCForThisThread(void *top_of_stack)
{
    pthread_mutex_lock(&g_mutex);
    InitializeThreadInfoLocked(top_of_stack);
    pthread_mutex_unlock(&g_mutex);
}


void GCThreading_DeinitializeGCForThisThread()
{
    pthread_mutex_lock(&g_mutex);
    g_thread_count -= 1;
    DeleteThreadInfoLocked();
    pthread_cond_signal(&g_stop_cond);
    pthread_mutex_unlock(&g_mutex);
}


void GCThreading_CheckForGCMark()
{
    if (g_is_multithreaded) {
        pthread_mutex_lock(&g_mutex);
        if (IsGCInProgress()) {
            WaitUntilGCDoneLocked();
        }
        pthread_mutex_unlock(&g_mutex);
    }
}


void GCThreading_EnterMarkSafe()
{
    if (g_is_multithreaded) {
        pthread_mutex_lock(&g_mutex);
        g_thread_stopped_count += 1;
        UpdateThreadInfoLocked();
        // So everything on the stack up to this point and in registers got
        // saved ... presumably no code in between EnterMarkSafe and
        // LeaveMarkSafe would do anything that could affect GC, so it doesn't
        // matter what ends up in the stack or registers after this.
        pthread_cond_signal(&g_stop_cond);
        pthread_mutex_unlock(&g_mutex);
    }
}


void GCThreading_LeaveMarkSafe()
{
    if (g_is_multithreaded) {
        pthread_mutex_lock(&g_mutex);
        while (g_gc_in_progress) {
            pthread_cond_wait(&g_resume_cond, &g_mutex);
        }
        g_thread_stopped_count -= 1;
        pthread_mutex_unlock(&g_mutex);
    }
}


void GCThreading_BeginGCMark()
{
    if (g_is_multithreaded) {
        pthread_mutex_lock(&g_mutex);
        if (g_gc_in_progress) {
            WaitUntilGCDoneLocked();
        }
        else {
            UpdateThreadInfoLocked();
        }
        g_gc_in_progress = true;
        while (g_thread_stopped_count != (g_thread_count - 1)) {
            pthread_cond_wait(&g_stop_cond, &g_mutex);
        }
    }
    else {
        UpdateThreadInfoLocked();
        g_gc_in_progress = true;
    }
}


int GCThreading_GetThreadCount()
{
    return g_thread_infos.size();
}


void GCThreading_GetThreadInfo(int index, GCThreading_ThreadInfo *&threadInfo)
{
    threadInfo = g_thread_infos[index];
}


void GCThreading_EndGCMark()
{
    g_gc_in_progress = false;
    if (g_is_multithreaded) {
        pthread_cond_broadcast(&g_resume_cond);
        pthread_mutex_unlock(&g_mutex);
    }
}
