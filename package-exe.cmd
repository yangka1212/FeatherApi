@echo off
setlocal

set "PROJECT_DIR=%~dp0"
set "BUILD_DIR=%PROJECT_DIR%package-build"
set "OUTPUT_DIR=%PROJECT_DIR%dist"
set "OUTPUT_NAME=FeatherApi.exe"
if not "%~1"=="" set "OUTPUT_NAME=%~1"
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%VCVARS%" (
    echo Visual Studio x64 build tools were not found.
    exit /b 1
)

call "%VCVARS%"
if errorlevel 1 exit /b %errorlevel%

if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
mkdir "%BUILD_DIR%"
if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"

pushd "%PROJECT_DIR%"

cl.exe /nologo /c /O2 /Oi /GL /W4 /WX /EHsc /MT /GS /Gy /std:c++17 /permissive- /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DNDEBUG /Fo"%BUILD_DIR%\\" main.cpp main_window.cpp storage.cpp http_client.cpp
if errorlevel 1 goto :failed

rc.exe /nologo /fo"%BUILD_DIR%\resources.res" resources.rc
if errorlevel 1 goto :failed

link.exe /nologo /OUT:"%OUTPUT_DIR%\%OUTPUT_NAME%" /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /LTCG /MANIFEST:EMBED /MANIFESTINPUT:app.manifest "%BUILD_DIR%\main.obj" "%BUILD_DIR%\main_window.obj" "%BUILD_DIR%\storage.obj" "%BUILD_DIR%\http_client.obj" "%BUILD_DIR%\resources.res" comctl32.lib winhttp.lib shlwapi.lib shell32.lib ole32.lib user32.lib gdi32.lib advapi32.lib
if errorlevel 1 goto :failed

popd
rmdir /s /q "%BUILD_DIR%"
echo Created: %OUTPUT_DIR%\%OUTPUT_NAME%
exit /b 0

:failed
set "BUILD_RESULT=%errorlevel%"
popd
rmdir /s /q "%BUILD_DIR%"
exit /b %BUILD_RESULT%
