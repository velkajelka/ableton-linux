// ALSA MIDI device-change filter for AbletonLive12.
//
// Live's ALSA backend (reader loop at 0x2db2d60) listens to the sequencer's
// System:Announce port and, on any PORT_START/EXIT/CHANGE from another client,
// closes every MIDI device, re-enumerates and reopens them (logged as
// "MidiDeviceManager: Start CloseAllActivatedDevices"). Reopening deletes and
// recreates Live's own ports. Live ignores announcements about its own client,
// but not about another Live: with two instances running, each one's reopen
// makes the other reopen, about every 2 s, for as long as both run. Every
// reopen drops incoming MIDI and unsubscribes the inputs, so notes to
// instruments and plug-ins cut out and hang.
//
// This drops announcements about ports of other "Ableton Live" clients before
// Live sees them. Real hardware and other applications still trigger rescans.
#include <alsa/asoundlib.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <QDateTime>
#include <QHash>
#include <QString>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);
void *nextSymbol(const char *name, const char *version); // embedded.cpp

namespace {

// Client names, remembered so a CLIENT_EXIT/PORT_EXIT of a client that is
// already gone can still be classified.
QHash<int, bool> otherLive;

bool isOtherLive(snd_seq_t *seq, int client)
{
    if (client == snd_seq_client_id(seq))
        return false;
    snd_seq_client_info_t *info;
    snd_seq_client_info_alloca(&info);
    if (snd_seq_get_any_client_info(seq, client, info) == 0)
        otherLive[client] = QString::fromUtf8(snd_seq_client_info_get_name(info)).startsWith("Ableton Live");
    return otherLive.value(client);
}

bool isOtherLiveAnnouncement(snd_seq_t *seq, const snd_seq_event_t *ev)
{
    if (ev->source.client != SND_SEQ_CLIENT_SYSTEM || ev->source.port != SND_SEQ_PORT_SYSTEM_ANNOUNCE)
        return false;
    switch (ev->type) {
    case SND_SEQ_EVENT_CLIENT_START:
    case SND_SEQ_EVENT_CLIENT_EXIT:
    case SND_SEQ_EVENT_CLIENT_CHANGE:
    case SND_SEQ_EVENT_PORT_START:
    case SND_SEQ_EVENT_PORT_EXIT:
    case SND_SEQ_EVENT_PORT_CHANGE:
        return isOtherLive(seq, ev->data.addr.client);
    case SND_SEQ_EVENT_PORT_SUBSCRIBED:
    case SND_SEQ_EVENT_PORT_UNSUBSCRIBED:
        return isOtherLive(seq, ev->data.connect.sender.client) || isOtherLive(seq, ev->data.connect.dest.client);
    }
    return false;
}

// MENUFIX_DEBUG: name the client behind every port change Live reopens its
// MIDI devices for (each reopen drops MIDI for a moment).
void traceRescanTrigger(snd_seq_t *seq, const snd_seq_event_t *ev)
{
    if (!getenv("MENUFIX_DEBUG") || ev->source.client != SND_SEQ_CLIENT_SYSTEM
        || ev->source.port != SND_SEQ_PORT_SYSTEM_ANNOUNCE || ev->type < SND_SEQ_EVENT_PORT_START
        || ev->type > SND_SEQ_EVENT_PORT_CHANGE || ev->data.addr.client == snd_seq_client_id(seq))
        return;
    snd_seq_client_info_t *info;
    snd_seq_client_info_alloca(&info);
    const QString name = snd_seq_get_any_client_info(seq, ev->data.addr.client, info) == 0
                             ? QString::fromUtf8(snd_seq_client_info_get_name(info))
                             : QStringLiteral("(gone)");
    const QByteArray line = QStringLiteral("alsamidi: %1 port %2 %3:%4 \"%5\" -> Live reopens MIDI devices\n")
                                .arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"))
                                .arg(ev->type == SND_SEQ_EVENT_PORT_START ? "start" : ev->type == SND_SEQ_EVENT_PORT_EXIT ? "exit" : "change")
                                .arg(ev->data.addr.client)
                                .arg(ev->data.addr.port)
                                .arg(name)
                                .toUtf8();
    if (FILE *f = fopen(getenv("MENUFIX_DEBUG"), "a")) {
        fputs(line.constData(), f);
        fclose(f);
    }
}

} // namespace

// Live's reference is versioned ALSA_0.9; shim/qt5.map gives this definition
// that version. Live opens the sequencer non-blocking, so the real call
// returns -EAGAIN once the queue is empty.
extern "C" int snd_seq_event_input(snd_seq_t *seq, snd_seq_event_t **ev)
{
    using InputFn = int (*)(snd_seq_t *, snd_seq_event_t **);
    static auto real = reinterpret_cast<InputFn>(nextSymbol("snd_seq_event_input", "ALSA_0.9"));
    for (;;) {
        const int result = real(seq, ev);
        if (result < 0 || !*ev)
            return result;
        if (!isOtherLiveAnnouncement(seq, *ev)) {
            traceRescanTrigger(seq, *ev);
            return result;
        }
        static int dropped = 0;
        if (++dropped <= 5)
            menufixTrace("alsamidi: ignored announcement %d about client %d\n", (*ev)->type, (*ev)->data.addr.client);
    }
}
