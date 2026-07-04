// serve/aura_platform.h — Platform abstraction for Linux-specific APIs
#ifndef AURA_SERVE_AURA_PLATFORM_H
#define AURA_SERVE_AURA_PLATFORM_H

// eventfd(2) and epoll(7) are Linux-only. On macOS/BSD these are
// unavailable, so serve-async (the fiber scheduler) is disabled at
// runtime. The core evaluator / REPL / IR / type-checker paths do
// not depend on these and work on all platforms.
//
// The macros below let the serve/*.cpp files compile without the
// Linux headers; at runtime the affected code paths are skipped
// (fds stay -1, scheduler::run() is a no-op, --serve-async prints
// "not supported").

#if defined(__linux__)
#define AURA_HAVE_EVENTFD 1
#define AURA_HAVE_EPOLL 1
#elif defined(__APPLE__) || defined(__MACH__)
#define AURA_HAVE_EVENTFD 0
#define AURA_HAVE_EPOLL 0
#else
#define AURA_HAVE_EVENTFD 0
#define AURA_HAVE_EPOLL 0
#endif

// macOS <sys/mman.h> defines MAP_ANON but not MAP_ANONYMOUS (the
// Linux/GNU name). Provide a fallback so fiber.cpp's mmap call works.
#ifndef MAP_ANONYMOUS
#ifdef MAP_ANON
#define MAP_ANONYMOUS MAP_ANON
#else
#define MAP_ANONYMOUS 0x1000
#endif
#endif

#endif // AURA_SERVE_AURA_PLATFORM_H
