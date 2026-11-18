#pragma once
#include "TranslationTypes.h"
#include <onnxruntime_cxx_api.h>

namespace ChromaTranslate {

    class PaddleOcrEngine {
    public:
        explicit PaddleOcrEngine(const TranslatorConfig& config);
        ~PaddleOcrEngine() = default;

        // Dispatches to Windows.Media.Ocr or PaddleOCR ONNX based on m_config.ocrBackend
        std::vector<TextBlock> ProcessFrame(const StagedFrame& frame);

        void UpdateConfig(const TranslatorConfig& config);

    private:
        // --- Engine A: Native Windows.Media.Ocr (WinRT) ---
        void InitializeWindowsOcr();
        std::vector<TextBlock> ProcessFrameWindowsOcr(const StagedFrame& frame);
        static std::wstring CleanCjkSpaces(const std::wstring& rawText);

        // --- Engine B: Fixed PaddleOCR (ONNX Runtime) ---
        void LoadDictionary(const std::string& dictPath);
        std::vector<TextBlock> ProcessFramePaddleOnnx(const StagedFrame& frame);
        std::vector<RECT> RunDetector(const StagedFrame& frame);
        std::vector<RECT> MergeHorizontalDetections(std::vector<RECT>& rawBoxes) const;
        std::vector<RECT> FilterFuriganaBoxes(const std::vector<RECT>& rawBoxes) const;
        std::wstring RunRecognizer(const StagedFrame& frame, const RECT& box);

        // --- Shared Post-Processing (Bubble Line Merger, Color Sampler, Screen-Space Mapping) ---
        std::vector<TextBlock> FinalizeAndMapBlocks(const StagedFrame& frame, std::vector<TextBlock>& lines) const;
        std::vector<TextBlock> MergeParagraphLines(std::vector<TextBlock>& lines) const;
        void SampleBoxColors(const StagedFrame& frame, TextBlock& block) const;

    private:
        TranslatorConfig m_config;

        // Windows.Media.Ocr state
        winrt::Windows::Media::Ocr::OcrEngine m_winOcrEngine{ nullptr };
        std::wstring m_activeWinOcrLang;

        // PaddleOCR ONNX state
        Ort::Env m_env;
        Ort::SessionOptions m_sessionOptions;
        std::unique_ptr<Ort::Session> m_detSession;
        std::unique_ptr<Ort::Session> m_recSession;
        Ort::MemoryInfo m_memoryInfo;
        std::vector<std::wstring> m_dictionary;
        bool m_modelsLoaded = false;
    };

} // namespace ChromaTranslate