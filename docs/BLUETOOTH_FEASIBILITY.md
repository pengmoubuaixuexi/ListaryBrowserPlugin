# 蓝牙连接可行性记录

日期：2026-08-13
范围：Windows Core Audio、设备拓扑和蓝牙音频驱动接口；不使用未公开 Shell 接口，不通过设备禁用/启用伪装连接。

## 已验证环境

- Windows 11 x64，Windows SDK 10.0.22621。
- 原生 C++20/Win32 + C++/WinRT。
- `DeviceInformationKind::AssociationEndpoint`。
- Classic：`BluetoothDevice::GetDeviceSelectorFromPairingState(true)`。
- LE：`BluetoothLEDevice::GetDeviceSelectorFromPairingState(true)`。

## 真实枚举结果

冷枚举约 58 ms，物理设备去重后恰好三项：

| 设备 | 传输 | 枚举状态 |
|---|---|---|
| AirPods | Classic | 已配对、存在、未连接 |
| MINOR III | Classic | 已配对、存在、未连接 |
| BT5.4 Mouse | Low Energy | 已配对、存在、未连接 |

完整蓝牙地址和设备 ID 只在 worker 与宿主的临时文件中使用，不显示在 Listary 或普通日志中；临时文件使用后删除。

## 真实连接结论

- 已删除 `FromIdAsync` + RFCOMM/GATT 服务查询充当连接动作的旧实现。该调用只能产生服务访问会话，不能建立可靠的 A2DP 音频连接。
- 当前实现枚举与物理蓝牙设备对应的 Windows 音频端点，通过设备拓扑取得蓝牙驱动的 `IKsControl`，发送 `KSPROPSETID_BtAudio / KSPROPERTY_ONESHOT_RECONNECT` 重连请求。
- AirPods：从 `IsConnected=false` 发起驱动重连，实测随后变为 `IsConnected=true`，证明该请求是真实连接动作而不是探针。
- MINOR III：使用相同音频驱动路径，尚未在本次测试中主动断开后验证。
- BT5.4 Mouse：HID 设备没有音频驱动连接端点，程序会明确提示必须从设备端唤醒，不会报告成功。

正式实现区分“动作结果”和“设备状态”：

1. worker 向目标物理设备对应的蓝牙音频驱动发送真实重连请求；
2. `IKsControl::KsProperty` 成功即返回“已发送连接请求”，失败则返回驱动错误；
3. 不为弹窗等待或轮询最终状态，下次正常的 Listary 设备枚举自然显示最新连接状态。

`IKsControl::KsProperty` 的返回值只表示蓝牙音频驱动接受或拒绝重连请求，不携带设备最终连接结果。Windows 会异步完成连接；由于 AirPods 上已经验证该驱动请求能够真实建立连接，交互层不再等待最终状态，也不会把“请求已发送”写成“已经连接”。

当前结论是：电脑可主动连接已配对且安装了 Windows 音频端点的经典蓝牙耳机；HID 和其他非音频 Profile 不具备相同的主机侧通用连接动作。程序会返回真实失败或唤醒提示，不会把短暂 GATT/RFCOMM 会话称为成功。

## 可重复命令

只测试枚举与 Listary 建议：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\test-bluetooth.ps1
```

额外对指定音频设备执行真实连接请求：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\test-bluetooth.ps1 -ConnectName 'AirPods'
```
