# Changelog

## 1.0.0 - 2026-08-09

首个公开版本：

- 在 Listary 下拉列表中按需查询本地 Chromium 浏览器历史记录。
- 支持 Chrome、Edge，以及通过系统注册信息和 `History.urls` 结构验证发现的兼容浏览器。
- 始终使用结果来源浏览器和 Profile 打开历史页面。
- 提供多浏览器批量配置、关键字设置、ICO 导出和 Listary 安全重启。
- 没有历史匹配时，把网址或搜索词直接交给来源浏览器地址栏。
- 提供当前用户图形安装器、开机启动、桌面快捷方式和干净卸载。
- 本机验证 Listary 6.3.5.94；Listary V7 已由实际用户验证兼容。
- 原生 C++20/Win32，无 Electron、WebView、Qt、Python 或 .NET 运行依赖。

已知限制：

- 安装器尚未进行 Authenticode 代码签名，Windows 可能提示未知发布者。
- Listary-only 空闲内存满足 15 MiB 目标；显示原生回退窗口后，受本机输入法模块影响会超过该目标。详见 `docs/RESOURCE_REPORT.md`。
