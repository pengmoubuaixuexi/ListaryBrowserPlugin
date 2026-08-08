# Release 实测报告

测量日期：2026-08-08

## 环境与口径

- Windows 11 家庭中文版 10.0.22621，x64。
- MSVC 19.38.33133，Release x64，C++20，静态 CRT。
- 系统 `winsqlite3.dll` 3.34.1。
- Chrome 151.0.7922.76；历史库 12,255,232 字节，`Default`。
- Edge 151.0.4129.72；历史库 63,766,528 字节，`Profile 1`。
- 私有工作集由原生测试使用 `QueryWorkingSet` 逐页统计，并用性能计数器 `Process/Working Set - Private` 交叉验证。
- CPU 由两次 `GetProcessTimes` 差值按 1 秒与逻辑处理器数归一化。
- 温呼出从发送显示消息到窗口可见；端到端查询含 90 ms 防抖、必要的冷快照复制、SQLite 查询与 UI 更新。
- 没有调用 `SetProcessWorkingSetSize`、`EmptyWorkingSet` 或其他强制修剪 API。

## 阶段一：Chrome 最小垂直链路

- Release Rebuild：通过。
- Chrome 正在运行时，`g` 查询下拉：20 条，通过。
- 10 万行隔离历史库 Unicode 子串查询：最终回归为 17.976 ms。
- Chrome 冷查询：266.808 ms，包含锁定原库检测和 12.3 MB 快照复制。
- Chrome 同会话快照复用查询：2.328 ms。
- Chrome 下拉端到端：400.695 ms。
- 查询期私有工作集：27,385,856 字节（26.12 MiB），低于 30 MiB 目标。

## 阶段二：配置驱动接入 Edge

- 接入改动：仅将已有 `[browser.edge]` 的 `Enabled` 改为 `true`；Chrome 与 Edge 使用同一 `ChromiumHistoryAdapter`。
- Release Rebuild：通过。
- Edge 正在运行时，`e` 查询下拉：20 条，通过。
- Edge 冷查询：299.506 ms，包含锁定原库检测和 63.8 MB 快照复制。
- Edge 同会话快照复用查询：3.388 ms。
- Chrome/Edge EXE 与配置文件发现：通过。
- 原生子进程捕获确认 `CreateProcessW` 正确传递 `--profile-directory` 和含空格、引号、特殊字符的 URL：通过。

## 资源与稳定性

| 项目 | 实测 | 目标/上限 | 结论 |
|---|---:|---:|---|
| 从未显示时隐藏私有工作集 | 约 1.62 MiB | 15/25 MiB | 通过 |
| 首次聚焦后再隐藏私有工作集 | 27,140,096 B（25.88 MiB） | 15/25 MiB | **未通过** |
| 搜索完成私有工作集 | 27,385,856 B（26.12 MiB） | 30/50 MiB | 通过 |
| 隐藏空闲 CPU | 0% | 接近 0% | 通过 |
| 温呼出 | 15.976 ms | 100 ms | 通过 |
| 100 次呼出/隐藏 | 27,488,256 B → 27,488,256 B | 不持续增长 | 通过 |
| 10 万行查询 | 17.976 ms | 100 ms | 通过 |
| EXE + INI | 408,828 B | 10 MiB | 通过 |
| 正常退出残留进程 | 0 | 0 | 通过 |

## 已确认的未达标原因

首次让原生 Edit 获得输入焦点后，本机默认搜狗输入法把 `SogouTSF.ime`、`SogouPY.ime`、`textinputframework.dll`、DirectWrite/CoreUI 及多个输入法扩展加载进本进程，线程数从 5 增至 14，私有工作集增加约 26 MiB。隐藏窗口和释放查询对象后这些系统/输入法模块仍驻留。

销毁子控件不能让这些模块卸载，且会把温呼出推高到约 108 ms。微软文档说明 `ImmDisableTextFrameService` 在 Windows Vista 起已不可用，替代的 `ImmDisableIME` 会禁用输入法；本项目不能为了内存数字牺牲中文输入，也没有强制修剪工作集。因此当前版本诚实保留这一验收失败，测试程序会返回非零。

本机从未显示窗口时的驻留基线为约 1.62 MiB，说明应用自身常驻、配置、托盘、工作线程和系统 SQLite 没有构成 15 MiB 问题。后续若要同时满足搜狗输入法环境下的 15 MiB 隐藏指标，需要在不破坏“复用原进程和窗口”与中文输入的前提下设计可卸载的输入隔离边界，不能用工作集修剪掩盖。
