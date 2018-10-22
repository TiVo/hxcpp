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

// This is the lock that protects all global GC data
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_stop_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_resume_cond = PTHREAD_COND_INITIALIZER;
static pthread_key_t g_info_key;
static pthread_once_t g_info_key_once = PTHREAD_ONCE_INIT;
static volatile bool g_gc_in_progress;
static volatile int g_thread_count;
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


// This function plays dirty games with cache coherency, and is represented
// here as inline functions just so that this can be fully documented
static inline bool IsMultiThreaded()
{
    // Because this value could be set or cleared by a thread other than the
    // calling thread, the bare access here could give the wrong result.  The
    // only way in which this thread could see the wrong value here is if
    // this thread sees g_thread_count as > 1, when in fact some other thread
    // has just exited and g_thread_count has been decremented to 1 but this
    // thread hasn't seen that yet.  In that case, the caller will be told
    // that we're currently running multi threaded when in fact we are not.
    // But in all cases where IsMultiThreaded() is called, the caller will
    // function correctly even if the wrong answer is given.
    // It's not possible for g_thread_count to be set to some value greater
    // than 1 but this thread to see 1, because the only way for another
    // thread to exist is if the calling thread created a thread.
    return (g_thread_count > 1);
}


static void make_key()
{
    pthread_key_create(&g_info_key, NULL);
}


static void InitializeThreadInfo(void *top_of_stack)
{
    pthread_once(&g_info_key_once, make_key);
    GCThreading_ThreadInfo *si =
        (GCThreading_ThreadInfo *) malloc(sizeof(GCThreading_ThreadInfo));
    si->top = top_of_stack;
    si->bottom = 0;
    g_thread_infos.push_back(si);
    pthread_setspecific(g_info_key, (void *) si);
}


// Use setjmp to save state to stack.  It is assumed that setjmp saves all
// pointers into the jmp_buf in an unmangled form, such that the mark will
// find any pointers to garbage collected allocated entries that were stored
// in registers on entry to InternalCollect in the jmp_buf on the stack.
static void UpdateThreadInfo()
{
    // Use setjmp to save registers onto stack
    jmp_buf jmpbuf;
    (void) setjmp(jmpbuf);
    ((GCThreading_ThreadInfo *) pthread_getspecific(g_info_key))->bottom = 
        &jmpbuf;
}
    

static void DeleteThreadInfo()
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
    UpdateThreadInfo();
    pthread_cond_signal(&g_stop_cond);
    do {
        pthread_cond_wait(&g_resume_cond, &g_mutex);
    } while (g_gc_in_progress);
    g_thread_stopped_count -= 1;
}


void GCThreading_Lock()
{
    if (IsMultiThreaded()) {
        pthread_mutex_lock(&g_lock);
    }
}


void GCThreading_Unlock()
{
    if (IsMultiThreaded()) {
        pthread_mutex_unlock(&g_lock);
    }
}


void GCThreading_ThreadAboutToBeCreated()
{
    pthread_mutex_lock(&g_mutex);
    g_thread_count += 1;
    pthread_mutex_unlock(&g_mutex);
}


void GCThreading_InitializeGCForThisThread(void *top_of_stack)
{
    pthread_mutex_lock(&g_mutex);
    InitializeThreadInfo(top_of_stack);
    pthread_mutex_unlock(&g_mutex);
}


void GCThreading_DeinitializeGCForThisThread()
{
    pthread_mutex_lock(&g_mutex);
    g_thread_count -= 1;
    DeleteThreadInfo();
    pthread_cond_signal(&g_stop_cond);
    pthread_mutex_unlock(&g_mutex);
}


void GCThreading_CheckForGCMark()
{
    if (IsGCInProgress()) {
        pthread_mutex_lock(&g_mutex);
        WaitUntilGCDoneLocked();
        pthread_mutex_unlock(&g_mutex);
    }
}


void GCThreading_EnterMarkSafe()
{
    if (IsMultiThreaded()) {
        pthread_mutex_lock(&g_mutex);
        g_thread_stopped_count += 1;
        UpdateThreadInfo();
        pthread_cond_signal(&g_stop_cond);
        pthread_mutex_unlock(&g_mutex);
    }
}


void GCThreading_LeaveMarkSafe()
{
    if (IsMultiThreaded()) {
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
    if (IsMultiThreaded()) {
        pthread_mutex_lock(&g_mutex);
        if (g_gc_in_progress) {
            WaitUntilGCDoneLocked();
        }
        else {
            UpdateThreadInfo();
        }
        g_gc_in_progress = true;
        while (g_thread_stopped_count != (g_thread_count - 1)) {
            pthread_cond_wait(&g_stop_cond, &g_mutex);
        }
    }
    else {
        UpdateThreadInfo();
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
    if (IsMultiThreaded()) {
        pthread_cond_broadcast(&g_resume_cond);
        pthread_mutex_unlock(&g_mutex);
    }
}
