#include <atomic>
#include <cstdio>
#include <cstring>
#include <time.h>

#include "cacheLine.hpp"
#include "sharedLayout.hpp"
#include "sharedMemory.hpp"


namespace
{
// both processes use the same shared-memory name and expect 1,000 messages
constexpr const char* kName = "/zcm.testFifo";
constexpr std::uint64_t kMessages = 1000;

// used for simple error checking and derived from seq number so it can be checked 
// for consistency
std::uint32_t channelFor(std::uint64_t s) noexcept
{
        return static_cast<std::uint32_t>(s % 7);
}

std::uint32_t valueFor(std::uint64_t s) noexcept
{
        // random equation for checking - can be made better
        return static_cast<std::uint32_t>(s * 10 + 3);
}

// get current time in nanoseconds
std::uint64_t nowNs() noexcept
{
        ::timespec ts{};
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull
                + static_cast<std::uint64_t>(ts.tv_nsec);
}

int runPublisher()
{
        zcm::SegmentOptions opts{};
        opts.prefault = true;

        // test hygiene, clear any process from name to be safe
        ::shm_unlink(kName);

        zcm::SharedRegion<zcm::SharedLayout> region;
        // create a shared region
        const zcm::SegmentError e = region.create(kName, opts);
        if (e != zcm::SegmentError::Ok)
        {
                std::fprintf(stderr, "[pub] create failed: %s (errno=%d)\n",
                                zcm::toString(e), region.segment().lastErrno());
                return 2; // return error
        }

        // inspect the payload created
        zcm::SharedLayout* layout = region.layout();
        std::printf("[pub] mapped at %p  segment=%zu B  payload=%zu B  signature=%llu\n",
                        region.segment().base(),
                        region.segment().bytes(),
                        sizeof(*layout),
                        static_cast<unsigned long long>(
                        zcm::SharedRegion<zcm::SharedLayout>::signature()));
        
        // publish messages onto the fifo
        for(std::uint64_t s = 0; s < kMessages ; ++s)
        {
                zcm::Sample* slot = nullptr;
                while((slot = layout->samples.acquireWriteSlot()) == nullptr)
                        zcm::detail::cpuRelax(); // wait until you acquire one
                
                // zero copy write in place
                slot->seq = s;
                slot->stampNs = nowNs();
                slot->channel = channelFor(s); // used for simple error checking
                slot->value = valueFor(s);

                layout->samples.commitWrite();
        }

        // notify all writes are done
        std::atomic_ref<std::uint32_t>{layout->control.producerDone}.store
                (1, std::memory_order_release);
        
        std::printf("[pub] published %llu messages\n",
                static_cast<unsigned long long>(kMessages));
        
        // relax cpu until consumer is done to not waste cycles
        while(std::atomic_ref<std::uint32_t>{layout->control.consumerDone}.load
                        (std::memory_order_acquire) == 0)
                zcm::detail::cpuRelax();

        // check if sucessfully consumed
        const std::uint64_t consumed = std::atomic_ref<std::uint64_t>{layout->control.consumed}
                .load(std::memory_order_acquire);

        const bool ok = (consumed == kMessages);
        std::printf("[pub] subscriber consumed %llu / %llu -> %s\n",
                static_cast<unsigned long long>(consumed),
                static_cast<unsigned long long>(kMessages),
                ok ? "PASS" : "FAIL");

        // detach AND unlink as it created it
        region.close(true);
        return ok ? 0 : 1;
}

int runSubscriber()
{
        // attach to the shared mem
        zcm::SegmentOptions opts{};
        opts.prefault = true;
        opts.attachTimeoutMs = 10000; // as publisher might not be up yet

        zcm::SharedRegion<zcm::SharedLayout> region;
        const zcm::SegmentError e = region.attach(kName, opts);

        if (e != zcm::SegmentError::Ok)
        {
                std::fprintf(stderr, "[sub] attach failed: %s (errno=%d)\n",
                        zcm::toString(e), region.segment().lastErrno());
                return 2;
        }

        zcm::SharedLayout* layout = region.layout();
        std::printf("[sub] mapped at %p note: different address, same bytes\n",
                region.segment().base());

        std::atomic_ref<std::uint32_t> doneRef{layout->control.producerDone};

        std::uint64_t expected = 0;
        std::uint64_t consumed = 0;
        std::uint64_t gaps     = 0;
        std::uint64_t bad      = 0;
        std::uint64_t latSum   = 0;

        while(true)
        {
                // try getting read slot if not available then yield for a while
                const zcm::Sample* m = layout->samples.acquireReadSlot();
                if (m == nullptr)
                {
                        if (doneRef.load(std::memory_order_acquire) != 0 &&
                                layout->samples.empty())
                                break;
                        zcm::detail::cpuRelax();
                        continue;
                }

                // record timestamp for approx latency
                const std::uint64_t recvNs = nowNs();

                if (m->seq != expected) ++gaps; // strict +1
                if (m->value   != valueFor(m->seq) ||  // consistency
                    m->channel != channelFor(m->seq)) ++bad;

                latSum  += (recvNs - m->stampNs);
                expected = m->seq + 1;
                ++consumed;

                layout->samples.releaseRead(); // release: frees the slot
        }

        // indicate that consumption is over and whether it succeeded
        std::atomic_ref<std::uint64_t>{layout->control.consumed}
                .store(consumed, std::memory_order_release);
        std::atomic_ref<std::uint32_t>{layout->control.consumerDone}
                .store(1, std::memory_order_release);

        std::printf("[sub] consumed=%llu gaps=%llu inconsistent=%llu mean=%llu ns\n",
                static_cast<unsigned long long>(consumed),
                static_cast<unsigned long long>(gaps),
                static_cast<unsigned long long>(bad),
                static_cast<unsigned long long>(consumed ? latSum / consumed : 0));
        
        region.close(false);   // an attacher NEVER unlinks
        return (gaps == 0 && bad == 0 && consumed == kMessages) ? 0 : 1;
}
} // namespace

int main(int argc, char** argv)
{
        // run pub or sub depending on input
        const char* role = (argc > 1) ? argv[1] : "";
        if (std::strcmp(role, "pub") == 0) return runPublisher();
        if (std::strcmp(role, "sub") == 0) return runSubscriber();

        // if neither cli error
        std::fprintf(stderr, "usage: %s pub|sub\n", argv[0]);
        return 64;
}