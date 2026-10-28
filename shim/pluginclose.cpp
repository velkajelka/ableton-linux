// Window-manager close for plug-in editor windows in the Push 3 Live build.
//
// Live creates plug-in editor windows on its own Xlib connection but never
// sets WM_PROTOCOLS, and registers no X event handlers on that connection.
// Clicking the window's close button therefore makes KWin kill the whole
// client connection (XKillClient), and Live exits from _XIOError on its next
// XSync.
//
// Fix:
//  1. Hook Live's "editor open" property observer (0x3f132a0, reached through
//     the thunk at 0x3f2b9a0) to know which property is being applied while
//     the editor window is created.
//  2. Interpose XCreateWindow: top-level windows created during that call get
//     WM_DELETE_WINDOW and are remembered as window -> property.
//  3. Interpose XNextEvent: a WM_DELETE_WINDOW for such a window sets the
//     property to false through Live's own setter (0x1e924c0), the same path
//     as the device's wrench button. Deferred to the next Qt loop pass.
//  4. Interpose XDestroyWindow to forget closed windows.
//
// Plug-in editor resizing: when a plug-in asks for another size
// (IPlugFrame::resizeView, e.g. u-he's GUI scale), Live calls
// XMoveResizeWindow on the editor (0x3e93692) but leaves the WM_NORMAL_HINTS
// it set at creation, min = max = the first size. The window manager clamps
// the request to those hints, so the window keeps its old size and the
// plug-in's view sits in a corner of it with stale pixels around it. The
// resize interposers below move the size hints along before resizing. Live
// also passes the window's client position, which a reparenting window
// manager reads as the frame position (default NorthWest gravity), so every
// resize moved the editor down by the title bar. Editor windows get
// StaticGravity in their hints, as Qt's own windows have, so client
// coordinates mean client coordinates.
//
// Opening: Live creates the editor at the size from IPlugView::getSize, calls
// attached(), where the plug-in creates its view at its real size (u-he at a
// saved GUI scale: 1080x540 in a 1200x600 window), maps the window, and only
// ~130 ms later gets the plug-in's resizeView. The window appeared at the
// first size and then changed. The XMapWindow interposer sizes the editor to
// the plug-in's view just before the map, so it shows once at its final size;
// the later resizeView is then a no-op. Live also hides and re-shows an
// editor within a few ms when it re-selects the device (after a drop); on
// KWin each unmap/map plays the close and open animations, so the editor
// looked opened twice. An editor's unmap is deferred by 50 ms and dropped
// together with a map that arrives in that time. Editor hints carry
// USPosition/PPosition, so KWin keeps Live's position instead of centring the
// transient window.
// Resizing an editor from its window frame: Live gets no event for it (it
// selects none on its plug-in connection), so a resizable plug-in (Pro-Q 3,
// through yabridge) kept drawing at its old size in a bigger, partly black
// window. Windows Live answers WM_SIZE with IPlugView::checkSizeConstraint and
// onSize. Here editor windows get StructureNotify on Live's connection, and a
// ConfigureNotify whose size is not the one Live or this file last gave the
// window is passed to the plug-in the same way (the view comes from Live's
// editor creation, vst3runloop.cpp): the size it accepts is applied to the
// window, then onSize. A plug-in that cannot resize gets its own size back.
// Addresses are for Live 12.4.7 (Push 3 2.4.7); the hook is skipped if the
// thunk does not hold the expected bytes.
#include <QHash>
#include <QTimer>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);
void *nextSymbol(const char *name, const char *version); // embedded.cpp
void liveOwnPluginWindow(Display *dpy, Window editor); // window.cpp

// VST3 IPlugView (pluginterfaces/gui/iplugview.h), only what is called here.
// Not in the anonymous namespace: GCC would then know that nothing in this
// program implements it and compile the calls to __cxa_pure_virtual (Live
// aborted on the first resize from the window frame).
namespace pluginview {

struct ViewRect {
    int32_t left, top, right, bottom;
};
struct IPlugView {
    virtual int32_t queryInterface(const char *iid, void **obj) = 0;
    virtual uint32_t addRef() = 0;
    virtual uint32_t release() = 0;
    virtual int32_t isPlatformTypeSupported(const char *type) = 0;
    virtual int32_t attached(void *parent, const char *type) = 0;
    virtual int32_t removed() = 0;
    virtual int32_t onWheel(float distance) = 0;
    virtual int32_t onKeyDown(char16_t key, int16_t keyCode, int16_t modifiers) = 0;
    virtual int32_t onKeyUp(char16_t key, int16_t keyCode, int16_t modifiers) = 0;
    virtual int32_t getSize(ViewRect *size) = 0;
    virtual int32_t onSize(ViewRect *newSize) = 0;
    virtual int32_t onFocus(uint8_t state) = 0;
    virtual int32_t setFrame(void *frame) = 0;
    virtual int32_t canResize() = 0;
    virtual int32_t checkSizeConstraint(ViewRect *rect) = 0;
};
constexpr int32_t kResultTrue = 0;

} // namespace pluginview
using pluginview::IPlugView;
using pluginview::ViewRect;
using pluginview::kResultTrue;

namespace {

using ObserverFn = void (*)(void *controller, void *prop, int changeType);
using SetBoolFn = void (*)(void *prop, bool value);

constexpr uintptr_t kThunk = 0x3f2b9a0;    // jmp 0x3f132a0, padded to 0x3f2b9b0
constexpr uintptr_t kObserver = 0x3f132a0; // editor-open property observer
constexpr uintptr_t kSetBool = 0x1e924c0;  // bool property setter (prop, value)

uintptr_t base;
ObserverFn observer;
SetBoolFn setBool;
void *currentProp;                    // set while the observer runs
QHash<Window, void *> windowProps;    // plug-in editor window -> property
QHash<Window, Display *> pendingUnmaps; // editor unmaps deferred by XUnmapWindow
Atom wmProtocols, wmDeleteWindow;


struct Editor {
    IPlugView *view = nullptr; // Live's view for the window; Live releases it when it closes the editor
    Display *dpy = nullptr;
    int width = 0, height = 0;    // the size Live or this file last gave the window
    int wantWidth = 0, wantHeight = 0; // the window manager's latest size
    bool pending = false;
};
QHash<Window, Editor> editors;
// The view Live created last, for the editor window that follows. No
// reference is taken: not every view Live creates gets a window, and some
// plug-ins destroy theirs regardless of references.
IPlugView *createdView;

void rememberSize(Window w, unsigned width, unsigned height)
{
    auto it = editors.find(w);
    if (it != editors.end()) {
        it->width = int(width);
        it->height = int(height);
    }
}

// The window manager resized an editor: let the plug-in take that size (or
// the nearest it accepts), as Windows Live does on WM_SIZE.
void applyFrameResize(Window w)
{
    auto it = editors.find(w);
    if (it == editors.end())
        return;
    it->pending = false;
    menufixTrace("pluginclose: frame resize to %dx%d, view %d\n", it->wantWidth, it->wantHeight, it->view != nullptr);
    if (!it->view)
        return;
    if (it->wantWidth == it->width && it->wantHeight == it->height)
        return;
    IPlugView *view = it->view;
    ViewRect rect = {0, 0, it->wantWidth, it->wantHeight};
    if (view->canResize() == kResultTrue) {
        view->checkSizeConstraint(&rect);
    } else {
        ViewRect own = {};
        if (view->getSize(&own) != kResultTrue)
            return;
        rect = own;
    }
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0)
        return;
    Display *dpy = it->dpy;
    if (width != it->wantWidth || height != it->wantHeight)
        XResizeWindow(dpy, w, unsigned(width), unsigned(height)); // also records the size
    else
        rememberSize(w, unsigned(width), unsigned(height));
    rect = {0, 0, width, height};
    const int32_t sized = view->onSize(&rect);
    menufixTrace("pluginclose: onSize %dx%d -> %d\n", width, height, sized);
    XFlush(dpy);
    menufixTrace("pluginclose: editor 0x%x resized by its frame to %dx", int(w), width);
    menufixTrace("%d\n", height);
}

void observerHook(void *controller, void *prop, int changeType)
{
    void *saved = currentProp;
    currentProp = prop;
    observer(controller, prop, changeType);
    currentProp = saved;
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

template <typename Fn>
Fn next(const char *name)
{
    return reinterpret_cast<Fn>(nextSymbol(name, nullptr));
}

} // namespace

void pluginViewCreated(void *view)
{
    createdView = static_cast<IPlugView *>(view);
}

void installPluginCloseHook()
{
    base = mainExecutableBase();
    auto *thunk = reinterpret_cast<unsigned char *>(base + kThunk);
    // e9 rel32 to the observer, then padding up to 0x3f2b9b0.
    int32_t rel;
    memcpy(&rel, thunk + 1, 4);
    if (thunk[0] != 0xe9 || base + kThunk + 5 + rel != base + kObserver) {
        menufixTrace("pluginclose: thunk mismatch, not hooked\n");
        return;
    }
    observer = reinterpret_cast<ObserverFn>(base + kObserver);
    setBool = reinterpret_cast<SetBoolFn>(base + kSetBool);

    // jmp qword [rip+0] ; .quad observerHook  (14 bytes, fits before 0x3f2b9b0)
    unsigned char patch[14] = {0xff, 0x25, 0, 0, 0, 0};
    const uintptr_t target = reinterpret_cast<uintptr_t>(&observerHook);
    memcpy(patch + 6, &target, 8);
    const long page = sysconf(_SC_PAGESIZE);
    auto *pageStart = reinterpret_cast<void *>(uintptr_t(thunk) & ~uintptr_t(page - 1));
    const size_t len = (uintptr_t(thunk) + sizeof patch > uintptr_t(pageStart) + page) ? 2 * page : page;
    if (mprotect(pageStart, len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        menufixTrace("pluginclose: mprotect failed\n");
        return;
    }
    memcpy(thunk, patch, sizeof patch);
    mprotect(pageStart, len, PROT_READ | PROT_EXEC);
    __builtin___clear_cache(reinterpret_cast<char *>(thunk), reinterpret_cast<char *>(thunk) + sizeof patch);
    menufixTrace("pluginclose: observer hooked\n");
}

extern "C" Window XCreateWindow(Display *dpy, Window parent, int x, int y, unsigned width,
                                unsigned height, unsigned border, int depth, unsigned cls,
                                Visual *visual, unsigned long mask, XSetWindowAttributes *attrs)
{
    static auto real = next<decltype(&XCreateWindow)>("XCreateWindow");
    const Window w = real(dpy, parent, x, y, width, height, border, depth, cls, visual, mask, attrs);
    if (w && currentProp && parent == DefaultRootWindow(dpy)) {
        if (!wmDeleteWindow) {
            wmProtocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
            wmDeleteWindow = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
        }
        XSetWMProtocols(dpy, w, &wmDeleteWindow, 1);
        windowProps.insert(w, currentProp);
        Editor editor;
        editor.view = createdView;
        createdView = nullptr;
        editor.dpy = dpy;
        editor.width = int(width);
        editor.height = int(height);
        editors.insert(w, editor);
        XWindowAttributes attributes;
        if (XGetWindowAttributes(dpy, w, &attributes))
            XSelectInput(dpy, w, attributes.your_event_mask | StructureNotifyMask);
        menufixTrace("pluginclose: editor window 0x%x\n", int(w));
        liveOwnPluginWindow(dpy, w);
    }
    return w;
}

namespace {

// Keeps a fixed-size editor fixed at the new size, and widens the limits of a
// resizable one so the new size fits.
void fitSizeHints(Display *dpy, Window w, unsigned width, unsigned height)
{
    XSizeHints *hints = XAllocSizeHints();
    long supplied = 0;
    if (hints && XGetWMNormalHints(dpy, w, hints, &supplied)) {
        hints->flags |= PWinGravity | USPosition | PPosition;
        hints->win_gravity = StaticGravity;
        const bool fixed = (hints->flags & PMinSize) && (hints->flags & PMaxSize)
                           && hints->min_width == hints->max_width && hints->min_height == hints->max_height;
        if (fixed) {
            hints->min_width = hints->max_width = int(width);
            hints->min_height = hints->max_height = int(height);
        } else {
            if (hints->flags & PMinSize) {
                hints->min_width = qMin(hints->min_width, int(width));
                hints->min_height = qMin(hints->min_height, int(height));
            }
            if (hints->flags & PMaxSize) {
                hints->max_width = qMax(hints->max_width, int(width));
                hints->max_height = qMax(hints->max_height, int(height));
            }
        }
        XSetWMNormalHints(dpy, w, hints);
        menufixTrace("pluginclose: editor 0x%x resized to %dx", int(w), int(width));
        menufixTrace("%d\n", int(height));
    }
    if (hints)
        XFree(hints);
}

// Sizes the editor to the plug-in's view (its first mapped child window),
// which the plug-in sized in attached() before Live maps the editor.
void fitToView(Display *dpy, Window w)
{
    XWindowAttributes outer;
    Window root, parent, *kids = nullptr;
    unsigned count = 0;
    if (!XGetWindowAttributes(dpy, w, &outer) || !XQueryTree(dpy, w, &root, &parent, &kids, &count))
        return;
    for (unsigned i = 0; i < count; ++i) {
        XWindowAttributes view;
        if (!XGetWindowAttributes(dpy, kids[i], &view) || view.map_state == IsUnmapped)
            continue;
        if (view.width != outer.width || view.height != outer.height) {
            menufixTrace("pluginclose: editor 0x%x fitted to its view, ", int(w));
            XResizeWindow(dpy, w, unsigned(view.width), unsigned(view.height));
        }
        break;
    }
    if (kids)
        XFree(kids);
}

} // namespace

extern "C" int XMoveResizeWindow(Display *dpy, Window w, int x, int y, unsigned width, unsigned height)
{
    static auto real = next<decltype(&XMoveResizeWindow)>("XMoveResizeWindow");
    if (!windowProps.contains(w))
        return real(dpy, w, x, y, width, height);
    fitSizeHints(dpy, w, width, height);
    rememberSize(w, width, height);
    return real(dpy, w, x, y, width, height);
}

extern "C" int XMapWindow(Display *dpy, Window w)
{
    static auto real = next<decltype(&XMapWindow)>("XMapWindow");
    if (pendingUnmaps.remove(w)) {
        menufixTrace("pluginclose: editor 0x%x hidden and shown again, kept mapped\n", int(w));
        return 1;
    }
    if (windowProps.contains(w))
        fitToView(dpy, w);
    return real(dpy, w);
}

extern "C" int XUnmapWindow(Display *dpy, Window w)
{
    static auto real = next<decltype(&XUnmapWindow)>("XUnmapWindow");
    if (!windowProps.contains(w))
        return real(dpy, w);
    pendingUnmaps.insert(w, dpy);
    QTimer::singleShot(50, [w] {
        if (Display *d = pendingUnmaps.take(w)) {
            real(d, w);
            XFlush(d);
        }
    });
    return 1;
}

extern "C" void XSetWMNormalHints(Display *dpy, Window w, XSizeHints *hints)
{
    static auto real = next<decltype(&XSetWMNormalHints)>("XSetWMNormalHints");
    if (hints && (windowProps.contains(w) || currentProp)) {
        hints->flags |= PWinGravity | USPosition | PPosition;
        hints->win_gravity = StaticGravity;
    }
    real(dpy, w, hints);
}

extern "C" int XResizeWindow(Display *dpy, Window w, unsigned width, unsigned height)
{
    static auto real = next<decltype(&XResizeWindow)>("XResizeWindow");
    if (!windowProps.contains(w))
        return real(dpy, w, width, height);
    fitSizeHints(dpy, w, width, height);
    rememberSize(w, width, height);
    return real(dpy, w, width, height);
}

extern "C" int XDestroyWindow(Display *dpy, Window w)
{
    static auto real = next<decltype(&XDestroyWindow)>("XDestroyWindow");
    windowProps.remove(w);
    pendingUnmaps.remove(w);
    editors.remove(w);
    return real(dpy, w);
}

extern "C" int XNextEvent(Display *dpy, XEvent *ev)
{
    static auto real = next<decltype(&XNextEvent)>("XNextEvent");
    const int r = real(dpy, ev);
    if (ev->type == ConfigureNotify && ev->xconfigure.window == ev->xconfigure.event) {
        auto it = editors.find(ev->xconfigure.window);
        if (it != editors.end()) {
            it->wantWidth = ev->xconfigure.width;
            it->wantHeight = ev->xconfigure.height;
            // One onSize per pause in a drag, from the next loop pass.
            if (!it->pending && (it->wantWidth != it->width || it->wantHeight != it->height)) {
                it->pending = true;
                const Window w = ev->xconfigure.window;
                QTimer::singleShot(30, [w] { applyFrameResize(w); });
            }
        }
    }
    if (ev->type == ClientMessage && wmDeleteWindow && ev->xclient.message_type == wmProtocols
        && Atom(ev->xclient.data.l[0]) == wmDeleteWindow && windowProps.contains(ev->xclient.window)) {
        const Window w = ev->xclient.window;
        menufixTrace("pluginclose: close requested for 0x%x\n", int(w));
        // Live is inside its own event pump here; close on the next loop pass,
        // and only if the window still belongs to an open editor by then.
        QTimer::singleShot(0, [w] {
            if (void *prop = windowProps.value(w))
                setBool(prop, false);
        });
    }
    return r;
}
