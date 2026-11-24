// Diagnostic logging for the optional debug build. Release builds compile
// every call out because VSYNC_DEBUG_LOG is not defined.

#include "common/debug_log.h"

#include <cstddef>

#if defined(VSYNC_DEBUG_LOG)

namespace {

wchar_t g_log_path[32768] = {};
bool g_log_ready = false;

void WriteRaw(const char* text) noexcept {
    if (!g_log_ready || text == nullptr) {
        return;
    }
    HANDLE file = CreateFileW(g_log_path, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    const int length = lstrlenA(text);
    if (length > 0) {
        DWORD written = 0;
        WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr);
    }
    CloseHandle(file);
}

void LogException(PEXCEPTION_POINTERS pointers) noexcept {
    if (pointers == nullptr || pointers->ExceptionRecord == nullptr ||
        pointers->ContextRecord == nullptr) {
        return;
    }
    const EXCEPTION_RECORD* record = pointers->ExceptionRecord;
    const CONTEXT* context = pointers->ContextRecord;

    char line[1024] = {};
    wsprintfA(line, "!!! EXCEPTION code=0x%08lX address=%p flags=0x%08lX\r\n",
              record->ExceptionCode, record->ExceptionAddress,
              record->ExceptionFlags);
    WriteRaw(line);

    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        record->NumberParameters >= 2) {
        wsprintfA(line, "    access=%Iu target=%p\r\n",
                  record->ExceptionInformation[0],
                  reinterpret_cast<void*>(record->ExceptionInformation[1]));
        WriteRaw(line);
    }

#ifdef _WIN64
    wsprintfA(line, "    RIP=%p RSP=%p RBP=%p\r\n",
              reinterpret_cast<void*>(context->Rip),
              reinterpret_cast<void*>(context->Rsp),
              reinterpret_cast<void*>(context->Rbp));
#else
    wsprintfA(line, "    EIP=%p ESP=%p EBP=%p\r\n",
              reinterpret_cast<void*>(context->Eip),
              reinterpret_cast<void*>(context->Esp),
              reinterpret_cast<void*>(context->Ebp));
#endif
    WriteRaw(line);

    // The first qword at RSP is the return address for a call through a null
    // pointer. Log a small stack window so addr2line can resolve our frames.
#ifdef _WIN64
    const auto* stack = reinterpret_cast<const unsigned long long*>(
        static_cast<unsigned long long>(context->Rsp));
    for (int i = 0; i < 12; ++i) {
        const auto value =
            reinterpret_cast<const void*>(static_cast<size_t>(stack[i]));
        wsprintfA(line, "    stack[%d]=%p\r\n", i, value);
        WriteRaw(line);
    }
#else
    const auto* stack = reinterpret_cast<const unsigned int*>(
        static_cast<unsigned int>(context->Esp));
    for (int i = 0; i < 12; ++i) {
        const auto value =
            reinterpret_cast<const void*>(static_cast<size_t>(stack[i]));
        wsprintfA(line, "    stack[%d]=%p\r\n", i, value);
        WriteRaw(line);
    }
#endif
}

LONG CALLBACK ExceptionHandler(PEXCEPTION_POINTERS pointers) noexcept {
    if (pointers != nullptr && pointers->ExceptionRecord != nullptr &&
        pointers->ExceptionRecord->ExceptionCode ==
            EXCEPTION_ACCESS_VIOLATION) {
        LogException(pointers);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

namespace vsync_proxy {

void DebugInstall(HMODULE self) noexcept {
    // Always log to %TEMP% so a game directory that is read-only or blocked
    // cannot hide an early crash. The PID keeps concurrent runs separate.
    wchar_t temp[32768] = {};
    const DWORD temp_length =
        GetTempPathW(static_cast<DWORD>(sizeof(temp) / sizeof(temp[0])), temp);
    if (temp_length == 0 ||
        temp_length >= (sizeof(temp) / sizeof(temp[0]))) {
        return;
    }

    const DWORD pid = GetCurrentProcessId();
    static constexpr wchar_t kLogName[] = L"vsync_proxy_debug.log";
    int written = wsprintfW(g_log_path, L"%s%s.%lu", temp, kLogName, pid);
    if (written <= 0 ||
        static_cast<std::size_t>(written) >=
            (sizeof(g_log_path) / sizeof(g_log_path[0]))) {
        return;
    }

    g_log_ready = true;
    DeleteFileW(g_log_path);
    WriteRaw("=== vsync proxy debug log ===\r\n");
    AddVectoredExceptionHandler(1, &ExceptionHandler);

    char line[512] = {};
    wsprintfA(line, "module base=%p path=%ls\r\n", self, g_log_path);
    WriteRaw(line);
}

void DebugLog(const char* message) noexcept {
    WriteRaw(message);
    WriteRaw("\r\n");
}

void DebugLogPtr(const char* label, const void* value) noexcept {
    char line[512] = {};
    wsprintfA(line, "%s=%p\r\n", label, value);
    WriteRaw(line);
}

void DebugLogAddress(const char* label, const void* address) noexcept {
    if (address == nullptr) {
        DebugLogPtr(label, address);
        return;
    }
    MEMORY_BASIC_INFORMATION info = {};
    wchar_t module_path[MAX_PATH] = {};
    if (VirtualQuery(address, &info, sizeof(info)) != 0 &&
        info.AllocationBase != nullptr) {
        GetModuleFileNameW(static_cast<HMODULE>(info.AllocationBase),
                           module_path, MAX_PATH);
    }
    char line[768] = {};
    wsprintfA(line, "%s=%p base=%p module=%ls\r\n", label, address,
              info.AllocationBase, module_path);
    WriteRaw(line);
}

}  // namespace vsync_proxy

#endif  // VSYNC_DEBUG_LOG
