// Window presentation for AbletonLive12.
// Live's TQtView paint handler (0x1ff7430) runs AViewManager::OnPaintEvent,
// which executes the frame's render commands in software into the view's
// QImage, then blits that QImage with QPainter. This file takes over the
// handler so the present step can be measured and replaced.
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QWidget>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);

void *hookFunction(uintptr_t fn, const unsigned char *prologue, size_t len, void *replacement); // hook.cpp
void *nextSymbol(const char *name, const char *version);                                          // embedded.cpp

namespace {

constexpr uintptr_t kPaintHandler = 0x1ff7430; // TQtView paint subscriber
constexpr uintptr_t kOnPaintEvent = 0x20c59f0; // AViewManager::OnPaintEvent
const unsigned char kPaintHandlerPrologue[] = {0x53, 0x48, 0x89, 0xfb, 0x80, 0xbf, 0x18, 0x01, 0x00, 0x00, 0x00};

uintptr_t base;
using OnPaintFn = void (*)(void *viewManager);

struct Stats {
    QElapsedTimer clock;
    qint64 renderNs = 0, presentNs = 0;
    int frames = 0;
} stats;

template <typename T> T field(void *obj, size_t offset)
{
    return *reinterpret_cast<T *>(static_cast<char *>(obj) + offset);
}

using VFn = void *(*)(void *);
void *callVirtual(void *obj, size_t slot)
{
    auto vtable = *reinterpret_cast<VFn **>(obj);
    return vtable[slot / 8](obj);
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

// Software executor of a layer's render commands (Windows: FUN_1431f8810).
constexpr uintptr_t kRenderCommands = 0x208bfb0;
const unsigned char kRenderCommandsPrologue[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                                 0x53, 0x48, 0x81, 0xec, 0x38, 0x0b, 0x00, 0x00};
using RenderCommandsFn = void (*)(void *, void *, size_t, int, void *, int, int, int, bool);
RenderCommandsFn originalRenderCommands;
struct {
    qint64 ns = 0;
    int calls = 0;
    size_t commands = 0;
    int w = 0, h = 0, sized = 0;
    qint64 sizedNs = 0;
} swStats;

void renderCommandsTimed(void *self, void *cmds, size_t count, int layer, void *pixels, int stride, int w, int h,
                         bool trace)
{
    QElapsedTimer t;
    t.start();
    originalRenderCommands(self, cmds, count, layer, pixels, stride, w, h, trace);
    swStats.ns += t.nsecsElapsed();
    ++swStats.calls;
    swStats.commands += count;
    if (w > 0 && h > 0) {
        swStats.w = w;
        swStats.h = h;
        ++swStats.sized;
        swStats.sizedNs += t.nsecsElapsed();
    }
}

} // namespace

void livePainted(void *viewManager, void *display); // nativemenu.cpp
bool livePaintHooked; // this file's handler is installed (nativemenu.cpp then uses it)

// Same logic as the original handler, minus the Push-only mode-4 return.
extern "C" void livePaintHandler(void *self)
{
    QElapsedTimer t;
    t.start();
    if (!field<bool>(self, 0x118))
        reinterpret_cast<OnPaintFn>(base + kOnPaintEvent)(field<void *>(self, 0xc0));
    const qint64 rendered = t.nsecsElapsed();

    void *display = callVirtual(field<void *>(self, 0xc8), 0x28);
    auto blit = reinterpret_cast<void (*)(void *, uint64_t)>((*reinterpret_cast<void ***>(display))[0x10 / 8]);
    blit(display, field<uint64_t>(self, 0x80));
    const qint64 total = t.nsecsElapsed();
    livePainted(field<void *>(self, 0xc0), display);

    if (const char *dir = getenv("LIVE_PAINT_DUMP")) {
        static QHash<void *, qint64> lastDump;
        static QElapsedTimer since;
        if (!since.isValid())
            since.start();
        qint64 &last = lastDump[display];
        if (since.elapsed() - last > 3000) {
            last = since.elapsed();
            auto *image = reinterpret_cast<QImage *>(static_cast<char *>(display) + 8);
            image->save(QString("%1/%2.png").arg(dir).arg(quintptr(display), 0, 16));
        }
    }

    stats.renderNs += rendered;
    stats.presentNs += total - rendered;
    ++stats.frames;
    if (!stats.clock.isValid())
        stats.clock.start();
    if (stats.clock.elapsed() >= 2000) {
        menufixTrace("paint: %d frames, render avg %d us, present avg %d us\n", stats.frames,
                     int(stats.renderNs / stats.frames / 1000), int(stats.presentNs / stats.frames / 1000));
        if (swStats.calls)
            menufixTrace("paint: sw commands %d calls, avg %d us, avg %d cmds\n", swStats.calls,
                         int(swStats.ns / swStats.calls / 1000), int(swStats.commands / swStats.calls));
        menufixTrace("paint: last sw target %dx%d\n", swStats.w, swStats.h);
        if (swStats.sized)
            menufixTrace("paint: sized sw calls %d, total %d us\n", swStats.sized, int(swStats.sizedNs / 1000));
        swStats = {};
        stats = Stats();
        stats.clock.start();
    }
}

namespace {

// Live's frame buffer for a window (the {size, stride, bits, ..., pixels}
// block at the window object + 0x80, which the display's QImage wraps) is
// reallocated by ResizeFrameBuffer (0x1ff37f0) on every resize of the view:
// a new malloc'd buffer, the old one freed. Live then renders into it what it
// considers changed. That is everything when its layout changed, but not when
// it did not: after a zoom change Live converts the window size to its own
// units and back, asks for a window 1 px larger than before (1920x1173 for
// 1172 at 130 %), the window manager keeps a maximized window at its size,
// and the second resize gives Live a fresh, empty buffer for a layout it
// already rendered. Most of the window then stayed black or showed garbage
// until the next real resize, after some zoom changes only (seen in about a
// third of them with Live's own menu bar).
//
// The hook keeps the previous frame: the new buffer starts with the old
// buffer's pixels (where both overlap), as a window system keeps a window's
// contents on resize, so a partial redraw lands on a complete picture.
// ResizeFrameBuffer frees the old pixels itself; it is handed a placeholder
// to free instead, and the old pixels are freed after the copy.
constexpr uintptr_t kResizeFrameBuffer = 0x1ff37f0; // (display, packed size, frame buffer *)
const unsigned char kResizeFrameBufferPrologue[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                                    0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x38};
struct FrameBuffer {
    int32_t width, height;
    int32_t stride, bits;
    uint64_t reserved;
    unsigned char *pixels; // malloc'd
};
using ResizeFrameBufferFn = void (*)(void *, uint64_t, FrameBuffer *);
ResizeFrameBufferFn liveResizeFrameBuffer;

void resizeFrameBuffer(void *display, uint64_t size, FrameBuffer *buffer)
{
    const FrameBuffer old = *buffer;
    void *placeholder = old.pixels ? malloc(1) : nullptr;
    if (old.pixels && !placeholder)
        return liveResizeFrameBuffer(display, size, buffer);
    buffer->pixels = static_cast<unsigned char *>(placeholder);
    liveResizeFrameBuffer(display, size, buffer);
    if (!old.pixels)
        return;
    if (buffer->pixels && buffer->bits == 32 && old.bits == 32) {
        const int rows = qMin(old.height, buffer->height);
        const size_t bytes = size_t(qMax(0, qMin(old.width, buffer->width))) * 4;
        for (int y = 0; y < rows; ++y)
            memcpy(buffer->pixels + size_t(y) * buffer->stride, old.pixels + size_t(y) * old.stride, bytes);
    }
    free(old.pixels);
}

} // namespace

namespace {

// Partial redraws at a fractional scale. Live keeps the whole window in its
// frame buffer, in device pixels, and at each frame renders again only what
// changed: AViewManager::OnPaintEvent hands its frame render (0x20d8440:
// renderer, root view, view manager, {rects, count}, flag; an empty list
// renders everything) a list of dirty rectangles, {left, top, right, bottom}
// in Live's units. The rendering is clipped to them in device pixels. At a
// whole-number ratio a unit is whole pixels; at 1.25 or 1.75 a rectangle's
// edge falls inside a pixel, and that edge column is not rendered again
// while what Live draws there (anti-aliased) reaches into it. With the
// transport running the playhead's rectangle is one unit wide ([697 115 698
// 579]) and moves at every frame, so each frame left a column of the old
// playhead behind: the arrangement filled with stale playhead lines, over
// clips too (smeared waveforms), and clicks left extra insert markers.
// Each rectangle is widened by one unit on every side at a fractional ratio,
// which covers the partial pixels; Live renders those from its own state,
// so nothing else changes.
constexpr uintptr_t kRenderFrame = 0x20d8440;
const unsigned char kRenderFramePrologue[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                              0x53, 0x48, 0x81, 0xec, 0xc8, 0x01, 0x00, 0x00};
struct DirtyRects {
    const int32_t *rects; // count x {left, top, right, bottom}
    size_t count;
};
using RenderFrameFn = void (*)(void *, void *, void *, DirtyRects *, int);
RenderFrameFn liveRenderFrame;

void renderFrame(void *renderer, void *root, void *viewManager, DirtyRects *dirty, int flag)
{
    const qreal ratio = qApp ? qApp->devicePixelRatio() : 1;
    if (!dirty || !dirty->count || ratio == qRound(ratio))
        return liveRenderFrame(renderer, root, viewManager, dirty, flag);
    std::vector<int32_t> wider(dirty->rects, dirty->rects + 4 * dirty->count);
    for (size_t i = 0; i < wider.size(); i += 4) {
        wider[i] = qMax(0, wider[i] - 1);
        wider[i + 1] = qMax(0, wider[i + 1] - 1);
        wider[i + 2] += 1;
        wider[i + 3] += 1;
    }
    DirtyRects widened{wider.data(), dirty->count};
    liveRenderFrame(renderer, root, viewManager, &widened, flag);
}

} // namespace

void *nextSymbol(const char *name, const char *version); // embedded.cpp

// The second half: Live then asks Qt to present the same rectangles
// (QWidget::update(QRect), 0x1ff43d0 and 0x1ff4430, in Live's units). Qt
// converts them to device pixels by rounding position and size separately
// and paints and flushes only those pixels, so the partial column is again
// left out: Live's frame buffer was right, the screen was not. The same
// widening applies.
extern "C" void _ZN7QWidget6updateERK5QRect(QWidget *w, const QRect &rect)
{
    using UpdateFn = void (*)(QWidget *, const QRect &);
    static auto real = reinterpret_cast<UpdateFn>(nextSymbol("_ZN7QWidget6updateERK5QRect", "Qt_5"));
    const qreal ratio = w->devicePixelRatioF();
    real(w, ratio == qRound(ratio) ? rect : rect.adjusted(-1, -1, 1, 1));
}

// Always on (not a diagnostic).
void installFrameBufferHook()
{
    const uintptr_t b = mainBase();
    liveResizeFrameBuffer = reinterpret_cast<ResizeFrameBufferFn>(hookFunction(
        b + kResizeFrameBuffer, kResizeFrameBufferPrologue, sizeof kResizeFrameBufferPrologue,
        reinterpret_cast<void *>(&resizeFrameBuffer)));
    menufixTrace("paint: frame buffer hook %d\n", liveResizeFrameBuffer != nullptr);
    liveRenderFrame = reinterpret_cast<RenderFrameFn>(hookFunction(
        b + kRenderFrame, kRenderFramePrologue, sizeof kRenderFramePrologue, reinterpret_cast<void *>(&renderFrame)));
    menufixTrace("paint: dirty rects hook %d\n", liveRenderFrame != nullptr);
}

void installPaintHook()
{
    if (!getenv("LIVE_PAINT_HOOK"))
        return;
    base = mainBase();
    originalRenderCommands = reinterpret_cast<RenderCommandsFn>(
        hookFunction(base + kRenderCommands, kRenderCommandsPrologue, sizeof kRenderCommandsPrologue,
                     reinterpret_cast<void *>(&renderCommandsTimed)));
    menufixTrace("paint: sw executor hook %d\n", originalRenderCommands != nullptr);
    auto *code = reinterpret_cast<unsigned char *>(base + kPaintHandler);
    if (memcmp(code, kPaintHandlerPrologue, sizeof kPaintHandlerPrologue) != 0) {
        menufixTrace("paint: handler bytes differ, not hooked\n");
        return;
    }
    // movabs rax, livePaintHandler; jmp rax
    unsigned char jump[12] = {0x48, 0xb8};
    const uintptr_t target = reinterpret_cast<uintptr_t>(&livePaintHandler);
    memcpy(jump + 2, &target, 8);
    jump[10] = 0xff;
    jump[11] = 0xe0;
    const long page = sysconf(_SC_PAGESIZE);
    auto *pageStart = reinterpret_cast<void *>(uintptr_t(code) & ~uintptr_t(page - 1));
    const size_t len = (uintptr_t(code) + sizeof jump > uintptr_t(pageStart) + page) ? 2 * page : page;
    if (mprotect(pageStart, len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        menufixTrace("paint: mprotect failed\n");
        return;
    }
    memcpy(code, jump, sizeof jump);
    mprotect(pageStart, len, PROT_READ | PROT_EXEC);
    __builtin___clear_cache(reinterpret_cast<char *>(code), reinterpret_cast<char *>(code) + sizeof jump);
    livePaintHooked = true;
    menufixTrace("paint: handler hooked\n");
}
