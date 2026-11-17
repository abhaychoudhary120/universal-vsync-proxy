// End-to-end smoke test for build/<arch>/d3d9.dll.
//
// Run this executable from a directory containing the proxy d3d9.dll. It
// checks that the Direct3D9 creation methods, device vtables and swap-chain
// presentation intervals are patched, that forwarding reaches the real system
// DLL, and that a base device created before Direct3DCreate9Ex still leaves the
// Ex paths hooked.

#include <windows.h>
#include <d3d9.h>

#include <cstdio>

namespace {

int g_failures = 0;

void Check(bool condition, const char* description) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) {
        ++g_failures;
    }
}

bool AddressInModule(HMODULE module, const void* address) {
    if (module == nullptr || address == nullptr) {
        return false;
    }
    const auto* base = reinterpret_cast<const unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    const auto* end = base + nt->OptionalHeader.SizeOfImage;
    return address >= base && address < end;
}

void PrintModulePath(HMODULE module, const char* label) {
    wchar_t path[MAX_PATH] = {};
    if (module != nullptr) {
        GetModuleFileNameW(module, path, MAX_PATH);
    }
    std::printf("%s: %ls\n", label, path);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                            LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

HWND CreateProbeWindow(const wchar_t* class_name) {
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = class_name;
    RegisterClassExW(&window_class);
    return CreateWindowExW(0, class_name, class_name, WS_OVERLAPPEDWINDOW, 0, 0,
                           320, 240, nullptr, nullptr, window_class.hInstance,
                           nullptr);
}

D3DPRESENT_PARAMETERS MakePresentParameters(HWND window, UINT back_buffers) {
    D3DPRESENT_PARAMETERS parameters = {};
    parameters.BackBufferWidth = 320;
    parameters.BackBufferHeight = 240;
    parameters.BackBufferFormat = D3DFMT_X8R8G8B8;
    parameters.BackBufferCount = back_buffers;
    parameters.MultiSampleType = D3DMULTISAMPLE_NONE;
    parameters.MultiSampleQuality = 0;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = window;
    parameters.Windowed = TRUE;
    parameters.EnableAutoDepthStencil = FALSE;
    parameters.AutoDepthStencilFormat = D3DFMT_UNKNOWN;
    parameters.Flags = 0;
    parameters.FullScreen_RefreshRateInHz = D3DPRESENT_RATE_DEFAULT;
    parameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    return parameters;
}

bool PresentIntervalIsOne(IDirect3DDevice9* device) {
    IDirect3DSwapChain9* swap_chain = nullptr;
    if (FAILED(device->GetSwapChain(0, &swap_chain)) ||
        swap_chain == nullptr) {
        return false;
    }
    D3DPRESENT_PARAMETERS parameters = {};
    const HRESULT result = swap_chain->GetPresentParameters(&parameters);
    swap_chain->Release();
    return SUCCEEDED(result) &&
           parameters.PresentationInterval == D3DPRESENT_INTERVAL_ONE;
}

}  // namespace

int main() {
    const HMODULE proxy = GetModuleHandleW(L"d3d9.dll");
    PrintModulePath(proxy, "loaded d3d9");
    Check(proxy != nullptr, "proxy d3d9.dll is loaded");

    if (proxy != nullptr) {
        using D3DPERFGetStatusFn = DWORD(WINAPI*)();
        const auto get_status = reinterpret_cast<D3DPERFGetStatusFn>(
            reinterpret_cast<void*>(
                GetProcAddress(proxy, "D3DPERF_GetStatus")));
        Check(get_status != nullptr, "forwarded export is present");
        if (get_status != nullptr) {
            get_status();
            Check(true, "forwarded export reaches the system DLL");
        }
        Check(AddressInModule(
                  proxy,
                  reinterpret_cast<const void*>(
                      GetProcAddress(proxy, "Direct3DCreate9"))),
              "Direct3DCreate9 export is implemented by the proxy");
    }

    HWND window = CreateProbeWindow(L"VSyncProxyD3D9Probe");
    Check(window != nullptr, "create probe window");

    // Base creation path, deliberately done before Direct3DCreate9Ex so the
    // probe also covers an application that touches both entry points.
    IDirect3D9* direct3d = Direct3DCreate9(D3D_SDK_VERSION);
    Check(direct3d != nullptr, "Direct3DCreate9 succeeds through the proxy");

    if (direct3d != nullptr) {
        Check(AddressInModule(proxy, (*(void***)direct3d)[16]),
              "IDirect3D9::CreateDevice vtable slot is hooked");
    }

    if (direct3d != nullptr && window != nullptr) {
        D3DPRESENT_PARAMETERS parameters =
            MakePresentParameters(window, 1);
        IDirect3DDevice9* device = nullptr;
        HRESULT result = direct3d->CreateDevice(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &device);
        Check(SUCCEEDED(result) && device != nullptr,
              "IDirect3D9::CreateDevice succeeds");
        Check(parameters.PresentationInterval == D3DPRESENT_INTERVAL_ONE,
              "CreateDevice rewrites PresentationInterval to ONE");

        if (device != nullptr) {
            Check(AddressInModule(proxy, (*(void***)device)[13]),
                  "CreateAdditionalSwapChain vtable slot is hooked");
            Check(AddressInModule(proxy, (*(void***)device)[16]),
                  "Device Reset vtable slot is hooked");
            Check(AddressInModule(proxy, (*(void***)device)[17]),
                  "Device Present vtable slot is hooked");
            Check(PresentIntervalIsOne(device),
                  "device presents with PresentationInterval ONE");

            result = device->Present(nullptr, nullptr, nullptr, nullptr);
            Check(SUCCEEDED(result), "Present is accepted");

            parameters = MakePresentParameters(window, 1);
            result = device->Reset(&parameters);
            Check(SUCCEEDED(result), "Reset succeeds");
            Check(parameters.PresentationInterval == D3DPRESENT_INTERVAL_ONE,
                  "Reset rewrites PresentationInterval to ONE");
            Check(PresentIntervalIsOne(device),
                  "reset device keeps PresentationInterval ONE");

            HWND second_window =
                CreateProbeWindow(L"VSyncProxyD3D9ProbeSecond");
            if (second_window != nullptr) {
                D3DPRESENT_PARAMETERS chain_parameters =
                    MakePresentParameters(second_window, 1);
                IDirect3DSwapChain9* swap_chain = nullptr;
                result = device->CreateAdditionalSwapChain(
                    &chain_parameters, &swap_chain);
                Check(SUCCEEDED(result) && swap_chain != nullptr,
                      "CreateAdditionalSwapChain succeeds");
                Check(chain_parameters.PresentationInterval ==
                          D3DPRESENT_INTERVAL_ONE,
                      "CreateAdditionalSwapChain rewrites interval to ONE");
                if (swap_chain != nullptr) {
                    D3DPRESENT_PARAMETERS actual = {};
                    result = swap_chain->GetPresentParameters(&actual);
                    Check(SUCCEEDED(result) &&
                              actual.PresentationInterval ==
                                  D3DPRESENT_INTERVAL_ONE,
                          "additional swap chain reports interval ONE");
                    swap_chain->Release();
                }
                DestroyWindow(second_window);
            } else {
                Check(false, "create second probe window");
            }

            device->Release();
        }
    }

    // Ex creation path. Creating this object after the base object must still
    // get the Ex creation methods patched.
    IDirect3D9Ex* direct3d_ex = nullptr;
    HRESULT result = Direct3DCreate9Ex(D3D_SDK_VERSION, &direct3d_ex);
    Check(SUCCEEDED(result) && direct3d_ex != nullptr,
          "Direct3DCreate9Ex succeeds through the proxy");

    if (direct3d_ex != nullptr) {
        Check(AddressInModule(proxy, (*(void***)direct3d_ex)[20]),
              "IDirect3D9Ex::CreateDeviceEx vtable slot is hooked");
    }

    if (direct3d_ex != nullptr && window != nullptr) {
        D3DPRESENT_PARAMETERS parameters =
            MakePresentParameters(window, 1);
        IDirect3DDevice9Ex* device_ex = nullptr;
        result = direct3d_ex->CreateDeviceEx(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, nullptr,
            &device_ex);
        Check(SUCCEEDED(result) && device_ex != nullptr,
              "IDirect3D9Ex::CreateDeviceEx succeeds");
        Check(parameters.PresentationInterval == D3DPRESENT_INTERVAL_ONE,
              "CreateDeviceEx rewrites PresentationInterval to ONE");

        if (device_ex != nullptr) {
            IDirect3DDevice9* device = device_ex;
            Check(AddressInModule(proxy, (*(void***)device)[17]),
                  "Ex device Present vtable slot is hooked");
            Check(AddressInModule(proxy, (*(void***)device)[121]),
                  "Ex device PresentEx vtable slot is hooked");
            Check(AddressInModule(proxy, (*(void***)device)[132]),
                  "Ex device ResetEx vtable slot is hooked");
            Check(PresentIntervalIsOne(device),
                  "Ex device presents with PresentationInterval ONE");

            result = device_ex->PresentEx(nullptr, nullptr, nullptr, nullptr,
                                          0);
            Check(SUCCEEDED(result), "PresentEx is accepted");

            parameters = MakePresentParameters(window, 1);
            result = device_ex->ResetEx(&parameters, nullptr);
            Check(SUCCEEDED(result), "ResetEx succeeds");
            Check(parameters.PresentationInterval == D3DPRESENT_INTERVAL_ONE,
                  "ResetEx rewrites PresentationInterval to ONE");
            Check(PresentIntervalIsOne(device),
                  "reset Ex device keeps PresentationInterval ONE");

            device_ex->Release();
        }
    }

    if (direct3d_ex != nullptr) {
        direct3d_ex->Release();
    }
    if (direct3d != nullptr) {
        direct3d->Release();
    }
    if (window != nullptr) {
        DestroyWindow(window);
    }

    std::printf("RESULT: %s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
