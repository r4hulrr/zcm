#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <cassert>

namespace zcm
{
namespace detail
{
#ifdef __cpp_lib_hardware_interference_size
inline constexpr std::size_t cacheLineSize = std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t cacheLineSize = 64;
#endif
} // namespace detail
template<typename ValueType, std::uint64_t Capacity>
class SpscFifo{
	static_assert(std::is_trivially_copyable_v<ValueType>,
		"ValueType must be trivially copyable as it lives in shared "
		"memory, where any pointer or heap handle inside T would be "
		"meaningless to peer process.");
	static_assert(std::is_trivially_destructible_v<ValueType>,
		"ValueType must be trivially destructible as the queue never "
		"runs destructors on slots");
	static_assert(Capacity > 0,
		"Capacity must be non-zero");
	static_assert((Capacity & (Capacity - 1)) == 0, 
		"Capacity must be a power of 2");
public:
	static constexpr std::uint64_t capacity{Capacity};

	SpscFifo() noexcept = default;

	SpscFifo(const SpscFifo&) = delete;
	SpscFifo& operator=(const SpscFifo&) = delete;
	SpscFifo(SpscFifo&&) = delete;
	SpscFifo& operator=(SpscFifo&&) = delete;
	
	// producer helpers
	[[nodiscard]] ValueType* acquireWriteSlot() noexcept;	
	void commitWrite() noexcept;

	// consumer helpers
	[[nodiscard]] const ValueType* acquireReadSlot() noexcept;
	void releaseRead() noexcept;

	// added for convenience and comparison, both make two copies
	// NOT ZERO COPY
	bool push(const ValueType& item) noexcept;
	bool pop(ValueType& item) noexcept;

	[[nodiscard]] bool empty() const noexcept; // not a guarantee, just a hint
	[[nodiscard]] std::uint64_t size() const noexcept; // not a guarantee, just a hint
private:
	static constexpr std::uint64_t cMask = Capacity - 1;

	static bool isFull(uint64_t readIdx, uint64_t writeIdx) noexcept{
		return (writeIdx - readIdx) == Capacity ;
	}

	// avoid cache line ping pong
	alignas(detail::cacheLineSize) std::array<ValueType, Capacity> _data;

	// consumer line
	alignas(detail::cacheLineSize) std::atomic<uint64_t> _readIdx{0};
	std::uint64_t _cachedWriteIdx{0};

	// producer line
	alignas(detail::cacheLineSize) std::atomic<uint64_t> _writeIdx{0};
	std::uint64_t _cachedReadIdx{0};

#if !defined(NDEBUG)
	bool _writeSlotOutstanding{false};
	bool _readSlotOutstanding{false}; 
#endif
};
} // namespace zcm

#include "spsc_fifo.inl"