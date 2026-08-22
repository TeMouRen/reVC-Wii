# Tools

这份仓库里当前还在使用、且建议保留的辅助脚本主要有下面这些：

## 资源与封装

- `prepare_gc_radio_assets.py`
  - 处理 / 复用当前控制台版本需要的广播资源输入。
- `wii/build_disc_assets.py`
  - 生成 Wii 光盘所需的 banner / icon / `opening.bnr` 相关资源。
- `..\pack_iso.ps1`
  - 把已经整理好的提取光盘目录重新封装成 Wii ISO。

## 运行与探针

- `wii/run_dolphin_probe.ps1`
  - 历史手动探针辅助脚本，会启动并关闭 Dolphin；自动化代理不得调用。
    交互式 Dolphin 与游戏内验证由用户执行。

## 使用建议

推荐顺序通常是：

1. 先把程序构建出新的 `main.dol`
2. 如有需要，更新 `wii/build_disc_assets.py` 生成的光盘资源
3. 把新的 `main.dol` 和资源放回提取目录
4. 用 `pack_iso.ps1` 重新封装 ISO
5. 再用 Dolphin 或实机验证

注意：

这些脚本默认都偏向当前这套本地工作流，而不是完全通用的“零配置发布工具”。

如果你准备在别的机器上复用，先检查脚本里的默认路径。
