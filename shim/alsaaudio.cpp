// Audio through PipeWire for AbletonLive12.
//
// Live's ALSA driver lists sound cards itself (snd_card_next,
// snd_ctl_pcm_next_device) and opens the chosen one as "hw:CARD=...,DEV=..."
// with mmap access. A hw device has one user, so while Live runs the desktop
// (PipeWire) loses that card, and Live fails with "Device or resource busy"
// when PipeWire already holds it. On the Push, Live owns the hardware alone.
//
// Live's hw opens are sent to the "pipewire" ALSA PCM instead (pipewire-alsa;
// it supports the mmap access Live uses). Live keeps its own device list and
// settings; the sound goes to PipeWire's default sink/source, which the
// desktop routes. LIVE_ALSA_DEVICE=<pcm> picks another ALSA PCM,
// LIVE_ALSA_EXCLUSIVE=1 keeps Live's direct hardware access.
//
// Sample format: Live offers S32_LE, S24_3LE, S16_LE and FLOAT_LE in one mask
// (0x2d625c6) and takes whatever snd_pcm_hw_params settles on, which is the
// lowest: S16_LE whenever the device has it, pipewire-alsa included. Its own
// mixing is float, so the output was cut to 16 bits and converted twice. The
// mask is narrowed to the best format the device takes: FLOAT_LE, then
// S32_LE, S24_3LE, S16_LE (LIVE_ALSA_FORMAT=device leaves the choice to ALSA).
//
// Channels: Live sets the channel count to snd_pcm_hw_params_get_channels_max
// (0x2d626ac). The pipewire PCM offers 1..128, so Live opened 128-channel
// streams and listed 64 stereo pairs in Input/Output Config, and PipeWire
// converted 128 channels every cycle. The count is capped at the channels of
// the hw device Live chose (opened briefly, or read from /proc/asound while
// PipeWire holds it; 2 when neither works). LIVE_ALSA_CHANNELS=<n> sets the cap.
#include <alsa/asoundlib.h>
#include <dlfcn.h>
#include <link.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include "pwdevices.h"

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);
void *nextSymbol(const char *name, const char *version); // embedded.cpp

namespace {

using OpenFn = int (*)(snd_pcm_t **, const char *, snd_pcm_stream_t, int);

// Shared PCMs opened for Live, with the channel count of the device Live asked for.
struct SharedPcm {
    snd_pcm_t *pcm;
    unsigned channels;
};
SharedPcm sharedPcms[8];
unsigned nextShared;

// "hw:CARD=Generic_1,DEV=0" or "hw:1,0" -> /proc/asound/<card>/pcm<dev><p|c>/sub0/hw_params
std::string hwParamsPath(const char *name, snd_pcm_stream_t stream)
{
    std::string spec = strchr(name, ':') + 1, card, device = "0";
    size_t comma = spec.find(',');
    card = spec.substr(0, comma);
    if (comma != std::string::npos)
        device = spec.substr(comma + 1);
    if (!card.compare(0, 5, "CARD="))
        card = card.substr(5);
    else
        card = "card" + card;
    if (!device.compare(0, 4, "DEV="))
        device = device.substr(4);
    return "/proc/asound/" + card + "/pcm" + device + (stream == SND_PCM_STREAM_PLAYBACK ? "p" : "c") + "/sub0/hw_params";
}

unsigned deviceChannels(OpenFn open, const char *name, snd_pcm_stream_t stream)
{
    if (const char *forced = getenv("LIVE_ALSA_CHANNELS"))
        return strtoul(forced, nullptr, 10);
    std::string hw = std::string("hw:") + (strchr(name, ':') + 1); // plughw converts any count; ask the hw device
    snd_pcm_t *device;
    unsigned channels = 0;
    if (open(&device, hw.c_str(), stream, SND_PCM_NONBLOCK) == 0) {
        snd_pcm_hw_params_t *params;
        snd_pcm_hw_params_alloca(&params);
        if (snd_pcm_hw_params_any(device, params) < 0 || snd_pcm_hw_params_get_channels_max(params, &channels) < 0)
            channels = 0;
        snd_pcm_close(device);
    } else if (FILE *file = fopen(hwParamsPath(hw.c_str(), stream).c_str(), "r")) {
        // busy: PipeWire has it open, with the channels it uses
        char line[128];
        while (fgets(line, sizeof line, file))
            if (sscanf(line, "channels: %u", &channels) == 1)
                break;
        fclose(file);
    }
    return channels ? channels : 2;
}

// Only Live's own opens: alsa-lib and its plug-ins open hw devices internally too.
bool calledFromLive(const void *returnAddress)
{
    Dl_info info;
    link_map *map = nullptr;
    if (!dladdr1(returnAddress, &info, reinterpret_cast<void **>(&map), RTLD_DL_LINKMAP) || !map)
        return false;
    return map->l_name == nullptr || map->l_name[0] == '\0'; // the main program
}

} // namespace

// Live's reference is versioned ALSA_0.9 (shim/qt5.map).
extern "C" int snd_pcm_hw_params_set_format_mask(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_format_mask_t *mask)
{
    using SetMaskFn = int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_format_mask_t *);
    static auto real = reinterpret_cast<SetMaskFn>(nextSymbol("snd_pcm_hw_params_set_format_mask", "ALSA_0.9"));
    const char *choice = getenv("LIVE_ALSA_FORMAT");
    if ((choice && !strcmp(choice, "device")) || !calledFromLive(__builtin_return_address(0)))
        return real(pcm, params, mask);
    for (snd_pcm_format_t format : {SND_PCM_FORMAT_FLOAT_LE, SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_S24_3LE}) {
        if (!snd_pcm_format_mask_test(mask, format) || snd_pcm_hw_params_test_format(pcm, params, format) != 0)
            continue;
        snd_pcm_format_mask_t *best;
        snd_pcm_format_mask_alloca(&best);
        snd_pcm_format_mask_none(best);
        snd_pcm_format_mask_set(best, format);
        if (real(pcm, params, best) == 0) {
            menufixTrace("alsaaudio: sample format %d\n", format);
            return 0;
        }
    }
    return real(pcm, params, mask);
}

// Live's reference is versioned ALSA_0.9; shim/qt5.map gives this definition that version.
extern "C" int snd_pcm_open(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream, int mode)
{
    static auto real = reinterpret_cast<OpenFn>(nextSymbol("snd_pcm_open", "ALSA_0.9"));
    const bool hw = name && (!strncmp(name, "hw:", 3) || !strncmp(name, "plughw:", 7));
    if (hw && !getenv("LIVE_ALSA_EXCLUSIVE") && calledFromLive(__builtin_return_address(0))) {
        VirtualDevice device;
        if (virtualDevicesEnabled() && virtualDevice(atoi(strchr(name, ':') + 1), device)) {
            // a PipeWire device from Live's menu (pwdevices.cpp)
            const std::string target = device.node.empty() ? "pipewire" : "pipewire:NODE=\"" + device.node + "\"";
            // A device chosen by name that goes away is not replaced by the
            // desktop's default (the laptop's speakers and microphone, which feed
            // back while an input is monitored): the stream fails and Live shows
            // No Device, as for an unplugged interface. node.dont-fallback would
            // also do that, but WirePlumber then refused to link to sound cards
            // at all (Live stuck with "Audio queue timeout").
            const char *props = getenv("PIPEWIRE_ALSA");
            const std::string saved = props ? props : "";
            if (!device.node.empty() && !saved.empty() && saved.back() == '}')
                setenv("PIPEWIRE_ALSA", (saved.substr(0, saved.size() - 1) + "node.dont-reconnect = true }").c_str(), 1);
            const int result = real(pcm, target.c_str(), stream, mode);
            if (props)
                setenv("PIPEWIRE_ALSA", saved.c_str(), 1);
            if (result == 0) {
                const unsigned channels = stream == SND_PCM_STREAM_PLAYBACK ? device.playbackChannels : device.captureChannels;
                sharedPcms[nextShared++ % 8] = {*pcm, channels ? channels : 2};
            } else {
                menufixTrace("alsaaudio: opening the PipeWire device failed (%d)\n", result);
            }
            return result;
        }
        const char *shared = getenv("LIVE_ALSA_DEVICE") ? getenv("LIVE_ALSA_DEVICE") : "pipewire";
        const unsigned channels = deviceChannels(real, name, stream);
        const int result = real(pcm, shared, stream, mode);
        if (result == 0) {
            sharedPcms[nextShared++ % 8] = {*pcm, channels};
            return 0;
        }
        menufixTrace("alsaaudio: opening the shared PCM failed (%d), using the hardware\n", result);
    }
    return real(pcm, name, stream, mode);
}

// Live's reference is versioned ALSA_0.9 (shim/qt5.map).
extern "C" int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, unsigned int val)
{
    using SetChannelsFn = int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned int);
    static auto real = reinterpret_cast<SetChannelsFn>(nextSymbol("snd_pcm_hw_params_set_channels", "ALSA_0.9"));
    if (calledFromLive(__builtin_return_address(0))) {
        for (const SharedPcm &shared : sharedPcms) {
            if (shared.pcm != pcm || val <= shared.channels)
                continue;
            menufixTrace("alsaaudio: %d channels instead of %d\n", shared.channels, val);
            val = shared.channels;
            break;
        }
    }
    return real(pcm, params, val);
}

// Live asks for a buffer of 3 periods (0x2d62683). Through pipewire-alsa the
// round trip is then one period longer than the latency Live reports and
// compensates recordings with (4 periods measured, 3 reported). The shared PCM
// gets 2 periods: the round trip matches Live's figure (8.00 ms at 128) and is
// one period shorter, with no more dropouts in the tests. LIVE_ALSA_PERIODS=<n>
// sets another count (3: Live's own). Periods under 64 frames keep 3: PipeWire
// runs 64-frame cycles then, which 2 periods of 32 cannot cover.
extern "C" int snd_pcm_hw_params_set_buffer_size(snd_pcm_t *pcm, snd_pcm_hw_params_t *params, snd_pcm_uframes_t val)
{
    using SetBufferFn = int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, snd_pcm_uframes_t);
    static auto real = reinterpret_cast<SetBufferFn>(nextSymbol("snd_pcm_hw_params_set_buffer_size", "ALSA_0.9"));
    const int periods = getenv("LIVE_ALSA_PERIODS") ? atoi(getenv("LIVE_ALSA_PERIODS")) : 2;
    if (periods >= 2 && periods != 3 && val / 3 >= 64 && calledFromLive(__builtin_return_address(0))) {
        for (const SharedPcm &shared : sharedPcms) {
            if (shared.pcm != pcm)
                continue;
            val = val / 3 * periods;
            menufixTrace("alsaaudio: buffer %d frames\n", int(val));
            break;
        }
    }
    return real(pcm, params, val);
}
