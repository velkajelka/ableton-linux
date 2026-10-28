// Native menu bar for AbletonLive12.
//
// Windows Live keeps its menus in a platform-neutral tree (LMenu holding
// AEntry items, each with a four-char command id) and mirrors it into a Win32
// HMENU: it builds the tree with CreateMenu/AppendMenuW, refreshes each popup's
// labels, enabled and checked state from the command manager before it opens
// (SetMenuItemInfoW), and runs the chosen command id on WM_COMMAND.
//
// The Push build has the same tree but draws it itself: an AMenuBarView band at
// the top of each main window plus Live-drawn popup windows. This file does
// what the Windows build does, with Qt instead of Win32: every Live main window
// gets a QMenuBar built from the same LMenu tree, refreshed from the same
// command-manager queries, and dispatching through Live's own menu-command
// entry point. The bar covers Live's AMenuBarView band exactly, so Live's layout
// and window geometry stay as Live computes them (Live places its view at the
// window origin itself and would fight a QMainWindow menu bar). The band height
// is the height of Live's AMenuBarView, read from Live's view tree.
// LIVE_NATIVE_MENU=0 turns it off.
//
// Addresses are file-relative virtual addresses in AbletonLive12 (PIE).
#include <QApplication>
#include <QFontMetrics>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPointer>
#include <QResizeEvent>
#include <QTimer>
#include <QVariant>
#include <link.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

void menufixTrace(const char *fmt, int a = 0, int b = 0, int c = 0);

namespace {

// Globals and functions in AbletonLive12.
constexpr uintptr_t kRootMenu = 0x9c0e2d8;         // LMenu *: the main menu tree
constexpr uintptr_t kLMenuVtable = 0x945d1e0;      // LMenu vtable address point
constexpr uintptr_t kCommandManager = 0x20e2d20;   // () -> manager
constexpr uintptr_t kIsEnabled = 0x20e2e40;        // (manager, id) -> bool
constexpr uintptr_t kHasCheckmark = 0x20e5fc0;     // (manager, id) -> bool
constexpr uintptr_t kIsChecked = 0x20e5ff0;        // (manager, id) -> bool
constexpr uintptr_t kFindCommand = 0x20e5d90;      // (manager, id, searchAll) -> command
constexpr uintptr_t kCommandTitle = 0x2071310;     // (TString *out, command): dynamic title ("Nothing to Undo")
constexpr uintptr_t kFreeString = 0x1d8db00;       // frees a TString block whose refcount hit 0
constexpr uintptr_t kRunMenuCommand = 0x160ba80;   // (id, validate): posts an AMenuCmdEvent
constexpr size_t kManagerCurrentId = 0x94;         // int: command id a shared handler is answering for

// LMenu / AEntry layout (Linux build; the Windows layout differs by the MSVC ABI).
constexpr size_t kEntryId = 0x24;        // int, four-char command id; 0 for separators and submenus
constexpr size_t kEntryKey = 0x28;       // int, key code of the shortcut (0: none)
constexpr size_t kEntryModifiers = 0x30; // int, 1 Shift, 2 Alt, 4|8 Ctrl
constexpr size_t kEntryTitle = 0x58;     // TString block *
constexpr size_t kMenuChildren = 0xc0;   // std::vector<AEntry *>: all entries, in order
constexpr size_t kMenuSubmenus = 0xd8;   // std::vector<LMenu *>: the submenus that are shown (not "Hidden")
constexpr size_t kMenuWillOpen = 0xb0;   // void (*)(void *) + argument at 0xb8; fills dynamic menus

uintptr_t base;

template <typename T> T field(uintptr_t obj, size_t offset)
{
    return *reinterpret_cast<T *>(obj + offset);
}

template <typename Fn> Fn live(uintptr_t address)
{
    return reinterpret_cast<Fn>(base + address);
}

// TString block: int refcount, int size in bytes including the terminator, UTF-16 text at +0x10.
QString fromTString(uintptr_t block)
{
    if (!block)
        return {};
    const int units = field<int>(block, 4) / 2 - 1;
    return units > 0 ? QString::fromUtf16(reinterpret_cast<const char16_t *>(block + 0x10), units) : QString();
}

bool isMenu(uintptr_t entry)
{
    return entry && field<uintptr_t>(entry, 0) == base + kLMenuVtable;
}

QVector<uintptr_t> vectorAt(uintptr_t menu, size_t offset)
{
    QVector<uintptr_t> out;
    for (auto p = field<uintptr_t *>(menu, offset); p && p < field<uintptr_t *>(menu, offset + 8); ++p)
        out.append(*p);
    return out;
}

QVector<uintptr_t> children(uintptr_t menu)
{
    return vectorAt(menu, kMenuChildren);
}

// Submenus Live keeps out of its menus (the "Hidden" menu of key-only commands).
bool isHiddenSubmenu(uintptr_t parent, uintptr_t entry)
{
    return isMenu(entry) && !vectorAt(parent, kMenuSubmenus).contains(entry);
}

uintptr_t rootMenu()
{
    return *reinterpret_cast<uintptr_t *>(base + kRootMenu);
}

// Resolves a menu from its index path below the root, so a QMenu never holds
// on to an LMenu that Live may have rebuilt.
uintptr_t resolve(const QVariantList &path)
{
    uintptr_t menu = rootMenu();
    for (const QVariant &i : path) {
        if (!isMenu(menu))
            return 0;
        const QVector<uintptr_t> items = children(menu);
        if (i.toInt() >= items.size())
            return 0;
        menu = items[i.toInt()];
    }
    return isMenu(menu) ? menu : 0;
}

// Key names, as Live formats them (Windows: FUN_142f3f080).
QString keyName(int key)
{
    if (key >= 1 && key <= 10)
        return QString::number(key - 1);
    if (key >= 0x0b && key < 0x0b + 26)
        return QChar('A' + key - 0x0b);
    if (key >= 0x34 && key <= 0x43)
        return QString("F%1").arg(key - 0x33);
    switch (key) {
    case 0x25: return QStringLiteral("←");
    case 0x26: return QStringLiteral("↑");
    case 0x27: return QStringLiteral("→");
    case 0x28: return QStringLiteral("↓");
    case 0x29: return QStringLiteral("Enter");
    case 0x2a: return QStringLiteral("Tab");
    case 0x2b: return QStringLiteral("Space");
    case 0x2c: return QStringLiteral("Escape");
    case 0x2d: return QStringLiteral("BS");
    case 0x2e: return QStringLiteral("Del");
    case 0x44: return QStringLiteral("+");
    case 0x45: return QStringLiteral("±");
    case 0x46: return QStringLiteral("≠");
    case 0x47: return QStringLiteral("-");
    case 0x48: return QStringLiteral("_");
    case 0x49: return QStringLiteral("–");
    case 0x4a: return QStringLiteral("—");
    case 0x4b: return QStringLiteral("=");
    case 0x4d: return QStringLiteral(",");
    case 0x4e: return QStringLiteral("<");
    case 0x4f: return QStringLiteral(">");
    case 0x50: return QStringLiteral("/");
    case 0x51: return QStringLiteral("\\");
    case 0x52: return QStringLiteral("(");
    case 0x53: return QStringLiteral(")");
    case 0x54: return QStringLiteral("?");
    case 0x55: return QStringLiteral("[");
    case 0x56: return QStringLiteral("]");
    }
    return {};
}

QString shortcutText(uintptr_t entry)
{
    const QString key = keyName(field<int>(entry, kEntryKey));
    if (key.isEmpty())
        return {};
    const int mods = field<int>(entry, kEntryModifiers);
    QString text;
    if (mods & 0xc)
        text += "Ctrl+";
    if (mods & 1)
        text += "Shift+";
    if (mods & 2)
        text += "Alt+";
    return text + key;
}

void *commandManager()
{
    return live<void *(*)()>(kCommandManager)();
}

// Titles that depend on state ("Hide Browser", "Go to Arrangement"): the
// menu tree has a placeholder there. Like the manager's own queries, the
// handler is told which id it answers for (Windows: FUN_14312cc80).
QString dynamicTitle(int id)
{
    void *manager = commandManager();
    void *command = live<void *(*)(void *, int, bool)>(kFindCommand)(manager, id, true);
    if (!command)
        return {};
    struct {
        uintptr_t block;
        char rest[16];
    } title{};
    int &currentId = *reinterpret_cast<int *>(static_cast<char *>(manager) + kManagerCurrentId);
    const int savedId = currentId;
    currentId = id;
    live<void *(*)(void *, void *)>(kCommandTitle)(&title, command);
    currentId = savedId;
    const QString text = fromTString(title.block);
    if (title.block && --*reinterpret_cast<int *>(title.block) == 0)
        live<void (*)(uintptr_t)>(kFreeString)(title.block);
    return text;
}

void fillMenu(QMenu *menu);

QMenu *addSubmenu(QWidget *parent, const QString &title, const QVariantList &path)
{
    auto *menu = new QMenu(QString(title).replace('&', "&&"), parent);
    menu->setProperty("livePath", path);
    QObject::connect(menu, &QMenu::aboutToShow, menu, [menu] { fillMenu(menu); });
    return menu;
}

// Rebuilds a popup from its LMenu right before it opens, like the Windows
// build does for each HMENU (FUN_1431af6f0).
void fillMenu(QMenu *menu)
{
    const QVariantList path = menu->property("livePath").toList();
    const uintptr_t lmenu = resolve(path);
    for (QAction *a : menu->actions())
        if (QMenu *sub = a->menu())
            sub->deleteLater();
    menu->clear();
    if (!lmenu)
        return;
    // Let Live fill dynamic menus (Open Recent Set, ...).
    if (auto willOpen = field<void (*)(void *)>(lmenu, kMenuWillOpen))
        willOpen(field<void *>(lmenu, kMenuWillOpen + 8));

    void *manager = commandManager();
    const QVector<uintptr_t> items = children(lmenu);
    for (int i = 0; i < items.size(); ++i) {
        const uintptr_t entry = items[i];
        if (!entry)
            continue;
        if (isHiddenSubmenu(lmenu, entry))
            continue;
        const QString title = fromTString(field<uintptr_t>(entry, kEntryTitle));
        if (isMenu(entry)) {
            menu->addMenu(addSubmenu(menu, title, QVariantList(path) << i));
            continue;
        }
        const int id = field<int>(entry, kEntryId);
        if (id == 0 || title.isEmpty()) {
            menu->addSeparator();
            continue;
        }
        QString label = dynamicTitle(id);
        if (label.isEmpty())
            label = title;
        label.replace('&', "&&");
        // Text after a tab is drawn as the shortcut column but not bound, so
        // the keys keep going to Live's own key handling.
        const QString shortcut = shortcutText(entry);
        QAction *action = menu->addAction(shortcut.isEmpty() ? label : label + '\t' + shortcut);
        action->setEnabled(live<bool (*)(void *, int)>(kIsEnabled)(manager, id));
        if (live<bool (*)(void *, int)>(kHasCheckmark)(manager, id)) {
            action->setCheckable(true);
            action->setChecked(live<bool (*)(void *, int)>(kIsChecked)(manager, id));
        }
        QObject::connect(action, &QAction::triggered, [id] {
            // Run it once the popup has closed and focus is back in Live's window.
            QTimer::singleShot(0, [id] { live<void (*)(int, bool)>(kRunMenuCommand)(id, true); });
        });
    }
    menufixTrace("nativemenu: menu with %d entries, %d px high\n", menu->actions().size(), menu->sizeHint().height());
}

QString signature()
{
    QString sig;
    const uintptr_t root = rootMenu();
    if (!isMenu(root))
        return sig;
    for (uintptr_t entry : children(root))
        sig += fromTString(entry ? field<uintptr_t>(entry, kEntryTitle) : 0) + '\n';
    return sig;
}

void buildBar(QMenuBar *bar)
{
    for (QAction *a : bar->actions())
        if (QMenu *sub = a->menu())
            sub->deleteLater();
    bar->clear();
    const uintptr_t root = rootMenu();
    if (!isMenu(root))
        return;
    const QVector<uintptr_t> items = children(root);
    for (int i = 0; i < items.size(); ++i)
        if (isMenu(items[i]) && !isHiddenSubmenu(root, items[i]))
            bar->addMenu(addSubmenu(bar, fromTString(field<uintptr_t>(items[i], kEntryTitle)), {i}));
    bar->setProperty("liveSignature", signature());
}

// Colours follow Live's theme: the bar takes the colour Live paints its own
// menu bar band with (Windows Live paints its HMENU bar in the theme too).
// Popups use a slightly smaller font and tighter rows than the bar, close to
// Live's own menus, so long ones (View) fit on screen.
// Bar font: 11.5 px at Live's 100 % band of 24 px.
int barFontSize(int bandHeight)
{
    return qRound(bandHeight * 11.5 / 24);
}

// Bar item padding: the items are laid out for Live's 22-unit menu bar
// (as Live's own band, the 2-unit strip under them stays empty): the
// highlight (the clickable area) nearly as tall as the menu bar, with the
// text above centre (top padding one step, 1 px at 100 %, smaller than the
// bottom padding). No
// spacing between items, the room is inside them instead.
int itemRowHeight(int bandHeight)
{
    return qRound(bandHeight * 22.0 / 24);
}

QString itemPadding(int bandHeight)
{
    QFont font = QApplication::font();
    font.setPixelSize(barFontSize(bandHeight));
    const int centred = qMax(0, (itemRowHeight(bandHeight) - QFontMetrics(font).height()) / 2);
    const int step = qMax(1, qRound(bandHeight / 24.0));
    return QStringLiteral("%1px %2px %3px")
        .arg(qMax(0, centred - 2 * step))
        .arg(qRound(bandHeight * 8.0 / 24))
        .arg(qMax(0, centred - step));
}

// Colours come from Live's theme as Live paints with it: the colour table of
// its ATheme, after the theme's brightness, colour intensity and hue settings
// (GetTheme, GetColor). Desktop is the colour of the menu band,
// SurfaceBackground of panels, ControlForeground and TextDisabled of text.
// Ids are the entries of Live's colour name table (0x9c0f220).
constexpr uintptr_t kGetTheme = 0x2260750; // () -> ATheme *, created on first use
constexpr uintptr_t kGetColor = 0x2260840; // (ATheme *, id) -> ARGB
const unsigned char kGetThemePrologue[] = {0x53, 0x48, 0x83, 0xec, 0x10};
const unsigned char kGetColorPrologue[] = {0x48, 0x63, 0xc6, 0x48, 0x8b, 0x8f, 0x78, 0x12, 0x00, 0x00};
constexpr int kDesktop = 3;
constexpr int kSurfaceBackground = 6;
constexpr int kControlForeground = 11;
constexpr int kTextDisabled = 212;

// Change notifications. Whenever ATheme recomputes its colour table (a theme
// switch, every brightness, colour intensity, hue or grid change), it then
// calls its notifier (ATheme + 0x1668) through Live's generic Notify
// (0x14b6620, notifier -> calls every listener), which is how Live's own
// views learn of it. The hook passes every Notify on and only looks at the
// theme's.
constexpr uintptr_t kTheme = 0x9c29828;         // ATheme *, GetTheme's instance
constexpr uintptr_t kThemeOverride = 0x9c29820; // ATheme *, returned instead when set
constexpr size_t kThemeNotifier = 0x1668;
constexpr uintptr_t kNotify = 0x14b6620; // (notifier)
const unsigned char kNotifyPrologue[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x30};

// Live's views (AView): the rectangle {left, top, right, bottom} at +0x30, in
// the window's pixels after zoom (0x20ac7b0 reads it so); the children as a
// std::list of {next, prev, AView *} at +0x48 (AddChild, 0x20a9f50). A
// window's view manager is its root view (AWindow), and holds its zoom state
// at +0xf0, the zoom at state + 0xd4 (GetZoom, 0x20c40d0; SetZoom 0x20c4110
// stores it there and lays the window out again). Coordinates are Live's
// units, before zoom. AMenuBarView (created at 0x21ba040) is identified by
// its vtable.
constexpr size_t kZoomState = 0xf0;
constexpr size_t kZoom = 0xd4;
// The bar also covers the toolbar's top margin under the menu bar (2 units,
// in the band's colour), as Live's band looks: 24 px at 100 %.
constexpr int kBandMargin = 2;
constexpr size_t kViewRect = 0x30;
constexpr size_t kViewChildren = 0x48;
constexpr uintptr_t kMenuBarViewVtable = 0x94c3c00;      // address point
constexpr uintptr_t kMenuBarViewDestructor = 0x21c0f90; // its first entry, checked

struct ThemeColours {
    QColor band, surface, text, disabled;
};

bool liveColours; // GetTheme and GetColor are the expected code
bool themeHooked; // restyle on Live's theme changes

ThemeColours themeColours()
{
    void *theme = live<void *(*)()>(kGetTheme)();
    auto colour = [theme](int id) { return QColor::fromRgb(live<QRgb (*)(void *, int)>(kGetColor)(theme, id)); };
    return {colour(kDesktop), colour(kSurfaceBackground), colour(kControlForeground), colour(kTextDisabled)};
}

// WCAG contrast ratio, 1 (same) to 21 (black on white).
double luminance(const QColor &c)
{
    auto channel = [](int v) {
        const double x = v / 255.0;
        return x <= 0.03928 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(c.red()) + 0.7152 * channel(c.green()) + 0.0722 * channel(c.blue());
}

double contrast(const QColor &a, const QColor &b)
{
    const double la = luminance(a), lb = luminance(b);
    return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
}

QColor mix(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

// Black or white, whichever stands out more on `back`.
QColor extreme(const QColor &back)
{
    return contrast(Qt::black, back) >= contrast(Qt::white, back) ? QColor(Qt::black) : QColor(Qt::white);
}

// `c`, moved towards `towards` just far enough to reach `ratio` against `back`.
QColor withContrast(const QColor &c, const QColor &back, const QColor &towards, double ratio)
{
    for (int i = 0; i <= 20; ++i) {
        const QColor m = mix(c, towards, i / 20.0);
        if (contrast(m, back) >= ratio)
            return m;
    }
    return towards;
}

QString styleFor(int bandHeight)
{
    // Live's theme colours. Live's own disabled text is faint in several themes
    // (TextDisabled on SurfaceBackground is about 2:1 in the Light themes), so
    // text gets at least 7:1 and disabled items 3.5:1 against their
    // background, and stay visibly fainter than enabled ones.
    const ThemeColours theme = themeColours();
    const QColor band = theme.band;
    const QColor popup = theme.surface;
    const QColor text = withContrast(theme.text, popup, extreme(popup), 7.0);
    const QColor barText = withContrast(text, band, extreme(band), 7.0);
    const QColor disabled = withContrast(theme.disabled, popup, text, 3.5);
    const QColor hover = mix(popup, text, 0.16);
    const QColor barHover = mix(band, barText, 0.16);
    const QColor line = mix(popup, text, 0.3);
    return QString(R"(
QMenuBar { background: %1; color: %2; border: none; padding: 0 2px; spacing: 0px; }
QMenuBar::item { background: transparent; padding: %8; }
QMenuBar::item:selected, QMenuBar::item:pressed { background: %3; }
QMenu { background: %4; color: %9; border: 1px solid %6; padding: 2px 0; font-size: %7px; }
QMenu::item { padding: 2px 24px 2px 22px; }
QMenu::item:selected { background: %10; }
QMenu::item:disabled { color: %5; }
QMenu::separator { height: 1px; background: %6; margin: 2px 6px; }
QMenu::indicator { width: 12px; height: 12px; left: 5px; }
)")
        .arg(band.name(), barText.name(), barHover.name(), popup.name(), disabled.name(), line.name())
        .arg(qRound(bandHeight * 12.0 / 24))
        .arg(itemPadding(bandHeight))
        .arg(text.name(), hover.name());
}

// Height of the band in Qt's logical pixels: the window's AMenuBarView's
// height in Live's units (22 at every zoom) plus kBandMargin, times the
// window's zoom; 0 when the window has none (Live's second
// window starts with the toolbar). The menu bar sits a few levels under the
// root view; the tree is walked level by level, to a depth of four.
int menuBarHeight(void *viewManager)
{
    QVector<uintptr_t> level{uintptr_t(viewManager)};
    for (int depth = 0; depth < 10 && !level.isEmpty(); ++depth) {
        QVector<uintptr_t> next;
        for (const uintptr_t view : level) {
            if (!view)
                continue;
            if (field<uintptr_t>(view, 0) == base + kMenuBarViewVtable) {
                const int height = field<int32_t>(view, kViewRect + 12) - field<int32_t>(view, kViewRect + 4);
                const float zoom = field<float>(field<uintptr_t>(uintptr_t(viewManager), kZoomState), kZoom);
                return qRound((height + kBandMargin) * zoom);
            }
            const uintptr_t head = view + kViewChildren;
            for (uintptr_t node = field<uintptr_t>(head, 0); node && node != head; node = field<uintptr_t>(node, 0))
                next.append(field<uintptr_t>(node, 0x10));
        }
        level = next;
    }
    return 0;
}

QMenuBar *barOf(QWidget *view)
{
    return view ? view->findChild<QMenuBar *>("liveNativeMenuBar", Qt::FindDirectChildrenOnly) : nullptr;
}

QVector<QWidget *> viewsWithBar()
{
    QVector<QWidget *> views;
    for (QWidget *w : QApplication::topLevelWidgets())
        if (auto *main = qobject_cast<QMainWindow *>(w))
            if (barOf(main->centralWidget()))
                views.append(main->centralWidget());
    return views;
}

void restyle(QMenuBar *bar)
{
    const int band = bar->property("liveBand").toInt();
    if (band <= 0)
        return;
    const QString style = styleFor(band);
    if (bar->property("liveStyle").toString() != style) {
        bar->setProperty("liveStyle", style);
        bar->setStyleSheet(style);
    }
}

void placeBar(QWidget *view)
{
    QMenuBar *bar = barOf(view);
    if (!bar)
        return;
    const int band = bar->property("liveBand").toInt();
    if (band <= 0) {
        bar->hide(); // Live has not drawn its band yet
        return;
    }
    restyle(bar);
    // Scale the bar's font with Live's zoom (Live's band is 24 px at 100 %).
    QFont font = bar->font();
    font.setPixelSize(barFontSize(band));
    bar->setFont(font);
    bar->setGeometry(0, 0, view->width(), band);
    bar->raise();
    bar->show();
}

// Runs `fn` once from Qt's event loop however often it is asked for before
// then (a brightness drag recomputes the theme many times per second). Safe
// from any thread.
template <typename Fn> void coalesce(std::atomic<bool> &pending, Fn fn)
{
    if (!qApp || pending.exchange(true))
        return;
    QMetaObject::invokeMethod(qApp, [&pending, fn] {
        pending = false;
        fn();
    }, Qt::QueuedConnection);
}

std::atomic<bool> themePending;

using NotifyFn = void (*)(void *);
NotifyFn liveNotify;

void notify(void *notifier)
{
    liveNotify(notifier);
    uintptr_t theme = field<uintptr_t>(base, kThemeOverride);
    if (!theme)
        theme = field<uintptr_t>(base, kTheme);
    if (!theme || reinterpret_cast<uintptr_t>(notifier) != theme + kThemeNotifier)
        return;
    coalesce(themePending, [] {
        for (QWidget *view : viewsWithBar())
            restyle(barOf(view));
    });
}

QPointer<QWidget> paintingView; // the view whose paint event is being delivered

} // namespace

void liveWindowPainted(void *viewManager); // window.cpp

// Called after Live has presented a frame of the view being painted.
void livePainted(void *viewManager, void *)
{
    liveWindowPainted(viewManager);
    QWidget *view = paintingView;
    paintingView = nullptr;
    QMenuBar *bar = barOf(view);
    if (!bar || !viewManager)
        return;
    // Read after every paint: a zoom change or a new window size has been laid
    // out by then.
    const int band = menuBarHeight(viewManager);
    if (band <= 0 || band == bar->property("liveBand").toInt())
        return;
    menufixTrace("nativemenu: band %d px\n", band);
    bar->setProperty("liveBand", band);
    // Not from inside the view's paint event.
    QTimer::singleShot(0, bar, [view] { placeBar(view); });
}

namespace {

// Live's paint handler for a window's view (0x1ff7430, called from the
// view's paintEvent; `self` is Live's window object):
//   if (!self->+0x118) OnPaintEvent(viewManager = self->+0xc0)  // render
//   self->+0xc8->display()->present(self->+0x80)                 // to the widget
// with a Push-only early return between the two that AbletonLive12 has
// patched out. paintHandler does the same and then calls livePainted, by
// when Live has laid the window out for the frame. glsurface.cpp's
// diagnostic handler (LIVE_PAINT_HOOK) calls livePainted too.
constexpr uintptr_t kPaintHandler = 0x1ff7430;
const unsigned char kPaintHandlerPrologue[] = {0x53, 0x48, 0x89, 0xfb, 0x80, 0xbf, 0x18,
                                               0x01, 0x00, 0x00, 0x00, 0x75, 0x0c};
constexpr uintptr_t kOnPaintEvent = 0x20c59f0; // (view manager)
bool paintHooked;

void *callVirtual(void *object, size_t offset)
{
    return (*reinterpret_cast<void *(**)(void *)>(field<uintptr_t>(uintptr_t(object), 0) + offset))(object);
}

void paintHandler(void *self)
{
    const auto window = uintptr_t(self);
    if (!field<bool>(window, 0x118))
        live<void (*)(void *)>(kOnPaintEvent)(field<void *>(window, 0xc0));
    void *display = callVirtual(field<void *>(window, 0xc8), 0x28);
    (*reinterpret_cast<void (**)(void *, uint64_t)>(field<uintptr_t>(uintptr_t(display), 0) + 0x10))(
        display, field<uint64_t>(window, 0x80));
    livePainted(field<void *>(window, 0xc0), display);
}

void attachBar(QMainWindow *window)
{
    QWidget *view = window->centralWidget();
    if (!view || barOf(view))
        return;
    auto *bar = new QMenuBar(view);
    bar->setObjectName("liveNativeMenuBar");
    bar->setNativeMenuBar(false);
    bar->hide();
    buildBar(bar);
    menufixTrace("nativemenu: bar attached, %d menus\n", bar->actions().size());
}

class MenuBarAttacher : public QObject
{
public:
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (e->type() == QEvent::Show && o->isWidgetType()) {
            auto *w = static_cast<QWidget *>(o);
            if (w->isWindow() && w->windowType() == Qt::Window)
                if (auto *main = qobject_cast<QMainWindow *>(w))
                    attachBar(main);
        } else if (e->type() == QEvent::Resize && o->isWidgetType()) {
            auto *view = static_cast<QWidget *>(o);
            if (QMenuBar *bar = barOf(view)) {
                bar->setGeometry(0, 0, view->width(), bar->height());
            }
        } else if (e->type() == QEvent::Paint && o->isWidgetType()) {
            if (barOf(static_cast<QWidget *>(o)))
                paintingView = static_cast<QWidget *>(o);
        }
        return false;
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

void *hookFunction(uintptr_t fn, const unsigned char *prologue, size_t len, void *replacement); // hook.cpp
bool redirectFunction(uintptr_t fn, const unsigned char *prologue, size_t len, void *target);    // hook.cpp
extern bool livePaintHooked; // glsurface.cpp: its diagnostic handler calls livePainted

void installNativeMenu()
{
    if (qgetenv("LIVE_NATIVE_MENU") == "0")
        return;
    base = mainBase();
    liveColours = !memcmp(reinterpret_cast<void *>(base + kGetTheme), kGetThemePrologue, sizeof kGetThemePrologue)
                  && !memcmp(reinterpret_cast<void *>(base + kGetColor), kGetColorPrologue, sizeof kGetColorPrologue);
    if (liveColours)
        liveNotify = reinterpret_cast<NotifyFn>(
            hookFunction(base + kNotify, kNotifyPrologue, sizeof kNotifyPrologue, reinterpret_cast<void *>(&notify)));
    themeHooked = liveNotify;
    paintHooked = livePaintHooked
                  || redirectFunction(base + kPaintHandler, kPaintHandlerPrologue, sizeof kPaintHandlerPrologue,
                                      reinterpret_cast<void *>(&paintHandler));
    menufixTrace("nativemenu: theme colours %d, theme hook %d, paint hook %d\n", liveColours, themeHooked, paintHooked);
    const bool menuBarView = field<uintptr_t>(base, kMenuBarViewVtable) == base + kMenuBarViewDestructor;
    if (!paintHooked || !liveColours || !menuBarView)
        return; // another Live build: Live's own menu band stays
    qApp->installEventFilter(new MenuBarAttacher);
    // Live adds or renames top-level menus at runtime (e.g. after loading a
    // language); check the menu tree regularly (cheap). Without the theme
    // hook, theme changes are found the same way.
    auto *timer = new QTimer(qApp);
    QObject::connect(timer, &QTimer::timeout, [] {
        const QString sig = signature();
        for (QWidget *view : viewsWithBar()) {
            QMenuBar *bar = barOf(view);
            if (bar->property("liveSignature").toString() != sig)
                buildBar(bar);
            if (!themeHooked)
                restyle(bar);
        }
    });
    timer->start(2000);
    menufixTrace("nativemenu: installed\n");
}
