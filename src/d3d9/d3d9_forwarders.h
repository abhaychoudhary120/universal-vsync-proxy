#pragma once

// Every standard d3d9.dll export except the four device creators, which are
// implemented by the proxy. Keep this list in sync with src/d3d9/d3d9.def.
#define VS_D3D9_FORWARD_LIST(X)                            \
    X(D3DPERF_BeginEvent)                                  \
    X(D3DPERF_EndEvent)                                    \
    X(D3DPERF_GetStatus)                                   \
    X(D3DPERF_QueryRepeatFrame)                            \
    X(D3DPERF_SetMarker)                                   \
    X(D3DPERF_SetOptions)                                  \
    X(D3DPERF_SetRegion)                                   \
    X(DebugSetLevel)                                       \
    X(DebugSetMute)                                        \
    X(Direct3D9EnableMaximizedWindowedModeShim)            \
    X(Direct3DShaderValidatorCreate9)                      \
    X(PSGPError)                                           \
    X(PSGPSampleTexture)
