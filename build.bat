@echo off
rem Builds the Noveske Chainsaw GML plugin, plus the alignment tool and the in-game test harness.
rem   build.bat            build into build\
rem   build.bat install    also copy the plugin into the game (refuses while the game runs)
rem
rem The mesh and textures are encrypted (tools\pack, a new key every build) and built into
rem NoveskeChainsaw.dll (src\NoveskeChainsaw.rc), so Assets\ must exist first: generate it from the
rem purchased source art with tools\prepare_runtime.py.
rem Needs the GML SDK 2.2+ (github.com/RemArrow/geronimo-mod-loader): the external\gml submodule,
rem or set GML_SDK to its include folder. Set GML_GAME_DIR to your ...\Geronimo\Binaries\Win64 if
rem the game is not in the default Steam library.
setlocal
set ROOT=%~dp0
set OUT=%ROOT%build
set SDK=%ROOT%external\gml\include
if defined GML_SDK set SDK=%GML_SDK%
set GAME=C:\Program Files (x86)\Steam\steamapps\common\GERONIMO\Geronimo\Binaries\Win64
if defined GML_GAME_DIR set GAME=%GML_GAME_DIR%

if not exist "%SDK%\GML\GML.hpp" (
  echo GML SDK not found at %SDK%
  echo Run: git submodule update --init   or set GML_SDK
  exit /b 1
)
findstr /c:"ImportDynamicMeshFromMemory" "%SDK%\GML\GML.h" >nul || (
  echo The GML SDK at %SDK% is older than 2.2 - update the submodule or set GML_SDK
  exit /b 1
)
if not exist "%ROOT%Assets\noveske.obj" (
  echo Assets\ is missing - generate it with tools\prepare_runtime.py first ^(it is built into the DLL^).
  exit /b 1
)
if not defined VCToolsInstallDir (
  call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
)
for %%D in ("%OUT%\obj" "%OUT%\gen" "%OUT%\NoveskeChainsaw" "%OUT%\tests\NoveskeDev") do if not exist "%%~D" mkdir "%%~D"
set CFLAGS=/nologo /std:c++20 /O2 /MT /EHsc /W3 /Zi /DUNICODE /D_UNICODE /I"%SDK%"
set LFLAGS=/DEBUG /INCREMENTAL:NO

echo === pack (encrypt Assets\ with a new key)
cl /nologo /std:c++20 /O2 /MT /EHsc /Fo"%OUT%\obj\\" "%ROOT%tools\pack\pack.cpp" /Fe"%OUT%\pack.exe" || exit /b 1
"%OUT%\pack.exe" "%ROOT%Assets" "%OUT%\gen\payload.bin" "%OUT%\gen\payload_key.h" || exit /b 1
echo === plugin (encrypted assets embedded)
rc /nologo /I "%ROOT%." /fo"%OUT%\obj\NoveskeChainsaw.res" "%ROOT%src\NoveskeChainsaw.rc" || exit /b 1
cl %CFLAGS% /I"%OUT%\gen" /LD /Fo"%OUT%\obj\\" /Fd"%OUT%\obj\\" "%ROOT%src\*.cpp" "%OUT%\obj\NoveskeChainsaw.res" /Fe"%OUT%\NoveskeChainsaw\NoveskeChainsaw.dll" /link %LFLAGS% || exit /b 1
echo === align tool
cl /nologo /std:c++20 /O2 /EHsc /Fo"%OUT%\obj\\" "%ROOT%tools\align\align.cpp" /Fe"%OUT%\align.exe" || exit /b 1
echo === test harness (never installed)
cl %CFLAGS% /LD /Fo"%OUT%\obj\\" /Fd"%OUT%\obj\\" "%ROOT%tests\NoveskeDev\NoveskeDev.cpp" /Fe"%OUT%\tests\NoveskeDev\NoveskeDev.dll" /link %LFLAGS% || exit /b 1
for /r "%OUT%" %%F in (*.exp *.lib *.ilk) do del /q "%%F"

if /i "%1"=="install" (
  tasklist /FI "IMAGENAME eq Geronimo-Win64-Shipping.exe" | find /I "Geronimo-Win64" >nul && (
    echo Geronimo is running - close it first.
    exit /b 1
  )
  echo === install to "%GAME%\GML\plugins\NoveskeChainsaw"
  if not exist "%GAME%\GML\plugins\NoveskeChainsaw" mkdir "%GAME%\GML\plugins\NoveskeChainsaw"
  copy /y "%OUT%\NoveskeChainsaw\NoveskeChainsaw.dll" "%GAME%\GML\plugins\NoveskeChainsaw\" >nul || exit /b 1
  copy /y "%ROOT%CREDITS.txt" "%GAME%\GML\plugins\NoveskeChainsaw\" >nul || exit /b 1
  rem symbols stay in build\ (they would map out the key handling); loose assets from plugin
  rem versions before 1.4 are no longer read
  if exist "%GAME%\GML\plugins\NoveskeChainsaw\NoveskeChainsaw.pdb" del /q "%GAME%\GML\plugins\NoveskeChainsaw\NoveskeChainsaw.pdb"
  if exist "%GAME%\GML\plugins\NoveskeChainsaw\Assets" rmdir /s /q "%GAME%\GML\plugins\NoveskeChainsaw\Assets"
)
echo === done
exit /b 0
