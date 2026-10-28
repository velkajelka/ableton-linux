// Value drags: Live's own pointer moves, done through Qt.
//
// Live drags a value (knob, fader, number field, slider, the Settings zoom
// and theme controls) the way it does on Windows: it hides the cursor, reads
// the pointer on every motion, takes the distance from the press point as the
// change and puts the pointer back on the press point. On release it puts the
// pointer where the control now is (the press point for a knob or a number,
// the fader's arrow, the Settings field after a zoom change) and shows the
// cursor again. The two pointer functions are Live's platform layer (vtable
// at 0x945c7e0): get-position (0x1fe4fe0, XQueryPointer on the root window)
// and set-position (0x1fe4f00, XWarpPointer to root coordinates), with the
// point packed as x | y << 32.
//
// Both take Live's coordinates, which are Qt's device-independent pixels:
// Live turns on Qt's high-DPI scaling, but the Push build runs at ratio 1 and
// hands those numbers to Xlib as they are. On a 125 % desktop a press at X
// pixel 197,75 (Live's 158,60) was warped to X pixel 158,60, and the
// pointer jumped on the first motion of every drag. Get-position is replaced
// with QCursor::pos(). Live's press and release points come from
// mapToGlobal(), which adds the window's X position and the position in it
// scaled by the window's ratio, so set-position converts back the same way
// through the focused window (with the screen under the point when there is
// none) and warps with Xlib. QCursor::setPos() could not be used: Qt 5.15.2
// scales from the primary screen's origin, so on a screen below a 125 % one
// (X 0,1080) a press at Live's y 1840 (X 2030) was warped to X 2300, the knob
// ran to its maximum and the fader to -inf.
//
// Not solved: a window across two screens. Qt 5 keeps every screen's
// top-left in X pixels and scales only the offset into it, so on the part of
// the window outside its own screen (the one holding most of it) Qt's
// mappings disagree by hundreds of pixels: events and the window's position
// use the window's screen, mapToGlobal() the screen under the point. A value
// drag there runs to its end. Making Live's mapToGlobal() follow the window's
// screen fixed the drag but opened popups hundreds of pixels away.
//
// Under Wayland a client cannot move the pointer. Xwayland emulates a warp
// while the cursor is hidden: it locks the pointer (pointer-constraints) on
// the focused window, turns mouse motion into relative motion from the warped
// position, and gives the compositor each warp target as the lock's cursor
// position hint. When Live shows the cursor again the lock ends and the
// compositor puts the cursor at the hint, where Live's release warp put it.
// So the cursor stays on a knob or number while it is dragged, comes back on
// the fader's arrow, and a drag never ends at the screen edge.
//
// Positions in whole X pixels. Xwayland moves the locked pointer by the
// mouse motion times its scale (1.25 X px per step at 125 %) and keeps the
// fraction, but a warp lands on a whole X pixel and Qt 5.15 truncates the
// XInput2 positions (1/65536 px) to whole pixels. With a fractional ratio both
// errors went the same way: one mouse step down from the press point read
// +0.8 of Live's pixels and one step up -1.6, so Live (which rounds) counted
// +1 and -2, and slow drags ran away upwards (tempo dragged 40 px up, 80 px
// down and 40 px up in 1 px steps: 120, 160, 111, 151; at 100 % 120, 144,
// 96, 121). A 125 % monitor next to a 100 % one always gives Xwayland a
// fractional scale. A native event filter now rounds XInput2 positions to
// the nearest X pixel instead, after adding back what the last warp lost
// while a button is held, so every step reads exactly one of Live's pixels,
// and moves the root position to agree with mapToGlobal() (see below).
//
// LIVE_WARP_TRACE=1 (with MENUFIX_DEBUG) logs every read and move.
#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
#include <X11/Xlib.h>
#include <link.h>
#include <xcb/xcb.h>
#include <xcb/xinput.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);
bool redirectFunction(uintptr_t fn, const unsigned char *prologue, size_t len, void *target); // hook.cpp

namespace {

constexpr uintptr_t kGetPosition = 0x1fe4fe0; // () -> x | y << 32
constexpr uintptr_t kSetPosition = 0x1fe4f00; // (platform *, x | y << 32)
constexpr uintptr_t kPointerDisabled = 0x20506a0; // Live's own guard in set-position
const unsigned char kGetPositionPrologue[] = {0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x38,
                                              0xc7, 0x44, 0x24, 0x0c, 0x00, 0x00, 0x00, 0x00};
const unsigned char kSetPositionPrologue[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                              0x41, 0x54, 0x53, 0x50, 0x48, 0x89, 0xf3};

uintptr_t base;
QPointF warpError; // the last warp's exact target minus the X pixel it landed on

bool traceWarps()
{
    static const bool trace = getenv("LIVE_WARP_TRACE") && *getenv("LIVE_WARP_TRACE") == '1';
    return trace;
}

uint64_t pack(const QPoint &p)
{
    return uint32_t(p.x()) | uint64_t(uint32_t(p.y())) << 32;
}

Display *display()
{
    static Display *dpy = XOpenDisplay(nullptr);
    return dpy;
}

uint64_t getPosition()
{
    const QPoint p = QCursor::pos();
    if (traceWarps())
        menufixTrace("pointerwarp: get %d,%d\n", p.x(), p.y());
    return pack(p);
}

void setPosition(void *, uint64_t packed)
{
    if (reinterpret_cast<bool (*)()>(base + kPointerDisabled)())
        return;
    const QPoint p(int32_t(packed), int32_t(packed >> 32));
    Display *dpy = display();
    if (!dpy)
        return;
    // Live's points come from mapToGlobal(), which adds the window's X
    // position and the position in it, so convert back the same way.
    QPointF exact;
    QWindow *window = QGuiApplication::focusWindow();
    Window child;
    int x, y;
    if (window && window->handle()
        && XTranslateCoordinates(dpy, window->winId(), DefaultRootWindow(dpy), 0, 0, &x, &y, &child)) {
        exact = QPointF(x, y) + QPointF(p - window->mapToGlobal(QPoint())) * window->devicePixelRatio();
    } else {
        QScreen *screen = QGuiApplication::screenAt(p);
        if (!screen)
            screen = QGuiApplication::primaryScreen();
        if (!screen)
            return;
        const QPoint origin = screen->geometry().topLeft();
        exact = origin + QPointF(p - origin) * screen->devicePixelRatio();
    }
    const QPoint x11 = exact.toPoint();
    warpError = exact - x11;
    if (traceWarps())
        menufixTrace("pointerwarp: set %d,%d (X %d)\n", p.x(), p.y(), x11.y());
    XWarpPointer(dpy, None, DefaultRootWindow(dpy), 0, 0, 0, 0, x11.x(), x11.y());
    XFlush(dpy);
}

int32_t roundedFp1616(int32_t value, qreal shift)
{
    return int32_t(std::lround(value / 65536.0 + shift)) * 65536;
}

struct PositionRounder : QAbstractNativeEventFilter {
    uint8_t xinput = 0; // XInputExtension's major opcode

    bool nativeEventFilter(const QByteArray &, void *message, long *) override
    {
        auto *generic = static_cast<xcb_ge_generic_event_t *>(message);
        if ((generic->response_type & 0x7f) != XCB_GE_GENERIC || generic->extension != xinput)
            return false;
        if (generic->event_type != XCB_INPUT_MOTION && generic->event_type != XCB_INPUT_BUTTON_PRESS
            && generic->event_type != XCB_INPUT_BUTTON_RELEASE)
            return false;
        if (generic->event_type == XCB_INPUT_BUTTON_PRESS)
            warpError = {};
        const QPointF shift = QGuiApplication::mouseButtons() ? warpError : QPointF();
        auto *event = static_cast<xcb_input_motion_event_t *>(message);
        const int32_t rawY = event->root_y;
        event->root_x = roundedFp1616(event->root_x, shift.x());
        event->root_y = roundedFp1616(event->root_y, shift.y());
        event->event_x = roundedFp1616(event->event_x, shift.x());
        event->event_y = roundedFp1616(event->event_y, shift.y());
        matchGlobalPosition(event);
        if (traceWarps())
            menufixTrace("pointerwarp: event y %d/100 -> %d/100 (event_y %d)\n", int(rawY * 100LL / 65536),
                         int(event->root_y * 100LL / 65536), event->event_y / 65536);
        return false;
    }

    // Qt converts an event's root position to its units with the window's
    // screen, rounding on its own, while mapToGlobal() adds the event's
    // position in the window to the window's position, each rounded. At a
    // fractional ratio the two came out a pixel apart (a window at X y 373
    // is at 298.4), and Live, which takes its press point from mapToGlobal()
    // and the motion from the events, read that pixel on every motion: the
    // tempo went 120, 168, 168, 215 for 40 px up, 80 down and 40 up. The
    // root position is set to where Qt's conversion gives mapToGlobal() of
    // the event's position in the window.
    static void matchGlobalPosition(xcb_input_motion_event_t *event)
    {
        QWindow *window = nullptr; // Live's view is a native child window
        for (QWindow *w : QGuiApplication::allWindows())
            if (w->handle() && xcb_window_t(w->winId()) == event->event)
                window = w;
        QScreen *screen = window ? window->screen() : nullptr;
        if (!screen)
            return;
        const QPointF local = QPointF(event->event_x / 65536, event->event_y / 65536) / window->devicePixelRatio();
        const QPoint global = window->mapToGlobal(local.toPoint());
        const QPoint origin = screen->geometry().topLeft();
        const QPointF native = origin + QPointF(global - origin) * screen->devicePixelRatio();
        event->root_x = int32_t(std::lround(native.x() * 65536));
        event->root_y = int32_t(std::lround(native.y() * 65536));
    }
};

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

void installPointerWarp()
{
    base = mainBase();
    const bool get = redirectFunction(base + kGetPosition, kGetPositionPrologue, sizeof kGetPositionPrologue,
                                      reinterpret_cast<void *>(&getPosition));
    const bool set = redirectFunction(base + kSetPosition, kSetPositionPrologue, sizeof kSetPositionPrologue,
                                      reinterpret_cast<void *>(&setPosition));
    static PositionRounder rounder;
    int opcode, event, error;
    if (display() && XQueryExtension(display(), "XInputExtension", &opcode, &event, &error)) {
        rounder.xinput = uint8_t(opcode);
        QCoreApplication::instance()->installNativeEventFilter(&rounder);
    }
    menufixTrace("pointerwarp: get %d, set %d, rounding %d\n", get, set, rounder.xinput);
}
