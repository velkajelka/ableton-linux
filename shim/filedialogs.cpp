// Desktop file dialogs for the Push 3 Live build.
//
// Live has three file-dialog entry points per platform (UiWidgets FileDialogs):
// open, save and choose-folder. Windows Live installs native IFileDialog
// versions at startup (DialogOsL_win.cpp, FUN_1431aec30); the Linux build
// installs Live's own drawn dialogs (0x22e5c23):
//   open   0x217ec70 (TText const &title, path const &dir, int kind,
//                     Receiver *receiver, ...)
//   save   0x217fbc0 (TText const &title, TText const &name, path const &dir,
//                     FileTypeMask mask, Receiver *receiver) -> int
//   folder 0x217f3d0 (path *result, TText const &title, path const &dir,
//                     bool) -> path
// All three run their dialog modally (0x217d930). On OK, open and save hand
// the path to the receiver (vtable +0x98); save then reports the file type
// (+0xa0, type from 0x1b387d0) and returns +0xa8. A cancelled dialog throws
// Live's silent exception, which its callers catch (all three do; without
// it Live goes on with an empty path: 'The file "" could not be opened').
//
// Live calls them through its dialog table, filled at startup (0x22e5c10):
// open and save are function pointers set through 0x1da6e70 / 0x1da6e80
// (stored at 0x9c08128 / 0x9c08130), choose-folder a std::function set
// through 0x205f620 (stored at 0x9c12e18), built from Live's own manager and
// invoker for a plain function pointer (0x13618e0 / 0x13618c0). This file
// puts QFileDialog versions into that table through the same setters, once
// Live has registered its own, as Windows Live puts its IFileDialog versions
// there; Live's code is not changed. With
// QT_QPA_PLATFORMTHEME=xdgdesktopportal (set by run-live-desktop.sh) Qt shows
// the desktop's own dialog through xdg-desktop-portal: KDE's on Plasma, GTK's
// on GNOME. Where no portal offers a FileChooser (a bare window manager), that
// theme would reject every dialog at once, so Qt's own dialog is used instead.
// LIVE_NATIVE_DIALOGS=0 keeps Live's dialogs.
//
// A path is a std::string (UTF-8). File kinds map to a 128-bit mask of file
// type ids (0x1be0b80); the ids, names and extensions below are Live's own
// file type table (registered in 0x1be15e0). Addresses are for Live 12.4.7.
#include <QApplication>
#include <QTimer>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QStringList>
#include <QWidget>
#include <cstdint>
#include <cstring>
#include <cxxabi.h>
#include <link.h>
#include <new>
#include <string>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);

extern "C" void *__cxa_allocate_exception(size_t) noexcept;
extern "C" void __cxa_throw(void *, void *, void (*)(void *));

namespace {

constexpr uintptr_t kOpen = 0x217ec70;
constexpr uintptr_t kSave = 0x217fbc0;
constexpr uintptr_t kFolder = 0x217f3d0;
constexpr uintptr_t kKindMask = 0x1be0b80;        // file kind -> file type mask
constexpr uintptr_t kTypeOfPath = 0x1b387d0;      // path -> file type id
constexpr uintptr_t kSilentException = 0x1ba8690; // constructor, 0x30 bytes
constexpr uintptr_t kSilentExceptionType = 0x9436bd0;
constexpr uintptr_t kSilentExceptionDtor = 0x1ba8900;

// The dialog table (see the top of the file).
constexpr uintptr_t kSetOpen = 0x1da6e70, kSetSave = 0x1da6e80, kSetFolder = 0x205f620;
constexpr uintptr_t kOpenSlot = 0x9c08128, kSaveSlot = 0x9c08130, kFolderSlot = 0x9c12e18;
constexpr uintptr_t kPointerManager = 0x13618e0, kPointerInvoker = 0x13618c0;

// libstdc++'s std::function: the stored callable, its manager, its invoker.
struct LiveFunction {
    void *callable, *unused;
    void (*manager)(LiveFunction *, const LiveFunction *, int);
    void *invoker;
};

struct FileType {
    int id;
    const char *name;
    const char *patterns;
};

// Live's file types with extensions (id, name, patterns).
const FileType kFileTypes[] = {
    {2, "Macintosh AIFF", "*.aif *.aiff"},
    {3, "Ableton Meta Sound", "*.ams"},
    {70, "Broadcast Wave", "*.bwf *.rf64"},
    {49, "Encrypted FLAC", "*.eflac"},
    {42, "EXS", "*.exs"},
    {34, "Lossless Audio", "*.flac"},
    {41, "GigaSampler", "*.gig"},
    {35, "MPEG 4 Audio", "*.m4a"},
    {5, "Standard MIDI File", "*.mid *.midi"},
    {28, "MPEG Audio Layer-3", "*.mp3"},
    {29, "Wave Simulation Text", "*.mp7"},
    {45, "NI Kontakt", "*.nki"},
    {33, "Ogg Vorbis", "*.ogg"},
    {36, "Propellerheads Rex", "*.rex *.rcy"},
    {37, "Propellerheads Rex", "*.rx2"},
    {4, "Sounddesigner II", "*.sd2"},
    {40, "SoundFont", "*.sf *.sf2"},
    {1, "Windows Wave", "*.wav"},
    {55, "WavPack", "*.wv"},
    {61, "Windows Media Audio", "*.wma"},
    {47, "Audio/Video", "*.avi"},
    {48, "Digital Video", "*.dv"},
    {50, "MPEG 4 Audio/Video", "*.mp4 *.m4v"},
    {46, "QuickTime Movie", "*.mov"},
    {62, "MPEG Media Video", "*.mpg *.m2v"},
    {60, "Windows Media Video", "*.wmv"},
    {44, "ISO Image", "*.iso"},
    {43, "Toast Image", "*.toast"},
    {7, "Bmp", "*.bmp"},
    {51, "JPEG graphics", "*.jpeg *.jpg"},
    {6, "Pcx", "*.pcx"},
    {57, "Portable Network Graphics", "*.png"},
    {8, "Tiff", "*.tif *.tiff"},
    {12, "Ableton Binary Live Set", "*.abl"},
    {17, "Ableton Device", "*.adv"},
    {18, "Ableton Device Group", "*.adg"},
    {16, "Ableton Groove", "*.agr"},
    {15, "Ableton Live Clip", "*.alc"},
    {11, "Ableton Live Set", "*.als"},
    {52, "Ableton Max Device", "*.amxd *.amp"},
    {25, "VST Bank", "*.fxb"},
    {26, "VST Preset", "*.fxp"},
    {71, "Scala Tuning", "*.ascl *.scl"},
    {53, "Ableton Instrument Rack Archive", "*.auz"},
    {9, "Ableton Language", "*.lng"},
    {20, "Ableton Log", "*.log"},
    {19, "Ableton Sample Data", "*.asd"},
    {64, "Ableton Push Cache", "*.appc"},
    {10, "Ableton Config", "*.cfg"},
    {23, "Ableton Text", "*.txt"},
    {22, "Ableton Skin", "*.ask"},
    {38, "XML", "*.xml"},
    {68, "Metadata File", "*.xmp"},
    {73, "Ableton Live Bundle", "*.ablx"},
    {21, "Executable", "*.exe *.com"},
    {63, "Ableton Analysis Data", "*.aad"},
    {67, "Band", "*.band"},
    {31, "Database", "*.db"},
    {54, "GNU Zip", "*.gz"},
    {30, "Index", "*.idx"},
    {39, "Ini", "*.ini"},
    {66, "JSON", "*.json"},
    {59, "Lock", "*.lock"},
    {32, "Ableton Live Pack", "*.alp"},
    {24, "Python", "*.py"},
    {56, "Ableton Remote Data", "*.ard"},
    {58, "Crash Dump", "*.dmp"},
    {65, "ZIP File", "*.zip"},
};

struct TypeMask {
    uint64_t lo, hi;
    bool has(int id) const { return id < 64 ? (lo >> id) & 1 : id < 128 && (hi >> (id - 64)) & 1; }
};

// Live's file receiver: +0x98 set path, +0xa0 set type, +0xa8 result.
using SetPathFn = void (*)(void *, const std::string *);
using SetTypeFn = void (*)(void *, int, bool);
using ResultFn = int (*)(void *);

uintptr_t base;

template <typename Fn>
Fn virtualAt(void *object, size_t offset)
{
    return *reinterpret_cast<Fn *>(*static_cast<uintptr_t *>(object) + offset);
}

// TText: pointer to a refcounted block {int refs; int bytes incl. NUL; ...;
// UTF-16 at +0x10}.
QString fromTText(const void *text)
{
    const auto block = text ? *static_cast<const uintptr_t *>(text) : 0;
    if (!block)
        return {};
    const int units = *reinterpret_cast<const int *>(block + 4) / 2 - 1;
    return units > 0 ? QString::fromUtf16(reinterpret_cast<const char16_t *>(block + 0x10), units) : QString();
}

QString fromPath(const std::string *path)
{
    return path ? QString::fromStdString(*path) : QString();
}

// "Ableton Live Set (*.als)" entries for the types in the mask; with more than
// one, an "All supported" entry first.
QStringList filtersFor(TypeMask mask, QStringList *allPatterns = nullptr)
{
    QStringList filters, patterns;
    for (const FileType &t : kFileTypes) {
        if (!mask.has(t.id))
            continue;
        filters << QStringLiteral("%1 (%2)").arg(QString::fromLatin1(t.name), QString::fromLatin1(t.patterns));
        patterns << QString::fromLatin1(t.patterns).split(' ');
    }
    if (filters.size() > 1)
        filters.prepend(QStringLiteral("All supported files (%1)").arg(patterns.join(' ')));
    if (allPatterns)
        *allPatterns = patterns;
    return filters;
}

QWidget *dialogParent()
{
    if (QWidget *w = QApplication::activeWindow())
        return w;
    for (QWidget *w : QApplication::topLevelWidgets())
        if (w->isVisible() && w->windowType() == Qt::Window)
            return w;
    return nullptr;
}

// Qt's own dialog when the portal theme is in use but no portal FileChooser
// answers; checked once, at the first dialog.
QFileDialog::Options dialogOptions()
{
    static const bool portal = [] {
        if (qEnvironmentVariable("QT_QPA_PLATFORMTHEME") != QLatin1String("xdgdesktopportal"))
            return true;
        QDBusMessage get = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.portal.Desktop"), QStringLiteral("/org/freedesktop/portal/desktop"),
            QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
        get << QStringLiteral("org.freedesktop.portal.FileChooser") << QStringLiteral("version");
        const bool ok = QDBusConnection::sessionBus().call(get, QDBus::Block, 5000).type() == QDBusMessage::ReplyMessage;
        menufixTrace("filedialogs: portal FileChooser %d\n", ok);
        return ok;
    }();
    return portal ? QFileDialog::Options() : QFileDialog::DontUseNativeDialog;
}

QString startDir(const std::string *dir)
{
    const QString d = fromPath(dir);
    return d.isEmpty() ? QDir::homePath() : d;
}

void throwSilent()
{
    void *exception = __cxa_allocate_exception(0x30);
    reinterpret_cast<void (*)(void *)>(base + kSilentException)(exception);
    __cxa_throw(exception, reinterpret_cast<void *>(base + kSilentExceptionType),
                reinterpret_cast<void (*)(void *)>(base + kSilentExceptionDtor));
}

void openFile(const void *title, const std::string *dir, int kind, void *receiver)
{
    const auto mask = reinterpret_cast<TypeMask (*)(int)>(base + kKindMask)(kind);
    const QString file = QFileDialog::getOpenFileName(dialogParent(), fromTText(title), startDir(dir),
                                                      filtersFor(mask).join(QStringLiteral(";;")), nullptr,
                                                      dialogOptions());
    menufixTrace("filedialogs: open kind %d -> %d\n", kind, !file.isEmpty());
    if (file.isEmpty())
        throwSilent();
    const std::string path = file.toStdString();
    virtualAt<SetPathFn>(receiver, 0x98)(receiver, &path);
}

int saveFile(const void *title, const void *name, const std::string *dir, uint64_t maskLo, uint64_t maskHi,
             void *receiver)
{
    const TypeMask mask{maskLo, maskHi};
    QStringList patterns;
    const QStringList filters = filtersFor(mask, &patterns);
    QString selected = filters.size() > 1 ? filters.at(1) : filters.value(0);
    const QString initial = QDir(startDir(dir)).filePath(fromTText(name));
    QString file = QFileDialog::getSaveFileName(dialogParent(), fromTText(title), initial,
                                                filters.join(QStringLiteral(";;")), &selected, dialogOptions());
    menufixTrace("filedialogs: save -> %d\n", !file.isEmpty());
    if (file.isEmpty())
        throwSilent();
    // Add the extension of the chosen type when the name has none of the
    // allowed ones, as Live's own dialog does.
    bool known = patterns.isEmpty();
    for (const QString &p : patterns)
        known |= file.endsWith(p.mid(1), Qt::CaseInsensitive);
    if (!known) {
        const int open = selected.lastIndexOf('('), close = selected.lastIndexOf(')');
        const QString first = selected.mid(open + 1, close - open - 1).split(' ').value(0);
        if (first.startsWith(QLatin1String("*.")))
            file += first.mid(1);
    }
    const std::string path = file.toStdString();
    virtualAt<SetPathFn>(receiver, 0x98)(receiver, &path);
    const int type = reinterpret_cast<int (*)(const std::string *)>(base + kTypeOfPath)(&path);
    virtualAt<SetTypeFn>(receiver, 0xa0)(receiver, type, true);
    return virtualAt<ResultFn>(receiver, 0xa8)(receiver);
}

std::string *chooseFolder(std::string *result, const void *title, const std::string *dir, bool)
{
    const QString folder = QFileDialog::getExistingDirectory(dialogParent(), fromTText(title), startDir(dir),
                                                             QFileDialog::ShowDirsOnly | dialogOptions());
    menufixTrace("filedialogs: folder -> %d\n", !folder.isEmpty());
    if (folder.isEmpty())
        throwSilent();
    return new (result) std::string(folder.toStdString());
}

uintptr_t mainExecutableBase()
{
    uintptr_t b = 0;
    dl_iterate_phdr(
        [](dl_phdr_info *info, size_t, void *out) {
            *static_cast<uintptr_t *>(out) = info->dlpi_addr;
            return 1;
        },
        &b);
    return b;
}

} // namespace

// True once Live's own dialogs are in the table; then ours replace them.
bool registerDialogs()
{
    auto slot = [](uintptr_t at) { return *reinterpret_cast<uintptr_t *>(base + at); };
    if (slot(kOpenSlot) != base + kOpen || slot(kSaveSlot) != base + kSave || slot(kFolderSlot) != base + kFolder)
        return false;
    reinterpret_cast<void (*)(void *)>(base + kSetOpen)(reinterpret_cast<void *>(&openFile));
    reinterpret_cast<void (*)(void *)>(base + kSetSave)(reinterpret_cast<void *>(&saveFile));
    LiveFunction folder{reinterpret_cast<void *>(&chooseFolder), nullptr,
                        reinterpret_cast<void (*)(LiveFunction *, const LiveFunction *, int)>(base + kPointerManager),
                        reinterpret_cast<void *>(base + kPointerInvoker)};
    reinterpret_cast<void (*)(LiveFunction *)>(base + kSetFolder)(&folder);
    folder.manager(&folder, &folder, 3); // destroy the source, as Live does after its own call
    return true;
}

void installFileDialogs()
{
    if (qEnvironmentVariable("LIVE_NATIVE_DIALOGS") == QLatin1String("0"))
        return;
    base = mainExecutableBase();
    // Live fills the table during its startup, after this runs: look from
    // the event loop until its dialogs are there (one minute at most).
    auto *timer = new QTimer(qApp);
    QObject::connect(timer, &QTimer::timeout, [timer] {
        static int tries;
        const bool done = registerDialogs();
        if (done || ++tries >= 600) {
            menufixTrace("filedialogs: registered %d after %d tries\n", done, tries);
            timer->deleteLater();
        }
    });
    timer->start(100);
}
