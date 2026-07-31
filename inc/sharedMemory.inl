#pragma once

#include "sharedMemory.hpp"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
 
#include <new>

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

// return current time in ms
inline std::uint64_t monotonicMillis() noexcept
{
        ::timespec ts{};
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<std::uint64_t>(ts.tv_sec) * 1000ull 
        + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000ull;
}

// sleep for specific time 
inline void sleepMicros(std::uint32_t us) noexcept
{
        ::timespec ts{};
        ts.tv_sec = static_cast<::time_t>(us / 1000000u);
        ts.tv_nsec = static_cast<long>((us % 1000000u) * 1000u); // remaning time
        ::nanosleep(&ts, nullptr);
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

[[nodiscard]] inline SegmentError SharedSegment::open(const char* name,
                                std::size_t expectedBytes,
                                const SegmentOptions& opts) noexcept{

        // validate the shm name
        if (!detail::validSegmentName(name)) return SegmentError::BadName;
        if (::strlen(name) >= kMaxName) return SegmentError::BadName;

        // if already connected to a mapping, detach it first
        if (valid()) detach();

        const std::size_t wantBytes = detail::roundUpToPage(expectedBytes);
        // get max time willing to wait until you attach to the shm
        const std::uint64_t deadline = detail::monotonicMillis() + opts.attachTimeoutMs;

        // try to attach until deadline, if still not attached fail
        int fd = -1;
        while(true)
        {
                fd = ::shm_open(name, O_RDWR, 0); // No O_CREAT
                if (fd >= 0) break;
                _errno = errno;

                if (errno != ENOENT) return SegmentError::OpenFailed;
                if (detail::monotonicMillis() >= deadline) return SegmentError::NotFound;
                // sleep for specified time before trying again
                detail::sleepMicros(opts.attachPollUs);
        }

        // the creator does shm::open() and fftruncate() as two separate syscalls
        // between them the object exists but with size 0. If we mapped it now and 
        // touched byte 0, we would take SIGBUS, so therefore we wait for size to appear
        // of course within out deadline
        while(true)
        {
                // get the metadata of the open shm object
                struct ::stat st{};
                if (::fstat(fd, &st) != 0)
                {
                        _errno = errno;
			::close(fd);
			return SegmentError::StatFailed;
                }

		if (static_cast<std::size_t>(st.st_size) >= wantBytes) break;
		if (detail::monotonicMillis() >= deadline)
		{
			::close(fd);
			return SegmentError::SizeMismatch;
		}
		detail::sleepMicros(opts.attachPollUs);
        }

	// apply flags similar to create() with mmap to control how the process connects
	// the shm into its own virtual address space
	int mapFlags = MAP_SHARED;

#ifdef MAP_POPULATE
        if (opts.prefault) mapFlags |= MAP_POPULATE;
#endif

        void* base = ::mmap(nullptr, wantBytes, PROT_READ | PROT_WRITE, mapFlags, fd, 0);

        // the mapping holds its own reference to underlying object so fd is not needed anymroe
        ::close(fd);

        if (base == MAP_FAILED) // this base is not the same as creators most probably its virtual
        {
                _errno = errno; 
                return SegmentError::MapFailed;
        }

// if its possible and requested set up huge pages
#ifdef MADV_HUGEPAGE
        if (opts.hugePages)
        {
                // hint, not for sure, we have to test it ourselves
                (void)::madvise(base, wantBytes, MADV_HUGEPAGE);
        }
#endif

        // explicitly touch each page if requested without relying on OS
        if (opts.prefault) detail::prefaultPages(base, wantBytes);

        // lock pages if requested so it isnt swapped to disk
        // enforced not a hint so returns an error code if fails
        if (opts.lockPages)
        {
                if (::mlock(base, wantBytes) != 0)
                {
                        _errno = errno;
                        ::munmap(base, wantBytes);
                        return SegmentError::LockFailed;
                }
                _locked = true;
        }

        _base = base;
        _bytes = wantBytes;
        _creator = false;
        ::strncpy(_name, name, kMaxName - 1);
        // need this as if str is >= size given, strncpy doesnt null term
        _name[kMaxName - 1] = '\0';
        return SegmentError::Ok;
}

inline SegmentError SharedSegment::unlink() noexcept
{
        if (_name[0] == '\0') return SegmentError::NotMapped; // empty
        // removes the name ONLY. Any process that already mapped the object keeps
        // its mapping and the pages stay alive; the object is destroyed when the
        // last mapping goes away
        const int rc = ::shm_unlink(_name);
        if (rc != 0 && errno != ENOENT)
        {
                _errno = errno;
                return SegmentError::OpenFailed;
        }
        return SegmentError::Ok;
}

template<typename LayoutType>
SegmentError SharedRegion<LayoutType>::create(const char* name,
                                                const SegmentOptions& opts) noexcept
{
        // create shm
        const SegmentError e = _segment.create(name, sizeof(Image), opts);
        if (e != SegmentError::Ok) return e;

        // it is State::Empty, now initialize it
        auto* raw = static_cast<unsigned char*>(_segment.base());
        auto* hdr = reinterpret_cast<SegmentHeader*>(raw);

        // change state to intializing
        std::atomic_ref<std::uint32_t> state{hdr->state};

        // relaxed is fine as nothing synchronizes on it
        state.store(static_cast<std::uint32_t>(SegmentHeader::State::Initializing),
                        std::memory_order_relaxed);

        // now construct it
        LayoutType* payload = ::new (static_cast<void*>(raw + Image::kHeaderReserve))
                                LayoutType{} ;
        
        hdr->magic           = SegmentHeader::kMagic;
        hdr->version         = SegmentHeader::kVersion;
        hdr->headerBytes     = static_cast<std::uint32_t>(Image::kHeaderReserve);
        hdr->totalBytes      = static_cast<std::uint64_t>(sizeof(Image));
        hdr->layoutSignature = signature();
        hdr->creatorPid      = static_cast<std::int64_t>(::getpid());
        std::atomic_ref<std::uint32_t>{hdr->attachedCount}.store(0, std::memory_order_relaxed);

        // now actually let everyone know its ready
        state.store(static_cast<std::uint32_t>(SegmentHeader::State::Ready), 
                        std::memory_order_release);

        _payload = payload;
        return SegmentError::Ok;
}

template<typename LayoutType>
SegmentError SharedRegion<LayoutType>::attach(const char* name,
                                                const SegmentOptions& opts) noexcept
{
        const SegmentError e = _segment.open(name, sizeof(Image), opts);
        if (e != SegmentError::Ok) return e;

        auto* raw = static_cast<unsigned char*>(_segment.base());
        auto* hdr = static_cast<SegmentHeader*>(_segment.base());

        std::atomic_ref<std::uint32_t> state{hdr->state};

        // attach ONLY WHEN STATE IS READY
        // we wait until deadline which is the max time process is willing to wait to connect
        const std::uint64_t deadline = detail::monotonicMillis() + opts.attachTimeoutMs;

        while(true)
        {
                // acquire the state
                const auto s = static_cast<SegmentHeader::State>(state.load(std::memory_order_acquire));
                
                if (s == SegmentHeader::State::Ready) break;
                if (s == SegmentHeader::State::Dead)
                {
                        _segment.detach();
                        return SegmentError::NotFound;
                }

                if (detail::monotonicMillis() >= deadline)
                {
                        _segment.detach();
                        return SegmentError::ReadyTimeout;
                }
                // sleep before checking again
                detail::sleepMicros(opts.attachPollUs);
        }

        // make sure it matches what you want
        if (hdr->magic != SegmentHeader::kMagic)
        { _segment.detach(); return SegmentError::MagicMismatch; }

        if (hdr->version != SegmentHeader::kVersion)
        { _segment.detach(); return SegmentError::VersionMismatch; }

        if (hdr->headerBytes != Image::kHeaderReserve ||
                hdr->totalBytes  != static_cast<std::uint64_t>(sizeof(Image)))
        { _segment.detach(); return SegmentError::SizeMismatch; }
        
        if (hdr->layoutSignature != signature())
        { _segment.detach(); return SegmentError::LayoutMismatch; }

        // increase the attacher count
        std::atomic_ref<std::uint32_t>{hdr->attachedCount}.fetch_add(1, std::memory_order_relaxed);

        _payload = reinterpret_cast<LayoutType*>(raw + Image::kHeaderReserve);
        return SegmentError::Ok;
}

template<typename LayoutType>
constexpr std::uint64_t SharedRegion<LayoutType>::signature() noexcept
{
        // needs to be improved
        // ideally check whether all data is in the layout we expect
        // maybe an xor
        return 1; 
}

template<typename LayoutType>
void SharedRegion<LayoutType>::close(bool unlinkName) noexcept
{       // NO destructors called
        // reduce the attached count if not the creator
        if (_segment.valid() && _payload != nullptr && !_segment.isCreator())
        {
                auto* hdr = reinterpret_cast<SegmentHeader*>(_segment.base());
                std::atomic_ref<std::uint32_t>{hdr->attachedCount}.fetch_sub(1, std::memory_order_relaxed);
        }

        // unlink name from shm if requested
        if (unlinkName && _segment.valid()) (void)_segment.unlink();
        _segment.detach();
        _payload = nullptr;
}

} // namespace zcm