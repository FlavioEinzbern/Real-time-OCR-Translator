@echo off
setlocal EnableDelayedExpansion
title ChromaTranslate - Automated Dependency & Build Script

echo ============================================================================
echo  ChromaTranslate Automated Dependency Checker ^& Builder (x64 Release)
echo ============================================================================
echo.

:: Ensure working directory is the script directory
cd /d "%~dp0"

:: ----------------------------------------------------------------------------
:: 1. Verify Project Source Files Exist
:: ----------------------------------------------------------------------------
if not exist "CMakeLists.txt" (
    echo [ERROR] CMakeLists.txt not found in %CD%.
    echo Please place build.bat in the root ChromaTranslate folder.
    pause
    exit /b 1
)

if not exist "include" mkdir "include"
if not exist "src" mkdir "src"
if not exist "models" mkdir "models"
if not exist "external\onnxruntime" mkdir "external\onnxruntime"

:: ----------------------------------------------------------------------------
:: 2. Check Built-in Windows Utilities (curl.exe and tar.exe)
:: ----------------------------------------------------------------------------
where curl >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [ERROR] curl.exe is required but was not found in PATH.
    pause
    exit /b 1
)

where tar >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [ERROR] tar.exe is required but was not found in PATH.
    pause
    exit /b 1
)

:: ----------------------------------------------------------------------------
:: 3. Check for Visual Studio 2022 C++ Compiler / Build Tools
:: ----------------------------------------------------------------------------
echo [1/6] Checking for Visual Studio C++ Build Tools...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_INSTALL_PATH="

if exist "!VSWHERE!" (
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VS_INSTALL_PATH=%%i"
    )
)

if "!VS_INSTALL_PATH!"=="" (
    echo [WARN] MSVC C++ x64 build tools not detected. Installing via winget...
    where winget >nul 2>&1
    if !ERRORLEVEL! neq 0 (
        echo [ERROR] winget is not available. Please install Visual Studio 2022 with "Desktop development with C++".
        pause
        exit /b 1
    )
    winget install -e --id Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VS_INSTALL_PATH=%%i"
    )
    if "!VS_INSTALL_PATH!"=="" (
        echo [ERROR] Visual Studio C++ Build Tools installation could not be verified.
        pause
        exit /b 1
    )
)
echo       Found MSVC at: !VS_INSTALL_PATH!

:: ----------------------------------------------------------------------------
:: 4. Check for CMake (System PATH, Program Files, or VS Bundled CMake)
:: ----------------------------------------------------------------------------
echo [2/6] Checking for CMake...
set "CMAKE_EXE="

where cmake >nul 2>&1
if %ERRORLEVEL% equ 0 (
    set "CMAKE_EXE=cmake"
) else if exist "%ProgramFiles%\CMake\bin\cmake.exe" (
    set "CMAKE_EXE=%ProgramFiles%\CMake\bin\cmake.exe"
) else if exist "!VS_INSTALL_PATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "CMAKE_EXE=!VS_INSTALL_PATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

if "!CMAKE_EXE!"=="" (
    echo [WARN] CMake not found. Installing CMake via winget...
    winget install -e --id Kitware.CMake
    if exist "%ProgramFiles%\CMake\bin\cmake.exe" (
        set "CMAKE_EXE=%ProgramFiles%\CMake\bin\cmake.exe"
    ) else (
        echo [ERROR] CMake installation failed or requires a terminal restart.
        pause
        exit /b 1
    )
)
echo       Using CMake: "!CMAKE_EXE!"

:: ----------------------------------------------------------------------------
:: 5. Check & Download ONNX Runtime C++ SDK (v1.17.1 win-x64)
:: ----------------------------------------------------------------------------
echo [3/6] Checking ONNX Runtime C++ SDK...
set "ORT_VERSION=1.17.1"
set "ORT_FOLDER=onnxruntime-win-x64-%ORT_VERSION%"
set "ORT_ZIP=external\%ORT_FOLDER%.zip"
set "ORT_URL=https://github.com/microsoft/onnxruntime/releases/download/v%ORT_VERSION%/%ORT_FOLDER%.zip"

if not exist "external\onnxruntime\lib\onnxruntime.lib" (
    echo       Downloading ONNX Runtime v%ORT_VERSION% ^(win-x64^)...
    curl -L --fail --retry 3 -o "%ORT_ZIP%" "%ORT_URL%"
    if !ERRORLEVEL! neq 0 (
        echo [ERROR] Failed to download ONNX Runtime from GitHub.
        pause
        exit /b 1
    )

    echo       Extracting ONNX Runtime...
    tar -xf "%ORT_ZIP%" -C "external"
    if exist "external\%ORT_FOLDER%\include" (
        xcopy /E /I /Y "external\%ORT_FOLDER%\include\*" "external\onnxruntime\include\" >nul
    )
    if exist "external\%ORT_FOLDER%\lib" (
        xcopy /E /I /Y "external\%ORT_FOLDER%\lib\*" "external\onnxruntime\lib\" >nul
    )
    rmdir /S /Q "external\%ORT_FOLDER%"
    del /F /Q "%ORT_ZIP%"
)
echo       ONNX Runtime C++ SDK verified.

:: ----------------------------------------------------------------------------
:: 6. Check & Download PP-OCRv4 Models and Dictionary
:: ----------------------------------------------------------------------------
echo [4/6] Checking OCR ONNX models and dictionary...

set "DET_MODEL=models\ch_PP-OCRv4_det_infer.onnx"
set "DET_URL=https://huggingface.co/SWHL/RapidOCR/resolve/main/PP-OCRv4/ch_PP-OCRv4_det_infer.onnx"

set "REC_MODEL=models\japan_PP-OCRv4_rec_infer.onnx"
set "REC_URL=https://huggingface.co/breezedeus/cnocr-ppocr-japan_PP-OCRv3/resolve/main/japan_PP-OCRv3_rec_infer.onnx"

set "DICT_FILE=models\japan_dict.txt"
set "DICT_URL=https://raw.githubusercontent.com/PaddlePaddle/PaddleOCR/main/ppocr/utils/dict/japan_dict.txt"

if not exist "%DET_MODEL%" (
    echo       Downloading Text Detector model ^(ch_PP-OCRv4_det_infer.onnx^)...
    curl -L --fail --retry 3 -o "%DET_MODEL%" "%DET_URL%"
    if !ERRORLEVEL! neq 0 (
        echo [ERROR] Failed to download Text Detector model.
        pause
        exit /b 1
    )
)

if not exist "%REC_MODEL%" (
    echo       Downloading Japanese Text Recognizer model...
    curl -L --fail --retry 3 -o "%REC_MODEL%" "%REC_URL%"
    if !ERRORLEVEL! neq 0 (
        echo [ERROR] Failed to download Japanese Recognizer model.
        pause
        exit /b 1
    )
)

if not exist "%DICT_FILE%" (
    echo       Downloading Japanese character dictionary ^(japan_dict.txt^)...
    curl -L --fail --retry 3 -o "%DICT_FILE%" "%DICT_URL%"
    if !ERRORLEVEL! neq 0 (
        echo [ERROR] Failed to download japan_dict.txt.
        pause
        exit /b 1
    )
)
echo       All OCR models and dictionaries verified.

:: ----------------------------------------------------------------------------
:: 7. Sanitize Known Source Typos Before Compiling
:: ----------------------------------------------------------------------------
echo [5/6] Running pre-compile source checks...
if exist "src\SimpleCapture.cpp" (
    powershell -NoProfile -Command "(Get-Content 'src\SimpleCapture.cpp' -Raw) -replace 'DWRITE_TEXT_metrics','DWRITE_TEXT_METRICS' | Set-Content 'src\SimpleCapture.cpp' -NoNewline"
)
if exist "src\PaddleOcrEngine.cpp" (
    powershell -NoProfile -Command "(Get-Content 'src\PaddleOcrEngine.cpp' -Raw) -replace 'uint32_tlongest','uint32_t longest' | Set-Content 'src\PaddleOcrEngine.cpp' -NoNewline"
)

:: ----------------------------------------------------------------------------
:: 8. Configure CMake & Build Release x64 Executable
:: ----------------------------------------------------------------------------
echo [6/6] Configuring and building ChromaTranslate ^(Release x64^)...
if not exist "build" mkdir "build"

"!CMAKE_EXE!" -S . -B build -A x64
if %ERRORLEVEL% neq 0 (
    echo [ERROR] CMake configuration failed.
    pause
    exit /b 1
)

"!CMAKE_EXE!" --build build --config Release --parallel
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Compilation failed. Check the compiler output above.
    pause
    exit /b 1
)

echo.
echo ============================================================================
echo  [SUCCESS] Build Complete!
echo  Executable : %CD%\build\Release\ChromaTranslate.exe
echo  Config     : %CD%\build\Release\config.ini
echo  Models     : %CD%\build\Release\models\
echo ============================================================================
echo.
pause
endlocal