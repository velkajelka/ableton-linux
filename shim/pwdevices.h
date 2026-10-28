// PipeWire devices listed in Live's audio device menus (pwdevices.cpp).
#pragma once
#include <string>

constexpr int kFirstVirtualCard = 100;

struct VirtualDevice {
    std::string node; // PipeWire node name; empty: the desktop's default devices
    std::string name;
    bool playback, capture;
    unsigned playbackChannels, captureChannels;
};

bool virtualDevicesEnabled();
bool virtualDevice(int card, VirtualDevice &device);
