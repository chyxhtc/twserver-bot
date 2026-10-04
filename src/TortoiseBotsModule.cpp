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
#include <cstdio>
#include <cstdlib>
#include <execinfo.h>

namespace
{
std::terminate_handler g_previousTerminateHandler = nullptr;

void TortoiseBotsTerminateHandler()
{
    char const* what = "unknown";
    try
    {
        if (std::current_exception())
            std::rethrow_exception(std::current_exception());
    }
    catch (std::exception const& e)
    {
        what = e.what();
    }
    catch (...)
    {
    }

    std::fprintf(stderr, "TortoiseBots: terminate handler: uncaught exception: %s\n", what);
    void* frames[64];
    int const n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, fileno(stderr));
    std::fflush(stderr);

    if (g_previousTerminateHandler)
        g_previousTerminateHandler();
    std::abort();
}
} // namespace

// Called by Penqle's generated native-module loader.
void AddTortoiseBotsScripts()
{
    g_previousTerminateHandler = std::set_terminate(&TortoiseBotsTerminateHandler);
    TortoiseBots::RegisterScripts();
}
