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
#include <execinfo.h>
#include <string>
#include <typeinfo>

namespace
{
std::terminate_handler g_previousTerminateHandler = nullptr;

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
    TortoiseBots::RegisterScripts();
}
