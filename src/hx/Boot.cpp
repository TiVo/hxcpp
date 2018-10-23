#include <hxcpp.h>
#include <hxMath.h>
#include <hx/Debug.h>

#ifdef HX_WINRT
#include<Roapi.h>
#endif

#include <signal.h>
#include <string.h>
#include <string>
#include <unistd.h>


static void setup_signals(void (*handler)(int))
{
    // Experience shows that SEGV is the only signal that really is ever
    // received unexpectedly by a haxe program, but do BUS and ILL too since
    // these seem reasonable

    static struct sigaction sa_sigsegv_old;
    static struct sigaction sa_sigbus_old;
    static struct sigaction sa_sigill_old;

    if (handler) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = handler;
        sigaction(SIGSEGV, &sa, &sa_sigsegv_old);
        sigaction(SIGBUS, &sa, &sa_sigbus_old);
        sigaction(SIGILL, &sa, &sa_sigill_old);
    }
    else {
        sigaction(SIGSEGV, &sa_sigsegv_old, 0);
        sigaction(SIGBUS, &sa_sigbus_old, 0);
        sigaction(SIGILL, &sa_sigill_old, 0);
    }
}

static void signal_handler(int code)
{
    // In case of recursive signal, revert to prior handling
    setup_signals(0);
    
    char codestr[64];
    snprintf(codestr, sizeof(codestr), "Signal caught: %d", code);

    // Use __hxcpp_crash_critical_error to do the real work
    __hxcpp_crash_critical_error(String(codestr));
}


static int g_crash_pipe[2];


String __hxcpp_crash_wait()
{
    // Create the pipe for waiting purposes
    pipe(g_crash_pipe);

    // Set up signal handling
    setup_signals(signal_handler);

    // Wait for string from pipe ... first read length
    unsigned char lenbuf[2];

    // Enter "GC free zone", since we'll be blocking
    __hxcpp_enter_gc_free_zone();
    
    read(g_crash_pipe[0], lenbuf, 2);

    unsigned int to_read = (lenbuf[0] << 8) + lenbuf[1];

    if (to_read > 256) {
        to_read = 256;
    }

    char msgbuf[257];
    read(g_crash_pipe[0], msgbuf, to_read);
    // Ensure null terminated
    msgbuf[to_read] = 0;

    // Exit "GC free zone" ...
    __hxcpp_exit_gc_free_zone();
    
    // Return it
    return String(msgbuf);
}


void __hxcpp_crash_critical_error(String s)
{
    // Write a message to the pipe so that it can be returned by
    // __hxcpp_crash_wait
    char msgbuf[257];
    snprintf(msgbuf, sizeof(msgbuf), "%s", s.c_str());

    unsigned int to_write = strlen(msgbuf);

    unsigned char lenbuf[2];

    lenbuf[0] = (to_write << 8) & 0xFF;
    lenbuf[1] = (to_write << 0) & 0xFF;

    write(g_crash_pipe[1], lenbuf, 2);

    write(g_crash_pipe[1], msgbuf, to_write);

    // Now wait forever ... the crash handler will ensure that the program
    // exits ...
    while (true) {
        sleep(10000);
    }
}


namespace hx
{

void Boot()
{
   //__hxcpp_enable(false);
   #ifdef HX_WINRT
   HRESULT hr = ::RoInitialize(  RO_INIT_MULTITHREADED );
   #endif

	#ifdef GPH
	 setvbuf( stdout , 0 , _IONBF , 0 );
	 setvbuf( stderr , 0 , _IONBF , 0 );
	#endif

   __hxcpp_stdlibs_boot();
   Object::__boot();
	Dynamic::__boot();
	hx::Class_obj::__boot();
	String::__boot();
	Anon_obj::__boot();
	ArrayBase::__boot();
	EnumBase_obj::__boot();
   Math_obj::__boot();
}

}


