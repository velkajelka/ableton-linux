// Lets AbletonLive12 start on its own, without run-live-desktop.sh.
//
// Two ways to run: AbletonLive12 is the launcher (tools/launcher.c), which
// preloads this shim into the unmodified Live; or AbletonLive12 is Live
// patched by tools/patch-live.py, carrying this shim inside its own file
// (shim/embedded.cpp, tools/embed-stub.S) and an RPATH ($ORIGIN/lib,
// $ORIGIN/../../compat_lib), so the loader finds the bundled libraries by
// itself. With the launcher, the byte patches are made here in memory
// (applyBytePatches). What the launcher script used to do with environment
// variables is done here, before main():
//   - Qt: bundled plug-ins and fonts, the xcb platform (the bundled Qt has no
//     Wayland plug-in), desktop file dialogs through xdg-desktop-portal
//     (LIVE_NATIVE_DIALOGS=0: Live's own);
//   - LC_NUMERIC=C: Live reads Live Set numbers with the locale's strtod;
//   - LIVEDEBUGOPTIONS: Options-desktop.txt next to push3_opt/;
//   - HOME: the user's own, as for any Linux program (Live then keeps its
//     settings in ~/.config/Ableton and ~/.local/share/Ableton, and its
//     libraries in the desktop's Music folder, XDG_MUSIC_DIR/Ableton), or an
//     isolated data folder: LIVE_DATA_DIR, or live-home/ next to push3_opt/
//     when that folder exists (the portable build, a development tree); the
//     real home then stays in LIVE_USER_HOME;
//   - LD_LIBRARY_PATH for Live's helper programs (Ableton Index, Plugin
//     Scanner, Web Connector), which have no RPATH of their own;
//   - the KWin rule for plug-in editors (tools/kwin-plugin-rule.sh), run in
//     the background with the real HOME;
//   - the working directory: push3_opt/push3, as the launcher had it.
// Variables that are already set (by the launcher, or by the user) are kept,
// except the Qt paths, which must point at the bundled Qt.
#include <climits>
#include <cstdint>
#include <dlfcn.h>
#include <link.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <spawn.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern __attribute__((visibility("hidden"))) bool shimActive; // embedded.cpp
void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);
bool patchBytes(uintptr_t at, const unsigned char *expected, const unsigned char *replacement, size_t len,
                int protection); // hook.cpp

namespace {

uintptr_t programBase()
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

// The byte patches tools/patch-live.py writes into the file (FINDINGS.md),
// made in memory for an unmodified Live, before its static constructors:
//   0x3d56530, 0x3e943d8  plug-in editor position (no drift, no offset)
//   0x760c9fc             menu window kind Qt::Popup -> Qt::ToolTip
void applyBytePatches(uintptr_t base)
{
    static const struct {
        uintptr_t at;
        int protection;
        size_t len;
        unsigned char expected[9], replacement[9];
    } patches[] = {
        {0x3d56530, PROT_READ | PROT_EXEC, 1, {0x83}, {0xc3}},
        {0x3e943d8, PROT_READ | PROT_EXEC, 9, {0x8b, 0x4c, 0x24, 0x30, 0x44, 0x8b, 0x44, 0x24, 0x34},
         {0x31, 0xc9, 0x66, 0x90, 0x45, 0x31, 0xc0, 0x66, 0x90}},
        {0x760c9fc, PROT_READ, 1, {0x09}, {0x0d}},
    };
    int done = 0;
    for (const auto &p : patches)
        done += patchBytes(base + p.at, p.expected, p.replacement, p.len, p.protection);
    menufixTrace("standalone: byte patches %d of 3\n", done);
}

std::string dirName(const std::string &path)
{
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

void setDefault(const char *name, const std::string &value)
{
    setenv(name, value.c_str(), 0);
}

bool isBundledPlatform(const char *name)
{
    for (const char *p : {"xcb", "offscreen", "minimal", "minimalegl", "vnc", "linuxfb", "eglfs"})
        if (!strcmp(name, p))
            return true;
    return false;
}

// Qt's X11 plugin (libQt5XcbQpa, loaded when the QApplication starts) needs
// libxkbcommon and libxkbcommon-x11 of one version: the x11 part builds keymaps
// with the other's internal structures, and a mixed pair crashed Live on the
// first key press. The system's pair is loaded here when it has both (found by
// soname: the package keeps no copy in Live's library folders), so plug-ins
// that need a newer xkbcommon get it too. Without the x11 part (some GTK
// desktops), the pair in lib/xkbcommon (1.5.0, tools/bundled/README) is loaded
// instead; Qt 5.15 needs nothing newer.
void loadXkbcommon(const std::string &app)
{
    if (dlopen("libxkbcommon-x11.so.0", RTLD_NOW | RTLD_GLOBAL)) {
        menufixTrace("standalone: system xkbcommon\n");
        return;
    }
    const std::string folder = app + "/lib/xkbcommon/";
    if (dlopen((folder + "libxkbcommon.so.0").c_str(), RTLD_NOW | RTLD_GLOBAL) &&
        dlopen((folder + "libxkbcommon-x11.so.0").c_str(), RTLD_NOW | RTLD_GLOBAL))
        menufixTrace("standalone: bundled xkbcommon 1.5.0\n");
}

void runKwinRule(const std::string &root, const char *userHome)
{
    const std::string script = root + "/tools/kwin-plugin-rule.sh";
    if (access(script.c_str(), X_OK) != 0)
        return;
    // Double fork, so the script never lingers as Live's child.
    const pid_t child = fork();
    if (child == 0) {
        if (fork() == 0) {
            setenv("HOME", userHome, 1);
            unsetenv("LD_LIBRARY_PATH");
            // This runs before menufix.cpp drops the launcher's LD_PRELOAD:
            // kreadconfig6 and kwriteconfig6 (Qt 6) fail with this Qt 5 shim in them.
            unsetenv("LD_PRELOAD");
            execl(script.c_str(), script.c_str(), static_cast<char *>(nullptr));
        }
        _exit(0);
    }
    if (child > 0)
        waitpid(child, nullptr, 0);
}

// Desktop window instead of Push: the view's paint and input handlers
// (TQtView, 0x1ff6748..0x1ff7454) skip presenting and drop mouse and key
// input in final release builds: Live's version type (AVersionInformation::
// VersionType, 0x1b35be0) is 4, TVersionType::KFinalVersion; Ableton's
// internal builds show the window. All eight checks read one cached copy of
// the version type (0x9c0e638, a function-local static with its guard at
// 0x9c0e640); nothing else reads that copy. It is filled here with 0 before
// any window exists, so Live's own handlers take their window path. The 18
// other callers of 0x1b35be0 (window title "Beta", support e-mail address,
// debug assertions, AutomationCapture's JSON dumps) still get 4, a final
// release. Checked against the first test's bytes (cmp dword [rip+..], 4).
void selectDesktopViews(uintptr_t base)
{
    const unsigned char check[] = {0x83, 0x3d, 0xdd, 0x71, 0xc1, 0x07, 0x04};
    auto *productType = reinterpret_cast<int32_t *>(base + 0x9c0e638);
    auto *guard = reinterpret_cast<uint8_t *>(base + 0x9c0e640);
    if (memcmp(reinterpret_cast<void *>(base + 0x1ff7454), check, sizeof check) != 0 || *guard)
        return;
    *productType = 0;
    *guard = 1;
}

__attribute__((constructor(102))) void setUpEnvironment()
{
    if (!shimActive)
        return;
    const uintptr_t base = programBase();
    applyBytePatches(base);
    selectDesktopViews(base);
    char exe[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0)
        return;
    exe[n] = '\0';
    const std::string app = dirName(exe);                 // push3_opt/push3
    char resolved[PATH_MAX];
    const std::string root = realpath((app + "/../..").c_str(), resolved) ? resolved : app + "/../..";

    setenv("QT_PLUGIN_PATH", (app + "/plugins").c_str(), 1);
    setenv("QT_QPA_FONTDIR", (app + "/lib/fonts").c_str(), 1);
    const char *platform = getenv("QT_QPA_PLATFORM");
    if (!platform || !isBundledPlatform(platform))
        setenv("QT_QPA_PLATFORM", "xcb", 1);
    // Live turns on Qt's high-DPI scaling itself; Qt 5 rounds the desktop's
    // scale to a whole number (125 % gave 1x, 150 to 200 % all 2x). Keep the
    // exact factor, so Live scales with the desktop as on Windows.
    setDefault("QT_SCALE_FACTOR_ROUNDING_POLICY", "PassThrough");
    const char *dialogs = getenv("LIVE_NATIVE_DIALOGS");
    if (!dialogs || strcmp(dialogs, "0") != 0)
        setDefault("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
    // Live's PipeWire streams (pipewire-alsa reads this) show as "Ableton Live"
    // in the desktop's volume mixer instead of "PipeWire ALSA [AbletonLive12]".
    loadXkbcommon(app);
    setDefault("PIPEWIRE_ALSA", "{ application.name = \"Ableton Live\" media.role = \"Production\" }");
    setenv("LC_NUMERIC", "C", 1);
    if (getenv("LC_ALL") && *getenv("LC_ALL"))
        setenv("LC_ALL", "C.UTF-8", 1);
    unsetenv("FACELESS_ROOT");

    if (!getenv("LIVEDEBUGOPTIONS")) {
        const std::string options = root + "/Options-desktop.txt";
        if (access(options.c_str(), R_OK) != 0)
            if (FILE *f = fopen(options.c_str(), "w")) {
                fputs("-_LinuxMidiBackend=Alsa\n-SuppressStartUpMessages\n-_DisableAutoUpdates\n", f);
                fclose(f);
            }
        setenv("LIVEDEBUGOPTIONS", options.c_str(), 1);
    }

    std::string libs = app + "/lib:" + root + "/compat_lib";
    if (const char *old = getenv("LD_LIBRARY_PATH"))
        if (!strstr(old, libs.c_str()))
            libs += std::string(":") + old;
    setenv("LD_LIBRARY_PATH", libs.c_str(), 1);

    // The launcher already switched HOME when LIVE_USER_HOME is set.
    if (!getenv("LIVE_USER_HOME")) {
        const char *home = getenv("HOME");
        const std::string userHome = home ? home : "";
        const char *dataDir = getenv("LIVE_DATA_DIR");
        struct stat st;
        std::string data = dataDir && *dataDir ? dataDir : root + "/live-home";
        if (!(dataDir && *dataDir) && (stat(data.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)))
            data.clear(); // no isolated folder: the user's own home
        if (!data.empty())
            mkdir(data.c_str(), 0755);
        setenv("LIVE_USER_HOME", userHome.c_str(), 1);
        if (!data.empty())
            setenv("HOME", data.c_str(), 1);
        if (!userHome.empty())
            runKwinRule(root, userHome.c_str());
    }

    if (chdir(app.c_str()) != 0)
        perror("AbletonLive12: chdir");
}

} // namespace

// Live's own desktop registration. At every start Live writes a menu entry
// ("Live", Ableton-Live.desktop) and Ableton-File-Types.xml into its program
// folder and installs them with xdg-desktop-menu and xdg-mime. That entry
// starts the bare Live binary (without the launcher, so without this shim)
// and claims ableton:// links and Live Sets, so in the user's own home it
// would shadow the menu entry install.sh writes. In an isolated home it only
// failed or cluttered that folder (a folder name with a space made
// xdg-desktop-menu fail, and Live showed an error dialog at every start).
// Live starts programs through execv, execve, posix_spawn(p) and system; for
// those two commands each is answered with success without running them.
// Everything else, xdg-open included, runs as before.
void *nextSymbol(const char *name, const char *version); // embedded.cpp

namespace {

bool isRegistration(const char *program)
{
    if (!program || !shimActive)
        return false;
    const char *end = program + strcspn(program, " \t");
    const char *name = program;
    for (const char *p = program; p < end; ++p)
        if (*p == '/')
            name = p + 1;
    const size_t len = size_t(end - name);
    return (len == 16 && !strncmp(name, "xdg-desktop-menu", len)) || (len == 8 && !strncmp(name, "xdg-mime", len));
}

// The program itself, or a shell running it: sh -c "xdg-mime install ...".
bool runsRegistration(const char *path, char *const argv[])
{
    if (isRegistration(path))
        return true;
    if (argv && argv[0] && argv[1] && argv[2] && !strcmp(argv[1], "-c"))
        return isRegistration(argv[2] + strspn(argv[2], " \t"));
    return false;
}

template <typename Fn> Fn real(const char *name, const char *version)
{
    return reinterpret_cast<Fn>(nextSymbol(name, version));
}

} // namespace

extern "C" int execv(const char *path, char *const argv[])
{
    static auto next = real<int (*)(const char *, char *const[])>("execv", "GLIBC_2.2.5");
    if (runsRegistration(path, argv))
        _exit(0); // in Live's child process, before the exec
    return next(path, argv);
}

extern "C" int execve(const char *path, char *const argv[], char *const envp[])
{
    static auto next = real<int (*)(const char *, char *const[], char *const[])>("execve", "GLIBC_2.2.5");
    if (runsRegistration(path, argv))
        _exit(0);
    return next(path, argv, envp);
}

// A spawned process has to exist (the caller waits for it): `true` runs instead.
extern "C" int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                           const posix_spawnattr_t *attr, char *const argv[], char *const envp[])
{
    static auto next = real<int (*)(pid_t *, const char *, const posix_spawn_file_actions_t *,
                                    const posix_spawnattr_t *, char *const[], char *const[])>("posix_spawn", "GLIBC_2.15");
    static char *const trueArgv[] = {const_cast<char *>("true"), nullptr};
    if (runsRegistration(path, argv))
        return next(pid, "/bin/true", actions, attr, trueArgv, envp);
    return next(pid, path, actions, attr, argv, envp);
}

extern "C" int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *actions,
                            const posix_spawnattr_t *attr, char *const argv[], char *const envp[])
{
    static auto next = real<int (*)(pid_t *, const char *, const posix_spawn_file_actions_t *,
                                    const posix_spawnattr_t *, char *const[], char *const[])>("posix_spawnp", "GLIBC_2.15");
    static char *const trueArgv[] = {const_cast<char *>("true"), nullptr};
    if (runsRegistration(file, argv))
        return next(pid, "true", actions, attr, trueArgv, envp);
    return next(pid, file, actions, attr, argv, envp);
}

extern "C" int system(const char *command)
{
    static auto next = real<int (*)(const char *)>("system", "GLIBC_2.2.5");
    if (command) {
        const char *start = command + strspn(command, " \t");
        if (isRegistration(start))
            return 0;
    }
    return next(command);
}
