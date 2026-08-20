#include "bluetooth_worker.h"

#include "browser_launcher.h"
#include "json_document.h"
#include "text_util.h"

#include <Windows.h>
#include <Shellapi.h>
#include <Psapi.h>
#include <TlHelp32.h>

#include <devicetopology.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propsys.h>

#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <thread>

namespace {
using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Enumeration;
using namespace Windows::Foundation;

constexpr wchar_t kEnumerateSwitch[] = L"--bluetooth-worker-enumerate";
constexpr wchar_t kConnectSwitch[] = L"--bluetooth-worker-connect";
constexpr wchar_t kDiagnosticConnectSwitch[] = L"--bluetooth-diagnostic-connect";

struct WorkerMetrics {
    std::uint64_t privateWorkingSetBytes = 0;
    std::uint64_t privateBytes = 0;
    unsigned threads = 0;
    unsigned handles = 0;
};

WorkerMetrics CaptureWorkerMetrics() {
    WorkerMetrics result;
    PROCESS_MEMORY_COUNTERS_EX2 memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
        result.privateBytes = memory.PrivateUsage;
    }
    std::vector<unsigned char> buffer(1024 * 1024);
    for (int attempt = 0; attempt < 8; ++attempt) {
        auto* information = reinterpret_cast<PSAPI_WORKING_SET_INFORMATION*>(buffer.data());
        if (QueryWorkingSet(GetCurrentProcess(), information, static_cast<DWORD>(buffer.size()))) {
            SYSTEM_INFO system{};
            GetSystemInfo(&system);
            for (ULONG_PTR i = 0; i < information->NumberOfEntries; ++i) {
                if (!information->WorkingSetInfo[i].Shared) result.privateWorkingSetBytes += system.dwPageSize;
            }
            break;
        }
        if (GetLastError() != ERROR_BAD_LENGTH) break;
        buffer.resize(buffer.size() * 2);
    }
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles)) result.handles = handles;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{sizeof(entry)};
        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID == GetCurrentProcessId()) ++result.threads;
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    return result;
}

std::filesystem::path TemporaryFile() {
    wchar_t directory[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, directory)) return {};
    wchar_t file[MAX_PATH]{};
    if (!GetTempFileNameW(directory, L"bhl", 0, file)) return {};
    return file;
}

bool WriteBytes(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

std::string ReadBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::wstring StringProperty(const DeviceInformation& device, std::wstring_view name) {
    try {
        const auto properties = device.Properties();
        const hstring key(name);
        if (!properties.HasKey(key)) return {};
        const auto property = properties.Lookup(key).try_as<IPropertyValue>();
        if (!property) return {};
        if (property.Type() == PropertyType::String) return property.GetString().c_str();
        if (property.Type() == PropertyType::Guid) return to_hstring(property.GetGuid()).c_str();
    } catch (...) {
    }
    return {};
}

bool BooleanProperty(const DeviceInformation& device, std::wstring_view name, bool fallback) {
    try {
        const auto properties = device.Properties();
        const hstring key(name);
        if (!properties.HasKey(key)) return fallback;
        const auto property = properties.Lookup(key).try_as<IPropertyValue>();
        return property && property.Type() == PropertyType::Boolean ? property.GetBoolean() : fallback;
    } catch (...) {
        return fallback;
    }
}

std::wstring NormalizeAddress(std::wstring value) {
    std::erase_if(value, [](wchar_t ch) { return ch == L':' || ch == L'-' || ch == L' '; });
    return ToLowerInvariant(value);
}

void AddEndpoint(std::map<std::wstring, BluetoothDeviceTarget>& merged,
    const DeviceInformation& device, BluetoothTransport transport) {
    if (!BooleanProperty(device, L"System.Devices.Aep.IsPaired", true)) return;
    const std::wstring container = ToLowerInvariant(StringProperty(device, L"System.Devices.ContainerId"));
    const std::wstring address = NormalizeAddress(StringProperty(device, L"System.Devices.Aep.DeviceAddress"));
    const std::wstring id = device.Id().c_str();
    std::wstring key = !container.empty() ? L"container:" + container :
        (!address.empty() ? L"address:" + address : L"id:" + ToLowerInvariant(id));
    if (key.empty()) return;

    auto [found, inserted] = merged.try_emplace(key);
    auto& target = found->second;
    if (inserted) {
        target.stableKey = key;
        target.displayName = device.Name().c_str();
        target.transport = transport;
    } else if (target.transport != transport) {
        target.transport = BluetoothTransport::DualMode;
    }
    if (target.displayName.empty() && !device.Name().empty()) target.displayName = device.Name().c_str();
    if (transport == BluetoothTransport::Classic) target.classicDeviceId = id;
    else target.lowEnergyDeviceId = id;
    target.connected = target.connected || BooleanProperty(device, L"System.Devices.Aep.IsConnected", false);
    target.present = target.present || BooleanProperty(device, L"System.Devices.Aep.IsPresent", true);
}

BluetoothEnumerationResult EnumerateCore() {
    BluetoothEnumerationResult result;
    const auto started = std::chrono::steady_clock::now();
    try {
        init_apartment(apartment_type::multi_threaded);
        const auto properties = single_threaded_vector<hstring>({
            L"System.Devices.Aep.IsPaired",
            L"System.Devices.Aep.IsConnected",
            L"System.Devices.Aep.IsPresent",
            L"System.Devices.Aep.DeviceAddress",
            L"System.Devices.ContainerId"});
        std::map<std::wstring, BluetoothDeviceTarget> merged;
        const auto classic = DeviceInformation::FindAllAsync(
            BluetoothDevice::GetDeviceSelectorFromPairingState(true), properties,
            DeviceInformationKind::AssociationEndpoint).get();
        for (const auto& device : classic) AddEndpoint(merged, device, BluetoothTransport::Classic);
        const auto lowEnergy = DeviceInformation::FindAllAsync(
            BluetoothLEDevice::GetDeviceSelectorFromPairingState(true), properties,
            DeviceInformationKind::AssociationEndpoint).get();
        for (const auto& device : lowEnergy) AddEndpoint(merged, device, BluetoothTransport::LowEnergy);
        for (auto& [key, target] : merged) {
            if (target.displayName.empty()) target.displayName = L"未知蓝牙设备";
            result.devices.push_back(std::move(target));
        }
        std::sort(result.devices.begin(), result.devices.end(), [](const auto& left, const auto& right) {
            if (left.connected != right.connected) return left.connected > right.connected;
            return CompareStringOrdinal(left.displayName.c_str(), -1, right.displayName.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        });
    } catch (const hresult_error& exception) {
        result.error = L"蓝牙设备枚举失败：" + std::wstring(exception.message().c_str());
    } catch (...) {
        result.error = L"蓝牙设备枚举发生未知错误。";
    }
    result.elapsedMilliseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count());
    const auto metrics = CaptureWorkerMetrics();
    result.workerPrivateWorkingSetBytes = metrics.privateWorkingSetBytes;
    result.workerPrivateBytes = metrics.privateBytes;
    result.workerThreads = metrics.threads;
    result.workerHandles = metrics.handles;
    return result;
}

struct BluetoothAudioControlResult {
    bool audioEndpointFound = false;
    bool audioEndpointActive = false;
    unsigned reconnectRequestsAccepted = 0;
    HRESULT lastReconnectError = S_OK;
};

bool TryGetContainerId(const BluetoothDeviceTarget& target, GUID& containerId) {
    constexpr std::wstring_view prefix = L"container:";
    if (!target.stableKey.starts_with(prefix)) return false;
    const std::wstring text = target.stableKey.substr(prefix.size());
    return SUCCEEDED(CLSIDFromString(text.c_str(), &containerId));
}

bool IsBluetoothTopologyId(std::wstring_view deviceId) {
    const std::wstring lower = ToLowerInvariant(deviceId);
    return lower.starts_with(L"{2}.\\\\?\\bth");
}

BluetoothAudioControlResult QueryBluetoothAudioControls(const BluetoothDeviceTarget& target, bool reconnect) {
    BluetoothAudioControlResult result;
    GUID targetContainer{};
    const bool hasTargetContainer = TryGetContainerId(target, targetContainer);

    com_ptr<IMMDeviceEnumerator> enumerator;
    check_hresult(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), enumerator.put_void()));
    com_ptr<IMMDeviceCollection> endpoints;
    check_hresult(enumerator->EnumAudioEndpoints(eAll, DEVICE_STATEMASK_ALL, endpoints.put()));

    UINT count = 0;
    check_hresult(endpoints->GetCount(&count));
    std::set<std::wstring> requestedTopologyIds;
    for (UINT endpointIndex = 0; endpointIndex < count; ++endpointIndex) {
        com_ptr<IMMDevice> endpoint;
        if (FAILED(endpoints->Item(endpointIndex, endpoint.put()))) continue;

        com_ptr<IPropertyStore> properties;
        if (FAILED(endpoint->OpenPropertyStore(STGM_READ, properties.put()))) continue;
        PROPVARIANT containerProperty{};
        PropVariantInit(&containerProperty);
        const HRESULT propertyResult = properties->GetValue(PKEY_Device_ContainerId, &containerProperty);
        const bool sameContainer = hasTargetContainer && SUCCEEDED(propertyResult) && containerProperty.vt == VT_CLSID &&
            containerProperty.puuid && IsEqualGUID(*containerProperty.puuid, targetContainer);
        PropVariantClear(&containerProperty);
        bool sameFriendlyName = false;
        if (!hasTargetContainer && !target.displayName.empty()) {
            PROPVARIANT nameProperty{};
            PropVariantInit(&nameProperty);
            if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &nameProperty)) &&
                    nameProperty.vt == VT_LPWSTR && nameProperty.pwszVal) {
                sameFriendlyName = ToLowerInvariant(nameProperty.pwszVal).find(
                    ToLowerInvariant(target.displayName)) != std::wstring::npos;
            }
            PropVariantClear(&nameProperty);
        }
        if (!sameContainer && !sameFriendlyName) continue;

        result.audioEndpointFound = true;
        DWORD state = 0;
        if (SUCCEEDED(endpoint->GetState(&state)) && state == DEVICE_STATE_ACTIVE) {
            result.audioEndpointActive = true;
        }
        if (!reconnect) continue;

        com_ptr<IDeviceTopology> topology;
        if (FAILED(endpoint->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, topology.put_void()))) continue;
        UINT connectorCount = 0;
        if (FAILED(topology->GetConnectorCount(&connectorCount))) continue;
        for (UINT connectorIndex = 0; connectorIndex < connectorCount; ++connectorIndex) {
            com_ptr<IConnector> connector;
            if (FAILED(topology->GetConnector(connectorIndex, connector.put()))) continue;
            com_ptr<IConnector> connectedConnector;
            if (FAILED(connector->GetConnectedTo(connectedConnector.put())) || !connectedConnector) continue;
            com_ptr<IPart> connectedPart;
            if (FAILED(connectedConnector->QueryInterface(__uuidof(IPart), connectedPart.put_void()))) continue;
            com_ptr<IDeviceTopology> connectedTopology;
            if (FAILED(connectedPart->GetTopologyObject(connectedTopology.put()))) continue;
            LPWSTR topologyIdRaw = nullptr;
            if (FAILED(connectedTopology->GetDeviceId(&topologyIdRaw)) || !topologyIdRaw) continue;
            const std::wstring topologyId(topologyIdRaw);
            CoTaskMemFree(topologyIdRaw);
            if (!IsBluetoothTopologyId(topologyId) ||
                    !requestedTopologyIds.insert(ToLowerInvariant(topologyId)).second) {
                continue;
            }

            com_ptr<IMMDevice> topologyDevice;
            if (FAILED(enumerator->GetDevice(topologyId.c_str(), topologyDevice.put()))) continue;
            com_ptr<IKsControl> control;
            if (FAILED(topologyDevice->Activate(__uuidof(IKsControl), CLSCTX_ALL, nullptr, control.put_void()))) continue;
            KSPROPERTY property{};
            property.Set = KSPROPSETID_BtAudio;
            property.Id = KSPROPERTY_ONESHOT_RECONNECT;
            property.Flags = KSPROPERTY_TYPE_GET;
            ULONG bytesReturned = 0;
            const HRESULT requestResult = control->KsProperty(
                &property, sizeof(property), nullptr, 0, &bytesReturned);
            if (SUCCEEDED(requestResult)) {
                ++result.reconnectRequestsAccepted;
            } else {
                result.lastReconnectError = requestResult;
            }
        }
    }
    return result;
}

BluetoothConnectionResult ConnectCore(const BluetoothDeviceTarget& target, unsigned long timeoutMs) {
    BluetoothConnectionResult result;
    result.attempted = true;
    (void)timeoutMs;
    const auto started = std::chrono::steady_clock::now();
    try {
        init_apartment(apartment_type::multi_threaded);
        auto audio = QueryBluetoothAudioControls(target, true);
        const bool reconnectRequested = audio.reconnectRequestsAccepted != 0;
        result.requestAccepted = reconnectRequested;
        if (!audio.audioEndpointFound) {
            result.message = target.displayName +
                L" 没有可由电脑主动连接的蓝牙音频端点。鼠标、键盘等 HID 设备需要从设备端唤醒连接。";
        } else if (audio.audioEndpointActive) {
            result.confirmedConnected = true;
            result.message = target.displayName + L" 的 Windows 音频端点已经处于活动连接状态。";
        } else if (!audio.reconnectRequestsAccepted) {
            result.message = L"Windows 蓝牙音频驱动拒绝连接 " + target.displayName;
            if (FAILED(audio.lastReconnectError)) {
                result.message += L"：" + FormatWindowsError(HRESULT_CODE(audio.lastReconnectError));
            } else {
                result.message += L"。";
            }
        }
        if (result.confirmedConnected) {
            result.message = target.displayName + L" 已经连接。";
        } else if (reconnectRequested) {
            result.message = L"已向 Windows 蓝牙音频驱动发送连接 " + target.displayName + L" 的请求。";
        }
    } catch (const hresult_error& exception) {
        result.message = L"连接 " + target.displayName + L" 失败：" + exception.message().c_str();
    } catch (...) {
        result.message = L"连接 " + target.displayName + L" 时发生未知错误。";
    }
    result.elapsedMilliseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count());
    const auto metrics = CaptureWorkerMetrics();
    result.workerPrivateWorkingSetBytes = metrics.privateWorkingSetBytes;
    result.workerPrivateBytes = metrics.privateBytes;
    return result;
}

JsonValue DeviceJson(const BluetoothDeviceTarget& device) {
    JsonValue value = JsonValue::ObjectValue();
    value.Set(L"stableKey", JsonValue::String(device.stableKey));
    value.Set(L"name", JsonValue::String(device.displayName));
    value.Set(L"classicId", JsonValue::String(device.classicDeviceId));
    value.Set(L"lowEnergyId", JsonValue::String(device.lowEnergyDeviceId));
    value.Set(L"transport", JsonValue::Number(std::to_wstring(static_cast<int>(device.transport))));
    value.Set(L"connected", JsonValue::Boolean(device.connected));
    value.Set(L"present", JsonValue::Boolean(device.present));
    return value;
}

BluetoothDeviceTarget ParseDevice(const JsonValue& value) {
    BluetoothDeviceTarget device;
    const auto text = [&](std::wstring_view name) {
        const auto* item = value.Find(name);
        return item && item->type() == JsonValue::Type::String ? item->text() : std::wstring{};
    };
    const auto boolean = [&](std::wstring_view name) {
        const auto* item = value.Find(name);
        return item && item->type() == JsonValue::Type::Boolean && item->boolean();
    };
    device.stableKey = text(L"stableKey");
    device.displayName = text(L"name");
    device.classicDeviceId = text(L"classicId");
    device.lowEnergyDeviceId = text(L"lowEnergyId");
    if (const auto* item = value.Find(L"transport"); item && item->type() == JsonValue::Type::Number) {
        device.transport = static_cast<BluetoothTransport>(_wtoi(item->text().c_str()));
    }
    device.connected = boolean(L"connected");
    device.present = boolean(L"present");
    return device;
}

JsonValue EnumerationJson(const BluetoothEnumerationResult& result) {
    JsonValue root = JsonValue::ObjectValue();
    root.Set(L"error", JsonValue::String(result.error));
    root.Set(L"elapsedMs", JsonValue::Number(std::to_wstring(result.elapsedMilliseconds)));
    root.Set(L"privateWorkingSetBytes", JsonValue::Number(std::to_wstring(result.workerPrivateWorkingSetBytes)));
    root.Set(L"privateBytes", JsonValue::Number(std::to_wstring(result.workerPrivateBytes)));
    root.Set(L"threads", JsonValue::Number(std::to_wstring(result.workerThreads)));
    root.Set(L"handles", JsonValue::Number(std::to_wstring(result.workerHandles)));
    JsonValue devices = JsonValue::ArrayValue();
    for (const auto& device : result.devices) devices.array().push_back(DeviceJson(device));
    root.Set(L"devices", std::move(devices));
    return root;
}

JsonValue ConnectionJson(const BluetoothConnectionResult& result) {
    JsonValue root = JsonValue::ObjectValue();
    root.Set(L"attempted", JsonValue::Boolean(result.attempted));
    root.Set(L"requestAccepted", JsonValue::Boolean(result.requestAccepted));
    root.Set(L"connected", JsonValue::Boolean(result.confirmedConnected));
    root.Set(L"message", JsonValue::String(result.message));
    root.Set(L"elapsedMs", JsonValue::Number(std::to_wstring(result.elapsedMilliseconds)));
    root.Set(L"privateWorkingSetBytes", JsonValue::Number(std::to_wstring(result.workerPrivateWorkingSetBytes)));
    root.Set(L"privateBytes", JsonValue::Number(std::to_wstring(result.workerPrivateBytes)));
    return root;
}

bool RunChild(const std::filesystem::path& executable, const std::vector<std::wstring>& arguments,
    unsigned long timeoutMs, std::wstring& error) {
    std::wstring command = BrowserLauncher::QuoteArgument(executable.wstring());
    for (const auto& argument : arguments) command += L" " + BrowserLauncher::QuoteArgument(argument);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup, &process)) {
        error = L"无法启动蓝牙按需工作进程：" + FormatWindowsError(GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, timeoutMs);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 124);
        WaitForSingleObject(process.hProcess, 1000);
        CloseHandle(process.hProcess);
        error = L"蓝牙按需工作进程超时。";
        return false;
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    if (exitCode != 0) {
        error = L"蓝牙按需工作进程失败，退出码 " + std::to_wstring(exitCode) + L"。";
        return false;
    }
    return true;
}
}

bool BluetoothWorker::TryRunCommandLine(int& exitCode) {
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return false;
    const bool enumerate = count == 3 && _wcsicmp(arguments[1], kEnumerateSwitch) == 0;
    const bool connect = count == 5 && _wcsicmp(arguments[1], kConnectSwitch) == 0;
    const bool diagnosticConnect = count == 5 && _wcsicmp(arguments[1], kDiagnosticConnectSwitch) == 0;
    if (!enumerate && !connect && !diagnosticConnect) {
        LocalFree(arguments);
        return false;
    }
    if (diagnosticConnect) {
        const std::wstring requestedName = arguments[2];
        const std::filesystem::path output = arguments[3];
        const unsigned long timeoutMs = std::clamp<unsigned long>(wcstoul(arguments[4], nullptr, 10), 1000, 30000);
        wchar_t modulePath[32768]{};
        GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
        LocalFree(arguments);
        const auto enumeration = Enumerate(modulePath, 5000);
        BluetoothConnectionResult result;
        if (!enumeration.error.empty()) {
            result.message = enumeration.error;
        } else {
            const auto found = std::find_if(enumeration.devices.begin(), enumeration.devices.end(),
                [&](const BluetoothDeviceTarget& device) {
                    return ToLowerInvariant(device.displayName) == ToLowerInvariant(requestedName);
                });
            if (found == enumeration.devices.end()) {
                result.message = L"没有找到已配对设备：" + requestedName;
            } else if (found->connected) {
                result.confirmedConnected = true;
                result.message = found->displayName + L" 已经连接。";
            } else {
                result = Connect(modulePath, *found, timeoutMs);
            }
        }
        exitCode = WriteBytes(output, SerializeJsonUtf8(ConnectionJson(result))) ? 0 : 6;
        return true;
    }
    const std::filesystem::path request = connect ? arguments[2] : L"";
    const std::filesystem::path output = enumerate ? arguments[2] : arguments[3];
    const unsigned long timeoutMs = connect ? std::clamp<unsigned long>(wcstoul(arguments[4], nullptr, 10), 1000, 30000) : 0;
    LocalFree(arguments);

    if (enumerate) {
        exitCode = WriteBytes(output, SerializeJsonUtf8(EnumerationJson(EnumerateCore()))) ? 0 : 3;
        return true;
    }
    JsonValue root;
    std::wstring parseError;
    if (!ParseJsonUtf8(ReadBytes(request), root, parseError)) {
        exitCode = 4;
        return true;
    }
    exitCode = WriteBytes(output, SerializeJsonUtf8(ConnectionJson(ConnectCore(ParseDevice(root), timeoutMs)))) ? 0 : 5;
    return true;
}

BluetoothEnumerationResult BluetoothWorker::Enumerate(
    const std::filesystem::path& executable, unsigned long timeoutMs) {
    BluetoothEnumerationResult result;
    const auto output = TemporaryFile();
    if (output.empty()) {
        result.error = L"无法创建蓝牙枚举临时文件。";
        return result;
    }
    std::wstring error;
    if (RunChild(executable, {kEnumerateSwitch, output.wstring()}, timeoutMs, error)) {
        JsonValue root;
        if (!ParseJsonUtf8(ReadBytes(output), root, result.error)) {
            result.error = L"无法解析蓝牙枚举结果：" + result.error;
        } else {
            if (const auto* item = root.Find(L"error"); item && item->type() == JsonValue::Type::String) result.error = item->text();
            if (const auto* item = root.Find(L"elapsedMs"); item && item->type() == JsonValue::Type::Number)
                result.elapsedMilliseconds = _wtoi64(item->text().c_str());
            if (const auto* item = root.Find(L"privateWorkingSetBytes"); item && item->type() == JsonValue::Type::Number)
                result.workerPrivateWorkingSetBytes = _wtoi64(item->text().c_str());
            if (const auto* item = root.Find(L"privateBytes"); item && item->type() == JsonValue::Type::Number)
                result.workerPrivateBytes = _wtoi64(item->text().c_str());
            if (const auto* item = root.Find(L"threads"); item && item->type() == JsonValue::Type::Number)
                result.workerThreads = _wtoi(item->text().c_str());
            if (const auto* item = root.Find(L"handles"); item && item->type() == JsonValue::Type::Number)
                result.workerHandles = _wtoi(item->text().c_str());
            if (const auto* devices = root.Find(L"devices"); devices && devices->type() == JsonValue::Type::Array) {
                for (const auto& device : devices->array()) result.devices.push_back(ParseDevice(device));
            }
        }
    } else {
        result.error = std::move(error);
    }
    std::error_code ignored;
    std::filesystem::remove(output, ignored);
    return result;
}

BluetoothConnectionResult BluetoothWorker::Connect(const std::filesystem::path& executable,
    const BluetoothDeviceTarget& target, unsigned long timeoutMs) {
    BluetoothConnectionResult result;
    result.attempted = true;
    const auto started = std::chrono::steady_clock::now();
    const auto request = TemporaryFile();
    const auto output = TemporaryFile();
    if (request.empty() || output.empty() || !WriteBytes(request, SerializeJsonUtf8(DeviceJson(target)))) {
        result.message = L"无法创建蓝牙连接临时文件。";
    } else {
        std::wstring error;
        if (RunChild(executable, {kConnectSwitch, request.wstring(), output.wstring(), std::to_wstring(timeoutMs)},
                timeoutMs + 3000, error)) {
            JsonValue root;
            std::wstring parseError;
            if (ParseJsonUtf8(ReadBytes(output), root, parseError)) {
                if (const auto* item = root.Find(L"attempted"); item && item->type() == JsonValue::Type::Boolean) result.attempted = item->boolean();
                if (const auto* item = root.Find(L"requestAccepted"); item && item->type() == JsonValue::Type::Boolean) result.requestAccepted = item->boolean();
                if (const auto* item = root.Find(L"connected"); item && item->type() == JsonValue::Type::Boolean) result.confirmedConnected = item->boolean();
                if (const auto* item = root.Find(L"message"); item && item->type() == JsonValue::Type::String) result.message = item->text();
                if (const auto* item = root.Find(L"elapsedMs"); item && item->type() == JsonValue::Type::Number)
                    result.elapsedMilliseconds = _wtoi64(item->text().c_str());
                if (const auto* item = root.Find(L"privateWorkingSetBytes"); item && item->type() == JsonValue::Type::Number)
                    result.workerPrivateWorkingSetBytes = _wtoi64(item->text().c_str());
                if (const auto* item = root.Find(L"privateBytes"); item && item->type() == JsonValue::Type::Number)
                    result.workerPrivateBytes = _wtoi64(item->text().c_str());
            } else {
                result.message = L"无法解析蓝牙连接结果：" + parseError;
            }
        } else {
            result.message = std::move(error);
        }
    }
    std::error_code ignored;
    if (!request.empty()) std::filesystem::remove(request, ignored);
    if (!output.empty()) std::filesystem::remove(output, ignored);
    if (!result.elapsedMilliseconds) {
        result.elapsedMilliseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count());
    }
    return result;
}
