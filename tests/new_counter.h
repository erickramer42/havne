// File/layer: tests/new_counter.h | test-only, C++17, Qt-free
// Testing: the seam itself; CoreTest.Dispatch* and *AllocatesNothing
//          show it counts (a five-callback dispatch is seen allocating)
//
// Replaces the global operator new with one that counts the calls made
// on each thread, so a test can show a path allocates nothing it could
// throw bad_alloc from: a dispatch, or a worker's steady state, read
// from a callback running on that worker. ecuio-core is a static
// library, so its calls land here; a DLL's own (the fake vendor DLL's)
// don't on Windows. Counts operator new only: malloc and fopen fail by
// returning null, which the code checks.
//
// Defines the replacement, so include it from one source file per test
// binary.

#ifndef ECUIO_TESTS_NEW_COUNTER_H
#define ECUIO_TESTS_NEW_COUNTER_H

#include <cstddef>
#include <cstdlib>
#include <new>

namespace new_counter {
inline thread_local unsigned long t_news = 0;

/* operator new calls made on this thread so far */
inline unsigned long on_this_thread()
{
    return t_news;
}
} // namespace new_counter

void* operator new(std::size_t size)
{
    ++new_counter::t_news;
    if (void* p = std::malloc(size != 0 ? size : 1)) {
        return p;
    }
    throw std::bad_alloc();
}

void operator delete(void* p) noexcept
{
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept
{
    std::free(p);
}

#endif /* ECUIO_TESTS_NEW_COUNTER_H */
