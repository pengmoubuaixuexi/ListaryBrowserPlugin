# Listary 集成

本机 Listary 6.3.5.94 支持“命令”把关键字后的输入作为 `"{query}"` 传给外部程序。Browser History Launcher 提供 `--query` 参数，并通过单实例消息把查询转发给已经运行的进程。

## 推荐配置：两个短命令

在 Listary 托盘菜单中打开“选项 → 命令”，分别新增：

### Chrome 历史

- 关键字：`gh`（如果确认没有命令冲突，也可以使用 `g`）
- 标题：`Chrome 历史`
- 路径：`E:\Program Files (x86)\BrowserHistoryLauncher\build\portable\BrowserHistoryLauncher.exe`
- 参数：`--query "g {query}"`
- 工作目录：`E:\Program Files (x86)\BrowserHistoryLauncher\build\portable`
- 静默：不勾选
- 管理员：不勾选

之后在 Listary 输入 `gh github` 并执行，即会把 `g github` 传给本工具。

### Edge 历史

- 关键字：`eh`（如果确认没有命令冲突，也可以使用 `e`）
- 标题：`Edge 历史`
- 路径：与上面相同
- 参数：`--query "e {query}"`
- 工作目录：与上面相同
- 静默：不勾选
- 管理员：不勾选

## 备选配置：单一命令

- 关键字：`bh`
- 参数：`--query "{query}"`

使用时输入 `bh g github` 或 `bh e baidu`。

如果以后只通过 Listary 呼出，可把 `BrowserHistoryLauncher.ini` 中的 `Hotkey` 改为 `none`，然后重启本工具，避免保留额外的全局快捷键。

## 当前边界

Listary 的官方命令接口只能启动程序并传入 `"{query}"`，不能读取子进程标准输出或接收自定义结果数组。因此当前流程是：

```text
Listary 输入 → --query 参数 → 本工具查询 → 本工具原生下拉框显示
```

不能可靠实现：

```text
Listary 输入 → 浏览器历史直接成为 Listary 自身的结果行
```

Listary 6 安装目录中的 `FileAppPlugin` 用于文件管理器/文件对话框集成，并非搜索结果提供器。旧版论坛存在未公开的 JavaScript 插件接口，但没有当前官方 SDK、稳定性或兼容性保证。Listary V7 在 2026 年 7 月的公开路线图中仍把 Browser history plugin 列为后续计划，因此本项目不依赖这些内部接口。

参考：

- Listary 官方命令文档：https://help.listary.com/options-commands
- Listary V7 Browser history plugin 路线图：https://discussion.listary.com/t/listary-v7-beta-is-here-the-launcher-now-recommends-plus-a-new-engine-multi-select-fresh-themes-updated-to-7-0-0-7-on-july-20/10259
