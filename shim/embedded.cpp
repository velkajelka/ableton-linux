// Embedded mode: AbletonLive12 carries this library inside its own file.
//
// tools/patch-live.py appends libmenufix.so to the binary and points DT_INIT
// at a small stub (tools/embed-stub.S). The stub copies the library into a
// memfd and dlopen()s it (RTLD_NOW, local) before Live's static constructors,
// then continues with Live's own _init.
//
// Loaded that way, the library does not come before libasound, Qt and Xlib in
// symbol lookup, as it does under LD_PRELOAD or as a DT_NEEDED entry, so:
//   - Live's own GOT entries (JUMP_SLOT and GLOB_DAT) for the functions this
//     library defines (snd_pcm_open, QWidget::resize, XMapWindow, ...) are
//     pointed at it, read from the binary's relocations in /proc/self/exe;
//     RELRO pages are made read-only again afterwards;
//   - the original functions are found in the global scope (RTLD_DEFAULT),
//     which does not contain this library; RTLD_NEXT finds nothing for a
//     library loaded like this.
// Calls the plug-ins make to Xlib are not redirected in this mode, only Live's.
//
// If another copy is already loaded (LIVE_SHIM test builds through
// LD_PRELOAD), the embedded copy stays inactive.
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <utility>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);

__attribute__((visibility("hidden"))) bool shimEmbedded = false; // loaded by the stub, not by the loader
__attribute__((visibility("hidden"))) bool shimActive = true;    // false: another copy of the shim does the work

// The function a hook replaces.
void *nextSymbol(const char *name, const char *version)
{
    void *scope = shimEmbedded ? RTLD_DEFAULT : RTLD_NEXT;
    return version ? dlvsym(scope, name, version) : dlsym(scope, name);
}

namespace {

link_map *mapOf(const void *address)
{
    Dl_info info;
    link_map *map = nullptr;
    return dladdr1(address, &info, reinterpret_cast<void **>(&map), RTLD_DL_LINKMAP) ? map : nullptr;
}

uintptr_t mainBase()
{
    uintptr_t base = 0;
    dl_iterate_phdr(
        [](dl_phdr_info *info, size_t, void *out) {
            *static_cast<uintptr_t *>(out) = info->dlpi_addr; // first entry = main program
            return 1;
        },
        &base);
    return base;
}

int pointGotAtShim(link_map *self)
{
    const int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0)
        return -1;
    const auto *file = static_cast<const char *>(mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd);
    if (file == MAP_FAILED)
        return -1;
    const auto *eh = reinterpret_cast<const Elf64_Ehdr *>(file);
    const auto *ph = reinterpret_cast<const Elf64_Phdr *>(file + eh->e_phoff);
    auto at = [&](Elf64_Addr vaddr) -> const char * {
        for (int i = 0; i < eh->e_phnum; ++i)
            if (ph[i].p_type == PT_LOAD && vaddr >= ph[i].p_vaddr && vaddr < ph[i].p_vaddr + ph[i].p_filesz)
                return file + ph[i].p_offset + (vaddr - ph[i].p_vaddr);
        return nullptr;
    };
    const Elf64_Dyn *dyn = nullptr;
    const Elf64_Phdr *relro = nullptr;
    for (int i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type == PT_DYNAMIC)
            dyn = reinterpret_cast<const Elf64_Dyn *>(file + ph[i].p_offset);
        if (ph[i].p_type == PT_GNU_RELRO)
            relro = &ph[i];
    }
    Elf64_Addr symtab = 0, strtab = 0, jmprel = 0, pltrelsz = 0, rela = 0, relasz = 0;
    for (; dyn && dyn->d_tag != DT_NULL; ++dyn)
        switch (dyn->d_tag) {
        case DT_SYMTAB: symtab = dyn->d_un.d_ptr; break;
        case DT_STRTAB: strtab = dyn->d_un.d_ptr; break;
        case DT_JMPREL: jmprel = dyn->d_un.d_ptr; break;
        case DT_PLTRELSZ: pltrelsz = dyn->d_un.d_val; break;
        case DT_RELA: rela = dyn->d_un.d_ptr; break;
        case DT_RELASZ: relasz = dyn->d_un.d_val; break;
        }
    const auto *syms = reinterpret_cast<const Elf64_Sym *>(at(symtab));
    const char *strs = at(strtab);
    const uintptr_t base = mainBase();
    const long page = sysconf(_SC_PAGESIZE);
    int patched = 0;
    for (auto [table, size] : {std::pair{jmprel, pltrelsz}, std::pair{rela, relasz}}) {
        const auto *r = reinterpret_cast<const Elf64_Rela *>(at(table));
        for (size_t i = 0; r && syms && strs && i < size / sizeof(Elf64_Rela); ++i) {
            const unsigned type = ELF64_R_TYPE(r[i].r_info);
            if (type != R_X86_64_JUMP_SLOT && type != R_X86_64_GLOB_DAT)
                continue;
            const Elf64_Sym &sym = syms[ELF64_R_SYM(r[i].r_info)];
            if (!sym.st_name || sym.st_shndx != SHN_UNDEF)
                continue;
            void *ours = dlsym(self, strs + sym.st_name);
            if (!ours || mapOf(ours) != self)
                continue;
            auto **slot = reinterpret_cast<void **>(base + r[i].r_offset);
            mprotect(reinterpret_cast<void *>(uintptr_t(slot) & ~uintptr_t(page - 1)), page, PROT_READ | PROT_WRITE);
            *slot = ours;
            ++patched;
        }
    }
    if (relro) {
        const uintptr_t start = (base + relro->p_vaddr) & ~uintptr_t(page - 1);
        const uintptr_t end = (base + relro->p_vaddr + relro->p_memsz) & ~uintptr_t(page - 1);
        mprotect(reinterpret_cast<void *>(start), end - start, PROT_READ);
    }
    munmap(const_cast<char *>(file), st.st_size);
    return patched;
}

// Before every other constructor here (standalone.cpp is 102).
// The stub opens this library as /proc/self/fd/<memfd>; a preloaded or
// DT_NEEDED copy has a real path. (dlsym(RTLD_DEFAULT) cannot tell them apart:
// called from here, it also searches this library's own local scope.)
__attribute__((constructor(101))) void detectEmbedding()
{
    link_map *self = mapOf(reinterpret_cast<const void *>(&mapOf));
    if (!self || !self->l_name || strncmp(self->l_name, "/proc/self/fd/", 14) != 0)
        return; // loaded by the dynamic loader: interposition works as usual
    // Another copy (LIVE_SHIM through LD_PRELOAD) does the work then.
    link_map *first = self;
    while (first->l_prev)
        first = first->l_prev;
    for (link_map *m = first; m; m = m->l_next)
        if (m != self && m->l_name && strstr(m->l_name, "libmenufix")) {
            shimActive = false;
            return;
        }
    shimEmbedded = true;
    const int patched = pointGotAtShim(self);
    menufixTrace("embedded: %d GOT entries point at the shim\n", patched);
}

} // namespace
