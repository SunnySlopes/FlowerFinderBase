cmake_minimum_required(VERSION 3.20)
project(FlowerFinderBase VERSION 1.0.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# FlowerFinderBase —— 全花农场搜索核心库
#
# 所有 C/C++ 计算逻辑（CTS 气候过渡搜索 + 群系/花噪声校验 + cubiomes）都在
# 本仓库；FlorFinderGUI 只保留 Swing 界面，通过下面两个产物与核心库交互：
#
#   FlowerFinderBase-cli.exe          独立命令行（--out 输出 txt 报告）
#   libFlowerFarmFinderLibJ.dll       JNI 桥（供 GUI 直接查询，契约见 README）
#
# 依赖一个子模块：cubiomes/（https://github.com/xpple/cubiomes）
#   git submodule update --init
# ---------------------------------------------------------------------------

if (MSVC)
  message(FATAL_ERROR
    "MSVC is not supported. Use the MSYS2 UCRT64 gcc toolchain "
    "(the search results must stay bit-identical to the existing builds).")
endif()

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FFB_BUILD_CLI "Build the standalone FlowerFinderBase-cli executable" ON)
option(FFB_BUILD_JNI "Build the JNI bridge shared library used by the GUI" ON)

set(FFB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(FFB_CUBIOMES_DIR "${FFB_ROOT}/cubiomes"
    CACHE PATH "Path to the cubiomes checkout")

if (NOT EXISTS "${FFB_CUBIOMES_DIR}/generator.c" OR
    NOT EXISTS "${FFB_CUBIOMES_DIR}/biomes.h")
  message(FATAL_ERROR
    "cubiomes submodule is missing: ${FFB_CUBIOMES_DIR}\n"
    "Run 'git submodule update --init' in the FlowerFinderBase repository, "
    "or pass -DFFB_CUBIOMES_DIR=<path-to-cubiomes>.")
endif()

# --- 核心源文件 -------------------------------------------------------------
# cubiomes 只编 getBiomeAt()/str2mc() 用得到的根目录源文件：
# finders/carver/xradv/quadbase/terrainnoise 与 features/ 都不需要。
set(FFB_CUBIOMES_SOURCES
    "${FFB_CUBIOMES_DIR}/generator.c"
    "${FFB_CUBIOMES_DIR}/layers.c"
    "${FFB_CUBIOMES_DIR}/biomenoise.c"
    "${FFB_CUBIOMES_DIR}/biomes.c"
    "${FFB_CUBIOMES_DIR}/noise.c"
    "${FFB_CUBIOMES_DIR}/util.c")

set(FFB_CTS_SOURCES
    "${FFB_ROOT}/cts/climate.c"
    "${FFB_ROOT}/cts/climate_simd.c"
    "${FFB_ROOT}/cts/biome.c"
    "${FFB_ROOT}/cts/engine.c"
    "${FFB_ROOT}/cts/parallel.cpp")

find_package(Threads REQUIRED)

add_library(flower_core STATIC
    "${FFB_ROOT}/src/FlowerFarmSearch.cpp"
    ${FFB_CTS_SOURCES}
    ${FFB_CUBIOMES_SOURCES})

set_target_properties(flower_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(flower_core
    PUBLIC
        "${FFB_ROOT}/include"
        "${FFB_ROOT}"
        "${FFB_CUBIOMES_DIR}"
        "${FFB_CUBIOMES_DIR}/.."
    PRIVATE
        "${FFB_ROOT}/cts")
target_compile_definitions(flower_core PRIVATE _WIN32)

# 逐位一致的硬性要求：必须 -ffp-contract=off，绝不可加 -ffast-math
# （与 All-In-One Finder 的 native 构建、SlimeFinderBase 保持一致）。
set(FFB_FAST_FLAGS
    -O3 -DNDEBUG -fwrapv -ffp-contract=off -fno-lto -mpopcnt)

target_compile_options(flower_core PRIVATE ${FFB_FAST_FLAGS})

# cubiomes rng.h 定义了 lerp()，C++20 <math.h> 有 using std::lerp；
# 用 pre_cubiomes.h 在首次包含时改名。只对 C++ 编译单元强制包含。
set(FFB_CXX_SOURCES
    "${FFB_ROOT}/src/FlowerFarmSearch.cpp"
    "${FFB_ROOT}/jni/FlowerFinderBaseJni.cpp"
    "${FFB_ROOT}/cli/main.cpp")
set_source_files_properties(${FFB_CXX_SOURCES}
    PROPERTIES COMPILE_OPTIONS
    "-include;${FFB_ROOT}/cxx_compat/pre_cubiomes.h")

if (MINGW OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # exe / dll 都静态链接运行库，可脱离 MSYS2 环境直接分发 / 被 JVM 加载。
  set(FFB_STATIC_LINK -static -static-libgcc -static-libstdc++)
  target_link_options(flower_core INTERFACE ${FFB_STATIC_LINK})
  if (NOT WIN32)
    target_link_libraries(flower_core PUBLIC m)
  endif()
endif()

# std::thread (winpthreads on MSYS2, pthreads under Emscripten -pthread).
target_link_libraries(flower_core INTERFACE Threads::Threads)

# --- 独立命令行 -------------------------------------------------------------
if (FFB_BUILD_CLI)
  add_executable(FlowerFinderBase-cli "${FFB_ROOT}/cli/main.cpp")
  target_link_libraries(FlowerFinderBase-cli PRIVATE flower_core)
  target_compile_definitions(FlowerFinderBase-cli PRIVATE
      FFB_VERSION="${PROJECT_VERSION}")
  set_target_properties(FlowerFinderBase-cli PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# --- JNI 桥（GUI 用）--------------------------------------------------------
if (FFB_BUILD_JNI)
  # 解析 JDK：优先 -DFFB_JAVA_HOME，其次 $ENV{JAVA_HOME}
  set(FFB_JAVA_HOME "" CACHE PATH "JDK root used for jni.h (defaults to JAVA_HOME)")
  if (NOT FFB_JAVA_HOME AND DEFINED ENV{JAVA_HOME})
    set(FFB_JAVA_HOME "$ENV{JAVA_HOME}")
  endif()
  if (FFB_JAVA_HOME)
    # 容忍 JAVA_HOME 被写成 <jdk>\bin 或 <jdk>\
    string(REGEX REPLACE "[\\\\/]+$" "" FFB_JAVA_HOME "${FFB_JAVA_HOME}")
    if (FFB_JAVA_HOME MATCHES "[\\\\/]bin$")
      string(REGEX REPLACE "[\\\\/]bin$" "" FFB_JAVA_HOME "${FFB_JAVA_HOME}")
    endif()
  endif()

  find_path(FFB_JNI_INCLUDE jni.h
      PATHS "${FFB_JAVA_HOME}/include" NO_DEFAULT_PATH)
  find_path(FFB_JNI_INCLUDE_MD jni_md.h
      PATHS "${FFB_JAVA_HOME}/include/win32"
            "${FFB_JAVA_HOME}/include/linux"
            "${FFB_JAVA_HOME}/include/darwin"
      NO_DEFAULT_PATH)

  if (FFB_JNI_INCLUDE AND FFB_JNI_INCLUDE_MD)
    add_library(FlowerFinderBaseJni SHARED
        "${FFB_ROOT}/jni/FlowerFinderBaseJni.cpp"
        "${FFB_ROOT}/jni/com_sunnyslopes_flowerfarmfinder_FlowerFarmFinderBridge.h")
    target_link_libraries(FlowerFinderBaseJni PRIVATE flower_core)
    target_include_directories(FlowerFinderBaseJni PRIVATE
        "${FFB_JNI_INCLUDE}" "${FFB_JNI_INCLUDE_MD}")
    target_compile_options(FlowerFinderBaseJni PRIVATE ${FFB_FAST_FLAGS})
    # JNI 契约（GUI 的 FlowerFarmFinderBridge 按这个名字加载）：
    #   libFlowerFarmFinderLibJ.dll
    set_target_properties(FlowerFinderBaseJni PROPERTIES
        OUTPUT_NAME "FlowerFarmFinderLibJ"
        PREFIX "lib"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    message(STATUS "FlowerFinderBase: JNI -> ${CMAKE_BINARY_DIR}/bin/libFlowerFarmFinderLibJ.dll")
  else()
    message(WARNING
      "JNI headers not found (looked under '${FFB_JAVA_HOME}/include'). "
      "Skipping libFlowerFarmFinderLibJ. Set -DFFB_JAVA_HOME=<jdk> to build it.")
  endif()
endif()

# --- 安装 / 打包 ------------------------------------------------------------
include(GNUInstallDirs)
if (FFB_BUILD_CLI)
  install(TARGETS FlowerFinderBase-cli
          RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
if (FFB_BUILD_JNI AND TARGET FlowerFinderBaseJni)
  install(TARGETS FlowerFinderBaseJni
          RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
install(FILES "${FFB_ROOT}/README.md"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(FILES "${FFB_ROOT}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}")
