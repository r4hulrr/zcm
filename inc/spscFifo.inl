#pragma once

#include "spscFifo.hpp"

namespace zcm
{
template<typename ValueType, std::uint64_t Capacity>
bool SpscFifo<ValueType, Capacity>::push(const ValueType& item) noexcept{

        ValueType* slot = acquireWriteSlot();
        
        if (slot == nullptr) return false; // full

        *slot = item;
        commitWrite();
        return true;
}

template<typename ValueType, std::uint64_t Capacity>
[[nodiscard]] ValueType* SpscFifo<ValueType, Capacity>::acquireWriteSlot() noexcept{

#if !defined(NDEBUG)
        assert(!_writeSlotOutstanding && "acquireWriteSlot called twice without commitWrite");
#endif
        const auto writeIdx = _writeIdx.load(std::memory_order_relaxed);

        if (isFull(_cachedReadIdx, writeIdx)){
                // its possibly full, not confirmed, as there is the case
                // that the read ptr advanced since our last cache
                _cachedReadIdx = _readIdx.load(std::memory_order_acquire);

                if (isFull(_cachedReadIdx, writeIdx)) return nullptr;
        }

#if !defined(NDEBUG)
        _writeSlotOutstanding = true;
#endif
        return &_data[writeIdx & cMask];
}

template<typename ValueType, std::uint64_t Capacity>
void SpscFifo<ValueType, Capacity>::commitWrite() noexcept{
#if !defined(NDEBUG)
        assert(_writeSlotOutstanding && "commitWrite without a matching acquireWriteSlot");
        _writeSlotOutstanding = false;
#endif
        const auto writeIdx = _writeIdx.load(std::memory_order_relaxed);
        _writeIdx.store(writeIdx + 1, std::memory_order_release);
}

template<typename ValueType, std::uint64_t Capacity>
bool SpscFifo<ValueType, Capacity>::pop(ValueType& item) noexcept{
        auto slot = acquireReadSlot();

        if (slot == nullptr) return false;
        item = *slot;
        releaseRead();
        return true;        
}

// consumer helpers
template<typename ValueType, std::uint64_t Capacity>
[[nodiscard]] const ValueType* SpscFifo<ValueType, Capacity>::acquireReadSlot() noexcept{

#if !defined(NDEBUG)
        assert(!_readSlotOutstanding && "acquireReadSlot called twice without releaseRead");
#endif
        const auto readIdx = _readIdx.load(std::memory_order_relaxed);

        if (readIdx == _cachedWriteIdx){
                // potentially empty but write idx could have advanced
                _cachedWriteIdx = _writeIdx.load(std::memory_order_acquire);

                if (readIdx == _cachedWriteIdx) return nullptr;
        }

#if !defined(NDEBUG)
        _readSlotOutstanding = true;
#endif
        return &_data[readIdx & cMask];
}

template<typename ValueType, std::uint64_t Capacity>
void SpscFifo<ValueType, Capacity>::releaseRead() noexcept{
#if !defined(NDEBUG)
        assert(_readSlotOutstanding && "releaseRead without a matching acquireReadSlot");
        _readSlotOutstanding = false;
#endif
        const auto readIdx = _readIdx.load(std::memory_order_relaxed);
        _readIdx.store(readIdx + 1, std::memory_order_release);
}

// HINTS NOT GUARANTEES
template<typename ValueType, std::uint64_t Capacity>
[[nodiscard]] bool SpscFifo<ValueType, Capacity>::empty() const noexcept{
        return (_readIdx.load(std::memory_order_relaxed) == _writeIdx.load(std::memory_order_relaxed));
}

template<typename ValueType, std::uint64_t Capacity>
[[nodiscard]] std::uint64_t SpscFifo<ValueType, Capacity>::size() const noexcept{
        return (_writeIdx.load(std::memory_order_relaxed) - _readIdx.load(std::memory_order_relaxed));
}

} // namespace zcm
