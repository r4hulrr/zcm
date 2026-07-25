#pragma once

#include "cacheLine.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace zcm
{
template<typename ValueType>
class TripleBuffer{
        static_assert(std::is_trivially_copyable_v<ValueType>,
		"ValueType must be trivially copyable as it lives in shared "
		"memory, where any pointer or heap handle inside T would be "
		"meaningless to peer process.");
	static_assert(std::is_trivially_destructible_v<ValueType>,
		"ValueType must be trivially destructible as the queue never "
		"runs destructors on slots");
public:
        TripleBuffer() = default;

        TripleBuffer(const TripleBuffer&) = delete;
        TripleBuffer& operator=(const TripleBuffer&) = delete;

        TripleBuffer(TripleBuffer&&) = delete;
        TripleBuffer& operator=(TripleBuffer&&) = delete;

        // write helpers
        [[nodiscard]] ValueType* acquireWrite() noexcept;
        void commitWrite() noexcept;

        // read helpers
        // pointer is only valid until next read
        [[nodiscard]] const ValueType* acquireRead(bool& isNew) noexcept;

private:
        // masks for the control line
        static constexpr std::uint64_t kIdxMask = 0b011;
        static constexpr std::uint64_t kFresh = 0b100;

        alignas(detail::cacheLineSize) ValueType _buffer[3]{};
        
        // private to writer and reader so not atomic
        std::uint64_t _writeIdx{0};
        std::uint64_t _readIdx{1};

        // middle atomic buffer control info store
        // bottom two bits represent the middle buffer idx
        // the third last bit represents whether the data is fresh
        alignas(detail::cacheLineSize) std::atomic<std::uint64_t> _control{2};

};
} // namespace zcm

#include "tripleBuffer.inl"