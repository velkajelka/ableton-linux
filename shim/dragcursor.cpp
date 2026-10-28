// Drop feedback while dragging inside Live (browser item onto a track, etc.).
//
// Live's internal drags are Qt drags (QDrag::exec(Qt::MoveAction) at
// 0x1ff2796). Its view's dragMoveEvent (TQtView, 0x1ff30f0) accepts the
// proposed action unconditionally and then passes the position to Live's own
// drag logic, which decides whether the item can be dropped there and shows
// NoDragCursor when it cannot (XDefineCursor through 0x1fe5100). On Windows
// that decision drives the OLE drag cursor. Here Qt's drag owns the pointer
// grab, so the window cursor Live sets is never visible and the cursor stays
// the same over every target.
//
// The TQtView vtable's dragMoveEvent slot gets a wrapper: after Live's handler
// ran, a move that left Live showing NoDragCursor is ignored, so Qt shows its
// "cannot drop" cursor; an accepted one keeps Qt's drag cursor. Cursor ids are
// learnt from Live's XcursorFilenameLoadCursor calls. Addresses are for Live
// 12.4.7; the slot is left alone if it does not hold the expected pointer.
//
// Where a drop is allowed, Qt shows the theme's dnd-move cursor (a closed hand
// in Breeze). Live's only drag (QDrag::exec at 0x1ff2796) instead gets the
// Windows move cursor: the theme's arrow with a small box below it.
//
// Under a Wayland compositor neither cursor is seen: Qt takes the
// XdndSelection when a drag starts, KWin (and Mutter) then run their own
// X-to-Wayland drag and draw the compositor's "not-allowed" cursor for its
// whole length. Live's drags only drop inside Live, where Qt hands the data
// over without the selection, so during them Qt's xcb_set_selection_owner
// call for XdndSelection is skipped (Qt's GOT entry in libQt5XcbQpa).
// LIVE_DRAG_SELECTION=1 keeps it.
//
// Live's busy cursor is WatchCursor.xcursor, a clock taken from the Mac build;
// the Windows build uses the system's busy cursor. It is replaced with the
// theme's "wait" cursor.
//
// Cursor sizes. Live loads all its cursors once at startup: the theme's at
// libXcursor's size (Xcursor.size, which KWin sets to 24 x the Xwayland
// scale) and its own files at 32 px, or 64 px when its first screen's scale
// is 1.5 or more (computed from the monitor's physical size; it came out as 1
// on the user's monitors and on the test displays). Its own cursors were
// therefore 80 % of the arrow's size at 125 % and half at 200 %. And when the
// Xwayland scale changed while Live ran (a monitor with another scale plugged
// in, or the scale changed), every cursor kept the old pixel size and looked
// smaller (or larger) than the compositor's cursor over title bars and the
// desktop. Each cursor Live loads is remembered with where it came from; its
// own files are scaled to 32 px x size / 24, as the Windows build's 32 px
// cursors go with a 24 px arrow. XDefineCursor swaps in a copy at the current
// Xcursor.size (read from the root window's resources, which libXcursor reads
// only once) when that changed since Live loaded the cursor.
//
// Hidden cursor during value drags. Live hides the cursor for a drag by
// setting a blank cursor (its only XCreatePixmapCursor call, 0x1fe4e16) on
// its windows; Windows Live uses ShowCursor, a counter that cursor shapes do
// not touch. In the arrangement overview Live keeps setting the arrow while
// the zoom drag runs (from its hover update), so the cursor came back about
// half a second in. Xwayland emulates Live's pointer warps only while the
// cursor is invisible: with the arrow it dropped the pointer lock, every
// motion then read the distance from the press point instead of the last
// step, and a drag down that turned up kept zooming in. While Live's blank
// cursor is set and a button is held, Live's other cursors are not applied;
// Live sets its cursor again on release.
#include <QDrag>
#include <QDragMoveEvent>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QSet>
#include <QWidget>
#include <X11/Xcursor/Xcursor.h>
#include <X11/Xlib.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);
void *nextSymbol(const char *name, const char *version); // embedded.cpp
double xResource(Display *dpy, const char *name);         // scalechange.cpp

namespace {

constexpr uintptr_t kDragMoveSlot = 0x945d508; // TQtView vtable, dragMoveEvent
constexpr uintptr_t kDragMoveEvent = 0x1ff30f0;
constexpr uintptr_t kBlankCursorReturn = 0x1fe4e1b; // after Live's XCreatePixmapCursor call

using DragMoveFn = void (*)(QWidget *, QDragMoveEvent *);
DragMoveFn liveDragMove;
QSet<Cursor> noDropCursors; // Live's NoDragCursor at each size
Cursor liveCursor;          // last cursor Live set on one of its windows
Cursor blankCursor;         // Live's hidden cursor
bool liveHidden;            // Live's last XDefineCursor set blankCursor
Display *liveDisplay;       // the display Live loads its cursors on
bool inLiveDrag;            // inside Live's QDrag::exec
Atom xdndSelection;

struct Cookie {
    unsigned int sequence;
}; // xcb_void_cookie_t
using SetOwnerFn = Cookie (*)(void *, uint32_t, uint32_t, uint32_t);
SetOwnerFn realSetOwner;

Cookie setSelectionOwner(void *c, uint32_t owner, uint32_t selection, uint32_t time)
{
    if (inLiveDrag && selection == xdndSelection)
        return Cookie{0};
    return realSetOwner(c, owner, selection, time);
}

// Points the GOT entry for `name` in the loaded library whose path contains
// `library` at `replacement`.
bool patchGot(const char *library, const char *name, void *replacement)
{
    struct Find {
        const char *library;
        uintptr_t base;
        const Elf64_Dyn *dynamic;
    } find{library, 0, nullptr};
    dl_iterate_phdr(
        [](dl_phdr_info *info, size_t, void *data) {
            auto *f = static_cast<Find *>(data);
            if (!info->dlpi_name || !strstr(info->dlpi_name, f->library))
                return 0;
            for (int i = 0; i < info->dlpi_phnum; ++i)
                if (info->dlpi_phdr[i].p_type == PT_DYNAMIC)
                    f->dynamic = reinterpret_cast<const Elf64_Dyn *>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
            f->base = info->dlpi_addr;
            return 1;
        },
        &find);
    if (!find.dynamic)
        return false;
    const uintptr_t base = find.base;
    auto abs = [base](Elf64_Addr a) { return a < base ? a + base : a; };
    Elf64_Addr symtab = 0, strtab = 0, jmprel = 0, pltrelsz = 0;
    for (const Elf64_Dyn *d = find.dynamic; d->d_tag != DT_NULL; ++d)
        switch (d->d_tag) {
        case DT_SYMTAB: symtab = abs(d->d_un.d_ptr); break;
        case DT_STRTAB: strtab = abs(d->d_un.d_ptr); break;
        case DT_JMPREL: jmprel = abs(d->d_un.d_ptr); break;
        case DT_PLTRELSZ: pltrelsz = d->d_un.d_val; break;
        }
    if (!symtab || !strtab || !jmprel)
        return false;
    const auto *syms = reinterpret_cast<const Elf64_Sym *>(symtab);
    const auto *strs = reinterpret_cast<const char *>(strtab);
    const auto *r = reinterpret_cast<const Elf64_Rela *>(jmprel);
    const long page = sysconf(_SC_PAGESIZE);
    for (size_t i = 0; i < pltrelsz / sizeof(Elf64_Rela); ++i) {
        if (strcmp(strs + syms[ELF64_R_SYM(r[i].r_info)].st_name, name) != 0)
            continue;
        auto **slot = reinterpret_cast<void **>(base + r[i].r_offset);
        if (mprotect(reinterpret_cast<void *>(uintptr_t(slot) & ~uintptr_t(page - 1)), page, PROT_READ | PROT_WRITE))
            return false;
        *slot = replacement;
        return true;
    }
    return false;
}

void skipDragSelection()
{
    static bool done = false;
    if (done || !liveDisplay)
        return;
    done = true;
    const char *keep = getenv("LIVE_DRAG_SELECTION");
    if (keep && *keep == '1')
        return;
    realSetOwner = reinterpret_cast<SetOwnerFn>(dlsym(RTLD_DEFAULT, "xcb_set_selection_owner"));
    xdndSelection = XInternAtom(liveDisplay, "XdndSelection", False);
    if (!realSetOwner || !xdndSelection
        || !patchGot("libQt5XcbQpa.so", "xcb_set_selection_owner", reinterpret_cast<void *>(&setSelectionOwner)))
        menufixTrace("dragcursor: XdndSelection not skipped\n");
}

void dragMoveHook(QWidget *view, QDragMoveEvent *event)
{
    liveDragMove(view, event);
    if (event->isAccepted() && noDropCursors.contains(liveCursor))
        event->ignore();
}

template <typename Fn> Fn xcursor(const char *name)
{
    return reinterpret_cast<Fn>(nextSymbol(name, nullptr));
}

// Where one of Live's cursors came from, to load it again at another size.
struct CursorSource {
    unsigned int shape = 0; // XcursorShapeLoadCursor's shape, unless
    QByteArray file;        // one of Live's .xcursor files, or
    QByteArray name;        // a theme cursor by name ("wait")
    int size = 0;           // the Xcursor size it was loaded at
};
QHash<Cursor, CursorSource> cursorSources;        // by the id Live got
QHash<QPair<Cursor, int>, Cursor> resizedCursors; // (Live's id, size) -> copy

// The Xcursor size Xwayland's clients should use now, read again at most once
// a second.
int cursorSize(Display *dpy)
{
    static int size = 0;
    static time_t checked = 0;
    const time_t now = time(nullptr);
    if (size && now == checked)
        return size;
    checked = now;
    const int found = int(xResource(dpy, "Xcursor.size"));
    size = found > 0 ? found : xcursor<int (*)(Display *)>("XcursorGetDefaultSize")(dpy);
    return size;
}

// Live's own cursor `file` at the pixel size that goes with theme size `size`,
// from its @2x file when that is larger than 40 px.
XcursorImages *liveCursorImages(const QByteArray &file, int size)
{
    const int pixels = int(std::lround(32.0 * size / 24));
    QByteArray source = file;
    source.replace("@2x.xcursor", ".xcursor");
    if (pixels > 40) {
        QByteArray large = source;
        large.replace(".xcursor", "@2x.xcursor");
        if (access(large.constData(), R_OK) == 0)
            source = large;
    }
    auto *images = xcursor<XcursorImages *(*)(const char *, int)>("XcursorFilenameLoadImages")(source.constData(), pixels);
    if (!images)
        return nullptr;
    for (int i = 0; i < images->nimage; ++i) {
        XcursorImage *from = images->images[i];
        if (int(from->width) == pixels || !from->width)
            continue;
        const double k = double(pixels) / from->width;
        const int w = pixels, h = std::max(1, int(std::lround(from->height * k)));
        const QImage scaled = QImage(reinterpret_cast<const uchar *>(from->pixels), int(from->width),
                                     int(from->height), QImage::Format_ARGB32_Premultiplied)
                                  .scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        XcursorImage *to = xcursor<XcursorImage *(*)(int, int)>("XcursorImageCreate")(w, h);
        if (!to)
            continue;
        to->size = pixels;
        to->xhot = std::min(w - 1, int(std::lround(from->xhot * k)));
        to->yhot = std::min(h - 1, int(std::lround(from->yhot * k)));
        to->delay = from->delay;
        for (int y = 0; y < h; ++y)
            memcpy(to->pixels + y * w, scaled.constScanLine(y), size_t(w) * 4);
        xcursor<void (*)(XcursorImage *)>("XcursorImageDestroy")(from);
        images->images[i] = to;
    }
    return images;
}

Cursor loadCursor(Display *dpy, const CursorSource &source, int size)
{
    const char *theme = xcursor<char *(*)(Display *)>("XcursorGetTheme")(dpy);
    XcursorImages *images =
        !source.file.isEmpty()   ? liveCursorImages(source.file, size)
        : !source.name.isEmpty() ? xcursor<XcursorImages *(*)(const char *, const char *, int)>("XcursorLibraryLoadImages")(
                                       source.name.constData(), theme, size)
                                 : xcursor<XcursorImages *(*)(unsigned int, const char *, int)>("XcursorShapeLoadImages")(
                                       source.shape, theme, size);
    if (!images)
        return None;
    const Cursor cursor = xcursor<Cursor (*)(Display *, const XcursorImages *)>("XcursorImagesLoadCursor")(dpy, images);
    xcursor<void (*)(XcursorImages *)>("XcursorImagesDestroy")(images);
    return cursor;
}

// The theme's arrow at the theme's size, with a box below and to the right of
// it. Qt puts a drag cursor's hotspot at the pixmap's centre, so the arrow's
// hotspot is placed there.
QPixmap moveCursorPixmap()
{
    if (!liveDisplay)
        return QPixmap();
    const char *theme = xcursor<char *(*)(Display *)>("XcursorGetTheme")(liveDisplay);
    const int size = cursorSize(liveDisplay);
    auto load = xcursor<XcursorImage *(*)(const char *, const char *, int)>("XcursorLibraryLoadImage");
    XcursorImage *arrow = load("default", theme, size);
    if (!arrow)
        arrow = load("left_ptr", theme, size);
    if (!arrow)
        return QPixmap();
    const QImage image = QImage(reinterpret_cast<const uchar *>(arrow->pixels), int(arrow->width),
                                int(arrow->height), QImage::Format_ARGB32_Premultiplied)
                             .copy();
    const int hx = int(arrow->xhot), hy = int(arrow->yhot);
    const int half = int(std::max(arrow->width, arrow->height));
    xcursor<void (*)(XcursorImage *)>("XcursorImageDestroy")(arrow);

    QImage canvas(2 * half, 2 * half, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    // A hollow box with a dotted black and white outline, under the arrow's
    // tail, as in Windows' move cursor.
    const double u = size / 24.0;
    const int b = std::max(1, int(std::lround(u)));
    const QRect box(half + int(std::lround(3 * u)), half + int(std::lround(15 * u)), int(std::lround(14 * u)),
                    int(std::lround(9 * u)));
    for (int y = box.top(); y <= box.bottom(); ++y)
        for (int x = box.left(); x <= box.right(); ++x)
            if (x < box.left() + b || x > box.right() - b || y < box.top() + b || y > box.bottom() - b)
                canvas.setPixel(x, y, ((x - box.left()) / b + (y - box.top()) / b) % 2 ? 0xff000000 : 0xffffffff);
    QPainter p(&canvas);
    p.drawImage(half - hx, half - hy, image);
    p.end();
    return QPixmap::fromImage(canvas);
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

extern "C" Cursor XcursorFilenameLoadCursor(Display *dpy, const char *file)
{
    static auto real = reinterpret_cast<Cursor (*)(Display *, const char *)>(
        nextSymbol("XcursorFilenameLoadCursor", nullptr));
    liveDisplay = dpy;
    if (!file)
        return real(dpy, file);
    CursorSource source;
    source.size = cursorSize(dpy);
    Cursor cursor = None;
    if (strstr(file, "/WatchCursor")) {
        source.name = "wait";
        cursor = loadCursor(dpy, source, source.size);
    }
    if (!cursor) {
        source.name.clear();
        source.file = file;
        cursor = loadCursor(dpy, source, source.size);
    }
    if (!cursor)
        return real(dpy, file);
    cursorSources.insert(cursor, source);
    if (strstr(file, "/NoDragCursor"))
        noDropCursors.insert(cursor);
    return cursor;
}

extern "C" Cursor XcursorShapeLoadCursor(Display *dpy, unsigned int shape)
{
    static auto real = reinterpret_cast<Cursor (*)(Display *, unsigned int)>(nextSymbol("XcursorShapeLoadCursor", nullptr));
    const Cursor cursor = real(dpy, shape);
    if (cursor) {
        CursorSource source;
        source.shape = shape;
        source.size = xcursor<int (*)(Display *)>("XcursorGetDefaultSize")(dpy);
        cursorSources.insert(cursor, source);
    }
    return cursor;
}

extern "C" Cursor XCreatePixmapCursor(Display *dpy, Pixmap source, Pixmap mask, XColor *foreground,
                                      XColor *background, unsigned int x, unsigned int y)
{
    using CreateFn = Cursor (*)(Display *, Pixmap, Pixmap, XColor *, XColor *, unsigned int, unsigned int);
    static auto real = reinterpret_cast<CreateFn>(nextSymbol("XCreatePixmapCursor", nullptr));
    const Cursor cursor = real(dpy, source, mask, foreground, background, x, y);
    if (uintptr_t(__builtin_return_address(0)) == mainExecutableBase() + kBlankCursorReturn)
        blankCursor = cursor;
    return cursor;
}

extern "C" int XDefineCursor(Display *dpy, Window window, Cursor cursor)
{
    static auto real = reinterpret_cast<int (*)(Display *, Window, Cursor)>(nextSymbol("XDefineCursor", nullptr));
    liveCursor = cursor;
    if (cursor && cursor == blankCursor)
        liveHidden = true;
    else if (liveHidden && QGuiApplication::mouseButtons() != Qt::NoButton)
        return 1;
    else
        liveHidden = false;
    const auto source = cursorSources.constFind(cursor);
    if (source != cursorSources.cend()) {
        const int size = cursorSize(dpy);
        if (size != source->size) {
            Cursor &resized = resizedCursors[qMakePair(cursor, size)];
            if (!resized)
                resized = loadCursor(dpy, *source, size);
            if (resized)
                cursor = resized;
        }
    }
    return real(dpy, window, cursor);
}

// Live calls QDrag::exec through its PLT with symbol version Qt_5 (see
// qt5.map). QFlags<Qt::DropAction> is passed as an int.
extern "C" int _ZN5QDrag4execE6QFlagsIN2Qt10DropActionEE(QDrag *drag, int actions)
{
    using ExecFn = int (*)(QDrag *, int);
    static auto real = reinterpret_cast<ExecFn>(nextSymbol("_ZN5QDrag4execE6QFlagsIN2Qt10DropActionEE", "Qt_5"));
    static QPixmap move;
    static int moveSize = 0;
    if (liveDisplay && cursorSize(liveDisplay) != moveSize) {
        moveSize = cursorSize(liveDisplay);
        move = moveCursorPixmap();
    }
    if (!move.isNull() && drag->dragCursor(Qt::MoveAction).isNull())
        drag->setDragCursor(move, Qt::MoveAction);
    skipDragSelection();
    inLiveDrag = true;
    const int result = real(drag, actions);
    inLiveDrag = false;
    return result;
}

void installDragCursor()
{
    const uintptr_t base = mainExecutableBase();
    auto *slot = reinterpret_cast<uintptr_t *>(base + kDragMoveSlot);
    if (*slot != base + kDragMoveEvent) {
        menufixTrace("dragcursor: slot mismatch, not hooked\n");
        return;
    }
    liveDragMove = reinterpret_cast<DragMoveFn>(base + kDragMoveEvent);
    const long page = sysconf(_SC_PAGESIZE);
    auto *start = reinterpret_cast<void *>(uintptr_t(slot) & ~uintptr_t(page - 1));
    if (mprotect(start, page, PROT_READ | PROT_WRITE) != 0) {
        menufixTrace("dragcursor: mprotect failed\n");
        return;
    }
    *slot = reinterpret_cast<uintptr_t>(&dragMoveHook);
    mprotect(start, page, PROT_READ);
    menufixTrace("dragcursor: hooked\n");
}
