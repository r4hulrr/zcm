#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "cacheLine.hpp"
#include "segmentConfig.hpp"

namespace zcm
{
// error codes. No exceptions in the hot path prevent unwind context asw
enum class SegmentError : std::uint32_t
{
        // Operation succeeded.
        Ok = 0,
        // Shared-memory name is invalid; it must look like "/name" with no second slash.
        BadName,
        // Creator tried to create the segment, but that name already exists.
        AlreadyExists,
        // Attacher waited, but no segment with that name appeared before timeout.
        NotFound,
        // shm_open() failed for another reason, such as permissions or resource limits.
        OpenFailed,
        // Creator could not resize the new shared-memory object with ftruncate().
        TruncateFailed,
        // Attacher could not inspect the object's current size using fstat().
        StatFailed,
        // Existing segment size differs from what this binary expects.
        SizeMismatch,
        // mmap() failed, so the process could not map the segment.
        MapFailed,
        // mlock() failed, usually because the process lacks enough lockable-memory allowance.
        LockFailed,
        // Segment existed and mapped, but creator never changed its state to Ready.
        ReadyTimeout,
        // The mapped object is not recognized as one of this project's segments.
        MagicMismatch,
        // The segment header version differs from this binary's expected version.
        VersionMismatch,
        // Header version matches, but the actual shared payload layout differs.
        LayoutMismatch,
        // An operation was attempted when this SharedSegment had no active mapping.
        NotMapped
};

// turns an error enum into readable text
[[nodiscard]] const char* toString(SegmentError e) noexcept;

// segment header 
// both processes HAVE to agree on this header
struct SegmentHeader
{
        enum class State : std::uint32_t
        {
                Empty           = 0, // creater has not started
                Initializing    = 1, // creator is running constructors
                Ready           = 2, // payload constructed, safe to publish/consume
                Dead            = 3 // creator is asking connectors to detach
        };

        static constexpr std::uint64_t kMagic = 0x5A434D5F53484D31ULL; // "ZCM_SHM1"
        static constexpr std::uint32_t kVersion = 1;

        std::uint64_t magic;
        std::uint32_t version;
        std::uint32_t headerBytes; // tells us where payload begins
        std::uint64_t totalBytes; // total segment size
        std::uint64_t layoutSignature; // Checks that both binaries compiled the same payload layout
        std::int64_t  creatorPid;
        std::uint32_t state;         // accessed via atomic_ref
        std::uint32_t attachedCount; // accessed via atomic_ref
};

static_assert(std::is_standard_layout_v<SegmentHeader>,
    "SegmentHeader must be standard layout");
static_assert(std::is_trivially_copyable_v<SegmentHeader>,
    "SegmentHeader must be trivially copyable.");
static_assert(std::is_trivially_default_constructible_v<SegmentHeader>,
    "SegmentHeader must need no constructor");
static_assert(std::is_trivially_destructible_v<SegmentHeader>);
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free,
    "SegmentHeader state and attachedCount must be lock free");
static_assert(sizeof(SegmentHeader) <= detail::headerReserveBytes);

// segment header metadata along with payload packed for ease of access
template <typename LayoutType>
struct SegmentImage
{
        static constexpr std::size_t kHeaderReserve = detail::headerReserveBytes;

        static_assert(kHeaderReserve % alignof(LayoutType) == 0,
        "The payload starts at a fixed offset of header bytes, so that "
        "offset must be a multiple of the payload's alignment.");

        SegmentHeader header;
        unsigned char headerPad[kHeaderReserve - sizeof(SegmentHeader)];
        LayoutType payload;
}


} // namespace zcm