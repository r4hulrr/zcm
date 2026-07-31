#pragma once

#include <cstdint>
#include <type_traits>

#include "cacheLine.hpp"
#include "spscFifo.hpp"

namespace zcm
{
// custom message type to test - JUST FOR TESTING
struct Sample
{
        std::uint64_t seq;
        std::uint64_t stampNs;
        std::uint32_t channel;
        std::uint32_t value;
};

static_assert(std::is_trivially_copyable_v<Sample>);
// should follow simple C style object layout
static_assert(std::is_standard_layout_v<Sample>); 
static_assert(sizeof(Sample) == 24); // check to avoid compiler suprises

// control block for out-of-band signalling that is not a message
struct ControlBlock
{
        alignas(detail::cacheLineSize) std::uint32_t producerDone;
        alignas(detail::cacheLineSize) std::uint32_t consumerDone;
        alignas(detail::cacheLineSize) std::uint64_t heartbeat; // publisher bumps
        alignas(detail::cacheLineSize) std::uint64_t consumed; // consumer reports
};

// defines the complete application payload stored in the shared-memory region
struct SharedLayout
{
        // bump whenever we add, remove, reorder or resize a channel
        static constexpr std::uint64_t kLayoutId = 1;

        SpscFifo<Sample, 1024> samples;
        ControlBlock control;
};

static_assert(std::is_trivially_destructible_v<SharedLayout>);

// Definetelty NOT copyable but checking 
// SpscFifo holds std::atomic, whose copy ctor is deleted. That is correct.
static_assert(!std::is_copy_constructible_v<SharedLayout>);
} // namespace zcm