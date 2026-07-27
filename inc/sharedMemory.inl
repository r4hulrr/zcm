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

} // namespace zcm