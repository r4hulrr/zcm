#pragma once

#include <cstddef>
#include <new>

namespace zcm
{
namespace detail
{
// get hardware cache line size
#ifdef __cpp_lib_hardware_interference_size
inline constexpr std::size_t cacheLineSize = std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t cacheLineSize = 64;
#endif

// shared memory segment header size
// big enough so that segment can be expanded without moving payload
// also keeps the headers constantly dirtied cache lines away from the payload's,
inline constexpr std::size_t headerReserveBytes = 4096;

// architetcure appropriate spin hints so spin-lock are less costly
// x86 is a compiler intrinic but arm is an assembly instruction
// so adding memory clobbering posibility to not reorder as a safety measure
inline void cpuRelax() noexcept
{
#if defined(__x86_64__) || defined(__i386__) 
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}
} // detail
} // namespace zcm