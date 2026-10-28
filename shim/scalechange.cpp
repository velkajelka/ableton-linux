// Live follows a change of the desktop scale while it runs.
//
// KWin runs Xwayland at the largest monitor scale and publishes Xft.dpi = 96
// x that scale (and Xcursor.size = 24 x it) in the root window's resources.
// Plugging in a monitor with another scale, or changing a scale, changes both
// and resizes every X window to keep its size on screen. Qt 5.15 reads
// Xft.dpi once, when it connects; later it only follows XSETTINGS' Xft/DPI,
// which Plasma's xsettingsd does not publish. So Live kept its old ratio:
// started on a 100 % laptop panel with a 125 % monitor plugged in later, it
// laid itself out in the 25 % larger X window at ratio 1 (everything 80 %
// of its size, until a restart), and value drags moved 1.25 of Live's pixels
// per pixel of mouse motion.
//
// The root window's Xft.dpi is read when its resources change, and once a
// second in case that notification is missed. When it changed,
// QXcbScreen::logicalDpi (its vtable slot in libQt5XcbQpa) returns the new
// value, every screen gets Qt's logical-DPI change (Qt recomputes its scale
// factors from it, as for an XSETTINGS change), and every top-level window is
// sent its X geometry again (a synthetic ConfigureNotify, as window managers
// send them), which Qt converts with the new ratio. Live then
// gets a resize and lays out and renders at the new ratio, as if started
// with it.
//
// KWin resizes the X windows first and publishes the new Xft.dpi about 25 ms
// later. In between, Live sees its window twice as large (or half) at the old
// ratio and answers with resizes of its own (its minimum size, limited to a
// work area that is half in the old units): the switch to 200 % left the
// window at its minimum, the way back made it 1600 px wide. A ConfigureNotify
// that scales a top-level window by 10 % or more in both directions alike (a
// scale change; a window manager's resize never jumps like that) starts a
// transition: until 300 ms after the new Xft.dpi is applied (at most a
// second), window.cpp drops Live's own resizes. Then every window gets back
// the size it had on screen before (Windows keeps a window's size in DIPs
// across a DPI change too).
//
// LIVE_FOLLOW_SCALE=0 turns this off.
#include <QAbstractNativeEventFilter>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QHash>
#include <QRegion>
#include <QScreen>
#include <QTimer>
#include <QWindow>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <sys/mman.h>
#include <xcb/xcb.h>
#include <unistd.h>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);

// A number from the root window's resources (RESOURCE_MANAGER), read from
// the server each time; 0 when it is not set. libXcursor and Qt read them once.
double xResource(Display *dpy, const char *name)
{
    double value = 0;
    Atom type;
    int format;
    unsigned long count, after;
    unsigned char *data = nullptr;
    if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), XA_RESOURCE_MANAGER, 0, 1 << 20, False, XA_STRING, &type,
                           &format, &count, &after, &data)
            == Success
        && data) {
        const size_t length = strlen(name);
        for (const char *line = reinterpret_cast<const char *>(data); line && *line;) {
            if (strncmp(line, name, length) == 0 && line[length] == ':') {
                value = atof(line + length + 1);
                break;
            }
            line = strchr(line, '\n');
            if (line)
                ++line;
        }
        XFree(data);
    }
    return value;
}

namespace {

struct Dpi {
    double x, y;
};
using LogicalDpiFn = Dpi (*)(const void *);
LogicalDpiFn realLogicalDpi;
double desktopDpi; // Xft.dpi Qt's screens report, once it changed
double appliedDpi; // Xft.dpi Qt's scale factors were computed from
QHash<xcb_window_t, QSize> knownSizes; // top-level windows' last size Qt was given
bool transition;                      // a change of the desktop scale is under way
// Each window's size (Qt's units) before the first ConfigureNotify of the
// last half second, with that notification's time.
QHash<QWindow *, QPair<qint64, QSize>> sizesBefore;
QElapsedTimer sinceStart;

Dpi logicalDpi(const void *screen)
{
    if (desktopDpi > 0)
        return {desktopDpi, desktopDpi};
    return realLogicalDpi(screen);
}

Display *display()
{
    static Display *dpy = XOpenDisplay(nullptr);
    return dpy;
}

template <typename Fn> Fn qtGui(const char *name)
{
    return reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, name));
}

// Points QXcbScreen's vtable entry for logicalDpi at logicalDpi() above.
bool hookLogicalDpi()
{
    void *xcb = dlopen("libQt5XcbQpa.so.5", RTLD_LAZY | RTLD_NOLOAD);
    if (!xcb)
        return false;
    auto **vtable = static_cast<void **>(dlsym(xcb, "_ZTV10QXcbScreen"));
    void *function = dlsym(xcb, "_ZNK10QXcbScreen10logicalDpiEv");
    dlclose(xcb);
    if (!vtable || !function)
        return false;
    for (int i = 2; i < 64; ++i) {
        if (vtable[i] != function)
            continue;
        const long page = sysconf(_SC_PAGESIZE);
        auto *start = reinterpret_cast<void *>(uintptr_t(&vtable[i]) & ~uintptr_t(page - 1));
        if (mprotect(start, page, PROT_READ | PROT_WRITE) != 0)
            return false;
        realLogicalDpi = reinterpret_cast<LogicalDpiFn>(function);
        vtable[i] = reinterpret_cast<void *>(&logicalDpi);
        mprotect(start, page, PROT_READ);
        return true;
    }
    return false;
}

// The window's X geometry (Qt's native geometry of a top-level window).
QRect nativeGeometry(QWindow *window)
{
    Display *dpy = display();
    Window root, child;
    int x, y;
    unsigned int w, h, border, depth;
    if (!dpy || !XGetGeometry(dpy, window->winId(), &root, &x, &y, &w, &h, &border, &depth)
        || !XTranslateCoordinates(dpy, window->winId(), root, 0, 0, &x, &y, &child))
        return QRect();
    return QRect(x, y, int(w), int(h));
}

// Sends every visible top-level window its X geometry again, as the
// synthetic ConfigureNotify window managers send, which Qt converts with its
// new ratio; returns how many.
int resendGeometries()
{
    Display *dpy = display();
    if (!dpy)
        return 0;
    int windows = 0;
    for (QWindow *window : QGuiApplication::topLevelWindows()) {
        if (!window->handle() || !window->isVisible())
            continue;
        const QRect rect = nativeGeometry(window);
        if (rect.isEmpty())
            continue;
        knownSizes.insert(xcb_window_t(window->winId()), rect.size());
        XEvent event{};
        event.xconfigure.type = ConfigureNotify;
        event.xconfigure.display = dpy;
        event.xconfigure.event = event.xconfigure.window = Window(window->winId());
        event.xconfigure.x = rect.x();
        event.xconfigure.y = rect.y();
        event.xconfigure.width = rect.width();
        event.xconfigure.height = rect.height();
        XSendEvent(dpy, event.xconfigure.window, False, StructureNotifyMask, &event);
        ++windows;
    }
    XFlush(dpy);
    return windows;
}

void applyDpi(double dpi)
{
    static bool hooked = hookLogicalDpi();
    if (!hooked) {
        menufixTrace("scalechange: logicalDpi not hooked\n");
        resendGeometries();
        return;
    }
    // Sizes on screen to keep: the window manager scaled the windows, but
    // within the size hints Live set at the old ratio, and a window that
    // shrank may already have been given its new size. (Read before the
    // switch: Qt asks logicalDpi() whenever it converts.)
    QHash<QWindow *, QSize> sizes;
    for (QWindow *window : QGuiApplication::topLevelWindows()) {
        if (!window->handle() || !window->isVisible()
            || (window->windowStates() & (Qt::WindowMaximized | Qt::WindowFullScreen)))
            continue;
        const auto before = sizesBefore.constFind(window);
        const bool recent = before != sizesBefore.cend() && sinceStart.elapsed() - before->first < 500;
        sizes.insert(window, recent ? before->second : window->size());
    }
    desktopDpi = dpi;
    using DpiChangeFn = void (*)(QScreen *, double, double);
    using FlushFn = bool (*)(int);
    auto dpiChange = qtGui<DpiChangeFn>("_ZN22QWindowSystemInterface36handleScreenLogicalDotsPerInchChangeEP7QScreendd");
    auto flush = qtGui<FlushFn>("_ZN22QWindowSystemInterface23flushWindowSystemEventsE6QFlagsIN10QEventLoop17ProcessEventsFlagEE");
    if (dpiChange && flush) {
        for (QScreen *screen : QGuiApplication::screens())
            dpiChange(screen, dpi, dpi);
        flush(0);
    }
    const int windows = resendGeometries();
    menufixTrace("scalechange: Xft.dpi %d, %d windows, ratio %d/100\n", int(dpi), windows,
                 int(std::lround(QGuiApplication::primaryScreen()->devicePixelRatio() * 100)));
    // After Live has drawn once at the new ratio and published its minimum.
    QTimer::singleShot(300, qApp, [sizes] {
        transition = false;
        const QWindowList windows = QGuiApplication::topLevelWindows();
        for (auto it = sizes.cbegin(); it != sizes.cend(); ++it)
            if (windows.contains(it.key()) && it.key()->size() != it.value()) {
                menufixTrace("scalechange: window back to %dx%d\n", it.value().width(), it.value().height());
                it.key()->resize(it.value());
            }
    });
}

// A resize of a top-level window by the same factor of at least 1.1 (or at
// most 1 / 1.1) in both directions.
bool scalesWindow(const xcb_configure_notify_event_t *event)
{
    const auto known = knownSizes.constFind(event->window);
    if (known == knownSizes.cend() || known->isEmpty() || !event->width || !event->height)
        return false;
    const double x = double(event->width) / known->width(), y = double(event->height) / known->height();
    return std::fabs(x - y) < 0.02 * std::max(x, y) && (x >= 1.1 || x <= 1 / 1.1);
}

void checkDpi()
{
    const double dpi = xResource(display(), "Xft.dpi");
    if (dpi <= 0 || std::fabs(dpi - appliedDpi) < 0.5)
        return;
    appliedDpi = dpi;
    applyDpi(dpi);
}

struct ResourceWatcher : QAbstractNativeEventFilter {
    bool nativeEventFilter(const QByteArray &, void *message, long *) override
    {
        auto *event = static_cast<xcb_generic_event_t *>(message);
        const int type = event->response_type & 0x7f;
        if (type == XCB_PROPERTY_NOTIFY
            && reinterpret_cast<xcb_property_notify_event_t *>(event)->atom == XA_RESOURCE_MANAGER)
            QTimer::singleShot(0, qApp, checkDpi);
        if (type != XCB_CONFIGURE_NOTIFY)
            return false;
        auto *configure = reinterpret_cast<xcb_configure_notify_event_t *>(event);
        if (configure->event != configure->window)
            return false; // the copy for the parent
        QWindow *topLevel = nullptr;
        for (QWindow *window : QGuiApplication::topLevelWindows())
            if (window->handle() && xcb_window_t(window->winId()) == configure->window)
                topLevel = window;
        if (!topLevel)
            return false;
        auto &before = sizesBefore[topLevel];
        if (!before.first || sinceStart.elapsed() - before.first >= 500)
            before = qMakePair(sinceStart.elapsed() + 1, topLevel->size());
        if (!transition && scalesWindow(configure)) {
            transition = true;
            QTimer::singleShot(1000, qApp, [] { transition = false; });
        }
        knownSizes.insert(configure->window, QSize(configure->width, configure->height));
        return false;
    }
};

} // namespace

// True while a change of the desktop scale is under way (window.cpp).
bool scaleTransition()
{
    return transition;
}

void installScaleFollow()
{
    const char *follow = getenv("LIVE_FOLLOW_SCALE");
    if ((follow && strcmp(follow, "0") == 0) || !display())
        return;
    appliedDpi = xResource(display(), "Xft.dpi");
    sinceStart.start();
    static ResourceWatcher watcher;
    qApp->installNativeEventFilter(&watcher);
    auto *timer = new QTimer(qApp);
    QObject::connect(timer, &QTimer::timeout, checkDpi);
    timer->start(1000);
}
