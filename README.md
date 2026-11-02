# FlowerFinderBase

全花农场（Flower Farm）窗口搜索的**核心计算库**，从
[FlowerFinderGUI](https://github.com/SunnySlopes/FlowerFinderGUI) 中拆出。

本仓库只做计算，不含任何 Swing / UI 代码：

* CTS 气候过渡搜索（`cts/`）
* 花农场窗口的群系 / 花噪声校验、排序与去重（`src/FlowerFarmSearch.cpp`）
* cubiomes 群系/噪声（`cubiomes/` 子模块，<https://github.com/xpple/cubiomes>）

产出两个可执行物：

| 产物 | 用途 |
| --- | --- |
| `FlowerFinderBase-cli.exe` | 独立命令行，`--out` 输出 txt 报告，可脱离 GUI 使用 |
| `libFlowerFarmFinderLibJ.dll` | JNI 桥，供 FlowerFinderGUI 直接查询（`FlowerFarmFinderBridge`） |

---

## 目录结构

| 路径 | 内容 |
| --- | --- |
| `include/FlowerFarmSearch.h` | **公开契约**：`runFlowerFarmSearch()` / `runFlowerFarmSearchRows()`、`FlowerSearchControl`、`FlowerSearchOptions`、`FlowerRow` |
| `src/FlowerFarmSearch.cpp` | 搜索主流程（屏幕 → 候选窗口 → 校验 → 排序去重） |
| `cts/` | CTS 1.0.0 气候过渡搜索（`engine.c` / `climate.c` / `biome.c` / `core.h`） |
| `cxx_compat/pre_cubiomes.h` | cubiomes `rng.h` 的 `lerp()` 与 C++20 `std::lerp` 冲突规避（强制包含） |
| `cubiomes/` | cubiomes 子模块 |
| `jni/` | JNI 入口 `FlowerFinderBaseJni.cpp` + 机器生成头文件 + `build-jni.bat` |
| `cli/main.cpp` | 命令行入口 |
| `CMakeLists.txt` | 唯一的构建描述（CLI 与 JNI 共用同一份核心库） |
| `build.ps1` | 一键构建（MSYS2 UCRT64 + Ninja） |

## 主子模块契约

GUI（`FlowerFinderGUI`）只通过下面三层与本库交互，任何一层变化都算破坏性修改：

1. **JNI DLL**：文件名固定 `libFlowerFarmFinderLibJ.dll`，放在 jar 的
   `/native/<os>/` 下；GUI 侧类 `com.sunnyslopes.flowerfarmfinder.FlowerFarmFinderBridge`
   的 `static native` 方法签名不变：
   * `flowerSearch(seed, minX, minZ, maxX, maxZ, longSide, shortSide, squareOnly,
     frameStep, threads, bothWildflowers, basaltDeltas, orientation, flowerCenterMargin, mcVersion)`
     → `String[]`（每个元素即一行 `/tp ...`）
   * `getSearchProgress()` → `int[12]`
   * `pause()` / `resume()` / `stop()` → `boolean`
2. **C++ API**：`include/FlowerFarmSearch.h`。
   `FlowerSearchControl` 的字段顺序/语义、`FlowerRow` 的字段、`FlowerSearchOptions`
   的默认值都属于契约。JNI 头文件由 GUI 的 Java 类生成，改动签名时必须同步
   `jni/com_sunnyslopes_flowerfarmfinder_FlowerFarmFinderBridge.h`。
3. **命令行**：见下文参数表；`--out` 写出的 txt 与 GUI 的 `Export` 完全同格式。

GUI 的 `build.gradle` 通过 `FlowerFinderBase/jni/build-jni.bat` 构建 DLL 并打进 jar；
本仓库单独 clone 时也能独立构建 CLI 与 DLL，不依赖 GUI。

## 子模块初始化

```powershell
git clone https://github.com/SunnySlopes/FlowerFinderBase.git
cd FlowerFinderBase
git submodule update --init
```

## 构建

需要 CMake 3.20+、支持 C++20 的 GCC 与 Ninja（仓库在 MSYS2 UCRT64 GCC 16 上验证）。
`build.ps1` 会把 `C:\msys64\ucrt64\bin` 加进 PATH（可用 `-Msys2Root` 改）。

```powershell
# CLI + JNI（找到 JDK 时）
powershell -ExecutionPolicy Bypass -File .\build.ps1

# 只构 CLI
powershell -ExecutionPolicy Bypass -File .\build.ps1 -CliOnly

# 只构 GUI 用的 DLL（构建脚本会自己找带 include\jni.h 的 JDK）
powershell -ExecutionPolicy Bypass -File .\jni\build-jni.bat
```

产物在 `build\bin\`：

```text
build\bin\FlowerFinderBase-cli.exe
build\bin\libFlowerFarmFinderLibJ.dll
```

MinGW 下静态链接 `libgcc` / `libstdc++` / `libgomp` / `libwinpthread`，exe 与 dll
都不依赖 MSYS2 运行库（只依赖 Win10+ 自带 UCRT），可以直接分发或被 JVM 加载。

也可以手动用 CMake：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFFB_JAVA_HOME="C:\Program Files\Java\jdk-17"
cmake --build build
```

`-DFFB_BUILD_CLI=OFF` / `-DFFB_BUILD_JNI=OFF` 可裁剪产物；`-DFFB_JAVA_HOME` 指定 JDK。

### 打包分发

```powershell
cmake --install build --prefix dist
```

`dist\bin\` 里就是 `FlowerFinderBase-cli.exe`（+ DLL），单文件即可分发。

## 命令行用法

```text
FlowerFinderBase-cli --seed <seed> [options]
```

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--seed <int64>` | 必填 | 世界种子，可负 |
| `--min-x` / `--max-x` | `-16384` / `16383` | 搜索范围 X（含端点，方块坐标） |
| `--min-z` / `--max-z` | `-16384` / `16383` | 搜索范围 Z（含端点） |
| `--long-side <int>` | `640` | 窗口长边（256–1024） |
| `--short-side <int>` | `320` | 窗口短边 |
| `--square-only` | 关 | 正方形窗口（两边都用长边） |
| `--frame-step <int>` | `16` | CTS 帧步长；`4` = 全窗口验证，越大越快越粗 |
| `--fast` / `--precise` | — | `--frame-step 16` / `--frame-step 4` 的快捷方式 |
| `--both-wildflowers` | 关 | 只保留同时有 wildflowers（桦木/草甸）与 pink petals（樱花）的窗口 |
| `--basalt-deltas` | 关 | 只保留下界对应位置含玄武岩三角洲的窗口 |
| `--orientation <o>` | `any` | `any` / `horizontal` / `vertical` |
| `--flower-center-margin <int>` | `0` | 两处花梯度端点距窗口边缘的最小距离，`0` = 关闭 |
| `--threads <int>` | CPU 线程数 | 工作线程数（上限 256） |
| `--mc <version>` | `26.3` | cubiomes 版本：`26.3`（= UI 的 1.21.4+）、`1.21.4`、`1.21.3`、`1.18~1.21.3`、`legacy`、`latest` 或数字 id |
| `--out <file>` | 无 | 同时把 `/tp` 报告写入 txt |
| `--quiet` | 关 | 不打印进度与摘要（stderr），结果仍走 stdout |
| `--help` / `-h` | — | 帮助 |
| `--version` | — | 版本 |

输出即 GUI 里那些 `/tp` 行，一行一个窗口：

```text
/tp 36256 64 29248 side=320x640 flowerArea=2752 paleArea=3328 swampArea=8400 nether=nether_wastes bonus=wildflowers
```

`--out` 写出的文件内容与 stdout 一致；进度（准备 / 气候屏幕 / 合并 / 校验）与耗时摘要
走 stderr，`Ctrl+C` 会让搜索在下一个检查点停下并输出已找到的结果（退出码 130）。

### 示例

```powershell
# 单种子 + 131072 方块范围，快速模式，写报告
.\build\bin\FlowerFinderBase-cli.exe --seed 24 `
    --min-x -65536 --max-x 65535 --min-z -65536 --max-z 65535 `
    --threads 12 --out flower.txt

# 1.18~1.21.3（无苍白花园要求），只保留玄武岩三角洲对应的窗口
.\build\bin\FlowerFinderBase-cli.exe --seed 24 --mc "1.18~1.21.3" `
    --basalt-deltas --out flower_legacy.txt
```

## 版本与一致性

* 核心源码、编译参数（`-O3 -fwrapv -ffp-contract=off -fno-lto`、`-include pre_cubiomes.h`）
  与 All-In-One Finder 的 native 构建保持一致，同一套输入下结果**逐行一致**；
  `--out` 与 GUI 导出的文本可直接 diff。
* 移植/升级时必须保持 `FlowerSearchControl` 的字段顺序，否则相位/进度会错位。

## 许可

本库以 **MIT 许可证**发布，完整文本见 [LICENSE](LICENSE)；主仓库
[FlowerFinderGUI](https://github.com/SunnySlopes/FlowerFinderGUI) 使用同一许可证，
因此 GUI、核心库、CLI 与 JNI 桥可以自由使用、修改与再分发（含商业用途）。

```text
Copyright (c) 2026 SunnySlopes & M33 Galaxy
```

## 第三方

见 [THIRD_PARTY.md](THIRD_PARTY.md)。cubiomes 以子模块形式引入，保留其上游 MIT 许可
（Copyright (c) 2020 Cubitect），不属于本库的版权声明范围。
