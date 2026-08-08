# Listary 原生结果集成

本机 Listary 6.3.5.94 可以把自定义网页搜索提示显示为 Listary 自己的下拉结果。本工具仅在 `127.0.0.1:32119` 提供按需建议接口；它不建立后台索引，也不持续扫描浏览器数据库。

选择结果时使用当前用户的 `bhl://` 协议把结果交回本工具，再由结果来源浏览器及 Profile 打开。这样不会经过 Windows 默认浏览器。

## 首次注册

保持 `BrowserHistoryLauncher.exe` 与 INI 在同一目录，执行：

```powershell
.\BrowserHistoryLauncher.exe --register-listary-protocol
```

注册位置为 `HKEY_CURRENT_USER\Software\Classes\bhl`，不需要管理员权限。移动 EXE 后需要从新位置重新执行注册。撤销时执行：

```powershell
.\BrowserHistoryLauncher.exe --unregister-listary-protocol
```

## Chrome 历史

在 Listary“选项 → 网页搜索”中新增：

- 关键字：`gh`
- 标题：`Chrome 历史`
- Url：`bhl://open?prefix=g&selection={query}`
- 搜索提示：`Custom`
- 自定义提示 URL：`http://127.0.0.1:32119/suggest?prefix=g&q={query}`
- 启用：勾选

输入 `gh github`，选择历史结果并按 Enter，即使用来源 Chrome Profile 打开。

## Edge 历史

在 Listary“选项 → 网页搜索”中新增：

- 关键字：`eh`
- 标题：`Edge 历史`
- Url：`bhl://open?prefix=e&selection={query}`
- 搜索提示：`Custom`
- 自定义提示 URL：`http://127.0.0.1:32119/suggest?prefix=e&q={query}`
- 启用：勾选

输入 `eh microsoft`，选择历史结果并按 Enter，即使用来源 Edge Profile 打开。

## 运行要求与边界

- 本工具必须保持运行，否则 Listary 无法访问本地建议接口，也没有本次查询的“显示项 → URL/Profile”临时映射。
- 可以将本工具快捷方式放入当前用户的“启动”目录；本项目不安装 Windows 服务，也不需要管理员权限。
- 建议映射只保存在内存中。工具重启后，需要重新在 Listary 输入查询再选择结果。
- 监听套接字只绑定 IPv4 loopback `127.0.0.1`，不接受局域网或公网连接。
- 普通 Listary“命令”模式和 `--query` 仍保留，作为原生窗口回退入口。
- 如果只使用 Listary，可把 INI 中的 `Hotkey` 设置为 `none`；Listary 自己的呼出快捷键不受此项影响。

参考：

- Listary 官方命令文档：https://help.listary.com/options-commands
- Listary V7 Browser history plugin 路线图：https://www.listary.com/v7
