// LD_PRELOAD shim for the Push 3 Live desktop build.
// AbletonLive12 creates its menu windows as Qt::ToolTip instead of Qt::Popup
// (no pointer grab, so submenus don't trap clicks). That also loses Qt's
// popup auto-close, so this restores it: a mouse press outside every menu
// window, or the app losing focus, closes all menu windows via close(),
// which goes through Live's own TQtWindow::closeEvent.
#include <QApplication>
#include <QPointer>
#include <QCursor>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QTimer>
#include <QRect>
#include <QWidget>
#include <dlfcn.h>
#include <climits>
#include <cstdio>
#include <cstdlib>

// Keep the preload out of Live's child processes (sh, lspci, helpers).
__attribute__((constructor)) static void dropPreload()
{
    unsetenv("LD_PRELOAD");
}

// MENUFIX_DEBUG=<file> appends a trace there (Live redirects stderr).
void menufixTrace(const char *fmt, int a, int b, int c)
{
    const char *path = getenv("MENUFIX_DEBUG");
    if (!path)
        return;
    if (FILE *f = fopen(path, "a")) {
        fprintf(f, fmt, a, b, c);
        fclose(f);
    }
}
void installVst3RunLoop();
void installPluginCloseHook();
void installPaintHook();
void installFrameBufferHook();
void installNativeMenu();
void installWindowIntegration();
void installFileDialogs();
void installDragCursor();
void installSleepInhibit();
void installPointerWarp();
void installScaleFollow();

namespace {

void trace(const char *fmt, int a = 0, int b = 0, int c = 0)
{
    menufixTrace(fmt, a, b, c);
}


bool isMenuWindow(const QWidget *w)
{
    return w->isVisible() && w->isWindow() && w->windowType() == Qt::ToolTip;
}

void closeMenus()
{
    // Closing one menu can delete its submenus, so guard each pointer.
    QList<QPointer<QWidget>> menus;
    const auto windows = QApplication::topLevelWidgets();
    for (QWidget *w : windows)
        if (isMenuWindow(w))
            menus.append(w);
    for (const auto &w : menus)
        if (w && isMenuWindow(w))
            w->close();
}

class MenuCloser : public QObject {
public:
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        switch (ev->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::NonClientAreaMouseButtonPress: {
            // Qt sends the press to the child view under the cursor (TQtView),
            // then propagates it upwards; handle it once, on the first widget.
            auto *w = qobject_cast<QWidget *>(obj);
            auto *me = static_cast<QMouseEvent *>(ev);
            const QPoint pos = me->globalPos();
            if (!w || (me->timestamp() == lastStamp && pos == lastPos))
                break;
            lastStamp = me->timestamp();
            lastPos = pos;
            const bool inside = isMenuWindow(w->window());
            trace("press %d,%d inside=%d\n", pos.x(), pos.y(), inside);
            if (!inside) {
                // Remember which top-level menu was open: if this press makes
                // Live reopen the same one (a click on its own menu bar title
                // or on the chooser that opened it), close it again so it
                // toggles. Only the very same menu counts: choosers stacked in
                // one column open different menus at the same x.
                toggled = QRect();
                int topY = INT_MAX;
                const auto windows = QApplication::topLevelWidgets();
                for (QWidget *m : windows)
                    if (isMenuWindow(m) && m->y() < topY) {
                        topY = m->y();
                        toggled = m->geometry();
                    }
                toggleClock.start();
                closeMenus();
            }
            break;
        }
        case QEvent::Show: {
            // A menu opened while the cursor is outside every open menu came
            // from the menu bar (hover-switch), not a submenu: close the rest.
            auto *w = qobject_cast<QWidget *>(obj);
            if (!w || !isMenuWindow(w))
                break;
            const QPoint pos = QCursor::pos();
            bool fromMenu = false;
            const auto windows = QApplication::topLevelWidgets();
            for (QWidget *m : windows)
                if (m != w && isMenuWindow(m) && m->frameGeometry().contains(pos))
                    fromMenu = true;
            trace("show %d,%d fromMenu=%d", pos.x(), pos.y(), fromMenu);
            trace(" win=%x geo=%dx%d", int(quintptr(w) & 0xffffff), w->width(), w->height());
            trace("+%d+%d\n", w->x(), w->y());
            if (!fromMenu && w->geometry() == toggled && toggleClock.isValid()
                && toggleClock.elapsed() < 300) {
                trace("toggle-close x=%d\n", w->x());
                toggled = QRect();
                QPointer<QWidget> self(w);
                QTimer::singleShot(0, [self] {
                    if (self && isMenuWindow(self))
                        self->close();
                });
                break;
            }
            if (!fromMenu) {
                // Live can show several windows for one menu in the same pass
                // (the 212x564 Options menu plus a 239x79 one beside it), so
                // collect this batch and close everything else afterwards.
                // Closing synchronously inside show() crashed Qt.
                batch.append(w);
                if (!closeScheduled) {
                    closeScheduled = true;
                    QTimer::singleShot(0, [this] {
                        closeScheduled = false;
                        const auto all = QApplication::topLevelWidgets();
                        QList<QPointer<QWidget>> victims;
                        for (QWidget *m : all)
                            if (isMenuWindow(m) && !batch.contains(m))
                                victims.append(m);
                        batch.clear();
                        for (const auto &m : victims)
                            if (m && isMenuWindow(m))
                                m->close();
                    });
                }
            }
            break;
        }
        case QEvent::ApplicationDeactivate:
            closeMenus();
            break;
        case QEvent::Move: {
            // Moving a window by its title bar goes to the window manager, so
            // Live sees no press; its menus would stay behind on screen.
            auto *w = qobject_cast<QWidget *>(obj);
            if (w && w->isWindow() && w->windowType() == Qt::Window && w->isVisible()) {
                QTimer::singleShot(0, [] {
                    int open = 0;
                    for (QWidget *m : QApplication::topLevelWidgets())
                        open += isMenuWindow(m);
                    if (open)
                        trace("window moved: closing %d menus\n", open);
                    closeMenus();
                    if (QWidget *popup = QApplication::activePopupWidget())
                        popup->close();
                });
            }
            break;
        }
        default:
            break;
        }
        return false;
    }

private:
    QList<QPointer<QWidget>> batch;
    bool closeScheduled = false;
    QRect toggled;
    QElapsedTimer toggleClock;
    ulong lastStamp = 0;
    QPoint lastPos;
};

} // namespace

// Install the filter from Qt's startup hook, which runs inside the
// QCoreApplication constructor. (Interposing QApplication's constructor does
// not work: Live's versioned Qt_5 references keep binding to Qt's own copy.)
extern __attribute__((visibility("hidden"))) bool shimActive; // embedded.cpp

static void installMenuCloser()
{
    if (!shimActive)
        return;
    static MenuCloser closer;
    QCoreApplication::instance()->installEventFilter(&closer);
    trace("filter installed\n");
    installVst3RunLoop();
    installPluginCloseHook();
    installFrameBufferHook();
    installPaintHook();
    installNativeMenu();
    installWindowIntegration();
    installFileDialogs();
    installDragCursor();
    installSleepInhibit();
    installPointerWarp();
    installScaleFollow();
}
Q_COREAPP_STARTUP_FUNCTION(installMenuCloser)
