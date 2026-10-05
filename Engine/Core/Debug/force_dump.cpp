#include "force_dump.h"
#include <DbgHelp.h>
#include <TlHelp32.h>
#include <chrono>
#include <format>

namespace dingosdk::debug {

DWORD find_skate_process_id() noexcept {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    DWORD found_pid = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"Skate.exe") == 0) {
                found_pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found_pid;
}

DumpResult write_process_dump(HANDLE process, DWORD pid, const std::filesystem::path& target_directory, DumpKind kind) noexcept {
    DumpResult result;
    std::error_code ec;
    std::filesystem::create_directories(target_directory, ec);
    if (ec) {
        result.error = std::format("Failed to create dump directory '{}': {}", target_directory.string(), ec.message());
        return result;
    }

    if (pid == 0) {
        pid = GetCurrentProcessId();
    }
    if (process == nullptr) {
        process = GetCurrentProcess();
    }

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto kind_str = (kind == DumpKind::full ? L"full" : L"mini");
    const auto dump_name = std::format(L"Skate-{}-{}-{}.dmp", pid, now, kind_str);
    const auto dump_path = target_directory / dump_name;

    HANDLE file = CreateFileW(dump_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        result.error = std::format("CreateFileW failed for '{}': error code {}", dump_path.string(), GetLastError());
        return result;
    }

    MINIDUMP_TYPE flags{};
    if (kind == DumpKind::full) {
        flags = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithFullMemory |
            MiniDumpWithHandleData |
            MiniDumpWithUnloadedModules |
            MiniDumpWithThreadInfo |
            MiniDumpWithProcessThreadData
        );
    } else {
        flags = static_cast<MINIDUMP_TYPE>(
            MiniDumpNormal |
            MiniDumpWithThreadInfo |
            MiniDumpWithIndirectlyReferencedMemory |
            MiniDumpWithDataSegs |
            MiniDumpWithProcessThreadData |
            MiniDumpWithHandleData |
            MiniDumpWithUnloadedModules
        );
    }

    BOOL written = MiniDumpWriteDump(process, pid, file, flags, nullptr, nullptr, nullptr);
    if (!written) {
        DWORD err = GetLastError();
        CloseHandle(file);
        DeleteFileW(dump_path.c_str());
        result.error = std::format("MiniDumpWriteDump failed: error code {}", err);
        return result;
    }

    FlushFileBuffers(file);
    LARGE_INTEGER file_size{};
    if (GetFileSizeEx(file, &file_size)) {
        result.size_bytes = static_cast<std::uint64_t>(file_size.QuadPart);
    }
    CloseHandle(file);

    result.success = true;
    result.path = dump_path;
    return result;
}

} // namespace dingosdk::debug
