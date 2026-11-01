@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem ---------------------------------------------------------------------------
rem 构建 GUI 用的 JNI 桥：build\bin\libFlowerFarmFinderLibJ.dll
rem
rem 由 FlowerFinderGUI 的 build.gradle 调用（也可手动执行）：
rem   FlowerFinderBase\jni\build-jni.bat
rem
rem 依赖：MSYS2 UCRT64 的 gcc/g++/cmake/ninja + 一个带 include\jni.h 的 JDK。
rem ---------------------------------------------------------------------------
set "HERE=%~dp0"
set "ROOT=%HERE%.."
set "BUILD=%ROOT%\build"

call :resolve_java_home
if errorlevel 1 exit /b 1

if not exist "C:\msys64\ucrt64\bin\gcc.exe" (
  echo ERROR: MSYS2 UCRT64 toolchain not found at C:\msys64\ucrt64\bin
  echo Install MSYS2 and the mingw-w64-ucrt-x86_64-gcc / cmake / ninja packages.
  exit /b 1
)
set "PATH=C:\msys64\ucrt64\bin;%PATH%"

where cmake >nul 2>nul
if errorlevel 1 (
  echo ERROR: cmake not found on PATH ^(expected in C:\msys64\ucrt64\bin^)
  exit /b 1
)
where ninja >nul 2>nul
if errorlevel 1 (
  echo ERROR: ninja not found on PATH ^(expected in C:\msys64\ucrt64\bin^)
  exit /b 1
)

echo [FlowerFinderBase] JAVA_HOME=%JAVA_HOME%
echo [FlowerFinderBase] cmake -S "%ROOT%" -B "%BUILD%" -G Ninja
cmake -S "%ROOT%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DFFB_JAVA_HOME="%JAVA_HOME%"
if errorlevel 1 exit /b 1

cmake --build "%BUILD%" --target FlowerFinderBaseJni
if errorlevel 1 exit /b 1

if not exist "%BUILD%\bin\libFlowerFarmFinderLibJ.dll" (
  echo ERROR: JNI library was not produced at "%BUILD%\bin\libFlowerFarmFinderLibJ.dll"
  exit /b 1
)

echo Built: %BUILD%\bin\libFlowerFarmFinderLibJ.dll
exit /b 0

rem --- 找到带 include\jni.h 的 JDK -----------------------------------------
:resolve_java_home
if defined JAVA_HOME (
  if /I "%JAVA_HOME:~-1%"=="\" set "JAVA_HOME=%JAVA_HOME:~0,-1%"
  if /I "%JAVA_HOME:~-4%"=="\bin" set "JAVA_HOME=%JAVA_HOME:~0,-4%"
)
if exist "%JAVA_HOME%\include\jni.h" exit /b 0
for %%D in ("C:\Program Files\Java\jdk-17" "C:\Program Files\Java\jdk-21" "C:\Program Files\Java\latest" "C:\Program Files\Java\jdk1.8") do (
  if exist "%%~D\include\jni.h" (
    set "JAVA_HOME=%%~D"
    exit /b 0
  )
)
for /f "delims=" %%i in ('where java 2^>nul') do (
  if exist "%%~dpi..\include\jni.h" (
    set "JAVA_HOME=%%~dpi.."
    exit /b 0
  )
)
echo ERROR: Cannot find a JDK with include\jni.h.
echo Point JAVA_HOME at a JDK root, for example C:\Program Files\Java\jdk-17
exit /b 1
