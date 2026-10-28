// VST3 Linux run loop for the Push 3 Live desktop build.
//
// On Linux a VST3 plug-in editor gets the host's event loop by asking its
// IPlugFrame for Steinberg::Linux::IRunLoop. Live's two IPlugFrame classes
// only answer FUnknown and IPlugFrame (queryInterface at 0x3e94210 and
// 0x3f91130), so plug-ins such as u-he's never get their X connection
// serviced or their redraw timers fired, and their editors stay blank.
//
// This file wraps both queryInterface vtable slots: IRunLoop requests get a
// Qt-backed run loop (QSocketNotifier for fds, QTimer for timers), everything
// else goes to Live's original function. Addresses are for Live 12.4.7
// (Push 3 2.4.7, BuildID 0e5b75d8...); the patch is skipped if a slot does
// not hold the expected pointer.
#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
#include <QHash>
#include <QList>
#include <QSocketNotifier>
#include <QTimer>
#include <cstdint>
#include <cstring>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);
void pluginViewCreated(void *view); // pluginclose.cpp

// The VST3 interfaces must not live in an anonymous namespace: GCC then
// knows no implementation exists in this program and compiles calls such as
// handler->onFDIsSet() straight to __cxa_pure_virtual.
namespace vst3 {

using tresult = int32_t;
using TUID = char[16];
constexpr tresult kResultOk = 0;
constexpr tresult kNoInterface = -1;
constexpr tresult kInvalidArgument = 2;

// Non-COM (Linux) FUID byte order: four big-endian 32-bit words.
constexpr unsigned char kFUnknownIID[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                            0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
constexpr unsigned char kIRunLoopIID[16] = {0x18, 0xC3, 0x53, 0x66, 0x97, 0x76, 0x4F, 0x1A,
                                            0x9C, 0x5B, 0x83, 0x85, 0x7A, 0x87, 0x13, 0x89};
constexpr unsigned char kIPlugViewContentScaleSupportIID[16] = {0x65, 0xED, 0x96, 0x90, 0x8A, 0xC4, 0x45, 0x25,
                                                                0x8A, 0xAD, 0xEF, 0x7A, 0x72, 0xEA, 0x70, 0x3F};

struct FUnknown {
    virtual tresult queryInterface(const TUID iid, void **obj) = 0;
    virtual uint32_t addRef() = 0;
    virtual uint32_t release() = 0;
};

struct IEventHandler : FUnknown {
    virtual void onFDIsSet(int fd) = 0;
};

struct ITimerHandler : FUnknown {
    virtual void onTimer() = 0;
};

struct IPlugViewContentScaleSupport : FUnknown {
    virtual tresult setContentScaleFactor(float factor) = 0;
};

struct IRunLoop : FUnknown {
    virtual tresult registerEventHandler(IEventHandler *handler, int fd) = 0;
    virtual tresult unregisterEventHandler(IEventHandler *handler) = 0;
    virtual tresult registerTimer(ITimerHandler *handler, uint64_t milliseconds) = 0;
    virtual tresult unregisterTimer(ITimerHandler *handler) = 0;
};

} // namespace vst3

namespace {

using namespace vst3;

class QtRunLoop final : public IRunLoop {
public:
    tresult queryInterface(const TUID iid, void **obj) override
    {
        if (!memcmp(iid, kIRunLoopIID, 16) || !memcmp(iid, kFUnknownIID, 16)) {
            *obj = static_cast<IRunLoop *>(this);
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    // Process-lifetime singleton: reference counting is a no-op.
    uint32_t addRef() override { return 1; }
    uint32_t release() override { return 1; }

    tresult registerEventHandler(IEventHandler *handler, int fd) override
    {
        if (!handler || fd < 0)
            return kInvalidArgument;
        handler->addRef();
        auto *notifier = new QSocketNotifier(fd, QSocketNotifier::Read);
        QObject::connect(notifier, &QSocketNotifier::activated, notifier,
                         [handler, fd] { handler->onFDIsSet(fd); });
        fdHandlers.insert(handler, notifier);
        menufixTrace("runloop: fd handler fd=%d h=%x\n", fd, int(uintptr_t(handler) & 0xffffff));
        return kResultOk;
    }

    tresult unregisterEventHandler(IEventHandler *handler) override
    {
        const auto notifiers = fdHandlers.values(handler);
        menufixTrace("runloop: unregister fd h=%x found=%d\n", int(uintptr_t(handler) & 0xffffff), notifiers.size());
        if (notifiers.isEmpty())
            return kInvalidArgument;
        for (QSocketNotifier *n : notifiers) {
            n->setEnabled(false);
            n->deleteLater();
            handler->release();
        }
        fdHandlers.remove(handler);
        return kResultOk;
    }

    tresult registerTimer(ITimerHandler *handler, uint64_t milliseconds) override
    {
        if (!handler || milliseconds == 0)
            return kInvalidArgument;
        handler->addRef();
        auto *timer = new QTimer;
        timer->setInterval(int(qMin<uint64_t>(milliseconds, INT32_MAX)));
        QObject::connect(timer, &QTimer::timeout, timer, [handler] { handler->onTimer(); });
        timer->start();
        timers.insert(handler, timer);
        menufixTrace("runloop: timer %d ms h=%x\n", int(milliseconds), int(uintptr_t(handler) & 0xffffff));
        return kResultOk;
    }

    tresult unregisterTimer(ITimerHandler *handler) override
    {
        const auto list = timers.values(handler);
        menufixTrace("runloop: unregister timer h=%x found=%d\n", int(uintptr_t(handler) & 0xffffff), list.size());
        if (list.isEmpty())
            return kInvalidArgument;
        for (QTimer *t : list) {
            t->stop();
            t->deleteLater();
            handler->release();
        }
        timers.remove(handler);
        return kResultOk;
    }

private:
    QMultiHash<IEventHandler *, QSocketNotifier *> fdHandlers;
    QMultiHash<ITimerHandler *, QTimer *> timers;
};

QtRunLoop runLoop;

using QueryFn = tresult (*)(void *self, const TUID iid, void **obj);

struct Slot {
    uintptr_t slot;     // vtable slot (file vaddr)
    uintptr_t original; // expected queryInterface (file vaddr)
};
constexpr Slot kSlots[] = {{0x967c740, 0x3e94210}, {0x96978b0, 0x3f91130}};
QueryFn originals[2];

template <int N>
tresult frameQueryInterface(void *self, const TUID iid, void **obj)
{
    if (obj && !memcmp(iid, kIRunLoopIID, 16)) {
        *obj = static_cast<IRunLoop *>(&runLoop);
        menufixTrace("runloop: handed to plug-in (frame %d) self=%x\n", N, int(uintptr_t(self) & 0xffffff));
        return kResultOk;
    }
    return originals[N](self, iid, obj);
}
constexpr QueryFn kWrappers[] = {&frameQueryInterface<0>, &frameQueryInterface<1>};

uintptr_t mainExecutableBase()
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

// Editor scale. Live's Linux build never tells a VST3 editor the desktop
// scale (no code uses IPlugViewContentScaleSupport's IID; the Windows build
// does), so editors open at 1x next to a scaled Live. Live creates the view
// in 0x3f90d30 (out: a {bool, IPlugView *} holder, then sizes its window from
// getSize). The hook passes the scale of the window Live is in right after
// the view exists, before Live asks for the size. LIVE_PLUGIN_SCALE=0 turns
// it off. The hook also hands each new view to pluginclose.cpp, which needs
// it for the editor window Live creates next (resizes by the window manager).
using CreateViewFn = void *(*)(void **out, void *plugin);
CreateViewFn liveCreateView;
constexpr uintptr_t kCreateView = 0x3f90d30;
const unsigned char kCreateViewPrologue[] = {0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec,
                                             0x10, 0x49, 0x89, 0xf7, 0x48, 0x89, 0xfb};

qreal editorScale()
{
    if (QWindow *w = QGuiApplication::focusWindow())
        return w->devicePixelRatio();
    return QGuiApplication::primaryScreen() ? QGuiApplication::primaryScreen()->devicePixelRatio() : 1;
}

bool scaleEditors = true;

void *createView(void **out, void *plugin)
{
    void *result = liveCreateView(out, plugin);
    auto *holder = static_cast<unsigned char *>(*out);
    auto *view = holder ? *reinterpret_cast<FUnknown **>(holder + 8) : nullptr;
    pluginViewCreated(view);
    void *support = nullptr;
    if (!scaleEditors)
        return result;
    if (view && view->queryInterface(reinterpret_cast<const char *>(kIPlugViewContentScaleSupportIID), &support) == kResultOk
        && support) {
        const float scale = float(editorScale());
        const tresult r = static_cast<IPlugViewContentScaleSupport *>(support)->setContentScaleFactor(scale);
        static_cast<IPlugViewContentScaleSupport *>(support)->release();
        menufixTrace("pluginscale: %d%% -> %d\n", qRound(scale * 100), r);
    } else {
        menufixTrace("pluginscale: view %d has no content scale support\n", view != nullptr);
    }
    return result;
}

} // namespace

void *hookFunction(uintptr_t fn, const unsigned char *prologue, size_t len, void *replacement); // hook.cpp

void installVst3RunLoop()
{
    const uintptr_t base = mainExecutableBase();
    const long page = sysconf(_SC_PAGESIZE);
    for (int i = 0; i < 2; ++i) {
        auto *slot = reinterpret_cast<uintptr_t *>(base + kSlots[i].slot);
        if (*slot != base + kSlots[i].original) {
            menufixTrace("runloop: slot %d mismatch, not patched\n", i);
            continue;
        }
        void *pageStart = reinterpret_cast<void *>(uintptr_t(slot) & ~uintptr_t(page - 1));
        if (mprotect(pageStart, page, PROT_READ | PROT_WRITE) != 0) {
            menufixTrace("runloop: mprotect failed for slot %d\n", i);
            continue;
        }
        originals[i] = reinterpret_cast<QueryFn>(*slot);
        *slot = reinterpret_cast<uintptr_t>(kWrappers[i]);
        mprotect(pageStart, page, PROT_READ);
        menufixTrace("runloop: patched IPlugFrame slot %d\n", i);
    }
    scaleEditors = qEnvironmentVariable("LIVE_PLUGIN_SCALE") != QLatin1String("0");
    liveCreateView = reinterpret_cast<CreateViewFn>(hookFunction(base + kCreateView, kCreateViewPrologue,
                                                                 sizeof kCreateViewPrologue, reinterpret_cast<void *>(&createView)));
    menufixTrace("pluginscale: hook %d\n", liveCreateView != nullptr);
}
