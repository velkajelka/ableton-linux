// Linked into the bundled tools/bundled/innoextract next to
// ../shim/glibccompat.cpp, so that it runs on glibc 2.35 (Ubuntu 22.04 /
// Mint 21): Arch's static Boost.Random calls arc4random (GLIBC_2.36). This
// one reads the same kernel source through getrandom (GLIBC_2.25).
#include <sys/random.h>

#include <cstdint>
#include <cstdlib>

extern "C" __attribute__((visibility("hidden"))) uint32_t arc4random(void)
{
    uint32_t value;
    if (getrandom(&value, sizeof value, 0) != sizeof value)
        abort();
    return value;
}
