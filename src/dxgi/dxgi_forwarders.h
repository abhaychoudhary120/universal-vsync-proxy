#pragma once

// Every standard dxgi.dll export except the three factory creators, which are
// implemented by the proxy. Keep this list in sync with src/dxgi/dxgi.def.
#define VS_DXGI_FORWARD_LIST(X)      \
    X(ApplyCompatResolutionQuirking) \
    X(CompatString)                  \
    X(CompatValue)                   \
    X(DXGID3D10CreateDevice)         \
    X(DXGID3D10CreateLayeredDevice)  \
    X(DXGID3D10GetLayeredDeviceSize) \
    X(DXGID3D10RegisterLayers)       \
    X(DXGIDeclareAdapterRemovalSupport) \
    X(DXGIDumpJournal)               \
    X(DXGIGetDebugInterface1)        \
    X(DXGIReportAdapterConfiguration) \
    X(PIXBeginCapture)               \
    X(PIXEndCapture)                 \
    X(PIXGetCaptureState)            \
    X(SetAppCompatStringPointer)     \
    X(UpdateHMDEmulationStatus)
