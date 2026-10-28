// Code hooks into AbletonLive12, shared by the files that change Live's own
// functions (filedialogs.cpp, nativemenu.cpp).
//
// redirectFunction: replaces a function. Its start is overwritten with
//   movabs rax, target; jmp rax (12 bytes).
// hookFunction: the same, but also returns a trampoline that runs the
//   original: the overwritten prologue (position-independent instructions,
//   checked byte for byte), then a jump to the rest of the function.
//
// Every hook first checks the bytes it replaces, so a different Live build
// leaves Live unchanged instead of crashing. rax is free at a function's
// entry (no argument travels in it here: none of the hooked functions is
// variadic).
#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);

namespace {

constexpr size_t kJumpSize = 12;

void writeJump(unsigned char *at, uintptr_t to)
{
    at[0] = 0x48; // movabs rax, imm64
    at[1] = 0xb8;
    memcpy(at + 2, &to, 8);
    at[10] = 0xff; // jmp rax
    at[11] = 0xe0;
}

// Makes [address, address + size) writable, runs `write`, restores `protection`.
template <typename Write> bool patch(uintptr_t address, size_t size, int protection, Write write)
{
    const uintptr_t page = sysconf(_SC_PAGESIZE);
    const uintptr_t start = address & ~(page - 1);
    const size_t span = ((address + size + page - 1) & ~(page - 1)) - start;
    if (mprotect(reinterpret_cast<void *>(start), span, PROT_READ | PROT_WRITE | (protection & PROT_EXEC)) != 0)
        return false;
    write();
    mprotect(reinterpret_cast<void *>(start), span, protection);
    return true;
}

} // namespace

bool redirectFunction(uintptr_t fn, const unsigned char *prologue, size_t len, void *target)
{
    auto *code = reinterpret_cast<unsigned char *>(fn);
    if (len < kJumpSize || memcmp(code, prologue, len) != 0)
        return false;
    if (!patch(fn, kJumpSize, PROT_READ | PROT_EXEC, [&] { writeJump(code, reinterpret_cast<uintptr_t>(target)); }))
        return false;
    __builtin___clear_cache(reinterpret_cast<char *>(code), reinterpret_cast<char *>(code) + kJumpSize);
    return true;
}

// Replaces `len` bytes at `at` (in a segment mapped with `protection`) when
// they hold `expected`. Bytes that already hold `replacement` (a binary that
// tools/patch-live.py patched) count as done.
bool patchBytes(uintptr_t at, const unsigned char *expected, const unsigned char *replacement, size_t len,
                int protection)
{
    auto *bytes = reinterpret_cast<unsigned char *>(at);
    if (memcmp(bytes, replacement, len) == 0)
        return true;
    if (memcmp(bytes, expected, len) != 0)
        return false;
    if (!patch(at, len, protection, [&] { memcpy(bytes, replacement, len); }))
        return false;
    if (protection & PROT_EXEC)
        __builtin___clear_cache(reinterpret_cast<char *>(bytes), reinterpret_cast<char *>(bytes) + len);
    return true;
}

void *hookFunction(uintptr_t fn, const unsigned char *prologue, size_t len, void *replacement)
{
    if (len < kJumpSize || memcmp(reinterpret_cast<void *>(fn), prologue, len) != 0)
        return nullptr;
    const size_t page = sysconf(_SC_PAGESIZE);
    void *mem = mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return nullptr;
    auto *trampoline = static_cast<unsigned char *>(mem);
    memcpy(trampoline, prologue, len);
    writeJump(trampoline + len, fn + len);
    if (mprotect(mem, page, PROT_READ | PROT_EXEC) != 0
        || !redirectFunction(fn, prologue, len, replacement)) {
        munmap(mem, page);
        return nullptr;
    }
    return trampoline;
}
