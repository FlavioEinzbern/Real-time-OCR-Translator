#pragma once
#include "pch.h"
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <dcomp.h>
#pragma comment(lib, "dcomp.lib")

namespace ChromaTranslate {

    inline auto CreateDirect3DDevice(ID3D11Device* d3dDevice) {
        winrt::com_ptr<IDXGIDevice> dxgiDevice;
        winrt::check_hresult(d3dDevice->QueryInterface(__uuidof(IDXGIDevice), dxgiDevice.put_void()));

        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
        return inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
    }

    inline auto GetDXGIInterfaceFromObject(winrt::Windows::Foundation::IInspectable const& object) {
        auto access = object.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
        return texture;
    }

    inline auto CreateCaptureItemForWindow(HWND hwnd) {
        auto factory = winrt::get_activation_factory<
            winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{ nullptr };
        winrt::check_hresult(factory->CreateForWindow(
            hwnd,
            winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
            winrt::put_abi(item)));
        return item;
    }

    inline auto CreateCaptureItemForMonitor(HMONITOR hmon) {
        auto factory = winrt::get_activation_factory<
            winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{ nullptr };
        winrt::check_hresult(factory->CreateForMonitor(
            hmon,
            winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
            winrt::put_abi(item)));
        return item;
    }

    struct CaptureTargetWindow {
        HWND hwnd = nullptr;
        std::wstring title;
    };

    struct CaptureTargetMonitor {
        HMONITOR hmon = nullptr;
        std::wstring name;
        RECT workArea{};
    };

    inline std::vector<CaptureTargetWindow> EnumerateCapturableWindows(HWND ignoreOverlayHwnd) {
        std::vector<CaptureTargetWindow> windows;
        struct EnumCtx {
            HWND ignore;
            std::vector<CaptureTargetWindow>* list;
        } ctx{ ignoreOverlayHwnd, &windows };

        EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
            auto* c = reinterpret_cast<EnumCtx*>(lParam);
            if (hwnd == c->ignore || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;

            LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
            if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

            DWORD cloaked = 0;
            DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            if (cloaked != 0) return TRUE;

            wchar_t title[256]{};
            if (GetWindowTextW(hwnd, title, 256) > 0) {
                c->list->push_back({ hwnd, title });
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));

        return windows;
    }

    inline std::vector<CaptureTargetMonitor> EnumerateMonitors() {
        std::vector<CaptureTargetMonitor> monitors;
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hmon, HDC, LPRECT, LPARAM lParam) -> BOOL {
            auto* list = reinterpret_cast<std::vector<CaptureTargetMonitor>*>(lParam);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hmon, &mi)) {
                std::wstring label = std::wstring(mi.szDevice) + L" (" +
                    std::to_wstring(mi.rcMonitor.right - mi.rcMonitor.left) + L"x" +
                    std::to_wstring(mi.rcMonitor.bottom - mi.rcMonitor.top) + L")";
                list->push_back({ hmon, label, mi.rcMonitor });
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&monitors));
        return monitors;
    }

} // namespace ChromaTranslate