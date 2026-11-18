#pragma once
#include "pch.h"

namespace ChromaTranslate {

    enum class CaptureMode : int {
        WindowLock = 0,
        MonitorFull = 1,
        CustomRegion = 2
    };

    enum class OcrBackend : int {
        WindowsMediaOcr = 0, // Built-in Windows 10/11 WinRT OCR (Default, multi-language line grouping)
        PaddleOnnx = 1       // Local ONNX Runtime PP-OCRv4
    };

    struct StagedFrame {
        std::vector<uint8_t> bgraPixels;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t rowPitch = 0;
        uint64_t timestampMs = 0;

        int32_t screenOriginX = 0;
        int32_t screenOriginY = 0;
        float screenScaleX = 1.0f;
        float screenScaleY = 1.0f;
    };

    struct TextBlock {
        RECT bounds{ 0, 0, 0, 0 };
        std::wstring originalText;
        std::wstring translatedText;
        D2D1_COLOR_F bgColor{ 0.1f, 0.1f, 0.1f, 0.92f };
        D2D1_COLOR_F textColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        float estimatedFontSize = 16.0f;
    };

    struct TranslatorConfig {
        std::wstring sourceLang = L"auto";
        std::wstring targetLang = L"en";
        uint32_t continuousPollMs = 300;
        uint32_t diffThresholdCells = 82;

        CaptureMode captureMode = CaptureMode::WindowLock;
        OcrBackend ocrBackend = OcrBackend::WindowsMediaOcr;

        int regionLeft = 200;
        int regionTop = 200;
        int regionWidth = 800;
        int regionHeight = 450;

        std::wstring detModelPath = L"models/ch_PP-OCRv4_det_infer.onnx";
        std::wstring recModelPath = L"models/japan_PP-OCRv4_rec_infer.onnx";
        std::string dictPath = "models/japan_dict.txt";
        int cpuThreads = 2;
        int furiganaHeightRatioPercent = 45;

        UINT hotkeyModifiers = MOD_ALT;
        UINT keySingleTranslate = 0x54;   // 'T'
        UINT keyToggleContinuous = 0x52;  // 'R'
        UINT keyClearOverlay = 0x43;      // 'C'
        UINT keyToggleLock = 0x4C;        // 'L'

        static std::wstring GetExeDirectory() {
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            std::wstring fullPath(path);
            size_t pos = fullPath.find_last_of(L"\\/");
            return (pos != std::wstring::npos) ? fullPath.substr(0, pos + 1) : L".\\";
        }

        static std::wstring ResolveIniPath(const std::wstring& requestedPath) {
            if (requestedPath.find(L':') != std::wstring::npos) return requestedPath;
            return GetExeDirectory() + L"config.ini";
        }

        static TranslatorConfig LoadFromIni(const std::wstring& rawIniPath) {
            std::wstring iniPath = ResolveIniPath(rawIniPath);
            std::wstring exeDir = GetExeDirectory();
            TranslatorConfig cfg;
            wchar_t buf[512]{};

            GetPrivateProfileStringW(L"General", L"SourceLang", L"auto", buf, 512, iniPath.c_str());
            cfg.sourceLang = buf;
            GetPrivateProfileStringW(L"General", L"TargetLang", L"en", buf, 512, iniPath.c_str());
            cfg.targetLang = buf;

            cfg.continuousPollMs = GetPrivateProfileIntW(L"General", L"ContinuousPollMs", 300, iniPath.c_str());
            cfg.diffThresholdCells = GetPrivateProfileIntW(L"General", L"DiffThresholdCells", 82, iniPath.c_str());
            cfg.captureMode = static_cast<CaptureMode>(GetPrivateProfileIntW(L"General", L"CaptureMode", 0, iniPath.c_str()));
            cfg.ocrBackend = static_cast<OcrBackend>(GetPrivateProfileIntW(L"OCR", L"OcrBackend", 0, iniPath.c_str()));

            cfg.regionLeft = GetPrivateProfileIntW(L"General", L"RegionLeft", 200, iniPath.c_str());
            cfg.regionTop = GetPrivateProfileIntW(L"General", L"RegionTop", 200, iniPath.c_str());
            cfg.regionWidth = std::max<int>(100, GetPrivateProfileIntW(L"General", L"RegionWidth", 800, iniPath.c_str()));
            cfg.regionHeight = std::max<int>(60, GetPrivateProfileIntW(L"General", L"RegionHeight", 450, iniPath.c_str()));

            GetPrivateProfileStringW(L"OCR", L"DetModelPath", L"models/ch_PP-OCRv4_det_infer.onnx", buf, 512, iniPath.c_str());
            cfg.detModelPath = ( std::wstring(buf).find(L':') == std::wstring::npos ) ? (exeDir + buf) : buf;

            GetPrivateProfileStringW(L"OCR", L"RecModelPath", L"models/japan_PP-OCRv4_rec_infer.onnx", buf, 512, iniPath.c_str());
            cfg.recModelPath = ( std::wstring(buf).find(L':') == std::wstring::npos ) ? (exeDir + buf) : buf;

            GetPrivateProfileStringW(L"OCR", L"DictPath", L"models/japan_dict.txt", buf, 512, iniPath.c_str());
            std::wstring fullDictW = ( std::wstring(buf).find(L':') == std::wstring::npos ) ? (exeDir + buf) : buf;
            int len = WideCharToMultiByte(CP_UTF8, 0, fullDictW.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                cfg.dictPath.resize(len - 1);
                WideCharToMultiByte(CP_UTF8, 0, fullDictW.c_str(), -1, cfg.dictPath.data(), len, nullptr, nullptr);
            }

            cfg.cpuThreads = GetPrivateProfileIntW(L"OCR", L"CpuThreads", 2, iniPath.c_str());
            cfg.furiganaHeightRatioPercent = GetPrivateProfileIntW(L"OCR", L"FuriganaHeightRatioPercent", 45, iniPath.c_str());

            cfg.hotkeyModifiers = GetPrivateProfileIntW(L"Hotkeys", L"ModifierMask", MOD_ALT, iniPath.c_str());
            cfg.keySingleTranslate = GetPrivateProfileIntW(L"Hotkeys", L"KeySingleTranslate", 0x54, iniPath.c_str());
            cfg.keyToggleContinuous = GetPrivateProfileIntW(L"Hotkeys", L"KeyToggleContinuous", 0x52, iniPath.c_str());
            cfg.keyClearOverlay = GetPrivateProfileIntW(L"Hotkeys", L"KeyClearOverlay", 0x43, iniPath.c_str());
            cfg.keyToggleLock = GetPrivateProfileIntW(L"Hotkeys", L"KeyToggleLock", 0x4C, iniPath.c_str());

            return cfg;
        }

        void SaveToIni(const std::wstring& rawIniPath) const {
            std::wstring iniPath = ResolveIniPath(rawIniPath);
            WritePrivateProfileStringW(L"General", L"SourceLang", sourceLang.c_str(), iniPath.c_str());
            WritePrivateProfileStringW(L"General", L"TargetLang", targetLang.c_str(), iniPath.c_str());
            WritePrivateProfileStringW(L"General", L"ContinuousPollMs", std::to_wstring(continuousPollMs).c_str(), iniPath.c_str());
            WritePrivateProfileStringW(L"General", L"DiffThresholdCells", std::to_wstring(diffThresholdCells).c_str(), iniPath.c_str());
            WritePrivateProfileStringW(L"General", L"CaptureMode", std::to_wstring(static_cast<int>(captureMode)).c_str(), iniPath.c_str());
            WritePrivateProfileStringW(L"General", L"RegionLeft", std::to_wstring(regionLeft).c_str(), iniPath.c_str());
            WritePrivateProfileStringW(L"General", L"RegionTop", std::to_wstring(regionTop).c_str(), iniPath.c_str());
            WritePrivateProfileStringW(L"General", L"RegionWidth", std::to_wstring(regionWidth).c_str(), iniPath.c_str());
            WritePrivateProfileStringW(L"General", L"RegionHeight", std::to_wstring(regionHeight).c_str(), iniPath.c_str());

            WritePrivateProfileStringW(L"OCR", L"OcrBackend", std::to_wstring(static_cast<int>(ocrBackend)).c_str(), iniPath.c_str());
            WritePrivateProfileStringW(L"OCR", L"CpuThreads", std::to_wstring(cpuThreads).c_str(), iniPath.c_str());
            WritePrivateProfileStringW(L"OCR", L"FuriganaHeightRatioPercent", std::to_wstring(furiganaHeightRatioPercent).c_str(), iniPath.c_str());
        }
    };

    struct PipelineSyncState {
        std::mutex frameMutex;
        std::condition_variable frameCv;
        StagedFrame latestFrame;
        bool hasNewFrame = false;

        std::mutex overlayMutex;
        std::vector<TextBlock> activeBlocks;
        std::atomic<bool> overlayDirty{ false };

        std::mutex configMutex;
        TranslatorConfig sharedConfig;
        std::atomic<bool> configUpdated{ false };

        std::atomic<bool> continuousMode{ false };
        std::atomic<bool> triggerSingleCapture{ false };
        std::atomic<bool> overlayInteractive{ false };
        std::atomic<bool> shutdownRequested{ false };
    };

} // namespace ChromaTranslate