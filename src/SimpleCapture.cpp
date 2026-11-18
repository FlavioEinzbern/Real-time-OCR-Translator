#include "SimpleCapture.h"

namespace ChromaTranslate {

    SimpleCapture::SimpleCapture(
        HWND overlayHwnd,
        const TranslatorConfig& config,
        PipelineSyncState& syncState)
        : m_overlayHwnd(overlayHwnd)
        , m_config(config)
        , m_sync(syncState)
    {
        InitializeDirectXResources();
        ResizeSwapChainIfNeeded(m_config.regionWidth, m_config.regionHeight);
    }

    SimpleCapture::~SimpleCapture() {
        Close();
    }

    void SimpleCapture::InitializeDirectXResources() {
        UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };

        winrt::check_hresult(D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            creationFlags,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            m_d3dDevice.put(),
            nullptr,
            m_d3dContext.put()));

        m_winrtDevice = CreateDirect3DDevice(m_d3dDevice.get());

        D2D1_FACTORY_OPTIONS options{};
        winrt::check_hresult(D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1),
            &options,
            m_d2dFactory.put_void()));

        winrt::com_ptr<IDXGIDevice> dxgiDevice = m_d3dDevice.as<IDXGIDevice>();
        winrt::check_hresult(m_d2dFactory->CreateDevice(dxgiDevice.get(), m_d2dDevice.put()));
        winrt::check_hresult(m_d2dDevice->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
            m_d2dContext.put()));

        // Force 1:1 physical pixel coordinate mapping regardless of Windows 125%/150% DPI scaling
        m_d2dContext->SetDpi(96.0f, 96.0f);

        winrt::check_hresult(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.put())));

        winrt::check_hresult(DCompositionCreateDevice(
            dxgiDevice.get(),
            __uuidof(IDCompositionDevice),
            m_dcompDevice.put_void()));

        winrt::check_hresult(m_dcompDevice->CreateTargetForHwnd(m_overlayHwnd, TRUE, m_dcompTarget.put()));
        winrt::check_hresult(m_dcompDevice->CreateVisual(m_dcompVisual.put()));
    }

    void SimpleCapture::RecreateCaptureSession(winrt::Windows::Graphics::Capture::GraphicsCaptureItem const& item) {
        m_frameArrivedRevoker.revoke();
        if (m_framePool) { m_framePool.Close(); m_framePool = nullptr; }
        if (m_session) { m_session.Close(); m_session = nullptr; }

        m_item = item;
        if (!m_item) return;

        auto size = m_item.Size();
        m_framePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            m_winrtDevice,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,
            size);

        m_session = m_framePool.CreateCaptureSession(m_item);
        try { m_session.IsCursorCaptureEnabled(false); } catch (...) {}

        m_frameArrivedRevoker = m_framePool.FrameArrived(
            winrt::auto_revoke,
            { this, &SimpleCapture::OnFrameArrived });

        m_session.StartCapture();
    }

    void SimpleCapture::SwitchToWindow(HWND targetHwnd) {
        if (!IsWindow(targetHwnd)) return;
        m_targetHwnd = targetHwnd;
        m_config.captureMode = CaptureMode::WindowLock;

        RecreateCaptureSession(CreateCaptureItemForWindow(targetHwnd));
        SyncOverlayPosition();
    }

    void SimpleCapture::SwitchToMonitor(HMONITOR targetMonitor) {
        if (!targetMonitor) {
            targetMonitor = MonitorFromWindow(m_overlayHwnd, MONITOR_DEFAULTTONEAREST);
        }
        m_targetMonitor = targetMonitor;
        m_config.captureMode = CaptureMode::MonitorFull;

        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(m_targetMonitor, &mi)) {
            uint32_t w = std::max<LONG>(1, mi.rcMonitor.right - mi.rcMonitor.left);
            uint32_t h = std::max<LONG>(1, mi.rcMonitor.bottom - mi.rcMonitor.top);
            SetWindowPos(m_overlayHwnd, HWND_TOPMOST, mi.rcMonitor.left, mi.rcMonitor.top, w, h, SWP_NOACTIVATE);
            ResizeSwapChainIfNeeded(w, h);
        }

        RecreateCaptureSession(CreateCaptureItemForMonitor(m_targetMonitor));
    }

    void SimpleCapture::SwitchToCustomRegion() {
        m_config.captureMode = CaptureMode::CustomRegion;
        m_targetMonitor = MonitorFromWindow(m_overlayHwnd, MONITOR_DEFAULTTONEAREST);

        SetWindowPos(
            m_overlayHwnd,
            HWND_TOPMOST,
            m_config.regionLeft,
            m_config.regionTop,
            m_config.regionWidth,
            m_config.regionHeight,
            SWP_NOACTIVATE);

        ResizeSwapChainIfNeeded(m_config.regionWidth, m_config.regionHeight);
        RecreateCaptureSession(CreateCaptureItemForMonitor(m_targetMonitor));
    }

    void SimpleCapture::UpdateConfig(const TranslatorConfig& config) {
        m_config = config;
    }

    void SimpleCapture::OnOverlayMovedOrResized() {
        RECT r{};
        if (GetWindowRect(m_overlayHwnd, &r)) {
            uint32_t w = std::max<LONG>(64, r.right - r.left);
            uint32_t h = std::max<LONG>(48, r.bottom - r.top);

            if (m_config.captureMode == CaptureMode::CustomRegion) {
                m_config.regionLeft = r.left;
                m_config.regionTop = r.top;
                m_config.regionWidth = static_cast<int>(w);
                m_config.regionHeight = static_cast<int>(h);

                HMONITOR curMon = MonitorFromWindow(m_overlayHwnd, MONITOR_DEFAULTTONEAREST);
                if (curMon != m_targetMonitor) {
                    m_targetMonitor = curMon;
                    RecreateCaptureSession(CreateCaptureItemForMonitor(m_targetMonitor));
                }
            }

            if (w != m_swapChainWidth || h != m_swapChainHeight) {
                ResizeSwapChainIfNeeded(w, h);
            } else {
                RenderTranslationOverlay();
            }
        }
    }

    void SimpleCapture::ResizeSwapChainIfNeeded(uint32_t width, uint32_t height) {
        width = std::max<uint32_t>(1, width);
        height = std::max<uint32_t>(1, height);

        m_d2dContext->SetTarget(nullptr);

        if (!m_swapChain) {
            winrt::com_ptr<IDXGIDevice> dxgiDevice = m_d3dDevice.as<IDXGIDevice>();
            winrt::com_ptr<IDXGIAdapter> adapter;
            winrt::check_hresult(dxgiDevice->GetAdapter(adapter.put()));
            winrt::com_ptr<IDXGIFactory2> factory;
            winrt::check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));

            DXGI_SWAP_CHAIN_DESC1 scDesc{};
            scDesc.Width = width;
            scDesc.Height = height;
            scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            scDesc.BufferCount = 2;
            scDesc.SampleDesc.Count = 1;
            scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            scDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

            winrt::check_hresult(factory->CreateSwapChainForComposition(
                m_d3dDevice.get(),
                &scDesc,
                nullptr,
                m_swapChain.put()));

            winrt::check_hresult(m_dcompVisual->SetContent(m_swapChain.get()));
            winrt::check_hresult(m_dcompTarget->SetRoot(m_dcompVisual.get()));
            winrt::check_hresult(m_dcompDevice->Commit());
        } else {
            winrt::check_hresult(m_swapChain->ResizeBuffers(
                2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0));
        }

        m_swapChainWidth = width;
        m_swapChainHeight = height;

        winrt::com_ptr<IDXGISurface> surface;
        winrt::check_hresult(m_swapChain->GetBuffer(0, __uuidof(IDXGISurface), surface.put_void()));

        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);

        winrt::com_ptr<ID2D1Bitmap1> bitmap;
        winrt::check_hresult(m_d2dContext->CreateBitmapFromDxgiSurface(surface.get(), &props, bitmap.put()));
        m_d2dContext->SetTarget(bitmap.get());
        m_d2dContext->SetDpi(96.0f, 96.0f);

        if (!m_solidBrush) {
            winrt::check_hresult(m_d2dContext->CreateSolidColorBrush(
                D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f),
                m_solidBrush.put()));
        }

        RenderTranslationOverlay();
    }

    void SimpleCapture::Close() {
        bool expected = false;
        if (m_closed.compare_exchange_strong(expected, true)) {
            m_frameArrivedRevoker.revoke();
            if (m_framePool) m_framePool.Close();
            if (m_session) m_session.Close();
        }
    }

    void SimpleCapture::SyncOverlayPosition() {
        if (m_sync.overlayInteractive.load(std::memory_order_relaxed)) return;
        if (m_config.captureMode != CaptureMode::WindowLock || !IsWindow(m_targetHwnd)) return;

        RECT targetRect{};
        if (FAILED(DwmGetWindowAttribute(m_targetHwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &targetRect, sizeof(targetRect)))) {
            GetWindowRect(m_targetHwnd, &targetRect);
        }

        uint32_t w = std::max<LONG>(1, targetRect.right - targetRect.left);
        uint32_t h = std::max<LONG>(1, targetRect.bottom - targetRect.top);

        RECT curOverlay{};
        GetWindowRect(m_overlayHwnd, &curOverlay);
        if (curOverlay.left != targetRect.left || curOverlay.top != targetRect.top ||
            (curOverlay.right - curOverlay.left) != static_cast<LONG>(w) ||
            (curOverlay.bottom - curOverlay.top) != static_cast<LONG>(h))
        {
            SetWindowPos(
                m_overlayHwnd,
                HWND_TOPMOST,
                targetRect.left,
                targetRect.top,
                w,
                h,
                SWP_NOACTIVATE);
        }

        if (w != m_swapChainWidth || h != m_swapChainHeight) {
            ResizeSwapChainIfNeeded(w, h);
        }
    }

    void SimpleCapture::EnsureStagingTexture(uint32_t width, uint32_t height, DXGI_FORMAT format) {
        if (m_stagingTexture && m_stagingWidth == width && m_stagingHeight == height) return;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        m_stagingTexture = nullptr;
        winrt::check_hresult(m_d3dDevice->CreateTexture2D(&desc, nullptr, m_stagingTexture.put()));
        m_stagingWidth = width;
        m_stagingHeight = height;
    }

    void SimpleCapture::OnFrameArrived(
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
        winrt::Windows::Foundation::IInspectable const&)
    {
        if (m_closed.load(std::memory_order_relaxed)) return;

        auto frame = sender.TryGetNextFrame();
        if (!frame) return;

        uint64_t nowMs = GetTickCount64();
        bool forceSingle = m_sync.triggerSingleCapture.exchange(false, std::memory_order_acq_rel);
        bool isContinuous = m_sync.continuousMode.load(std::memory_order_relaxed);

        if (!forceSingle) {
            if (!isContinuous || (nowMs - m_lastPollTickMs < m_config.continuousPollMs)) {
                return;
            }
        }
        m_lastPollTickMs = nowMs;

        auto gpuTexture = GetDXGIInterfaceFromObject(frame.Surface());
        StagedFrame staged;
        if (TryStageFrameIfChanged(gpuTexture.get(), staged, forceSingle)) {
            {
                std::lock_guard<std::mutex> lock(m_sync.frameMutex);
                m_sync.latestFrame = std::move(staged);
                m_sync.hasNewFrame = true;
            }
            m_sync.frameCv.notify_one();
        }
    }

    bool SimpleCapture::TryStageFrameIfChanged(
        ID3D11Texture2D* gpuTexture,
        StagedFrame& outFrame,
        bool bypassDiffGatekeeper)
    {
        D3D11_TEXTURE2D_DESC desc{};
        gpuTexture->GetDesc(&desc);
        if (desc.Width < 64 || desc.Height < 64) return false;

        EnsureStagingTexture(desc.Width, desc.Height, desc.Format);
        m_d3dContext->CopyResource(m_stagingTexture.get(), gpuTexture);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(m_d3dContext->Map(m_stagingTexture.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            return false;
        }

        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);

        uint32_t cropX = 0, cropY = 0, cropW = desc.Width, cropH = desc.Height;
        int32_t originX = 0, originY = 0;
        float scaleX = 1.0f, scaleY = 1.0f;

        if (m_config.captureMode == CaptureMode::WindowLock && IsWindow(m_targetHwnd)) {
            RECT winRect{};
            if (FAILED(DwmGetWindowAttribute(m_targetHwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &winRect, sizeof(winRect)))) {
                GetWindowRect(m_targetHwnd, &winRect);
            }
            originX = winRect.left;
            originY = winRect.top;
            LONG winW = std::max<LONG>(1, winRect.right - winRect.left);
            LONG winH = std::max<LONG>(1, winRect.bottom - winRect.top);
            scaleX = static_cast<float>(winW) / static_cast<float>(desc.Width);
            scaleY = static_cast<float>(winH) / static_cast<float>(desc.Height);
        } else if (m_targetMonitor) {
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            GetMonitorInfoW(m_targetMonitor, &mi);

            if (m_config.captureMode == CaptureMode::CustomRegion) {
                RECT ovRect{};
                GetWindowRect(m_overlayHwnd, &ovRect);
                int relX = std::max<int>(0, ovRect.left - mi.rcMonitor.left);
                int relY = std::max<int>(0, ovRect.top - mi.rcMonitor.top);
                int relW = std::min<int>(static_cast<int>(desc.Width) - relX, ovRect.right - ovRect.left);
                int relH = std::min<int>(static_cast<int>(desc.Height) - relY, ovRect.bottom - ovRect.top);
                if (relW >= 64 && relH >= 48) {
                    cropX = static_cast<uint32_t>(relX);
                    cropY = static_cast<uint32_t>(relY);
                    cropW = static_cast<uint32_t>(relW);
                    cropH = static_cast<uint32_t>(relH);
                }
            }
            originX = mi.rcMonitor.left + static_cast<int32_t>(cropX);
            originY = mi.rcMonitor.top + static_cast<int32_t>(cropY);
        }

        if (!bypassDiffGatekeeper) {
            uint32_t diffCells = 0;
            uint32_t stepX = std::max<uint32_t>(1, cropW / 64);
            uint32_t stepY = std::max<uint32_t>(1, cropH / 64);
            size_t idx = 0;

            for (uint32_t y = 0; y < 64; ++y) {
                const uint8_t* row = src + ((cropY + y * stepY) * mapped.RowPitch);
                for (uint32_t x = 0; x < 64; ++x, ++idx) {
                    uint32_t px = (cropX + x * stepX) * 4;
                    uint8_t luma = static_cast<uint8_t>((row[px + 2] + (row[px + 1] << 1) + row[px]) >> 2);
                    if (std::abs(static_cast<int>(luma) - static_cast<int>(m_prevLumaGrid[idx])) > 12) {
                        ++diffCells;
                    }
                    m_prevLumaGrid[idx] = luma;
                }
            }

            if (diffCells < m_config.diffThresholdCells) {
                m_d3dContext->Unmap(m_stagingTexture.get(), 0);
                return false;
            }
        }

        outFrame.width = cropW;
        outFrame.height = cropH;
        outFrame.rowPitch = cropW * 4;
        outFrame.timestampMs = GetTickCount64();
        outFrame.screenOriginX = originX;
        outFrame.screenOriginY = originY;
        outFrame.screenScaleX = scaleX;
        outFrame.screenScaleY = scaleY;
        outFrame.bgraPixels.resize(static_cast<size_t>(outFrame.rowPitch) * cropH);

        for (uint32_t y = 0; y < cropH; ++y) {
            const uint8_t* srcRow = src + (static_cast<size_t>(cropY + y) * mapped.RowPitch) + (cropX * 4);
            uint8_t* dstRow = outFrame.bgraPixels.data() + (static_cast<size_t>(y) * outFrame.rowPitch);
            memcpy(dstRow, srcRow, outFrame.rowPitch);
        }

        m_d3dContext->Unmap(m_stagingTexture.get(), 0);
        return true;
    }

    void SimpleCapture::RenderTranslationOverlay() {
        if (!m_d2dContext || !m_swapChain) return;

        std::vector<TextBlock> blocksCopy;
        {
            std::lock_guard<std::mutex> lock(m_sync.overlayMutex);
            blocksCopy = m_sync.activeBlocks;
        }

        RECT overlayScreenRect{};
        GetWindowRect(m_overlayHwnd, &overlayScreenRect);

        bool isInteractive = m_sync.overlayInteractive.load(std::memory_order_relaxed);

        m_d2dContext->BeginDraw();

        if (isInteractive) {
            m_d2dContext->Clear(D2D1::ColorF(0.04f, 0.14f, 0.24f, 0.28f));

            D2D1_RECT_F borderRect = D2D1::RectF(
                2.0f, 2.0f,
                static_cast<float>(m_swapChainWidth) - 2.0f,
                static_cast<float>(m_swapChainHeight) - 2.0f);

            m_solidBrush->SetColor(D2D1::ColorF(0.0f, 0.82f, 1.0f, 0.95f));
            m_d2dContext->DrawRectangle(borderRect, m_solidBrush.get(), 3.0f);

            D2D1_RECT_F bannerRect = D2D1::RectF(
                10.0f, 10.0f,
                std::min(540.0f, static_cast<float>(m_swapChainWidth) - 10.0f), 42.0f);
            m_solidBrush->SetColor(D2D1::ColorF(0.05f, 0.08f, 0.12f, 0.90f));
            m_d2dContext->FillRoundedRectangle(D2D1::RoundedRect(bannerRect, 5.0f, 5.0f), m_solidBrush.get());

            winrt::com_ptr<IDWriteTextFormat> bannerFormat;
            m_dwriteFactory->CreateTextFormat(
                L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.5f, L"en-us", bannerFormat.put());

            const wchar_t* hint = L"UNLOCKED: Drag inside to move, drag edges to resize | Press ALT+L to Lock";
            m_solidBrush->SetColor(D2D1::ColorF(0.0f, 0.90f, 1.0f, 1.0f));
            m_d2dContext->DrawText(
                hint, static_cast<UINT32>(wcslen(hint)), bannerFormat.get(),
                D2D1::RectF(18.0f, 16.0f, bannerRect.right - 6.0f, 38.0f), m_solidBrush.get());
        } else {
            m_d2dContext->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        }

        for (const auto& block : blocksCopy) {
            if (block.translatedText.empty()) continue;

            // Convert global physical desktop coordinates into local overlay window coordinates
            float localLeft   = static_cast<float>(block.bounds.left   - overlayScreenRect.left);
            float localTop    = static_cast<float>(block.bounds.top    - overlayScreenRect.top);
            float localRight  = static_cast<float>(block.bounds.right  - overlayScreenRect.left);
            float localBottom = static_cast<float>(block.bounds.bottom - overlayScreenRect.top);

            D2D1_RECT_F boxRect = D2D1::RectF(
                localLeft - 4.0f,
                localTop - 2.0f,
                localRight + 4.0f,
                localBottom + 2.0f);

            float boxW = std::max(20.0f, boxRect.right - boxRect.left);
            float boxH = std::max(14.0f, boxRect.bottom - boxRect.top);

            m_solidBrush->SetColor(block.bgColor);
            m_d2dContext->FillRoundedRectangle(D2D1::RoundedRect(boxRect, 4.0f, 4.0f), m_solidBrush.get());

            float fontSize = std::clamp(block.estimatedFontSize, 10.0f, 36.0f);
            winrt::com_ptr<IDWriteTextLayout> textLayout;

            while (fontSize >= 9.0f) {
                winrt::com_ptr<IDWriteTextFormat> format;
                m_dwriteFactory->CreateTextFormat(
                    L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize, L"en-us", format.put());

                format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

                textLayout = nullptr;
                m_dwriteFactory->CreateTextLayout(
                    block.translatedText.c_str(),
                    static_cast<UINT32>(block.translatedText.length()),
                    format.get(), boxW - 4.0f, boxH, textLayout.put());

                DWRITE_TEXT_METRICS metrics{};
                if (SUCCEEDED(textLayout->GetMetrics(&metrics)) && metrics.height <= boxH) {
                    break;
                }
                fontSize -= 1.5f;
            }

            if (textLayout) {
                m_solidBrush->SetColor(block.textColor);
                m_d2dContext->DrawTextLayout(
                    D2D1::Point2F(boxRect.left + 2.0f, boxRect.top + 1.0f),
                    textLayout.get(), m_solidBrush.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        }

        m_d2dContext->EndDraw();
        m_swapChain->Present(1, 0);
    }

} // namespace ChromaTranslate