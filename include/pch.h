#pragma once

#include <windows.h>
#include <windowsx.h>
#include <winhttp.h>
#include <dwmapi.h>
#include <unknwn.h>
#include <inspectable.h>

// DirectX 11, Direct2D, DirectWrite
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d2d1_3.h>
#include <dwrite_3.h>

// C++/WinRT headers for Capture + Windows.Media.Ocr (Solution OCR-3)
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>

// Standard C++17 STL
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <algorithm>