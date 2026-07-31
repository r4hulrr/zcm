#pragma once

#include <cstdint>

// every optimization is kept as an option to help us identify
// the effects of the optimization and also flexibility as it 
// allows us to disable particular optimizations if required
namespace zcm
{
struct SegmentOptions
{       
        // this touches every page of the mapping at the setup time so 
        // that the first message does not pay a minor page fault price
        bool prefault{true};

        // asks the operating system to keep the mapped pages resident in physical 
        // memory and not swap it, failure is reported here
        bool lockPages{false};

        // asks for 2MiB pages, this is a hint not a guarantee
        bool hugePages{false};

        // How long attach() waits for the creator to publish READY. 0 means "fail
        // immediately if the creator is not already up".
        std::uint32_t attachTimeoutMs{5000};

        // Poll interval while waiting during attach. Attach is a cold path; there
        // is no reason to burn a core on it.
        std::uint32_t attachPollUs{200};

};
} // namespace zcm