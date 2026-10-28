// Window-manager integration for AbletonLive12's top-level windows.
//
// Windows Live is a native Win32 app: its windows carry the app icon, and it
// answers WM_GETMINMAXINFO, so the shell knows the minimum window size before
// a resize starts. The Push build's Qt windows have neither: no _NET_WM_ICON,
// WM_CLASS "AbletonLive12", and no minimum size in WM_NORMAL_HINTS. Live keeps
// its minimum (about 800x620) by resizing the window back after the window
// manager made it smaller, so every interactive resize below the limit
// fights the window manager.
//
// This file sets the app icon (exported from the Windows build by
// tools/make_linux_assets.py), names the window class, and publishes Live's
// own minimum: after every frame of a window, the minimum Live clamps the
// window to (AWindow's virtual at +0x200, in Live's units; the clamp is
// 0x20c3de0) is converted to pixels as Live converts it (x zoom x the
// window's scale, virtual +0x588) and set as the window's minimum size, so
// the window manager stops at it. A minimum is never larger than the
// screen's work area.
//
// Zoom changes: Live's SetZoom (0x20c4110) keeps a window's size by
// converting it to Live's units and back, and resizes the window to the
// result, which is off by a rounding error (1920x1173 for 1172 at 130 %), even
// when the window is maximized. Repeated zoom changes made a normal window
// creep, and a maximized one could be pushed past the work area. During
// SetZoom such a resize is dropped: the window keeps its pixel size, as on
// Windows. Any resize Live asks for is limited to the work area of the
// window's screen (with the window frame), so a window Live restores or
// enlarges never ends up under a panel or past the screen edge. The work area
// is read in X pixels (KWin on Wayland reports it in scaled units), and a
// maximized or full-screen window is left at the window manager's size.
//
// Plug-in editor windows: Windows Live creates them as windows owned by the
// main window, so they stay above it and minimize with it. The Push build
// creates them as plain top-level X windows on its own Xlib connection, so
// clicking Live's main window buries the editor behind it. The editor
// windows (seen by pluginclose.cpp's XCreateWindow interposer) are made
// transient for Live's active main window, with Live's window class and icon.
#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QMainWindow>
#include <QPointer>
#include <QScreen>
#include <QThread>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <link.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);
void *nextSymbol(const char *name, const char *version); // embedded.cpp
void *hookFunction(uintptr_t fn, const unsigned char *prologue, size_t len, void *replacement); // hook.cpp
bool scaleTransition(); // scalechange.cpp

namespace {

bool isLiveMainWindow(QWidget *w)
{
    return w && w->isWindow() && w->windowType() == Qt::Window && w->inherits("QMainWindow");
}

// The Live main window whose view is being painted (see liveWindowPainted).
QPointer<QWidget> paintingWindow;

class WindowFilter : public QObject
{
public:
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (e->type() == QEvent::Paint && o->isWidgetType()) {
            QWidget *window = static_cast<QWidget *>(o)->window();
            if (isLiveMainWindow(window) && static_cast<QMainWindow *>(window)->centralWidget() == o)
                paintingWindow = window;
        }
        return false;
    }
};

void setAppIcon()
{
    const QDir dir(QCoreApplication::applicationDirPath() + "/products/live/Live/AppLive/Linux/Icons");
    QIcon icon;
    for (const QString &f : dir.entryList({"live_*.png"}))
        icon.addFile(dir.filePath(f));
    if (!icon.isNull())
        QApplication::setWindowIcon(icon);
}

// Motif hints (_MOTIF_WM_HINTS): flags, functions, decorations, input mode,
// status. Removes the minimize function and keeps everything else, merged
// with whatever the window already has. Mutter, Xfwm, Openbox and most other
// window managers then show no minimize button; KWin ignores the bit
// (tools/kwin-plugin-rule.sh).
void setNoMinimize(Display *dpy, Window window)
{
    constexpr unsigned long kHintsFunctions = 1, kFuncAll = 1, kFuncMinimize = 8;
    const Atom atom = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
    unsigned long hints[5] = {};
    Atom type;
    int format;
    unsigned long items, rest;
    unsigned char *data = nullptr;
    if (XGetWindowProperty(dpy, window, atom, 0, 5, False, atom, &type, &format, &items, &rest, &data) == Success
        && data && format == 32)
        memcpy(hints, data, std::min<unsigned long>(items, 5) * sizeof(long));
    if (data)
        XFree(data);
    if (!(hints[0] & kHintsFunctions))
        hints[1] = kFuncAll; // no function hint yet: all functions allowed
    // With MWM_FUNC_ALL set the listed functions are the ones removed.
    hints[1] = (hints[1] & kFuncAll) ? hints[1] | kFuncMinimize : hints[1] & ~kFuncMinimize;
    hints[0] |= kHintsFunctions;
    XChangeProperty(dpy, window, atom, atom, 32, PropModeReplace, reinterpret_cast<unsigned char *>(hints), 5);
}

// Root window property `name` (32-bit cardinals), empty when missing.
std::vector<long> rootCardinals(Display *dpy, const char *name)
{
    std::vector<long> values;
    Atom type;
    int format;
    unsigned long items, rest;
    unsigned char *data = nullptr;
    if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), XInternAtom(dpy, name, False), 0, 1024, False, XA_CARDINAL,
                           &type, &format, &items, &rest, &data)
            == Success
        && data && format == 32)
        values.assign(reinterpret_cast<long *>(data), reinterpret_cast<long *>(data) + items);
    if (data)
        XFree(data);
    return values;
}

// The work area of `screen` in X pixels.
//
// KWin on Wayland writes _NET_WORKAREA and _NET_DESKTOP_GEOMETRY in its own
// scaled units, not in X pixels, when Xwayland clients scale themselves
// (Plasma's default): a 1920x1080 screen at 125 % has the work area 0,0
// 1536x864. Qt takes it as pixels, so its availableGeometry() was 1/1.25 of the
// screen. After the desktop scale was changed while Live ran, maximizing Live
// gave a 1920x1045 window with Live drawn at 1536x836 in its corner and the
// rest black: Live asks for its window size again and again, and each request
// was cut to that work area. The work area is scaled back by the ratio of the
// root window's size to _NET_DESKTOP_GEOMETRY (1 elsewhere: X11 sessions,
// Plasma's legacy scaling, other window managers).
QRect nativeWorkArea(QScreen *screen)
{
    // Qt 5 keeps a screen's top-left in pixels and scales only its size.
    const QRect screenRect(screen->geometry().topLeft(), screen->geometry().size() * screen->devicePixelRatio());
    static Display *dpy = XOpenDisplay(nullptr);
    if (!dpy)
        return screenRect;
    const std::vector<long> area = rootCardinals(dpy, "_NET_WORKAREA"); // x, y, w, h per desktop
    const std::vector<long> current = rootCardinals(dpy, "_NET_CURRENT_DESKTOP");
    const size_t at = current.empty() ? 0 : size_t(current[0]) * 4;
    if (area.size() < at + 4)
        return screenRect;
    qreal units = 1;
    const std::vector<long> desktop = rootCardinals(dpy, "_NET_DESKTOP_GEOMETRY");
    Window root;
    int x, y;
    unsigned int rootWidth, rootHeight, border, depth;
    if (desktop.size() >= 2 && desktop[0] > 0
        && XGetGeometry(dpy, DefaultRootWindow(dpy), &root, &x, &y, &rootWidth, &rootHeight, &border, &depth))
        units = qreal(rootWidth) / desktop[0];
    static int tracedUnits = 100;
    if (qRound(units * 100) != tracedUnits) {
        tracedUnits = qRound(units * 100);
        menufixTrace("window: work area reported in units of %d/100 px\n", tracedUnits);
    }
    const QRect work(qRound(area[at] * units), qRound(area[at + 1] * units), qRound(area[at + 2] * units),
                     qRound(area[at + 3] * units));
    const QRect fit = work & screenRect;
    return fit.isEmpty() ? screenRect : fit;
}

// Largest size of `w` (without its frame) that fits its screen's work area.
QSize workAreaSize(QWidget *w)
{
    QScreen *screen = w->screen();
    if (!screen)
        return {QWIDGETSIZE_MAX, QWIDGETSIZE_MAX};
    const QSize frame = w->isVisible() ? w->frameGeometry().size() - w->size() : QSize();
    return nativeWorkArea(screen).size() / w->devicePixelRatioF() - frame;
}

constexpr uintptr_t kSetZoom = 0x20c4110; // (window zoom state *, float zoom)
const unsigned char kSetZoomPrologue[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                          0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x68};
using SetZoomFn = void (*)(void *, float);

// AWindow (a window's view manager): minimum size in Live's units, packed
// w | h << 32 (the maximum is the next slot); the window's scale (Live's
// units to pixels before zoom); the zoom at zoom state (+0xf0) + 0xd4.
constexpr size_t kMinimumSizeSlot = 0x200;
constexpr size_t kScaleSlot = 0x588;
constexpr size_t kZoomState = 0xf0;
constexpr size_t kZoom = 0xd4;
SetZoomFn liveSetZoom;
int zoomChanging; // depth of SetZoom calls on the GUI thread

void setZoom(void *state, float zoom)
{
    const bool gui = qApp && QThread::currentThread() == qApp->thread();
    if (gui)
        ++zoomChanging;
    liveSetZoom(state, zoom);
    if (gui)
        --zoomChanging;
}

uintptr_t mainBase()
{
    uintptr_t b = 0;
    dl_iterate_phdr(
        [](dl_phdr_info *info, size_t, void *out) {
            *static_cast<uintptr_t *>(out) = info->dlpi_addr; // first entry = main program
            return 1;
        },
        &b);
    return b;
}

} // namespace

// Live calls QWidget::resize through its PLT with symbol version Qt_5; the
// version script (shim/qt5.map) gives this definition that version, so the
// preload wins the lookup. The real one is fetched with dlvsym.
extern "C" void _ZN7QWidget6resizeERK5QSize(QWidget *w, const QSize &size)
{
    using ResizeFn = void (*)(QWidget *, const QSize &);
    static auto real = reinterpret_cast<ResizeFn>(nextSymbol("_ZN7QWidget6resizeERK5QSize", "Qt_5"));
    if (!isLiveMainWindow(w))
        return real(w, size);
    if (scaleTransition())
        return; // Live's answer to a window the window manager scaled; scalechange.cpp restores it
    if (zoomChanging) {
        const QSize delta = size - w->size();
        if ((w->windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen))
            || (qAbs(delta.width()) <= 2 && qAbs(delta.height()) <= 2))
            return; // the zoom change's rounding error; keep the window as it is
    }
    // A maximized or full-screen window has the size the window manager gave it.
    const QSize fit = (w->windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen))
        ? size
        : size.boundedTo(workAreaSize(w));
    if (fit != size)
        menufixTrace("window: %dx%d limited to the work area\n", size.width(), size.height());
    real(w, fit);
}

// Called after Live has painted a frame of a window's view (from the paint
// handlers in nativemenu.cpp and glsurface.cpp), with the window's view
// manager (Live's AWindow).
void liveWindowPainted(void *viewManager)
{
    QWidget *w = paintingWindow;
    paintingWindow = nullptr;
    if (!w || !viewManager)
        return;
    auto *vtable = *reinterpret_cast<void ***>(viewManager);
    const uint64_t minimum = reinterpret_cast<uint64_t (*)(void *)>(vtable[kMinimumSizeSlot / 8])(viewManager);
    const float scale = reinterpret_cast<float (*)(void *)>(vtable[kScaleSlot / 8])(viewManager);
    const float zoom = *reinterpret_cast<float *>(*reinterpret_cast<char **>(static_cast<char *>(viewManager) + kZoomState) + kZoom);
    const QSize size = QSize(qRound(int32_t(minimum) * zoom * scale), qRound(int32_t(minimum >> 32) * zoom * scale))
                           .boundedTo(workAreaSize(w));
    if (size.isValid() && size != w->minimumSize()) {
        menufixTrace("window: minimum size %dx%d\n", size.width(), size.height());
        w->setMinimumSize(size);
    }
}

void liveOwnPluginWindow(Display *dpy, Window editor)
{
    if (!qApp || QThread::currentThread() != qApp->thread())
        return;
    QWidget *owner = QApplication::activeWindow();
    if (!isLiveMainWindow(owner)) {
        owner = nullptr;
        for (QWidget *w : QApplication::topLevelWidgets())
            if (isLiveMainWindow(w) && w->isVisible()) {
                owner = w;
                break;
            }
    }
    // Own instance name, so a KWin rule can match editors only
    // (tools/kwin-plugin-rule.sh: no minimize button); same class for grouping.
    XClassHint hint = {const_cast<char *>("ableton-live-plugin"), const_cast<char *>("Ableton Live 12")};
    XSetClassHint(dpy, editor, &hint);
    setNoMinimize(dpy, editor);
    if (!owner)
        return;
    const Window ownerId = owner->winId();
    XSetTransientForHint(dpy, editor, ownerId);
    // Same icon as Live's windows.
    const Atom icon = XInternAtom(dpy, "_NET_WM_ICON", False);
    Atom type;
    int format;
    unsigned long items, rest;
    unsigned char *data = nullptr;
    if (XGetWindowProperty(dpy, ownerId, icon, 0, 1 << 20, False, XA_CARDINAL, &type, &format, &items, &rest, &data)
            == Success
        && data && items) {
        XChangeProperty(dpy, editor, icon, XA_CARDINAL, 32, PropModeReplace, data, int(items));
    }
    if (data)
        XFree(data);
    menufixTrace("window: plug-in editor 0x%x owned by 0x%x\n", int(editor), int(ownerId));
}

namespace {

// Diagnostics for Qt warnings about Live's windows ("QWidget::setMinimumSize:
// ... Negative sizes"): with MENUFIX_DEBUG set, each one is written there with
// the call stack as offsets into AbletonLive12.
QtMessageHandler previousHandler;

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    const char *path = getenv("MENUFIX_DEBUG");
    if (path && type == QtWarningMsg && message.startsWith("QWidget::")) {
        if (FILE *f = fopen(path, "a")) {
            uintptr_t base = 0;
            dl_iterate_phdr([](dl_phdr_info *i, size_t, void *o) { *static_cast<uintptr_t *>(o) = i->dlpi_addr; return 1; },
                            &base);
            void *frames[24];
            const int n = backtrace(frames, 24);
            fprintf(f, "qt: %s\n  stack:", message.toLocal8Bit().constData());
            for (int i = 1; i < n; ++i) {
                Dl_info info;
                if (dladdr(frames[i], &info) && info.dli_fname && !strstr(info.dli_fname, "Live"))
                    fprintf(f, " %s+%lx", strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname,
                            uintptr_t(frames[i]) - uintptr_t(info.dli_fbase));
                else
                    fprintf(f, " Live+%lx", uintptr_t(frames[i]) - base);
            }
            fprintf(f, "\n");
            fclose(f);
        }
    }
    if (previousHandler)
        previousHandler(type, context, message);
    else
        fprintf(stderr, "%s\n", message.toLocal8Bit().constData());
}

} // namespace

void installWindowIntegration()
{
    previousHandler = qInstallMessageHandler(messageHandler);
    // WM_CLASS: Qt takes the instance from RESOURCE_NAME and the class from the
    // application name; both are read when the first window is created.
    if (!qEnvironmentVariableIsSet("RESOURCE_NAME"))
        qputenv("RESOURCE_NAME", "ableton-live");
    QCoreApplication::setApplicationName("Ableton Live 12");
    setAppIcon();
    qApp->installEventFilter(new WindowFilter);
    liveSetZoom = reinterpret_cast<SetZoomFn>(hookFunction(mainBase() + kSetZoom, kSetZoomPrologue,
                                                           sizeof kSetZoomPrologue, reinterpret_cast<void *>(&setZoom)));
    menufixTrace("window: zoom hook %d\n", liveSetZoom != nullptr);
}
