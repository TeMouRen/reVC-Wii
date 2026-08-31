# reVC-Wii

[English](README.en.md) | 简体中文

这是一个为 `reVC` 整理的、以 Nintendo Wii 为主目标的精简上传版本。

它继承了此前 GameCube GX 分支已经验证过的 PowerPC、大端序和 GX 渲染路径。Wii 原生目标、MEM2 分区和 GX 驻留账本已经落地；当前工作集中在流送验证、静态纹理内存优化、渲染兼容和资产构建。

## 仓库定位

- 当前主目标平台：Nintendo Wii
- 保留对照平台：Nintendo GameCube
- 当前渲染后端：`vendor/librw/` 下的 GX 后端
- 当前代码基线：`reVC` + 已落地的 GC 移植工作 + 当前 Wii 迁移工作

## 文档索引

- `README.md` - 中文总览
- `README.en.md` - 英文总览
- `docs/README-WII-MIGRATION.md` - 当前 Wii 文档入口
- `docs/wii-memory-streaming-performance-analysis.md` - Wii 内存、流送与性能参考
- `docs/PS2_WII_STREAMING_HANDOFF.md` - PS2 对照证据与已实现的驻留账本记录
- `docs/WII_STATIC_TEXTURE_OPTIMIZATION_PLAN.md` - 当前静态纹理优化方向
- `docs/wii-gx-rendering-lessons.md` - 当前 GX 渲染与 alpha 经验
- `docs/WII_ISO打包与提取.md` - Wii 提取 / 回封装说明
- `tools/README.md` - 当前仓库内工具说明
- `vendor/librw/README.md` - vendored librw 上游说明

## 这份副本保留了什么

- `src/` 下当前有效的 GX / PowerPC / Wii 相关移植代码
- `src/skel/wii/` 下的 Wii 启动、兼容和诊断代码
- `src/skel/gamecube/` 下仍然有参考价值的 GC 控制台路径代码
- 为当前控制台构建保留的 `vendor/librw/`
- `cmake/Wii.cmake` 与 `cmake/GameCube.cmake`
- `build.sh`、`build.ps1`、`pack_iso.ps1`
- `tools/wii/` 下当前仍在用的 Wii 资源 / 探针脚本

## 这份副本为什么没有完全删光 GC 痕迹

需要注意：

`GAMECUBE` 宏、`src/skel/gamecube/` 目录，以及 `vendor/librw/src/` 里部分名字看起来像其他平台的文件，并不都代表“这个仓库还在做多平台完整支持”。

它们现在留下来的主要原因有两个：

- 当前 Wii 版本仍然复用了部分 GC 控制台路径和大端序前提
- librw 的公共代码在编译时仍会引用一些平台公共文件

当前真正的主目标仍然是 Wii + GX。

## 构建环境要求

这份仓库默认要求本机存在 Windows 版 `devkitPro` 环境：

1. `C:\devkitPro`
2. `C:\devkitPro\devkitPPC`
3. `C:\devkitPro\libogc2\include`
4. `C:\devkitPro\libogc2\lib\wii`
5. `C:\devkitPro\tools\bin\elf2dol.exe`
6. `C:\devkitPro\msys2\usr\bin\bash.exe`
7. `C:\devkitPro\msys2\usr\bin\cmake.exe`
8. `C:\devkitPro\msys2\usr\bin\ninja.exe`

`build.sh` 直接使用这些路径。`build.ps1` 只是无参数兼容包装，最终仍执行 `bash ./build.sh`。
默认构建关闭特殊角色流送、流送内存驻留和大建筑 LOD 诊断，避免正常运行刷屏；需要排查时可分别显式设置 `WII_SPECIAL_STREAM_DIAGNOSTICS=ON`、`WII_STREAM_MEMORY_DIAGNOSTICS=ON` 或 `WII_STREAM_BIG_BUILDING_PROBE=ON`。事件级调试日志保持关闭。

## 快速构建

标准 Wii 构建只使用：

```bash
bash ./build.sh
```

在 PowerShell 中可以无参数运行 `.\build.ps1`，它只会转发到同一命令；自定义参数会被拒绝。

## 输出文件

构建成功后，主要产物是：

- `build\src\reVC`
- `build\src\main.dol`
- `F:\Wii Work\提取测试\reVC\sys\main.dol`

`build.sh` 会把 `main.dol` 部署到上面的固定 extracted-disc `sys` 目录。

## 运行与资源说明

- 这份仓库不包含游戏资源。
- 如果你要本地运行或封装 ISO，请从你自己的合法来源准备资源。
- 当前常见的工作目录形态是一个提取后的光盘目录，例如：
  - `reVC/sys`
  - `reVC/files`
- 仅更新程序时，通常只需要替换 `sys/main.dol`。

## ISO 打包

仓库根目录提供了：

- `pack_iso.ps1`

示例：

```powershell
.\pack_iso.ps1 -SourceDir "F:\path\to\extracted\reVC" -OutputIso ".\out\reVC-wii.iso"
```

默认依赖：

- `C:\Program Files\Wiimm\WIT\wit.exe`

更完整的提取与回封装说明见：

- `docs/WII_ISO打包与提取.md`

## 工具说明

当前仓库里仍保留并建议继续使用的工具主要在：

- `tools/`

总览说明见：

- `tools/README.md`

## 当前状态

这份上传版的重点不是“已经做完的最终 Wii 发布版”，而是“当前可继续推进的 Wii 基线”。

当前已落地：

- 保留 GX 渲染主线
- Wii 原生构建入口和 `src/skel/wii/`
- MEM2 generic/newlib/GX 分区与驻留归因日志
- `bash ./build.sh` 的固定构建、DOL 转换和部署流程

当前推进方向：

- 用 run-specific 日志验证高速移动下的流送和驻留压力
- 通过离线逐纹理格式选择降低 GX 驻留，而不改 admission/淘汰策略
- 保持 MatFX、材质查找和资产 fallback 行为可验证

如果你接手继续做，建议先从 `docs/README-WII-MIGRATION.md` 开始读。
