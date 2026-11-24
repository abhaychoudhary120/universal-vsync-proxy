// End-to-end smoke test for build/<arch>/dxgi.dll.
//
// Run this executable from a directory containing the proxy dxgi.dll. It
// checks that factory/Present vtable hooks are installed, that the forwarding
// path reaches the real system DLL, and that the D3D11-internal factory path is
// hooked as well.

#include <windows.h>
#include <dxgi1_4.h>
#include <d3d11.h>

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
    const bool inside = address >= base && address < end;
    if (!inside) {
        std::printf("  [debug] address=%p base=%p end=%p\n", address,
                    static_cast<const void*>(base),
                    static_cast<const void*>(end));
    }
    return inside;
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

HWND CreateProbeWindow() {
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"VSyncProxyDxgiProbe";
    RegisterClassExW(&window_class);
    return CreateWindowExW(0, window_class.lpszClassName, L"VSyncProxyDxgiProbe",
                           WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr,
                           nullptr, window_class.hInstance, nullptr);
}

}  // namespace

int main() {
    const HMODULE proxy = GetModuleHandleW(L"dxgi.dll");
    PrintModulePath(proxy, "loaded dxgi");
    Check(proxy != nullptr, "proxy dxgi.dll is loaded");

    if (proxy != nullptr) {
        using DeclareRemovalSupportFn = HRESULT(WINAPI*)();
        const auto declare_removal_support =
            reinterpret_cast<DeclareRemovalSupportFn>(
                GetProcAddress(proxy, "DXGIDeclareAdapterRemovalSupport"));
        Check(declare_removal_support != nullptr,
              "forwarded export is present");
        if (declare_removal_support != nullptr) {
            Check(SUCCEEDED(declare_removal_support()),
                  "forwarded export reaches the system DLL");
        }

        // Regression: AcGenral (Application Compatibility) calls this export
        // while the process is still initializing, before the proxy has
        // resolved the real dxgi. It must be a safe wrapper, never a jump
        // through a null target slot.
        using SetAppCompatStringPointerFn = void(WINAPI*)(ULONG_PTR,
                                                          const char*);
        const auto set_app_compat =
            reinterpret_cast<SetAppCompatStringPointerFn>(
                GetProcAddress(proxy, "SetAppCompatStringPointer"));
        Check(set_app_compat != nullptr,
              "SetAppCompatStringPointer export is present");
        if (set_app_compat != nullptr) {
            set_app_compat(0, "UniversalVSyncProxyProbe");
            Check(true, "SetAppCompatStringPointer is callable");
        }
    }

    HWND window = CreateProbeWindow();
    Check(window != nullptr, "create probe window");

    IDXGIFactory2* factory = nullptr;
    HRESULT result = CreateDXGIFactory2(
        0, __uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
    Check(SUCCEEDED(result) && factory != nullptr,
          "CreateDXGIFactory2 succeeds through the proxy");
    if (factory != nullptr) {
        Check(AddressInModule(proxy, (*(void***)factory)[10]),
              "factory CreateSwapChain vtable slot is hooked");
        Check(AddressInModule(proxy, (*(void***)factory)[21]),
              "factory CreateSwapChainForComposition slot is hooked");

        // Slot 24 is IDXGIFactory4::EnumWarpAdapter, not a swap-chain creator.
        // Calling it proves the proxy did not overwrite it.
        IDXGIFactory4* factory4 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(
                __uuidof(IDXGIFactory4),
                reinterpret_cast<void**>(&factory4))) &&
            factory4 != nullptr) {
            IDXGIAdapter1* warp_adapter = nullptr;
            result = factory4->EnumWarpAdapter(
                __uuidof(IDXGIAdapter1),
                reinterpret_cast<void**>(&warp_adapter));
            Check(SUCCEEDED(result) || result == DXGI_ERROR_NOT_FOUND,
                  "IDXGIFactory4::EnumWarpAdapter is intact");
            if (warp_adapter != nullptr) {
                warp_adapter->Release();
            }
            factory4->Release();
        } else {
            Check(false, "query IDXGIFactory4");
        }
    }

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    if (factory != nullptr) {
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0};
        result = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
            static_cast<UINT>(sizeof(levels) / sizeof(levels[0])),
            D3D11_SDK_VERSION, &device, &feature_level, &context);
    }
    Check(SUCCEEDED(result) && device != nullptr,
          "create D3D11 device");

    if (factory != nullptr && device != nullptr) {
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        desc.Width = 320;
        desc.Height = 240;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        IDXGISwapChain1* swap_chain = nullptr;
        result = factory->CreateSwapChainForHwnd(
            device, window, &desc, nullptr, nullptr, &swap_chain);
        Check(SUCCEEDED(result) && swap_chain != nullptr,
              "create tear-capable flip swap chain");
        if (swap_chain != nullptr) {
            Check(AddressInModule(proxy, (*(void***)swap_chain)[8]),
                  "Present vtable slot is hooked");
            Check(AddressInModule(proxy, (*(void***)swap_chain)[22]),
                  "Present1 vtable slot is hooked");

            // The proxy must rewrite this to SyncInterval=1 and clear the
            // tearing flag. A successful Present proves the call is accepted.
            result = swap_chain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
            Check(SUCCEEDED(result), "Present(0, ALLOW_TEARING) is accepted");
            swap_chain->Release();
        }
    }

    if (factory != nullptr && device != nullptr) {
        IDXGIFactory* factory1 = nullptr;
        result = factory->QueryInterface(
            __uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory1));
        if (SUCCEEDED(result) && factory1 != nullptr) {
            DXGI_SWAP_CHAIN_DESC desc = {};
            desc.BufferDesc.Width = 320;
            desc.BufferDesc.Height = 240;
            desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 1;
            desc.OutputWindow = window;
            desc.Windowed = TRUE;
            desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

            IDXGISwapChain* legacy_swap_chain = nullptr;
            result = factory1->CreateSwapChain(
                device, &desc, &legacy_swap_chain);
            Check(SUCCEEDED(result) && legacy_swap_chain != nullptr,
                  "legacy CreateSwapChain succeeds");
            if (legacy_swap_chain != nullptr) {
                Check(AddressInModule(
                          proxy, (*(void***)legacy_swap_chain)[8]),
                      "legacy swap chain shares the hooked Present vtable");
                legacy_swap_chain->Release();
            }
            factory1->Release();
        } else {
            Check(false, "query IDXGIFactory");
        }
    }

    if (context != nullptr) {
        context->Release();
    }
    if (device != nullptr) {
        device->Release();
    }

    // Validate the D3D11CreateDeviceAndSwapChain path, which uses d3d11.dll's
    // own CreateDXGIFactory2 import rather than an app-created factory.
    if (factory != nullptr) {
        DXGI_SWAP_CHAIN_DESC desc = {};
        desc.BufferDesc.Width = 320;
        desc.BufferDesc.Height = 240;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 1;
        desc.OutputWindow = window;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        ID3D11Device* d3d11_device = nullptr;
        ID3D11DeviceContext* d3d11_context = nullptr;
        IDXGISwapChain* swap_chain = nullptr;
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0};
        result = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
            static_cast<UINT>(sizeof(levels) / sizeof(levels[0])),
            D3D11_SDK_VERSION, &desc, &swap_chain, &d3d11_device, &level,
            &d3d11_context);
        Check(SUCCEEDED(result) && swap_chain != nullptr,
              "D3D11CreateDeviceAndSwapChain succeeds");
        if (swap_chain != nullptr) {
            Check(AddressInModule(proxy, (*(void***)swap_chain)[8]),
                  "D3D11-internal swap chain is hooked");
            swap_chain->Release();
        }
        if (d3d11_context != nullptr) {
            d3d11_context->Release();
        }
        if (d3d11_device != nullptr) {
            d3d11_device->Release();
        }
    }

    if (factory != nullptr) {
        factory->Release();
    }
    if (window != nullptr) {
        DestroyWindow(window);
    }

    std::printf("RESULT: %s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
