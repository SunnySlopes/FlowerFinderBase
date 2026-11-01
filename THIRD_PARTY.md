# 第三方组件 / Third-party notices

## cubiomes

* Upstream: <https://github.com/xpple/cubiomes> (fork of <https://github.com/Cubitect/cubiomes>)
* License: MIT — Copyright (c) 2020 Cubitect
* Usage: 群系 / 噪声 / 版本解析（`getBiomeAt()`、`str2mc()`、`biomenoise`、`layers`）。
* 以 git 子模块形式引入（`cubiomes/`），未修改其源码；许可文本见 `cubiomes/LICENSE`。

本库只为 cubiomes 编入搜索实际用到的根目录源文件
（`generator.c`、`layers.c`、`biomenoise.c`、`biomes.c`、`noise.c`、`util.c`），
不编 `finders.c` / `carver.c` / `xradv.c` / `quadbase.c` / `terrainnoise.c` 与 `features/`。
