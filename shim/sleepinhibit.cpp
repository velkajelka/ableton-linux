// Keeps the desktop from sleeping while Live runs, as Windows Live does.
//
// Windows Live (TSystemSleep, 0x1431b2940-0x1431b2a21) calls
// SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED |
// ES_DISPLAY_REQUIRED) from startup, and lets the PC sleep only when its
// Windows-only "allow sleep mode" setting is on and nothing holds a request.
// The Push build has no equivalent, and Live's ALSA/PipeWire stream does not
// stop KDE from suspending after the idle timeout (tested: no inhibition is
// listed while such a stream plays).
//
// This asks the session for the same over D-Bus. First through
// xdg-desktop-portal (org.freedesktop.portal.Inhibit: suspend and idle), which
// every current desktop provides (GNOME, KDE, wlroots-based, ...); without a
// portal, through org.freedesktop.ScreenSaver (no dimming, screen off or lock)
// and org.freedesktop.PowerManagement (no suspend), which KDE, Xfce and others
// provide. All of them end when Live's D-Bus connection closes, so when Live
// exits or crashes. The calls are asynchronous, so a slow or missing service
// never delays Live. LIVE_INHIBIT_SLEEP=system keeps only the suspend
// inhibition, LIVE_INHIBIT_SLEEP=0 turns both off.
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QTimer>
#include <QVariantMap>
#include <cstdlib>
#include <cstring>
#include <functional>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);

namespace {

// org.freedesktop.portal.Inhibit flags.
constexpr uint kPortalSuspend = 4;
constexpr uint kPortalIdle = 8;

void callAsync(const QDBusMessage &message, std::function<void(bool)> done)
{
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message),
                                                QCoreApplication::instance());
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, [watcher, done] {
        done(!watcher->isError());
        watcher->deleteLater();
    });
}

QDBusMessage legacyInhibit(const QString &service, const QString &path)
{
    QDBusMessage message = QDBusMessage::createMethodCall(service, path, service, QStringLiteral("Inhibit"));
    message << QStringLiteral("Ableton Live") << QStringLiteral("Ableton Live is running");
    return message;
}

void inhibitLegacy(bool screen)
{
    callAsync(legacyInhibit(QStringLiteral("org.freedesktop.PowerManagement.Inhibit"),
                            QStringLiteral("/org/freedesktop/PowerManagement/Inhibit")),
              [](bool ok) { menufixTrace("sleepinhibit: suspend %d\n", ok); });
    if (screen)
        callAsync(legacyInhibit(QStringLiteral("org.freedesktop.ScreenSaver"), QStringLiteral("/org/freedesktop/ScreenSaver")),
                  [](bool ok) { menufixTrace("sleepinhibit: screen %d\n", ok); });
}

} // namespace

void installSleepInhibit()
{
    const char *mode = getenv("LIVE_INHIBIT_SLEEP");
    if (mode && !strcmp(mode, "0"))
        return;
    const bool screen = !(mode && !strcmp(mode, "system"));
    // After startup, from the event loop: QtDBus is not ready inside the QCoreApplication constructor.
    QTimer::singleShot(0, QCoreApplication::instance(), [screen] {
        QDBusMessage portal = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.portal.Desktop"), QStringLiteral("/org/freedesktop/portal/desktop"),
            QStringLiteral("org.freedesktop.portal.Inhibit"), QStringLiteral("Inhibit"));
        portal << QString() << (screen ? kPortalSuspend | kPortalIdle : kPortalSuspend)
               << QVariantMap{{QStringLiteral("reason"), QStringLiteral("Ableton Live is running")}};
        callAsync(portal, [screen](bool ok) {
            menufixTrace("sleepinhibit: portal %d\n", ok);
            if (!ok)
                inhibitLegacy(screen);
        });
    });
}
