// PipeWire devices in Live's audio device menus.
//
// Live lists sound cards itself (0x2d60150..0x2d60820): snd_card_next, then
// per card snd_ctl_open("hw:<card>"), snd_ctl_card_info, and per PCM device
// snd_ctl_pcm_info for playback and capture; a device is shown as the card
// name, a space and the PCM name, and opened as "hw:<card>,<device>". All its
// opens go to PipeWire (alsaaudio.cpp), so the menu showed the raw cards and
// the choice did nothing: sound went to the desktop's default device.
//
// Live's scan now sees PipeWire's devices instead, as cards from 100 up (no
// real card has such a number): 100 is "System Default" (both directions,
// follows the desktop's default devices), then one card per output (sink)
// and input (source), named by its description. Opening one of them opens the
// "pipewire" PCM aimed at that device. Card numbers stay with their device for
// the whole run, so a device unplugged and plugged back keeps its number.
// Devices added or removed while Live runs reach its menus through Live's own
// hotplug check (see watchDevices below).
//
// The list comes from the PulseAudio API (pipewire-pulse), loaded with dlopen;
// without it only "System Default" is listed. LIVE_ALSA_DEVICES=hw shows the
// sound cards as before; LIVE_ALSA_EXCLUSIVE and LIVE_ALSA_DEVICE do too.
#include "pwdevices.h"
#include <alsa/asoundlib.h>
#include <dlfcn.h>
#include <link.h>
#include <pulse/pulseaudio.h>
#include <sys/stat.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);
void *nextSymbol(const char *name, const char *version); // embedded.cpp

namespace {

std::mutex devicesMutex;
std::vector<VirtualDevice> devices; // index = card - kFirstVirtualCard; entries are never removed
std::vector<bool> present;           // in the latest scan

bool fromLive(const void *returnAddress)
{
    Dl_info info;
    link_map *map = nullptr;
    if (!dladdr1(returnAddress, &info, reinterpret_cast<void **>(&map), RTLD_DL_LINKMAP) || !map)
        return false;
    return map->l_name == nullptr || map->l_name[0] == '\0';
}

// --- the PulseAudio API, loaded on first use -------------------------------

struct Pulse {
    decltype(&pa_mainloop_new) mainloopNew;
    decltype(&pa_mainloop_get_api) mainloopApi;
    decltype(&pa_mainloop_prepare) prepare;
    decltype(&pa_mainloop_poll) poll;
    decltype(&pa_mainloop_dispatch) dispatch;
    decltype(&pa_mainloop_free) mainloopFree;
    decltype(&pa_context_new) contextNew;
    decltype(&pa_context_connect) connect;
    decltype(&pa_context_get_state) state;
    decltype(&pa_context_get_server_info) serverInfo;
    decltype(&pa_context_get_sink_info_list) sinks;
    decltype(&pa_context_get_source_info_list) sources;
    decltype(&pa_operation_get_state) operationState;
    decltype(&pa_operation_unref) operationUnref;
    decltype(&pa_context_disconnect) disconnect;
    decltype(&pa_context_unref) contextUnref;
    decltype(&pa_mainloop_iterate) iterate;
    decltype(&pa_context_set_subscribe_callback) setSubscribeCallback;
    decltype(&pa_context_subscribe) subscribe;
};

const Pulse *pulse()
{
    static Pulse api;
    static const bool loaded = [] {
        void *lib = dlopen("libpulse.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!lib)
            return false;
        bool ok = true;
        auto get = [&](auto &fn, const char *name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            ok = ok && fn;
        };
        get(api.mainloopNew, "pa_mainloop_new");
        get(api.mainloopApi, "pa_mainloop_get_api");
        get(api.prepare, "pa_mainloop_prepare");
        get(api.poll, "pa_mainloop_poll");
        get(api.dispatch, "pa_mainloop_dispatch");
        get(api.mainloopFree, "pa_mainloop_free");
        get(api.contextNew, "pa_context_new");
        get(api.connect, "pa_context_connect");
        get(api.state, "pa_context_get_state");
        get(api.serverInfo, "pa_context_get_server_info");
        get(api.sinks, "pa_context_get_sink_info_list");
        get(api.sources, "pa_context_get_source_info_list");
        get(api.operationState, "pa_operation_get_state");
        get(api.operationUnref, "pa_operation_unref");
        get(api.disconnect, "pa_context_disconnect");
        get(api.contextUnref, "pa_context_unref");
        get(api.iterate, "pa_mainloop_iterate");
        get(api.setSubscribeCallback, "pa_context_set_subscribe_callback");
        get(api.subscribe, "pa_context_subscribe");
        return ok;
    }();
    return loaded ? &api : nullptr;
}

struct Node {
    std::string name, description;
    bool playback;
    unsigned channels;
};

struct Query {
    std::vector<Node> nodes;
    std::string defaultSink, defaultSource;
    int pending = 0;
};

// Outputs and inputs (monitors left out) and the default devices; false when
// the server cannot be reached within a second and a half.
bool queryPulse(Query &query)
{
    const Pulse *pa = pulse();
    if (!pa)
        return false;
    pa_mainloop *loop = pa->mainloopNew();
    pa_context *context = pa->contextNew(pa->mainloopApi(loop), "Ableton Live device list");
    bool ok = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    auto iterate = [&] {
        if (pa->prepare(loop, 100000) < 0 || pa->poll(loop) < 0 || pa->dispatch(loop) < 0)
            return false;
        return std::chrono::steady_clock::now() < deadline;
    };
    if (pa->connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) >= 0) {
        while (iterate()) {
            const pa_context_state_t state = pa->state(context);
            if (state == PA_CONTEXT_READY) {
                ok = true;
                break;
            }
            if (!PA_CONTEXT_IS_GOOD(state))
                break;
        }
    }
    if (ok) {
        query.pending = 3;
        pa_operation *operations[] = {
            pa->serverInfo(context, [](pa_context *, const pa_server_info *info, void *data) {
                auto *q = static_cast<Query *>(data);
                if (info) {
                    q->defaultSink = info->default_sink_name ? info->default_sink_name : "";
                    q->defaultSource = info->default_source_name ? info->default_source_name : "";
                }
                --q->pending;
            }, &query),
            pa->sinks(context, [](pa_context *, const pa_sink_info *info, int eol, void *data) {
                auto *q = static_cast<Query *>(data);
                if (eol)
                    --q->pending;
                else if (info)
                    q->nodes.push_back({info->name, info->description ? info->description : info->name, true, info->channel_map.channels});
            }, &query),
            pa->sources(context, [](pa_context *, const pa_source_info *info, int eol, void *data) {
                auto *q = static_cast<Query *>(data);
                if (eol)
                    --q->pending;
                else if (info && info->monitor_of_sink == PA_INVALID_INDEX)
                    q->nodes.push_back({info->name, info->description ? info->description : info->name, false, info->channel_map.channels});
            }, &query),
        };
        while (query.pending > 0 && iterate()) {
        }
        ok = query.pending == 0;
        for (pa_operation *operation : operations)
            if (operation)
                pa->operationUnref(operation);
        pa->disconnect(context);
    }
    pa->contextUnref(context);
    pa->mainloopFree(loop);
    return ok;
}

// Called at the start of each of Live's scans.
void rescan()
{
    Query query;
    const bool ok = queryPulse(query);
    std::lock_guard<std::mutex> lock(devicesMutex);
    if (devices.empty())
        devices.push_back({"", "System Default", true, true, 2, 2});
    present.assign(devices.size(), false);
    present[0] = true;
    if (!ok) {
        menufixTrace("pwdevices: no PulseAudio/PipeWire server answered; listing System Default only\n");
        return;
    }
    for (const Node &node : query.nodes) {
        if (node.name == query.defaultSink)
            devices[0].playbackChannels = node.channels;
        if (node.name == query.defaultSource)
            devices[0].captureChannels = node.channels;
        size_t i = 1;
        while (i < devices.size() && !(devices[i].node == node.name && devices[i].playback == node.playback))
            ++i;
        if (i == devices.size()) {
            devices.push_back({node.name, node.description, node.playback, !node.playback, node.channels, node.channels});
            present.push_back(false);
        }
        devices[i].name = node.description;
        devices[i].playbackChannels = devices[i].captureChannels = node.channels;
        present[i] = true;
    }
    menufixTrace("pwdevices: %d PipeWire devices\n", int(query.nodes.size()));
}

// Live's snd_ctl_t and info structs for the listed devices.
char fakeControls[1024];

int controlCard(snd_ctl_t *ctl)
{
    const char *p = reinterpret_cast<const char *>(ctl);
    if (p < fakeControls || p >= fakeControls + sizeof fakeControls)
        return -1;
    return kFirstVirtualCard + int(p - fakeControls);
}

// Info structs Live filled through a fake control: Live's memory, remembered
// with the card they describe (the latest first).
struct Filled {
    const void *info;
    int card;
};
Filled filledInfos[16];
unsigned nextFilled;
std::mutex filledMutex;

void remember(const void *info, int card)
{
    std::lock_guard<std::mutex> lock(filledMutex);
    for (Filled &f : filledInfos)
        if (f.info == info)
            f.info = nullptr;
    filledInfos[nextFilled++ % 16] = {info, card};
}

int filledCard(const void *info)
{
    std::lock_guard<std::mutex> lock(filledMutex);
    for (const Filled &f : filledInfos)
        if (f.info == info)
            return f.card;
    return -1;
}

// Where Live's name strings point: they must outlive the info struct.
const char *stableName(int card)
{
    std::lock_guard<std::mutex> lock(devicesMutex);
    static std::deque<std::string> names; // growing keeps the strings in place; Live copies them at once
    const size_t i = card - kFirstVirtualCard;
    if (names.size() < devices.size())
        names.resize(devices.size());
    if (i >= devices.size())
        return "";
    if (names[i] != devices[i].name)
        names[i] = devices[i].name;
    return names[i].c_str();
}

// Hotplug. Live polls the modification date of /dev/snd (0x2d60d30, "Failed
// to get the mod date of /dev/snd") and rescans its cards when it changed,
// which is how it sees a USB interface come and go. A thread subscribed to
// PipeWire's outputs and inputs counts additions and removals, and stat() of
// /dev/snd reports the date that many seconds later, so Live rescans then.
std::atomic<int> deviceChanges{0};

void watchDevices()
{
    const Pulse *pa = pulse();
    if (!pa)
        return;
    for (;;) {
        pa_mainloop *loop = pa->mainloopNew();
        pa_context *context = pa->contextNew(pa->mainloopApi(loop), "Ableton Live device watch");
        if (pa->connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) >= 0) {
            bool subscribed = false;
            while (pa->iterate(loop, 1, nullptr) >= 0) {
                const pa_context_state_t state = pa->state(context);
                if (!PA_CONTEXT_IS_GOOD(state))
                    break;
                if (state == PA_CONTEXT_READY && !subscribed) {
                    pa->setSubscribeCallback(context, [](pa_context *, pa_subscription_event_type_t type, uint32_t, void *) {
                        const int what = type & PA_SUBSCRIPTION_EVENT_TYPE_MASK;
                        if (what == PA_SUBSCRIPTION_EVENT_NEW || what == PA_SUBSCRIPTION_EVENT_REMOVE)
                            ++deviceChanges;
                    }, nullptr);
                    if (pa_operation *operation = pa->subscribe(context, pa_subscription_mask_t(PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE), nullptr, nullptr))
                        pa->operationUnref(operation);
                    subscribed = true;
                }
            }
        }
        pa->contextUnref(context);
        pa->mainloopFree(loop);
        ++deviceChanges; // the server went away (restart): the devices may differ when it is back
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

void startWatching()
{
    static std::once_flag once;
    std::call_once(once, [] { std::thread(watchDevices).detach(); });
}

template <typename Stat, typename Fn>
int statDevSnd(Fn real, const char *path, Stat *buf)
{
    const int result = real(path, buf);
    if (result == 0 && path && !strcmp(path, "/dev/snd") && virtualDevicesEnabled())
        buf->st_mtim.tv_sec += deviceChanges.load();
    return result;
}

} // namespace

bool virtualDevicesEnabled()
{
    static const bool enabled = [] {
        const char *mode = getenv("LIVE_ALSA_DEVICES");
        return !(mode && !strcmp(mode, "hw")) && !getenv("LIVE_ALSA_EXCLUSIVE") && !getenv("LIVE_ALSA_DEVICE");
    }();
    return enabled;
}

bool virtualDevice(int card, VirtualDevice &device)
{
    std::lock_guard<std::mutex> lock(devicesMutex);
    const size_t i = card - kFirstVirtualCard;
    if (card < kFirstVirtualCard || i >= devices.size())
        return false;
    device = devices[i];
    return true;
}

// Live's references are versioned ALSA_0.9 (shim/qt5.map).
extern "C" int snd_card_next(int *card)
{
    using Fn = int (*)(int *);
    static auto real = reinterpret_cast<Fn>(nextSymbol("snd_card_next", "ALSA_0.9"));
    if (!card || !virtualDevicesEnabled() || !fromLive(__builtin_return_address(0)))
        return real(card);
    if (*card < kFirstVirtualCard) {
        startWatching();
        rescan();
    }
    std::lock_guard<std::mutex> lock(devicesMutex);
    size_t i = *card < kFirstVirtualCard ? 0 : *card - kFirstVirtualCard + 1;
    while (i < devices.size() && !present[i])
        ++i;
    *card = i < devices.size() ? kFirstVirtualCard + int(i) : -1;
    return 0;
}

extern "C" int snd_ctl_open(snd_ctl_t **ctl, const char *name, int mode)
{
    using Fn = int (*)(snd_ctl_t **, const char *, int);
    static auto real = reinterpret_cast<Fn>(nextSymbol("snd_ctl_open", "ALSA_0.9"));
    if (ctl && name && !strncmp(name, "hw:", 3) && virtualDevicesEnabled() && fromLive(__builtin_return_address(0))) {
        const int card = atoi(name + 3);
        VirtualDevice device;
        if (card >= kFirstVirtualCard && card - kFirstVirtualCard < int(sizeof fakeControls) && virtualDevice(card, device)) {
            *ctl = reinterpret_cast<snd_ctl_t *>(fakeControls + (card - kFirstVirtualCard));
            return 0;
        }
    }
    return real(ctl, name, mode);
}

extern "C" int snd_ctl_close(snd_ctl_t *ctl)
{
    using Fn = int (*)(snd_ctl_t *);
    static auto real = reinterpret_cast<Fn>(nextSymbol("snd_ctl_close", "ALSA_0.9"));
    return controlCard(ctl) >= 0 ? 0 : real(ctl);
}

extern "C" int snd_ctl_card_info(snd_ctl_t *ctl, snd_ctl_card_info_t *info)
{
    using Fn = int (*)(snd_ctl_t *, snd_ctl_card_info_t *);
    static auto real = reinterpret_cast<Fn>(nextSymbol("snd_ctl_card_info", "ALSA_0.9"));
    const int card = controlCard(ctl);
    if (card < 0)
        return real(ctl, info);
    remember(info, card);
    return 0;
}

extern "C" const char *snd_ctl_card_info_get_name(const snd_ctl_card_info_t *info)
{
    using Fn = const char *(*)(const snd_ctl_card_info_t *);
    static auto real = reinterpret_cast<Fn>(nextSymbol("snd_ctl_card_info_get_name", "ALSA_0.9"));
    const int card = filledCard(info);
    return card >= 0 ? stableName(card) : real(info);
}

// One PCM device (0) per listed device.
extern "C" int snd_ctl_pcm_next_device(snd_ctl_t *ctl, int *device)
{
    using Fn = int (*)(snd_ctl_t *, int *);
    static auto real = reinterpret_cast<Fn>(nextSymbol("snd_ctl_pcm_next_device", "ALSA_0.9"));
    if (controlCard(ctl) < 0)
        return real(ctl, device);
    *device = *device < 0 ? 0 : -1;
    return 0;
}

extern "C" int snd_ctl_pcm_info(snd_ctl_t *ctl, snd_pcm_info_t *info)
{
    using Fn = int (*)(snd_ctl_t *, snd_pcm_info_t *);
    static auto real = reinterpret_cast<Fn>(nextSymbol("snd_ctl_pcm_info", "ALSA_0.9"));
    const int card = controlCard(ctl);
    if (card < 0)
        return real(ctl, info);
    VirtualDevice device;
    if (!virtualDevice(card, device) || snd_pcm_info_get_device(info) != 0)
        return -ENOENT;
    const bool playback = snd_pcm_info_get_stream(info) == SND_PCM_STREAM_PLAYBACK;
    if (playback ? !device.playback : !device.capture)
        return -ENOENT;
    remember(info, card);
    return 0;
}

// Live shows "<card name> <PCM name>"; the card name says it all.
extern "C" const char *snd_pcm_info_get_name(const snd_pcm_info_t *info)
{
    using Fn = const char *(*)(const snd_pcm_info_t *);
    static auto real = reinterpret_cast<Fn>(nextSymbol("snd_pcm_info_get_name", "ALSA_0.9"));
    return filledCard(info) >= 0 ? "" : real(info);
}

// Live's references are versioned GLIBC_2.33 (shim/qt5.map).
extern "C" int stat(const char *path, struct stat *buf)
{
    using Fn = int (*)(const char *, struct stat *);
    static auto real = reinterpret_cast<Fn>(nextSymbol("stat", "GLIBC_2.33"));
    return statDevSnd(real, path, buf);
}

extern "C" int stat64(const char *path, struct stat64 *buf)
{
    using Fn = int (*)(const char *, struct stat64 *);
    static auto real = reinterpret_cast<Fn>(nextSymbol("stat64", "GLIBC_2.33"));
    return statDevSnd(real, path, buf);
}
