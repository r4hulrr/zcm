#pragma once

#include "sharedMemory.hpp"

namespace zcm
{

namespace detail
{

inline bool validSegmentName(const char* n) noexcept
{
        // POSIX portability rule: a shm name is "/" followed by one component, no
        // further '/'
        if (n == nullptr || n[0] != '/' || n[1] == '\0') return false;
        for (const char* p = n + 1; *p != '\0'; ++p)
        if (*p == '/') return false;
        return true;
}

inline std::size_t pageSize() noexcept
{
        static const std::size_t ps = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
        return ps;
}

inline std::size_t roundUpToPage(std::size_t n) noexcept
{
        const std::size_t ps = pageSize();
        return (n + ps - 1) & ~(ps - 1); // can do this as page size always a power of 2
}

// for each page, read and write back one byte
// need to write not just read as it can result in read-only PTE if only read
inline void prefaultPages(void* base, std::size_t bytes)
{
        const std::size_t ps = pageSize();
        // need volatile so compiler doesnt optimize it away
        auto *p = static_cast<volatile unsigned char*>(base); 
        for(std::size_t off = 0; off < bytes ; off += ps){
                p[off] = p[off];
        }
}

} // namespace detail

inline const char* toString(SegmentError e) noexcept
{
        switch(e)
        {
                case SegmentError::Ok:              return "Ok";
                case SegmentError::BadName:         return "BadName";
                case SegmentError::AlreadyExists:   return "AlreadyExists";
                case SegmentError::NotFound:        return "NotFound";
                case SegmentError::OpenFailed:      return "OpenFailed";
                case SegmentError::TruncateFailed:  return "TruncateFailed";
                case SegmentError::StatFailed:      return "StatFailed";
                case SegmentError::SizeMismatch:    return "SizeMismatch";
                case SegmentError::MapFailed:       return "MapFailed";
                case SegmentError::LockFailed:      return "LockFailed";
                case SegmentError::ReadyTimeout:    return "ReadyTimeout";
                case SegmentError::MagicMismatch:   return "MagicMismatch";
                case SegmentError::VersionMismatch: return "VersionMismatch";
                case SegmentError::LayoutMismatch:  return "LayoutMismatch";
                case SegmentError::NotMapped:       return "NotMapped";
        }
        return "Unknown";
}

// Shared Segment
inline SharedSegment::~SharedSegment() noexcept
{
        detach();
}

inline SharedSegment::SharedSegment(SharedSegment&& other) noexcept
{
        moveFrom(other);
}

inline SharedSegment& SharedSegment::operator=(SharedSegment&& other) noexcept
{
        // does not unlink, ONLY DETACHES
        if (this == &other) return *this;
        detach();
        moveFrom(other);
        return *this;
}

inline void SharedSegment::detach() noexcept
{
        if (_base == nullptr) return;        

        if (_locked) (void)::munlock(_base, _bytes);
        ::munmap(_base, _bytes); // calls munmap, does not unlink
        _base   = nullptr;
        _bytes  = 0;
        _locked = false;  
}

inline void SharedSegment::moveFrom(SharedSegment& other) noexcept
{
        _base    = other._base;
        _bytes   = other._bytes;
        _creator = other._creator;
        _locked  = other._locked;
        _errno   = other._errno;
        ::memcpy(_name, other._name, kMaxName);

        other._base    = nullptr;
        other._bytes   = 0;
        other._creator = false;
        other._locked  = false;
        other._name[0] = '\0';
}

inline SegmentError SharedSegment::create(const char* name,
                                        std::size_t bytes,
                                        const SegmentOptions& opts) noexcept
{
        // validate the shm name
        if (!detail::validSegmentName(name)) return SegmentError::BadName;
        if (::strlen(name) >= kMaxName) return SegmentError::BadName;

        // if already owns a mapping, detach it first
        if (valid()) detach();

        // want boundaries page aligned so we dont have partially real last pages
        const std::size_t mapBytes = detail::roundUpToPage(bytes);

        // creates the shared memory object
        // create if it does not exist, fail if it does and owner/user can write/read
        const int fd = ::shm_open(name, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);

        if (fd < 0){
                _errno = errno;
                return (errno == EEXIST) ? SegmentError::AlreadyExists 
                                        : SegmentError::OpenFailed;
        }

        // now size the mapping, this is what makes the mapping legal
        // without it we crash with SIGBUS
        if (::ftruncate(fd, static_cast<::off_t>(mapBytes)) != 0)
        {
                // failed so unlink and return error
                _errno = errno;
                ::close(fd);
                ::shm_unlink(name);
                return SegmentError::TruncateFailed;
        }

        // we need map_shared so that writes through this mapping are shared with other
        // processes mapping the same object otherwise we get a copy on write mapping
        int mapFlags = MAP_SHARED;

// if possible and asked by caller we prefault the pages to avoid page fault initially
#ifdef MAP_POPULATE
        if (opts.prefault) mapFlags |= MAP_POPULATE;
#endif

        // now we create the mapping from the shared memory object to process virtual address
        // so that processes can access it through a pointer
        void* base = ::mmap(nullptr, mapBytes, PROT_READ | PROT_WRITE, mapFlags, fd, 0);

        // the mapping holds its own reference to underlying object so fd is not needed anymroe
        ::close(fd);

        if (base == MAP_FAILED)
        {
                _errno = errno;
                ::shm_unlink(name);
                return SegmentError::MapFailed;
        }

// if its possible and requested set up huge pages
#ifdef MADV_HUGEPAGE
        if (opts.hugePages)
        {
                // hint, not for sure, we have to test it ourselves
                (void)::madvise(base, mapBytes, MADV_HUGEPAGE);
        }
#endif

        // explicitly touch each page if requested without relying on OS
        if (opts.prefault) detail::prefaultPages(base, mapBytes);

        // lock pages if requested so it isnt swapped to disk
        // enforced not a hint so returns an error code if fails
        if (opts.lockPages)
        {
                if (::mlock(base, mapBytes) != 0)
                {
                        _errno = errno;
                        ::munmap(base, mapBytes);
                        ::shm_unlink(name);
                        return SegmentError::LockFailed;
                }
                _locked = true;
        }

        _base = base;
        _bytes = mapBytes;
        _creator = true;
        ::strncpy(_name, name, kMaxName - 1);
        // need this as if str is >= size given, strncpy doesnt null term
        _name[kMaxName - 1] = '\0';
        return SegmentError::Ok; 
}

} // namespace zcm