// glibc 2.38 headers turn strtol, strtoul and sscanf into their C23 versions
// (__isoc23_*, symbol version GLIBC_2.38), so a shim built on this system would
// not load on Ubuntu 22.04 / Mint 21 (glibc 2.35), where Live itself runs. These
// definitions serve the shim's own calls (hidden: nothing else sees them) with
// the older versions, which differ only in not reading a "0b" binary prefix.
// No standard headers here: they declare the C23 names with other attributes.
#include <cstdarg>

extern "C" {
long old_strtol(const char *, char **, int);
unsigned long old_strtoul(const char *, char **, int);
int old_vsscanf(const char *, const char *, va_list);
__asm__(".symver old_strtol,strtol@GLIBC_2.2.5");
__asm__(".symver old_strtoul,strtoul@GLIBC_2.2.5");
__asm__(".symver old_vsscanf,__isoc99_vsscanf@GLIBC_2.7");

__attribute__((visibility("hidden"))) long __isoc23_strtol(const char *text, char **end, int base)
{
    return old_strtol(text, end, base);
}

__attribute__((visibility("hidden"))) unsigned long __isoc23_strtoul(const char *text, char **end, int base)
{
    return old_strtoul(text, end, base);
}

__attribute__((visibility("hidden"))) int __isoc23_sscanf(const char *text, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int result = old_vsscanf(text, format, args);
    va_end(args);
    return result;
}
}
