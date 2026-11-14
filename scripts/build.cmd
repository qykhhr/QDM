@echo off
setlocal
rem Build only inside this repository. The Qt binary kit requires v120 x64.
cd /d "%~dp0.."
set "QDM_ROOT=%CD%"
rem Only one supported application build. Avoid keeping stale runnable versions.
set "QDM_BUILD=release"
if not "%~1"=="" (
    echo Only build\release is supported. Run scripts\build.cmd without arguments.
    exit /b 2
)
call "D:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\vcvarsall.bat" amd64
if errorlevel 1 exit /b 1
if not exist "build\%QDM_BUILD%" mkdir "build\%QDM_BUILD%"
cd "build\%QDM_BUILD%"
lib /nologo /machine:x64 /def:"%QDM_ROOT%\third_party\curl\curl-8.23.0_9-win64-mingw\bin\libcurl-x64.def" /name:libcurl-x64.dll /out:curl-import.lib
if errorlevel 1 exit /b 1
"D:\Qt\Qt5.8.0\5.8\msvc2013_64\bin\qmake.exe" "%QDM_ROOT%\QDM.pro" "CONFIG+=release" "CONFIG-=debug"
if errorlevel 1 exit /b 1
nmake /nologo
if errorlevel 1 exit /b 1
if not exist host mkdir host
cd host
"D:\Qt\Qt5.8.0\5.8\msvc2013_64\bin\qmake.exe" "%QDM_ROOT%\native-host\QDMHost.pro" "CONFIG+=release" "CONFIG-=debug"
if errorlevel 1 exit /b 1
nmake /nologo
if errorlevel 1 exit /b 1
cd ..
copy /y "%QDM_ROOT%\third_party\curl\curl-8.23.0_9-win64-mingw\bin\libcurl-x64.dll" bin\ >nul
if errorlevel 1 exit /b 1
copy /y "D:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\redist\x64\Microsoft.VC120.CRT\msvcp120.dll" bin\ >nul
if errorlevel 1 exit /b 1
copy /y "D:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\redist\x64\Microsoft.VC120.CRT\msvcr120.dll" bin\ >nul
if errorlevel 1 exit /b 1
"D:\Qt\Qt5.8.0\5.8\msvc2013_64\bin\windeployqt.exe" --release --no-translations --no-compiler-runtime bin\QDM.exe
if errorlevel 1 exit /b 1
echo Built: %QDM_ROOT%\build\%QDM_BUILD%\bin\QDM.exe
