#!/bin/sh
# Builds libmenufix.so against the host Qt 5 headers; Live loads it with its bundled Qt 5.15.2.
cd "$(dirname "$0")"
g++ -O2 -fPIC -shared -std=c++17 $(pkg-config --cflags Qt5Widgets Qt5DBus) \
    menufix.cpp vst3runloop.cpp pluginclose.cpp glsurface.cpp nativemenu.cpp window.cpp alsamidi.cpp alsaaudio.cpp pwdevices.cpp filedialogs.cpp hook.cpp dragcursor.cpp pointerwarp.cpp scalechange.cpp sleepinhibit.cpp standalone.cpp embedded.cpp glibccompat.cpp \
    -o "${1:-libmenufix.so}" -Wl,-soname,libmenufix.so -Wl,-Bsymbolic-functions -Wl,--version-script=qt5.map \
    -Wl,--no-as-needed -lQt5Widgets -lQt5Gui -lQt5DBus -lQt5Core -lasound -lX11 -ldl
