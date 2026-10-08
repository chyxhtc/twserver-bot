#include "../host/Module.h"

// M7: the world server has died three times with no usable location - an uncaught
// std::length_error out of basic_string::_M_create twice (2026-10-05 01:13:03 and
// 06:03:35) and one std::bad_alloc (01:15:19). All three landed within a few minutes of
// a restart, and two of them in different module builds, so it is not one bad cut. The
// core prints only "terminate called after throwing ..." and aborts; there is no core
// dump and WSL's crash capture does not give a stack. Install a terminate handler that
// prints what() plus a backtrace to stderr (the container log) before handing over to
// whatever handler was there before, so the next occurrence names the frame. Offsets
// printed as module(+0x...) resolve against the staged .so with addr2line.
#include <exception>
#include <cxxabi.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <execinfo.h>
#include <new>
#include <string>
#include <typeinfo>

namespace
{
std::terminate_handler g_previousTerminateHandler = nullptr;

// M11: the second heap-corruption source. The module has been throwing
// std::bad_alloc out of single bot ticks since 2026-10-07 - 103 rows in one session on
// 2026-10-08, spread over thirteen bots, and one crash that stopped the world thread inside
// TravelNodeMap::getRoute's unordered_map emplace/rehash - and every row so far only names the
// catch site, because the stack is already unwound by then (the M8 adapter logs from its handler).
// A std::new_handler runs before the throw, with the stack still intact, so it is the one place
// that can name the allocation that failed. Nothing here may allocate - a handler that allocates
// would re-enter itself - so the output goes straight to fd 2 through fputs and
// backtrace_symbols_fd, the same pair the terminate handler above uses.
//
// This deliberately does NOT replace operator new: a global allocation hook would change the
// allocation path of every component in the process, which is a bigger risk than the failure it
// is meant to explain, and the module cannot see the requested size that way either without
// duplicating the allocator. A new_handler observes failure without touching the happy path.
std::new_handler g_previousNewHandler = nullptr;

void TortoiseBotsNewHandler()
{
    static time_t lastLog = 0;
    static unsigned suppressed = 0;

    time_t const now = std::time(nullptr);
    if (lastLog && now - lastLog < 60)
    {
        ++suppressed;
    }
    else
    {
        char header[160];
        std::snprintf(header, sizeof(header),
            "TortoiseBots: new handler: allocation failed (suppressed=%u since the previous row)\n",
            suppressed);
        suppressed = 0;
        lastLog = now;

        std::fputs(header, stderr);
        void* frames[64];
        int const n = backtrace(frames, 64);
        backtrace_symbols_fd(frames, n, fileno(stderr));
        std::fflush(stderr);
    }

    // This must not simply return. Measured 2026-10-09 against the same libstdc++ the module is
    // built with (_twserver\modules\build-out\_observe\newhandler-test.cc): with no previous
    // handler installed, set_new_handler reports NULL and operator new calls the handler in a
    // tight loop - "retries once and then throws" is not what it does, it calls the handler
    // again, forever. An allocation-failure hook that hangs the world thread would be strictly
    // worse than the bad_alloc it is meant to explain (2-5 rows an hour today).
    if (g_previousNewHandler)
        g_previousNewHandler();
    throw std::bad_alloc();
}

void TortoiseBotsTerminateHandler()
{
    // M12: this handler used to rethrow the in-flight exception to read its what(), and gave
    // that text back through a char const* that dangled as soon as the catch block ended. Both
    // 2026-10-05 crashes faulted inside this handler itself - 19:58 with RIP inside
    // libstdc++'s std::rethrow_exception (called on the line above), 21:15 inside libc's
    // abort - so the handler never printed the reason and the server died with a 5 GB dump
    // instead of a named exception. Name the exception through the C++ ABI instead: no
    // rethrow, no borrowed pointer, nothing that can fault before the line is out.
    std::type_info* typeInfo = abi::__cxa_current_exception_type();
    std::string const typeName = typeInfo && typeInfo->name() ? typeInfo->name() : "unknown";

    std::fprintf(stderr, "TortoiseBots: terminate handler: uncaught exception of type %s\n", typeName.c_str());
    void* frames[64];
    int const n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, fileno(stderr));
    std::fflush(stderr);

    // Deliberately do not hand over to the previous handler: the libstdc++ verbose handler
    // names the exception by rethrowing it, which is the very path that faulted above.
    // The type name and the backtrace are already out; aborting from here keeps the death clean.
    std::abort();
}
} // namespace

// Called by Penqle's generated native-module loader.
void AddTortoiseBotsScripts()
{
    g_previousTerminateHandler = std::set_terminate(&TortoiseBotsTerminateHandler);
    g_previousNewHandler = std::set_new_handler(&TortoiseBotsNewHandler);
    TortoiseBots::RegisterScripts();
}
