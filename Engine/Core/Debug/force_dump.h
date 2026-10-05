#pragma once

#include <Windows.h>
#include <filesystem>
#include <string>
#include <cstdint>

namespace dingosdk::debug {

enum class DumpKind {
    mini,
    full
};

struct DumpResult {
    bool success{false};
    std::filesystem::path path;
    std::uint64_t size_bytes{0};
    std::string error;
};

// Returns the PID of Skate.exe if running, or 0 if not found.
DWORD find_skate_process_id() noexcept;

// Writes a minidump or full memory dump of the given process.
// If process is NULL or GetCurrentProcess(), dumps the current process.
DumpResult write_process_dump(HANDLE process, DWORD pid, const std::filesystem::path& target_directory, DumpKind kind) noexcept;

} // namespace dingosdk::debug
