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
- 10 万行隔离历史库 Unicode 子串查询：最终回归为 54.670 ms。
- Chrome 冷查询：270.872 ms，包含锁定原库检测和 12.3 MB 快照复制。
- Chrome 同会话快照复用查询：2.425 ms。
- Chrome 下拉端到端：94.511 ms。
- 查询期私有工作集：28,733,440 字节（27.40 MiB），低于 30 MiB 目标。

## 阶段二：配置驱动接入 Edge

- 接入改动：仅将已有 `[browser.edge]` 的 `Enabled` 改为 `true`；Chrome 与 Edge 使用同一 `ChromiumHistoryAdapter`。
- Release Rebuild：通过。
- Edge 正在运行时，`e` 查询下拉：20 条，通过。
- Edge 冷查询：353.706 ms，包含锁定原库检测和 63.8 MB 快照复制。
- Edge 同会话快照复用查询：5.464 ms。
- Chrome/Edge EXE 与配置文件发现：通过。
- 原生子进程捕获确认 `CreateProcessW` 正确传递 `--profile-directory` 和含空格、引号、特殊字符的 URL：通过。

## 资源与稳定性

| 项目 | 实测 | 目标/上限 | 结论 |
|---|---:|---:|---|
| Listary-only 启动、尚未查询 | 1,691,648 B（1.61 MiB） | 15/25 MiB | 通过 |
| Listary-only 完成 Chrome+Edge 查询 | 2,568,192 B（2.45 MiB） | 15/25 MiB | 通过 |
| 首次聚焦后再隐藏私有工作集 | 28,925,952 B（27.59 MiB） | 15/25 MiB | **未通过** |
| 搜索完成私有工作集 | 28,733,440 B（27.40 MiB） | 30/50 MiB | 通过 |
| 隐藏空闲 CPU | 0% | 接近 0% | 通过 |
| 温呼出 | 15.553 ms | 100 ms | 通过 |
| 100 次呼出/隐藏 | 28,839,936 B → 29,495,296 B | 增长不超过 1 MiB | 通过 |
| 10 万行查询 | 54.670 ms | 100 ms | 通过 |
| EXE + INI | 526,076 B | 10 MiB | 通过 |
| 正常退出残留进程 | 0 | 0 | 通过 |

## 已确认的未达标原因

首次让原生 Edit 获得输入焦点后，本机默认搜狗输入法把 `SogouTSF.ime`、`SogouPY.ime`、`textinputframework.dll`、DirectWrite/CoreUI 及多个输入法扩展加载进本进程，线程数从 5 增至 14，私有工作集增加约 26 MiB。隐藏窗口和释放查询对象后这些系统/输入法模块仍驻留。

销毁子控件不能让这些模块卸载，且会把温呼出推高到约 108 ms。微软文档说明 `ImmDisableTextFrameService` 在 Windows Vista 起已不可用，替代的 `ImmDisableIME` 会禁用输入法；本项目不能为了内存数字牺牲中文输入，也没有强制修剪工作集。因此当前版本诚实保留这一验收失败，测试程序会返回非零。

本机 Listary-only 启动驻留为 1.61 MiB，完成 Chrome+Edge 查询后为 2.45 MiB，说明应用自身常驻、配置、托盘、工作线程和系统 SQLite 没有构成 15 MiB 问题。后续若要让已经显示过原生窗口的模式也满足搜狗输入法环境下的 15 MiB 隐藏指标，需要在不破坏“复用原进程和窗口”与中文输入的前提下设计可卸载的输入隔离边界，不能用工作集修剪掩盖。

## 图标与 Listary 集成阶段

- 使用 AI 生成的项目专用图形，经色键去除生成透明 PNG，再打包为 16/20/24/32/40/48/64/128/256 px ICO。
- EXE、窗口类和托盘均使用嵌入图标：原生 UI 测试通过。
- 新增 `--query`：第二个短命令进程通过 `WM_COPYDATA` 把查询交给已有实例，不创建第二个常驻实例。
- 首次以 `--query g` 启动，以及向已有实例转发 `--query g` / `--query e`：均通过；Chrome/Edge 分别返回 20 条下拉结果。
- `Hotkey=none` 禁用原生全局快捷键：核心测试通过。
- 仅监听 `127.0.0.1:32119` 的原生建议服务按需查询 SQLite；Chrome `github` 返回 18 条、Edge `microsoft` 返回 9 条，中文 JSON 通过。
- Listary 6 的 `Custom` 网页搜索提示已真实显示 Chrome 历史结果。
- 当前用户 `bhl://` 协议把 Listary 选择转发给已有单实例；Windows 规范化 URI 回归通过，Chrome 结果固定由 Chrome 打开。
- 注册与注销均为显式命令，不需要管理员权限；协议命令当前指向便携版 EXE。
- 原生 Edit/ListView 改为首次显示时才创建：自动化回归的 Listary-only 启动私有工作集 1,691,648 B，完成 Chrome+Edge 查询后的独立实测为 2,568,192 B；未加载搜狗或 TextInput 模块，满足 15 MiB 空闲目标。
