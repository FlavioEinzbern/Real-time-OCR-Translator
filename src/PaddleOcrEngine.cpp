#include "PaddleOcrEngine.h"
#include <queue>

// Standard Windows COM interface for direct zero-overhead byte access to SoftwareBitmap buffers
struct __declspec(uuid("5b0d3235-4dba-4d44-865e-8f1d0e4fd04d")) __declspec(novtable)
IMemoryBufferByteAccess : ::IUnknown {
    virtual HRESULT __stdcall GetBuffer(uint8_t** value, uint32_t* capacity) = 0;
};

namespace ChromaTranslate {

    namespace {

        bool ContainsCjkCharacters(const std::wstring& text) {
            for (wchar_t c : text) {
                if ((c >= 0x3000 && c <= 0x9FFF) ||
                    (c >= 0xAC00 && c <= 0xD7AF) ||
                    (c >= 0xFF00 && c <= 0xFFEF)) {
                    return true;
                }
            }
            return false;
        }

        // Helper structure for decoding CTC output with confidence scoring
        struct RecCandidate {
            std::wstring text;
            float avgConfidence = 0.0f;
        };

        RecCandidate DecodeCtcTensor(
            Ort::Session* recSession,
            const Ort::MemoryInfo& memInfo,
            std::vector<float>& inputValues,
            uint32_t recH,
            uint32_t recW,
            const std::vector<std::wstring>& dictionary)
        {
            RecCandidate out;
            if (!recSession || recW < 8 || dictionary.size() <= 1) return out;

            try {
                int64_t inputShape[4] = { 1, 3, static_cast<int64_t>(recH), static_cast<int64_t>(recW) };
                Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
                    memInfo, inputValues.data(), inputValues.size(), inputShape, 4);

                Ort::AllocatorWithDefaultOptions allocator;
                auto inputName = recSession->GetInputNameAllocated(0, allocator);
                auto outputName = recSession->GetOutputNameAllocated(0, allocator);
                const char* inputNames[] = { inputName.get() };
                const char* outputNames[] = { outputName.get() };

                auto outputTensors = recSession->Run(
                    Ort::RunOptions{ nullptr }, inputNames, &inputTensor, 1, outputNames, 1);

                auto shape = outputTensors.front().GetTensorTypeAndShapeInfo().GetShape();
                if (shape.size() < 3) return out;

                int64_t timeSteps = shape[1];
                int64_t numClasses = shape[2];
                const float* logits = outputTensors.front().GetTensorData<float>();

                int64_t prevClass = 0;
                float confidenceSum = 0.0f;
                int validTokens = 0;

                for (int64_t t = 0; t < timeSteps; ++t) {
                    const float* stepProbs = logits + (t * numClasses);
                    int64_t bestClass = 0;
                    float maxProb = stepProbs[0];

                    for (int64_t c = 1; c < numClasses; ++c) {
                        if (stepProbs[c] > maxProb) {
                            maxProb = stepProbs[c];
                            bestClass = c;
                        }
                    }

                    if (bestClass > 0 && bestClass != prevClass &&
                        static_cast<size_t>(bestClass) < dictionary.size())
                    {
                        out.text += dictionary[bestClass];
                        confidenceSum += maxProb;
                        ++validTokens;
                    }
                    prevClass = bestClass;
                }

                if (validTokens > 0) {
                    out.avgConfidence = confidenceSum / static_cast<float>(validTokens);
                }
            } catch (...) {
                return {};
            }
            return out;
        }

        // Bilinear pixel sampler from StagedFrame
        inline void SampleBilinearBgr(
            const StagedFrame& frame,
            float srcX,
            float srcY,
            float& outB,
            float& outG,
            float& outR)
        {
            int x0 = std::clamp<int>(static_cast<int>(std::floor(srcX)), 0, static_cast<int>(frame.width) - 1);
            int x1 = std::clamp<int>(x0 + 1, 0, static_cast<int>(frame.width) - 1);
            int y0 = std::clamp<int>(static_cast<int>(std::floor(srcY)), 0, static_cast<int>(frame.height) - 1);
            int y1 = std::clamp<int>(y0 + 1, 0, static_cast<int>(frame.height) - 1);

            float wx = srcX - std::floor(srcX);
            float wy = srcY - std::floor(srcY);

            const uint8_t* row0 = frame.bgraPixels.data() + (static_cast<size_t>(y0) * frame.rowPitch);
            const uint8_t* row1 = frame.bgraPixels.data() + (static_cast<size_t>(y1) * frame.rowPitch);

            const uint8_t* p00 = row0 + (x0 * 4);
            const uint8_t* p01 = row0 + (x1 * 4);
            const uint8_t* p10 = row1 + (x0 * 4);
            const uint8_t* p11 = row1 + (x1 * 4);

            auto interp = [&](int ch) -> float {
                float top = p00[ch] * (1.0f - wx) + p01[ch] * wx;
                float bot = p10[ch] * (1.0f - wx) + p11[ch] * wx;
                float val = (top * (1.0f - wy) + bot * wy) / 255.0f;
                return (val - 0.5f) / 0.5f;
            };

            outB = interp(0);
            outG = interp(1);
            outR = interp(2);
        }

    } // anonymous namespace

    PaddleOcrEngine::PaddleOcrEngine(const TranslatorConfig& config)
        : m_config(config)
        , m_env(ORT_LOGGING_LEVEL_WARNING, "ChromaTranslateOCR")
        , m_memoryInfo(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault))
    {
        InitializeWindowsOcr();

        m_sessionOptions.SetIntraOpNumThreads(std::max(1, m_config.cpuThreads));
        m_sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        try {
            m_detSession = std::make_unique<Ort::Session>(m_env, m_config.detModelPath.c_str(), m_sessionOptions);
            m_recSession = std::make_unique<Ort::Session>(m_env, m_config.recModelPath.c_str(), m_sessionOptions);
            LoadDictionary(m_config.dictPath);
            // Strictly require valid sessions and a populated vocabulary (>100 tokens)
            m_modelsLoaded = (m_detSession != nullptr && m_recSession != nullptr && m_dictionary.size() > 100);
        } catch (...) {
            m_modelsLoaded = false;
        }
    }

    void PaddleOcrEngine::UpdateConfig(const TranslatorConfig& config) {
        bool langChanged = (m_config.sourceLang != config.sourceLang);
        m_config = config;
        if (langChanged || !m_winOcrEngine) {
            InitializeWindowsOcr();
        }
    }

    void PaddleOcrEngine::InitializeWindowsOcr() {
        using namespace winrt::Windows::Globalization;
        using namespace winrt::Windows::Media::Ocr;

        m_winOcrEngine = nullptr;

        try {
            if (m_config.sourceLang != L"auto" && !m_config.sourceLang.empty()) {
                Language requestedLang(m_config.sourceLang);
                if (OcrEngine::IsLanguageSupported(requestedLang)) {
                    m_winOcrEngine = OcrEngine::TryCreateFromLanguage(requestedLang);
                }
            }

            if (!m_winOcrEngine) {
                m_winOcrEngine = OcrEngine::TryCreateFromUserProfileLanguages();
            }

            if (!m_winOcrEngine) {
                auto available = OcrEngine::AvailableRecognizerLanguages();
                if (available.Size() > 0) {
                    m_winOcrEngine = OcrEngine::TryCreateFromLanguage(available.GetAt(0));
                }
            }
        } catch (...) {
            m_winOcrEngine = nullptr;
        }
    }

    std::wstring PaddleOcrEngine::CleanCjkSpaces(const std::wstring& rawText) {
        std::wstring out;
        out.reserve(rawText.size());

        auto isCjk = [](wchar_t c) -> bool {
            return (c >= 0x3000 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xFF00 && c <= 0xFFEF);
        };

        for (size_t i = 0; i < rawText.size(); ++i) {
            if (rawText[i] == L' ' && i > 0 && i + 1 < rawText.size()) {
                if (isCjk(rawText[i - 1]) && isCjk(rawText[i + 1])) {
                    continue;
                }
            }
            out.push_back(rawText[i]);
        }
        return out;
    }

    std::vector<TextBlock> PaddleOcrEngine::ProcessFrame(const StagedFrame& frame) {
        if (frame.bgraPixels.empty() || frame.width < 16 || frame.height < 16) return {};

        if (m_config.ocrBackend == OcrBackend::WindowsMediaOcr) {
            auto blocks = ProcessFrameWindowsOcr(frame);
            if (!blocks.empty() || !m_modelsLoaded) {
                return blocks;
            }
        }

        return ProcessFramePaddleOnnx(frame);
    }

    std::vector<TextBlock> PaddleOcrEngine::ProcessFrameWindowsOcr(const StagedFrame& frame) {
        if (!m_winOcrEngine) {
            InitializeWindowsOcr();
            if (!m_winOcrEngine) return {};
        }

        try {
            using namespace winrt::Windows::Graphics::Imaging;

            SoftwareBitmap bitmap(
                BitmapPixelFormat::Bgra8,
                static_cast<int32_t>(frame.width),
                static_cast<int32_t>(frame.height),
                BitmapAlphaMode::Ignore);

            {
                BitmapBuffer bmpBuffer = bitmap.LockBuffer(BitmapBufferAccessMode::Write);
                auto reference = bmpBuffer.CreateReference();
                auto byteAccess = reference.as<::IMemoryBufferByteAccess>();

                uint8_t* dstPixels = nullptr;
                uint32_t capacity = 0;
                winrt::check_hresult(byteAccess->GetBuffer(&dstPixels, &capacity));

                uint32_t dstPitch = frame.width * 4;
                if (dstPixels && capacity >= dstPitch * frame.height) {
                    for (uint32_t y = 0; y < frame.height; ++y) {
                        memcpy(
                            dstPixels + static_cast<size_t>(y) * dstPitch,
                            frame.bgraPixels.data() + static_cast<size_t>(y) * frame.rowPitch,
                            dstPitch);
                    }
                }
                reference.Close();
                bmpBuffer.Close();
            }

            auto ocrResult = m_winOcrEngine.RecognizeAsync(bitmap).get();
            auto lines = ocrResult.Lines();
            if (lines.Size() == 0) return {};

            std::vector<TextBlock> recognizedLines;
            recognizedLines.reserve(lines.Size());

            for (auto const& line : lines) {
                std::wstring text = CleanCjkSpaces(std::wstring(line.Text()));
                if (text.empty()) continue;

                RECT rect{ LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN };
                for (auto const& word : line.Words()) {
                    auto r = word.BoundingRect();
                    rect.left   = std::min<LONG>(rect.left,   static_cast<LONG>(std::floor(r.X)));
                    rect.top    = std::min<LONG>(rect.top,    static_cast<LONG>(std::floor(r.Y)));
                    rect.right  = std::max<LONG>(rect.right,  static_cast<LONG>(std::ceil(r.X + r.Width)));
                    rect.bottom = std::max<LONG>(rect.bottom, static_cast<LONG>(std::ceil(r.Y + r.Height)));
                }

                if (rect.right <= rect.left || rect.bottom <= rect.top) continue;

                LONG w = rect.right - rect.left;
                LONG h = rect.bottom - rect.top;
                TextBlock block;
                block.bounds = rect;
                block.originalText = std::move(text);
                block.estimatedFontSize = static_cast<float>(std::min(w, h)) * 0.80f;
                recognizedLines.push_back(std::move(block));
            }

            return FinalizeAndMapBlocks(frame, recognizedLines);
        } catch (...) {
            return {};
        }
    }

    void PaddleOcrEngine::LoadDictionary(const std::string& dictPath) {
        m_dictionary.clear();

        std::ifstream file(dictPath);
        if (!file.is_open()) {
            file.open("models/japan_dict.txt");
        }
        if (!file.is_open()) {
            file.open("../models/japan_dict.txt");
        }
        if (!file.is_open()) return;

        m_dictionary.push_back(L""); // Index 0: CTC blank token

        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            int wlen = MultiByteToWideChar(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0);
            if (wlen > 0) {
                std::wstring wline(wlen, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), wline.data(), wlen);
                m_dictionary.push_back(std::move(wline));
            }
        }
        m_dictionary.push_back(L" "); // Trailing space token
    }

    std::vector<TextBlock> PaddleOcrEngine::ProcessFramePaddleOnnx(const StagedFrame& frame) {
        if (!m_modelsLoaded) {
            // Surface a visible diagnostic box if the user selected PaddleOCR but models failed to load
            TextBlock errBlock;
            errBlock.bounds = RECT{
                frame.screenOriginX + 30,
                frame.screenOriginY + 30,
                frame.screenOriginX + 520,
                frame.screenOriginY + 85
            };
            errBlock.originalText = L"[ONNX-ERR] Missing models/japan_PP-OCRv4_rec_infer.onnx or japan_dict.txt";
            errBlock.translatedText = errBlock.originalText;
            errBlock.bgColor = D2D1::ColorF(0.65f, 0.08f, 0.08f, 0.95f);
            errBlock.textColor = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
            errBlock.estimatedFontSize = 15.0f;
            return { errBlock };
        }

        std::vector<RECT> rawBoxes = RunDetector(frame);
        if (rawBoxes.empty()) return {};

        // 1. Only merge horizontal word islands; keep vertical columns intact
        std::vector<RECT> lineBoxes = MergeHorizontalDetections(rawBoxes);

        // 2. Filter furigana using minor-axis stroke thickness (std::min(width, height))
        std::vector<RECT> cleanBoxes = FilterFuriganaBoxes(lineBoxes);
        if (cleanBoxes.empty()) return {};

        // 3. Recognize text (automatically handles both horizontal lines and vertical manga columns)
        std::vector<TextBlock> recognizedLines;
        recognizedLines.reserve(cleanBoxes.size());

        for (const auto& rect : cleanBoxes) {
            std::wstring text = RunRecognizer(frame, rect);
            if (text.empty()) continue;

            LONG w = rect.right - rect.left;
            LONG h = rect.bottom - rect.top;
            TextBlock block;
            block.bounds = rect;
            block.originalText = std::move(text);
            block.estimatedFontSize = static_cast<float>(std::min(w, h)) * 0.78f;
            recognizedLines.push_back(std::move(block));
        }

        return FinalizeAndMapBlocks(frame, recognizedLines);
    }

    std::vector<TextBlock> PaddleOcrEngine::FinalizeAndMapBlocks(
        const StagedFrame& frame,
        std::vector<TextBlock>& recognizedLines) const
    {
        if (recognizedLines.empty()) return {};

        std::vector<TextBlock> mergedBlocks = MergeParagraphLines(recognizedLines);

        for (auto& block : mergedBlocks) {
            SampleBoxColors(frame, block);

            block.bounds.left   = frame.screenOriginX + static_cast<LONG>(std::round(block.bounds.left   * frame.screenScaleX));
            block.bounds.top    = frame.screenOriginY + static_cast<LONG>(std::round(block.bounds.top    * frame.screenScaleY));
            block.bounds.right  = frame.screenOriginX + static_cast<LONG>(std::round(block.bounds.right  * frame.screenScaleX));
            block.bounds.bottom = frame.screenOriginY + static_cast<LONG>(std::round(block.bounds.bottom * frame.screenScaleY));
        }

        return mergedBlocks;
    }

    std::vector<RECT> PaddleOcrEngine::RunDetector(const StagedFrame& frame) {
        const uint32_t maxSide = 960;
        float scale = 1.0f;
        uint32_t longest = std::max(frame.width, frame.height);
        if (longest > maxSide) {
            scale = static_cast<float>(maxSide) / static_cast<float>(longest);
        }

        uint32_t detW = std::max<uint32_t>(32, static_cast<uint32_t>(std::round(frame.width * scale / 32.0f)) * 32);
        uint32_t detH = std::max<uint32_t>(32, static_cast<uint32_t>(std::round(frame.height * scale / 32.0f)) * 32);

        float scaleX = static_cast<float>(frame.width) / static_cast<float>(detW);
        float scaleY = static_cast<float>(frame.height) / static_cast<float>(detH);

        size_t planeSize = static_cast<size_t>(detW) * detH;
        std::vector<float> inputTensorValues(planeSize * 3);
        float* cB = inputTensorValues.data();
        float* cG = cB + planeSize;
        float* cR = cG + planeSize;

        for (uint32_t y = 0; y < detH; ++y) {
            uint32_t srcY = std::min(frame.height - 1, static_cast<uint32_t>(y * scaleY));
            const uint8_t* row = frame.bgraPixels.data() + (static_cast<size_t>(srcY) * frame.rowPitch);
            for (uint32_t x = 0; x < detW; ++x) {
                uint32_t srcX = std::min(frame.width - 1, static_cast<uint32_t>(x * scaleX));
                const uint8_t* px = row + (srcX * 4);
                size_t idx = static_cast<size_t>(y) * detW + x;

                cB[idx] = ((px[0] / 255.0f) - 0.485f) / 0.229f;
                cG[idx] = ((px[1] / 255.0f) - 0.456f) / 0.224f;
                cR[idx] = ((px[2] / 255.0f) - 0.406f) / 0.225f;
            }
        }

        int64_t inputShape[4] = { 1, 3, static_cast<int64_t>(detH), static_cast<int64_t>(detW) };
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            m_memoryInfo, inputTensorValues.data(), inputTensorValues.size(), inputShape, 4);

        Ort::AllocatorWithDefaultOptions allocator;
        auto inputName = m_detSession->GetInputNameAllocated(0, allocator);
        auto outputName = m_detSession->GetOutputNameAllocated(0, allocator);
        const char* inputNames[] = { inputName.get() };
        const char* outputNames[] = { outputName.get() };

        auto outputTensors = m_detSession->Run(
            Ort::RunOptions{ nullptr }, inputNames, &inputTensor, 1, outputNames, 1);

        const float* probMap = outputTensors.front().GetTensorData<float>();

        std::vector<uint8_t> visited(planeSize, 0);
        std::vector<RECT> boxes;
        std::queue<std::pair<uint32_t, uint32_t>> q;

        for (uint32_t y = 0; y < detH; ++y) {
            for (uint32_t x = 0; x < detW; ++x) {
                size_t idx = static_cast<size_t>(y) * detW + x;
                if (visited[idx] || probMap[idx] <= 0.28f) continue;

                visited[idx] = 1;
                q.push({ x, y });

                uint32_t minX = x, maxX = x, minY = y, maxY = y;
                uint32_t pixelCount = 0;

                while (!q.empty()) {
                    auto [cx, cy] = q.front();
                    q.pop();
                    ++pixelCount;

                    minX = std::min(minX, cx);
                    maxX = std::max(maxX, cx);
                    minY = std::min(minY, cy);
                    maxY = std::max(maxY, cy);

                    const int dx[4] = { -1, 1, 0, 0 };
                    const int dy[4] = { 0, 0, -1, 1 };
                    for (int dir = 0; dir < 4; ++dir) {
                        int nx = static_cast<int>(cx) + dx[dir];
                        int ny = static_cast<int>(cy) + dy[dir];
                        if (nx >= 0 && nx < static_cast<int>(detW) && ny >= 0 && ny < static_cast<int>(detH)) {
                            size_t nIdx = static_cast<size_t>(ny) * detW + nx;
                            if (!visited[nIdx] && probMap[nIdx] > 0.28f) {
                                visited[nIdx] = 1;
                                q.push({ static_cast<uint32_t>(nx), static_cast<uint32_t>(ny) });
                            }
                        }
                    }
                }

                if (pixelCount < 6 || (maxX - minX) < 2 || (maxY - minY) < 2) continue;

                // Fix Option 1: Aspect-aware DBNet Vatti unclipping distance (works for both vertical & horizontal)
                float boxW = static_cast<float>(maxX - minX + 1);
                float boxH = static_cast<float>(maxY - minY + 1);
                float unclipDist = (boxW * boxH * 1.65f) / (2.0f * (boxW + boxH));

                int padX = std::max(3, static_cast<int>(std::round(unclipDist * scaleX)));
                int padY = std::max(3, static_cast<int>(std::round(unclipDist * scaleY)));

                RECT r{};
                r.left   = std::max<LONG>(0, static_cast<LONG>(minX * scaleX) - padX);
                r.top    = std::max<LONG>(0, static_cast<LONG>(minY * scaleY) - padY);
                r.right  = std::min<LONG>(frame.width,  static_cast<LONG>(maxX * scaleX) + padX);
                r.bottom = std::min<LONG>(frame.height, static_cast<LONG>(maxY * scaleY) + padY);

                if ((r.right - r.left) >= 8 && (r.bottom - r.top) >= 8) {
                    boxes.push_back(r);
                }
            }
        }
        return boxes;
    }

    std::vector<RECT> PaddleOcrEngine::MergeHorizontalDetections(std::vector<RECT>& rawBoxes) const {
        if (rawBoxes.size() <= 1) return rawBoxes;

        auto isVerticalBox = [](const RECT& r) -> bool {
            LONG w = r.right - r.left;
            LONG h = r.bottom - r.top;
            return (h > (w * 135) / 100);
        };

        std::sort(rawBoxes.begin(), rawBoxes.end(), [](const RECT& a, const RECT& b) {
            if (std::abs(a.top - b.top) > 10) return a.top < b.top;
            return a.left < b.left;
        });

        std::vector<RECT> merged;
        std::vector<bool> used(rawBoxes.size(), false);

        for (size_t i = 0; i < rawBoxes.size(); ++i) {
            if (used[i]) continue;
            RECT cur = rawBoxes[i];
            used[i] = true;

            // Keep vertical Japanese columns intact until after character recognition
            if (isVerticalBox(cur)) {
                merged.push_back(cur);
                continue;
            }

            bool grew = true;
            while (grew) {
                grew = false;
                LONG curH = std::max<LONG>(1, cur.bottom - cur.top);

                for (size_t j = 0; j < rawBoxes.size(); ++j) {
                    if (used[j] || isVerticalBox(rawBoxes[j])) continue;
                    const RECT& cand = rawBoxes[j];
                    LONG candH = std::max<LONG>(1, cand.bottom - cand.top);

                    LONG overlapY = std::min(cur.bottom, cand.bottom) - std::max(cur.top, cand.top);
                    LONG minH = std::min(curH, candH);
                    if (overlapY < (minH * 45) / 100) continue;

                    LONG gapX = std::max(cand.left - cur.right, cur.left - cand.right);
                    if (gapX <= (minH * 140) / 100) {
                        cur.left   = std::min(cur.left, cand.left);
                        cur.top    = std::min(cur.top, cand.top);
                        cur.right  = std::max(cur.right, cand.right);
                        cur.bottom = std::max(cur.bottom, cand.bottom);
                        used[j] = true;
                        grew = true;
                    }
                }
            }
            merged.push_back(cur);
        }
        return merged;
    }

    std::vector<RECT> PaddleOcrEngine::FilterFuriganaBoxes(const std::vector<RECT>& rawBoxes) const {
        if (rawBoxes.size() <= 1) return rawBoxes;

        // Measure minor-axis stroke thickness so vertical columns (24x240) and horizontal lines (240x24) both measure 24px
        std::vector<LONG> thicknesses;
        thicknesses.reserve(rawBoxes.size());
        for (const auto& b : rawBoxes) {
            LONG w = b.right - b.left;
            LONG h = b.bottom - b.top;
            thicknesses.push_back(std::min(w, h));
        }

        std::nth_element(thicknesses.begin(), thicknesses.begin() + thicknesses.size() / 2, thicknesses.end());
        LONG medianThickness = thicknesses[thicknesses.size() / 2];
        LONG minAllowedThickness = (medianThickness * m_config.furiganaHeightRatioPercent) / 100;

        std::vector<RECT> filtered;
        filtered.reserve(rawBoxes.size());
        for (const auto& b : rawBoxes) {
            LONG w = b.right - b.left;
            LONG h = b.bottom - b.top;
            if (std::min(w, h) >= minAllowedThickness) {
                filtered.push_back(b);
            }
        }
        return filtered;
    }

    std::wstring PaddleOcrEngine::RunRecognizer(const StagedFrame& frame, const RECT& box) {
        uint32_t cropW = std::max<LONG>(1, box.right - box.left);
        uint32_t cropH = std::max<LONG>(1, box.bottom - box.top);

        // Query the ONNX model's expected input height dynamically (supports both 32px and 48px PP-OCR models)
        uint32_t recH = 48;
        try {
            auto inShape = m_recSession->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
            if (inShape.size() >= 4 && inShape[2] > 0) {
                recH = static_cast<uint32_t>(inShape[2]);
            }
        } catch (...) {}

        bool isVerticalColumn = (cropH > (cropW * 135) / 100);

        if (!isVerticalColumn) {
            // Standard Horizontal Line Recognition
            float aspect = static_cast<float>(cropW) / static_cast<float>(cropH);
            uint32_t validW = std::clamp<uint32_t>(static_cast<uint32_t>(std::round(recH * aspect)), 32, 1600);

            size_t planeSize = static_cast<size_t>(recH) * validW;
            std::vector<float> inputValues(planeSize * 3, 0.0f);
            float* cB = inputValues.data();
            float* cG = cB + planeSize;
            float* cR = cG + planeSize;

            for (uint32_t y = 0; y < recH; ++y) {
                float srcY = box.top + (y + 0.5f) * (static_cast<float>(cropH) / recH) - 0.5f;
                for (uint32_t x = 0; x < validW; ++x) {
                    float srcX = box.left + (x + 0.5f) * (static_cast<float>(cropW) / validW) - 0.5f;
                    size_t idx = static_cast<size_t>(y) * validW + x;
                    SampleBilinearBgr(frame, srcX, srcY, cB[idx], cG[idx], cR[idx]);
                }
            }

            RecCandidate cand = DecodeCtcTensor(m_recSession.get(), m_memoryInfo, inputValues, recH, validW, m_dictionary);
            return (cand.avgConfidence >= 0.18f) ? cand.text : L"";
        }

        // --- Vertical Japanese Column Recognition (Fix Option 1) ---
        // Method A: Upright Character Strip Tiling (slices column Top-to-Bottom into upright tiles placed Left-to-Right)
        RecCandidate candTiled;
        {
            uint32_t tileSrcH = std::max<uint32_t>(8, cropW);
            uint32_t stepY = std::max<uint32_t>(6, (tileSrcH * 82) / 100);
            uint32_t numTiles = std::clamp<uint32_t>((cropH + stepY - 1) / stepY, 1, 32);
            uint32_t tileDstW = recH;
            uint32_t totalW = std::max<uint32_t>(48, numTiles * tileDstW);

            size_t planeSize = static_cast<size_t>(recH) * totalW;
            std::vector<float> tiledValues(planeSize * 3, 0.0f);
            float* cB = tiledValues.data();
            float* cG = cB + planeSize;
            float* cR = cG + planeSize;

            for (uint32_t t = 0; t < numTiles; ++t) {
                float segTop = static_cast<float>(box.top + t * stepY);
                if (segTop + tileSrcH > box.bottom && cropH >= tileSrcH) {
                    segTop = static_cast<float>(box.bottom - tileSrcH);
                }

                for (uint32_t y = 0; y < recH; ++y) {
                    float srcY = segTop + (y + 0.5f) * (static_cast<float>(tileSrcH) / recH) - 0.5f;
                    for (uint32_t x = 0; x < tileDstW; ++x) {
                        float srcX = box.left + (x + 0.5f) * (static_cast<float>(cropW) / tileDstW) - 0.5f;
                        uint32_t dstX = t * tileDstW + x;
                        size_t idx = static_cast<size_t>(y) * totalW + dstX;
                        SampleBilinearBgr(frame, srcX, srcY, cB[idx], cG[idx], cR[idx]);
                    }
                }
            }
            candTiled = DecodeCtcTensor(m_recSession.get(), m_memoryInfo, tiledValues, recH, totalW, m_dictionary);
        }

        // Method B: 90-Degree Counter-Clockwise Rotation (PaddleOCR vertical rotation fallback)
        RecCandidate candRot90;
        {
            float aspectRot = static_cast<float>(cropH) / static_cast<float>(cropW);
            uint32_t rotW = std::clamp<uint32_t>(static_cast<uint32_t>(std::round(recH * aspectRot)), 32, 1600);

            size_t planeSize = static_cast<size_t>(recH) * rotW;
            std::vector<float> rotValues(planeSize * 3, 0.0f);
            float* cB = rotValues.data();
            float* cG = cB + planeSize;
            float* cR = cG + planeSize;

            for (uint32_t y = 0; y < recH; ++y) {
                // Counter-clockwise 90 deg: tensor Y maps right-to-left across column width
                float srcX = box.right - 1.0f - (y + 0.5f) * (static_cast<float>(cropW) / recH);
                for (uint32_t x = 0; x < rotW; ++x) {
                    // Tensor X maps top-to-bottom down column height
                    float srcY = box.top + (x + 0.5f) * (static_cast<float>(cropH) / rotW) - 0.5f;
                    size_t idx = static_cast<size_t>(y) * rotW + x;
                    SampleBilinearBgr(frame, srcX, srcY, cB[idx], cG[idx], cR[idx]);
                }
            }
            candRot90 = DecodeCtcTensor(m_recSession.get(), m_memoryInfo, rotValues, recH, rotW, m_dictionary);
        }

        const RecCandidate& best = (candTiled.avgConfidence >= candRot90.avgConfidence && !candTiled.text.empty())
            ? candTiled
            : candRot90;

        return (best.avgConfidence >= 0.15f) ? best.text : L"";
    }

    std::vector<TextBlock> PaddleOcrEngine::MergeParagraphLines(std::vector<TextBlock>& lines) const {
        if (lines.size() <= 1) return lines;

        // Fix Option 2: Detect whether lines are vertical/CJK manga columns or standard horizontal lines
        int cjkOrVerticalCount = 0;
        for (const auto& b : lines) {
            LONG w = b.bounds.right - b.bounds.left;
            LONG h = b.bounds.bottom - b.bounds.top;
            if (h > (w * 120) / 100 || (ContainsCjkCharacters(b.originalText) && b.originalText.size() <= 4)) {
                ++cjkOrVerticalCount;
            }
        }

        bool useMangaBubbleMerger = (cjkOrVerticalCount * 2 >= static_cast<int>(lines.size()));

        if (useMangaBubbleMerger) {
            // Step 1: Assemble stacked vertical fragments belonging to the SAME vertical column (Top-to-Bottom)
            std::sort(lines.begin(), lines.end(), [](const TextBlock& a, const TextBlock& b) {
                return a.bounds.top < b.bounds.top;
            });

            std::vector<TextBlock> columns;
            std::vector<bool> colUsed(lines.size(), false);

            for (size_t i = 0; i < lines.size(); ++i) {
                if (colUsed[i]) continue;
                TextBlock col = lines[i];
                colUsed[i] = true;

                bool grew = true;
                while (grew) {
                    grew = false;
                    LONG colW = std::max<LONG>(1, col.bounds.right - col.bounds.left);

                    for (size_t j = 0; j < lines.size(); ++j) {
                        if (colUsed[j]) continue;
                        const RECT& cand = lines[j].bounds;
                        LONG candW = std::max<LONG>(1, cand.right - cand.left);
                        LONG minW = std::min(colW, candW);

                        LONG overlapX = std::min(col.bounds.right, cand.right) - std::max(col.bounds.left, cand.left);
                        LONG gapY = cand.top - col.bounds.bottom;

                        if (overlapX >= (minW * 45) / 100 && gapY >= -10 && gapY <= minW * 2) {
                            col.bounds.left   = std::min(col.bounds.left, cand.left);
                            col.bounds.top    = std::min(col.bounds.top, cand.top);
                            col.bounds.right  = std::max(col.bounds.right, cand.right);
                            col.bounds.bottom = std::max(col.bounds.bottom, cand.bottom);
                            col.originalText += lines[j].originalText;
                            colUsed[j] = true;
                            grew = true;
                        }
                    }
                }
                columns.push_back(std::move(col));
            }

            // Step 2: Group adjacent vertical columns inside the SAME speech bubble in Right-to-Left manga order
            std::sort(columns.begin(), columns.end(), [](const TextBlock& a, const TextBlock& b) {
                return a.bounds.right > b.bounds.right; // Right-to-Left
            });

            std::vector<TextBlock> bubbles;
            std::vector<bool> bubUsed(columns.size(), false);

            for (size_t i = 0; i < columns.size(); ++i) {
                if (bubUsed[i]) continue;
                std::vector<TextBlock> cluster;
                cluster.push_back(columns[i]);
                bubUsed[i] = true;

                RECT clusterRect = columns[i].bounds;
                LONG refColW = std::max<LONG>(16, columns[i].bounds.right - columns[i].bounds.left);

                bool added = true;
                while (added) {
                    added = false;
                    for (size_t j = 0; j < columns.size(); ++j) {
                        if (bubUsed[j]) continue;
                        const RECT& cand = columns[j].bounds;
                        LONG candW = std::max<LONG>(16, cand.right - cand.left);
                        LONG maxColW = std::max(refColW, candW);

                        LONG overlapY = std::min(clusterRect.bottom, cand.bottom) - std::max(clusterRect.top, cand.top);
                        LONG minH = std::max<LONG>(1, std::min(clusterRect.bottom - clusterRect.top, cand.bottom - cand.top));

                        LONG gapX = std::max(cand.left - clusterRect.right, clusterRect.left - cand.right);

                        // Columns belong to the same speech bubble if they overlap vertically and sit side-by-side
                        if (overlapY >= (minH * 25) / 100 && gapX <= (maxColW * 185) / 100) {
                            cluster.push_back(columns[j]);
                            clusterRect.left   = std::min(clusterRect.left, cand.left);
                            clusterRect.top    = std::min(clusterRect.top, cand.top);
                            clusterRect.right  = std::max(clusterRect.right, cand.right);
                            clusterRect.bottom = std::max(clusterRect.bottom, cand.bottom);
                            refColW = std::max(refColW, candW);
                            bubUsed[j] = true;
                            added = true;
                        }
                    }
                }

                // Sort columns inside the speech bubble strictly Right-to-Left
                std::sort(cluster.begin(), cluster.end(), [](const TextBlock& a, const TextBlock& b) {
                    return (a.bounds.left + a.bounds.right) > (b.bounds.left + b.bounds.right);
                });

                TextBlock mergedBubble = cluster.front();
                mergedBubble.bounds = clusterRect;
                mergedBubble.originalText.clear();
                for (const auto& c : cluster) {
                    mergedBubble.originalText += c.originalText;
                }

                float area = static_cast<float>(clusterRect.right - clusterRect.left) *
                             static_cast<float>(clusterRect.bottom - clusterRect.top);
                float charEst = std::max(4.0f, static_cast<float>(mergedBubble.originalText.size()) * 1.8f);
                mergedBubble.estimatedFontSize = std::clamp(std::sqrt(area / charEst) * 0.85f, 12.0f, 24.0f);

                bubbles.push_back(std::move(mergedBubble));
            }

            return bubbles;
        }

        // Standard Horizontal Paragraph Merger (Left-to-Right, Top-to-Bottom)
        std::sort(lines.begin(), lines.end(), [](const TextBlock& a, const TextBlock& b) {
            if (std::abs(a.bounds.top - b.bounds.top) > 10)
                return a.bounds.top < b.bounds.top;
            return a.bounds.left < b.bounds.left;
        });

        std::vector<TextBlock> merged;
        std::vector<bool> used(lines.size(), false);

        for (size_t i = 0; i < lines.size(); ++i) {
            if (used[i]) continue;
            TextBlock current = lines[i];
            used[i] = true;

            bool addedAny = true;
            while (addedAny) {
                addedAny = false;
                LONG lineH = lines[i].bounds.bottom - lines[i].bounds.top;

                for (size_t j = i + 1; j < lines.size(); ++j) {
                    if (used[j]) continue;

                    const RECT& cand = lines[j].bounds;
                    LONG verticalGap = cand.top - current.bounds.bottom;
                    LONG horizOverlap = std::min(current.bounds.right, cand.right) - std::max(current.bounds.left, cand.left);

                    if (verticalGap >= -8 && verticalGap <= (lineH * 85) / 100 && horizOverlap > 12) {
                        current.bounds.left = std::min(current.bounds.left, cand.left);
                        current.bounds.top = std::min(current.bounds.top, cand.top);
                        current.bounds.right = std::max(current.bounds.right, cand.right);
                        current.bounds.bottom = std::max(current.bounds.bottom, cand.bottom);
                        current.originalText += L" " + lines[j].originalText;
                        used[j] = true;
                        addedAny = true;
                    }
                }
            }
            merged.push_back(std::move(current));
        }
        return merged;
    }

    void PaddleOcrEngine::SampleBoxColors(const StagedFrame& frame, TextBlock& block) const {
        uint64_t sumR = 0, sumG = 0, sumB = 0, count = 0;

        LONG x0 = std::clamp<LONG>(block.bounds.left, 0, frame.width - 1);
        LONG x1 = std::clamp<LONG>(block.bounds.right - 1, 0, frame.width - 1);
        LONG y0 = std::clamp<LONG>(block.bounds.top, 0, frame.height - 1);
        LONG y1 = std::clamp<LONG>(block.bounds.bottom - 1, 0, frame.height - 1);

        for (LONG x = x0; x <= x1; x += 2) {
            const uint8_t* pTop = frame.bgraPixels.data() + (static_cast<size_t>(y0) * frame.rowPitch) + (x * 4);
            const uint8_t* pBot = frame.bgraPixels.data() + (static_cast<size_t>(y1) * frame.rowPitch) + (x * 4);
            sumB += pTop[0] + pBot[0];
            sumG += pTop[1] + pBot[1];
            sumR += pTop[2] + pBot[2];
            count += 2;
        }

        if (count > 0) {
            float r = (sumR / static_cast<float>(count)) / 255.0f;
            float g = (sumG / static_cast<float>(count)) / 255.0f;
            float b = (sumB / static_cast<float>(count)) / 255.0f;

            block.bgColor = D2D1::ColorF(r, g, b, 0.94f);

            float luminance = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            block.textColor = (luminance > 0.52f)
                ? D2D1::ColorF(0.08f, 0.08f, 0.08f, 1.0f)
                : D2D1::ColorF(0.98f, 0.98f, 0.98f, 1.0f);
        }
    }

} // namespace ChromaTranslate