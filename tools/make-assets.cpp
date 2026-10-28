// make-assets: builds the desktop assets the Push 3 Live build lacks, from the
// Windows Suite's files: the custom cursors and the app icon. Same output as
// make_linux_assets.py (wic_unpacked/tools), without Python: it runs on
// Live's own bundled Qt (PNG and the ico plug-in).
//
// Live loads custom cursors from products/live/Live/AppLive/Linux/Cursors/
// <Name>.xcursor (and <Name>@2x.xcursor on HiDPI) with
// XcursorFilenameLoadCursor. Images and hotspots come from the Windows build's
// Resources/Cursors/*.cur (32 px and 64 px); the few cursors only the Mac
// build has come from the image's AppLive/Mac/Cursors/*.png, with hotspots
// derived from the image. The window icon comes from Resources/Icons/
// live_suite.ico, written as PNGs to AppLive/Linux/Icons/ (shim/window.cpp).
//
// usage: LD_LIBRARY_PATH=<push3>/lib QT_PLUGIN_PATH=<push3>/plugins \
//            make-assets <Windows app/Resources> <push3 root>
// build: g++ -O2 -std=c++17 $(pkg-config --cflags Qt5Gui) -o make-assets make-assets.cpp \
//            ../shim/glibccompat.cpp -lQt5Gui -lQt5Core -static-libstdc++ -static-libgcc -s
// (glibccompat.cpp: runs on glibc 2.35, like Live)
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QMap>
#include <climits>
#include <cstdio>
#include <cstdlib>

namespace {

const char *const kNames[] = {
    "WatchCursor", "NoDragCursor", "DragCopyCursor", "DragLeftEdgeCursor", "DragRightEdgeCursor",
    "ZoomCursor", "InMarkerCursor", "OutMarkerCursor", "DrawingPencilCursor", "SpeakerCursor",
    "CurveCursor", "ConvertCursor", "StretchLeftEdgeCursor", "StretchRightEdgeCursor",
    "AddBreakpointCursor", "ResizeLeftWithBarCursor", "ResizeRightWithBarCursor",
    "ResizeUpWithBarCursor", "ResizeDownWithBarCursor", "SliceCursor", "SliceFromEndCursor",
    "ChopCursor"};

struct Cursor {
    QImage image; // Format_RGBA8888
    int xhot = 0, yhot = 0;
};

quint32 le(const QByteArray &d, int at, int bytes)
{
    quint32 v = 0;
    for (int i = bytes - 1; i >= 0; --i)
        v = v << 8 | static_cast<uchar>(d.at(at + i));
    return v;
}

[[noreturn]] void fail(const QString &message)
{
    fprintf(stderr, "make-assets: %s\n", qPrintable(message));
    exit(1);
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        fail(path + ": " + f.errorString());
    return f.readAll();
}

// Every image of a Windows .cur file, by width.
QMap<int, Cursor> readCur(const QString &path)
{
    const QByteArray d = readFile(path);
    QMap<int, Cursor> out;
    for (int i = 0, n = le(d, 4, 2); i < n; ++i) {
        const int entry = 6 + 16 * i;
        const int w = static_cast<uchar>(d.at(entry)) ? static_cast<uchar>(d.at(entry)) : 256;
        const int h = static_cast<uchar>(d.at(entry + 1)) ? static_cast<uchar>(d.at(entry + 1)) : 256;
        const int off = le(d, entry + 12, 4);
        const int px = off + le(d, off, 4);
        const int bpp = le(d, off + 14, 2);
        Cursor c{QImage(w, h, QImage::Format_RGBA8888), static_cast<int>(le(d, entry + 4, 2)),
                 static_cast<int>(le(d, entry + 6, 2))};
        for (int y = 0; y < h; ++y) {
            const int sy = h - 1 - y; // bottom-up rows
            uchar *row = c.image.scanLine(y);
            for (int x = 0; x < w; ++x) {
                uchar r, g, b, a;
                if (bpp == 32) {
                    const char *p = d.constData() + px + sy * w * 4 + 4 * x;
                    b = p[0], g = p[1], r = p[2], a = p[3];
                } else if (bpp == 24) {
                    // 24-bit colour plus the 1-bit AND mask for transparency.
                    const int stride = (w * 3 + 3) & ~3, mstride = (w + 31) / 32 * 4;
                    const char *p = d.constData() + px + sy * stride + 3 * x;
                    b = p[0], g = p[1], r = p[2];
                    const bool transparent = static_cast<uchar>(d.at(px + stride * h + sy * mstride + x / 8)) >> (7 - x % 8) & 1;
                    a = transparent ? 0 : 255;
                } else {
                    fail(QStringLiteral("%1: unsupported %2 bpp").arg(path).arg(bpp));
                }
                row[4 * x] = r, row[4 * x + 1] = g, row[4 * x + 2] = b, row[4 * x + 3] = a;
            }
        }
        out[w] = c;
    }
    return out;
}

int alpha(const QImage &im, int x, int y) { return im.constScanLine(y)[4 * x + 3]; }

void macHotspot(const QString &name, Cursor &c)
{
    const QImage &im = c.image;
    const int w = im.width(), h = im.height();
    if (name.startsWith("Resize")) {
        const bool horizontal = name.contains("Left") || name.contains("Right");
        // The bar is the longest straight run: the column (or row) with most opaque pixels.
        QMap<int, int> counts;
        int lo = INT_MAX, hi = INT_MIN;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (alpha(im, x, y) > 128) {
                    ++counts[horizontal ? x : y];
                    lo = qMin(lo, horizontal ? y : x);
                    hi = qMax(hi, horizontal ? y : x);
                }
        int bar = 0, most = -1;
        for (auto it = counts.cbegin(); it != counts.cend(); ++it)
            if (it.value() > most)
                bar = it.key(), most = it.value();
        const int mid = (lo + hi) / 2;
        c.xhot = horizontal ? bar : mid;
        c.yhot = horizontal ? mid : bar;
        return;
    }
    if (name == "NoDragCursor") {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (alpha(im, x, y) > 128) {
                    c.xhot = x, c.yhot = y; // leftmost pixel of the top row
                    return;
                }
    }
    c.xhot = w / 2, c.yhot = h / 2;
}

void put32(QByteArray &out, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        out.append(static_cast<char>(v >> (8 * i) & 0xff));
}

void writeXcursor(const QString &path, const Cursor &c)
{
    const QImage &im = c.image;
    const quint32 w = im.width(), h = im.height(), nominal = qMax(w, h);
    QByteArray out("Xcur");
    for (quint32 v : {16u, 0x10000u, 1u, 0xFFFD0002u, nominal, 16u + 12u, 36u, 0xFFFD0002u, nominal, 1u, w, h,
                      static_cast<quint32>(c.xhot), static_cast<quint32>(c.yhot), 0u})
        put32(out, v);
    for (quint32 y = 0; y < h; ++y) {
        const uchar *p = im.constScanLine(y);
        for (quint32 x = 0; x < w; ++x, p += 4) {
            const quint32 r = p[0], g = p[1], b = p[2], a = p[3];
            // Xcursor pixels are premultiplied ARGB.
            put32(out, a << 24 | (r * a / 255) << 16 | (g * a / 255) << 8 | (b * a / 255));
        }
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(out) != out.size())
        fail(path + ": " + f.errorString());
}

// One PNG per size of live_suite.ico, the deepest image of each size.
void icons(const QString &win, const QString &root)
{
    const QString dst = root + "/products/live/Live/AppLive/Linux/Icons";
    QDir().mkpath(dst);
    const QString path = win + "/Icons/live_suite.ico";
    const QByteArray d = readFile(path);
    QMap<int, QPair<int, int>> best; // size -> (bpp, index)
    for (int i = 0, n = le(d, 4, 2); i < n; ++i) {
        const int entry = 6 + 16 * i;
        const int size = static_cast<uchar>(d.at(entry)) ? static_cast<uchar>(d.at(entry)) : 256;
        const int bpp = le(d, entry + 6, 2);
        if (!best.contains(size) || bpp > best[size].first)
            best[size] = qMakePair(bpp, i);
    }
    QImageReader reader(path, "ico");
    for (auto it = best.cbegin(); it != best.cend(); ++it) {
        if (!reader.jumpToImage(it.value().second))
            fail(path + ": cannot read image " + QString::number(it.value().second));
        const QImage im = reader.read().convertToFormat(QImage::Format_RGBA8888);
        if (im.isNull() || !im.save(QStringLiteral("%1/live_%2.png").arg(dst).arg(it.key())))
            fail(path + ": " + reader.errorString());
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv); // for the image plug-ins
    if (argc != 3)
        fail("usage: make-assets <Windows app/Resources> <push3 root>");
    const QString res = QString::fromLocal8Bit(argv[1]), root = QString::fromLocal8Bit(argv[2]);
    icons(res, root);
    const QString mac = root + "/products/live/Live/AppLive/Mac/Cursors";
    const QString dst = root + "/products/live/Live/AppLive/Linux/Cursors";
    QDir().mkpath(dst);
    for (const char *n : kNames) {
        const QString name = QString::fromLatin1(n);
        const QString cur = res + "/Cursors/" + name + ".cur";
        if (QFile::exists(cur)) {
            const QMap<int, Cursor> images = readCur(cur);
            if (!images.contains(32) || !images.contains(64))
                fail(cur + ": no 32 and 64 px images");
            writeXcursor(dst + "/" + name + ".xcursor", images[32]);
            writeXcursor(dst + "/" + name + "@2x.xcursor", images[64]);
        } else {
            for (const char *suffix : {"", "@2x"}) {
                const QString png = mac + "/" + name + suffix + ".png";
                Cursor c{QImage(png).convertToFormat(QImage::Format_RGBA8888)};
                if (c.image.isNull())
                    fail(png + ": cannot read");
                macHotspot(name, c);
                writeXcursor(dst + "/" + name + suffix + ".xcursor", c);
            }
        }
    }
    return 0;
}
