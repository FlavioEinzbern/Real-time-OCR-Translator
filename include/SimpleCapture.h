#pragma once
#include "TranslationTypes.h"
#include "ChromaInterop.h"

namespace ChromaTranslate {

    class SimpleCapture {
    public:
        SimpleCapture(
            HWND overlayHwnd,
            const TranslatorConfig& config,
            PipelineSyncState& syncState);

        ~SimpleCapture();

        // Dynamically switch capture mode & target at runtime
        void SwitchToWindow(HWND targetHwnd);
        void SwitchToMonitor(HMONITOR targetMonitor);
        void SwitchToCustomRegion();

        void UpdateConfig(const TranslatorConfig& config);
        void Close();

        void RenderTranslationOverlay();
        void SyncOverlayPosition();
        void OnOverlayMovedOrResized();

        CaptureMode GetCaptureMode() const { return m_config.captureMode; }
        HWND GetTargetWindow() const { return m_targetHwnd; }
        HMONITOR GetTargetMonitor() const { return m_targetMonitor; }

    private:
        void InitializeDirectXResources();
        void RecreateCaptureSession(winrt::Windows::Graphics::Capture::GraphicsCaptureItem const& item);
        void EnsureStagingTexture(uint32_t width, uint32_t height, DXGI_FORMAT format);
        void ResizeSwapChainIfNeeded(uint32_t width, uint32_t height);

        void OnFrameArrived(
            winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
            winrt::Windows::Foundation::IInspectable const& args);

        bool TryStageFrameIfChanged(
            ID3D11Texture2D* gpuTexture,
            StagedFrame& outFrame,
            bool bypassDiffGatekeeper);

    private:
        HWND m_overlayHwnd = nullptr;
        HWND m_targetHwnd = nullptr;
        HMONITOR m_targetMonitor = nullptr;
        TranslatorConfig m_config;
        PipelineSyncState& m_sync;

        winrt::com_ptr<ID3D11Device> m_d3dDevice;
        winrt::com_ptr<ID3D11DeviceContext> m_d3dContext;
        winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{ nullptr };

        winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{ nullptr };
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{ nullptr };
        winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{ nullptr };
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker m_frameArrivedRevoker;

        winrt::com_ptr<ID3D11Texture2D> m_stagingTexture;
        uint32_t m_stagingWidth = 0;
        uint32_t m_stagingHeight = 0;
        uint8_t m_prevLumaGrid[4096]{};
        uint64_t m_lastPollTickMs = 0;

        winrt::com_ptr<IDXGISwapChain1> m_swapChain;
        winrt::com_ptr<IDCompositionDevice> m_dcompDevice;
        winrt::com_ptr<IDCompositionTarget> m_dcompTarget;
        winrt::com_ptr<IDCompositionVisual> m_dcompVisual;

        winrt::com_ptr<ID2D1Factory1> m_d2dFactory;
        winrt::com_ptr<ID2D1Device> m_d2dDevice;
        winrt::com_ptr<ID2D1DeviceContext> m_d2dContext;
        winrt::com_ptr<ID2D1SolidColorBrush> m_solidBrush;
        winrt::com_ptr<IDWriteFactory> m_dwriteFactory;

        uint32_t m_swapChainWidth = 0;
        uint32_t m_swapChainHeight = 0;
        std::atomic<bool> m_closed{ false };
    };

} // namespace ChromaTranslate