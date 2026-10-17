#pragma once

#include <filesystem>

bool TryRunAppTerminationTestHelper(int argc, wchar_t** argv, int& exitCode);
void RunAppTerminationTests(void (*check)(bool, const char*), const std::filesystem::path& tempRoot);
