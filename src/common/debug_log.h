#pragma once

#include <windows.h>

#if defined(VSYNC_DEBUG_LOG)

namespace vsync_proxy {

void DebugInstall(HMODULE self) noexcept;
void DebugLog(const char* message) noexcept;
void DebugLogPtr(const char* label, const void* value) noexcept;
void DebugLogAddress(const char* label, const void* address) noexcept;

}  // namespace vsync_proxy

#define VS_LOG(message) vsync_proxy::DebugLog(message)
#define VS_LOG_PTR(label, value) vsync_proxy::DebugLogPtr(label, value)
#define VS_LOG_ADDRESS(label, address) \
    vsync_proxy::DebugLogAddress(label, address)
#define VS_ODS(message) OutputDebugStringA(message)

#else

#define VS_LOG(message) ((void)0)
#define VS_LOG_PTR(label, value) ((void)0)
#define VS_LOG_ADDRESS(label, address) ((void)0)
#define VS_ODS(message) ((void)0)

#endif
