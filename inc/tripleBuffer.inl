#pragma once

#include "tripleBuffer.hpp"

namespace zcm
{
template<typename ValueType>
[[nodiscard]] ValueType* TripleBuffer<ValueType>::acquireWrite() noexcept{
        return &_buffer[_writeIdx];
}

template<typename ValueType>
void TripleBuffer<ValueType>::commitWrite() noexcept{
        // exchange the write idx with the middle
        const auto old = _control.exchange(_writeIdx | kFresh, std::memory_order_acq_rel);
        _writeIdx = old & kIdxMask;
}

template<typename ValueType>
[[nodiscard]] const ValueType* TripleBuffer<ValueType>::acquireRead(bool& isNew) noexcept{
        // exchange read idx with middle if data is fresh

        isNew = false;
        if (_control.load(std::memory_order_acquire) & kFresh){
                const auto old = _control.exchange(_readIdx, std::memory_order_acq_rel);
                _readIdx = old & kIdxMask;
                isNew = true;
        }
        
        return &_buffer[_readIdx];
}

} // namespace zcm