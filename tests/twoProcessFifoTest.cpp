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
constexpr const char* kName = "zcm/testFifo";
constexpr std::uin64_t kMessages = 1000;

}