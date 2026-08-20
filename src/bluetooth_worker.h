#pragma once

#include "model.h"

#include <filesystem>

class BluetoothWorker {
public:
    static bool TryRunCommandLine(int& exitCode);

    static BluetoothEnumerationResult Enumerate(
        const std::filesystem::path& executable, unsigned long timeoutMs = 5000);
    static BluetoothConnectionResult Connect(const std::filesystem::path& executable,
        const BluetoothDeviceTarget& target, unsigned long timeoutMs);
};
