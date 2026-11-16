#include "prx/libkernel/include/Api.hpp"
#include "prx/libc/include/Api.hpp"
#include <algorithm>
#include <optional>
#include <deque>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include "SceTypes.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libc/include/FilesystemError.hpp"
#include "prx/libc/include/PreciseWait.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/WindowsVerbatimPath.hpp"
#include "prx/libkernel/Apr/include/AprCommandBuffer.hpp"
#include "prx/libkernel/Apr/include/AmmPool.hpp"
#include "prx/libkernel/DirectMemory/DirectMemory.hpp"
#include "prx/libkernel/File/include/NativeStat.hpp"
#include "prx/libkernel/Equeue/Equeue.hpp"
#include "prx/libkernel/Time/include/Time.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {

constexpr int GUEST_ENOENT = 2;
constexpr int GUEST_EFAULT = 14;
constexpr int GUEST_EINVAL = 22;

struct AprFile {
    std::filesystem::path path;
    std::uint64_t size;
};

std::mutex& g_filesLock = *new std::mutex();
std::vector<AprFile>& g_files = *new std::vector<AprFile>();
std::unordered_map<std::string, std::uint32_t>& g_idsByPath = *new std::unordered_map<std::string, std::uint32_t>();

// File sizes by directory, listed on first use. Titles resolve thousands of package paths at startup,
// and one size query per path took over 20 s; a listing costs one directory read. Names are compared
// case-insensitively, like the host file system.
std::mutex& g_directoriesLock = *new std::mutex();
std::unordered_map<std::string, std::unordered_map<std::string, std::uint64_t>>& g_directories = *new std::unordered_map<std::string, std::unordered_map<std::string, std::uint64_t>>();

std::string _foldCase(std::string text) {
    for (auto& character : text) {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    }
    return text;
}

std::filesystem::path _hostPath(const std::filesystem::path& path) {
    std::error_code error;
    auto verbatim = NativeVerbatimPath_nid_no_patch(path, error);
    return error ? path : verbatim;
}

bool _fileSize(const std::filesystem::path& path, std::uint64_t& bytes) {
    {
        std::lock_guard lock(g_directoriesLock);
        const auto directory = _foldCase(path.parent_path().string());
        auto listed = g_directories.find(directory);
        if (listed == g_directories.end()) {
            std::unordered_map<std::string, std::uint64_t> sizes;
#ifdef _WIN32
            // The find data carries each size; std::filesystem would query every entry again.
            WIN32_FIND_DATAW entry{};
            const HANDLE find = FindFirstFileExW((_hostPath(path.parent_path()) / L"*").c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (find != INVALID_HANDLE_VALUE) {
                do {
                    if ((entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) continue;
                    const auto size = (static_cast<std::uint64_t>(entry.nFileSizeHigh) << 32u) | entry.nFileSizeLow;
                    sizes.emplace(_foldCase(std::filesystem::path(entry.cFileName).string()), size);
                } while (FindNextFileW(find, &entry));
                FindClose(find);
            }
#else
            std::error_code error;
            for (std::filesystem::directory_iterator it(path.parent_path(), error), end; !error && it != end; it.increment(error)) {
                std::error_code entryError;
                if (!it->is_regular_file(entryError)) continue;
                const auto size = it->file_size(entryError);
                if (!entryError) sizes.emplace(_foldCase(it->path().filename().string()), size);
            }
#endif
            listed = g_directories.emplace(directory, std::move(sizes)).first;
        }
        if (const auto file = listed->second.find(_foldCase(path.filename().string())); file != listed->second.end()) {
            bytes = file->second;
            return true;
        }
    }
    // Not in the listing (created since, or an unusual name): ask the file system directly.
    std::error_code error;
    if (!std::filesystem::is_regular_file(_hostPath(path), error)) return false;
    FileStat stat{};
    File::FillFileStat(path, &stat);
    bytes = static_cast<std::uint64_t>(stat.st_size);
    return true;
}

int _fail(int guestErrno) {
    *__error_nid_postfix() = guestErrno;
    return -1;
}

bool _traceApr() {
    static const bool trace = std::getenv("APS5_TRACE_APR") != nullptr;
    return trace;
}

bool _resolve(const char* guestPath, std::uint32_t* id, std::uint64_t* size) {
    if (!guestPath) return false;
    std::filesystem::path hostPath;
    std::uint64_t bytes = 0;
    if (ResolvePathError_nid_no_patch(guestPath, hostPath) != 0 || !_fileSize(hostPath, bytes)) {
        // The caller gets INVALID_FILE_ID for this path; a read of that id is reported below.
        if (_traceApr()) std::fprintf(stderr, "[apr] resolve failed: %s\n", guestPath);
        return false;
    }
    std::lock_guard lock(g_filesLock);
    const auto key = hostPath.string();
    auto found = g_idsByPath.find(key);
    if (found == g_idsByPath.end()) {
        found = g_idsByPath.emplace(key, static_cast<std::uint32_t>(g_files.size())).first;
        g_files.push_back({std::move(hostPath), bytes});
    }
    if (id) *id = found->second;
    if (size) *size = bytes;
    return true;
}

AprFile _file(std::uint32_t id) {
    std::lock_guard lock(g_filesLock);
    if (id >= g_files.size()) throw std::runtime_error("APR: unknown file id " + std::to_string(id));
    return g_files[id];
}

constexpr std::uint32_t INVALID_FILE_ID = 0xFFFFFFFFu;
constexpr int SCE_KERNEL_ERROR_ENOENT = static_cast<int>(0x80020002);

// A file id no resolve call returned: INVALID_FILE_ID, which the resolve calls write for a path they
// cannot find, or a stale or uninitialised id. Wolverine's Loose Load queue reads id 0xFFFFFFFF after
// the Marvel Games logo (KytyPS5 issue #2183). Throwing here aborted the process (the queue worker
// turns any exception into std::abort). Such a read now fails with SCE_KERNEL_ERROR_ENOENT (the code
// the resolve calls give a missing path, and the one KytyPS5 PR #937 reports for this read), which
// goes to the submission's result buffer with the command's offset, and the rest of the command
// buffer still runs, so its trailing WriteKernelEventQueue or WriteAddress still signals the queue
// that waits on it. (KytyPS5 stops the buffer at the failed read, which is the black screen of
// #2183.) What the console does after a failed command is not documented here; this is the choice
// that cannot hang a waiter.
// APS5_APR_BAD_FILE_ID: "fail" (default) fails the read and leaves the destination as it was, "zero"
// also zero-fills the destination, "abort" restores the exception.
enum class BadFileIdPolicy { Fail, Zero, Abort };

BadFileIdPolicy _badFileIdPolicy() {
    static const BadFileIdPolicy policy = [] {
        const char* value = std::getenv("APS5_APR_BAD_FILE_ID");
        if (value == nullptr || *value == '\0' || std::strcmp(value, "fail") == 0 || std::strcmp(value, "1") == 0) return BadFileIdPolicy::Fail;
        if (std::strcmp(value, "zero") == 0) return BadFileIdPolicy::Zero;
        if (std::strcmp(value, "abort") == 0 || std::strcmp(value, "0") == 0) return BadFileIdPolicy::Abort;
        std::fprintf(stderr, "[apr] APS5_APR_BAD_FILE_ID=%s is not fail, zero or abort; using fail\n", value);
        return BadFileIdPolicy::Fail;
    }();
    return policy;
}

bool _knownFileId(std::uint32_t id) {
    std::lock_guard lock(g_filesLock);
    return id < g_files.size();
}

// Once per distinct id (every time with APS5_TRACE_APR), so a queue that retries does not flood the log.
void _reportUnknownFileId(std::uint32_t id, const char* use) {
    constexpr std::size_t MaxReportedIds = 64;
    static std::mutex& lock = *new std::mutex();
    static std::set<std::uint32_t>& reported = *new std::set<std::uint32_t>();
    bool first = false;
    bool last = false;
    std::size_t files = 0;
    {
        std::lock_guard guard(lock);
        if (reported.size() < MaxReportedIds) {
            first = reported.insert(id).second;
            last = first && reported.size() == MaxReportedIds;
        }
    }
    if (!first && !_traceApr()) return;
    {
        std::lock_guard guard(g_filesLock);
        files = g_files.size();
    }
    std::fprintf(stderr, "[apr] %s of unknown file id 0x%08x failed with 0x%08x (ENOENT; %zu ids resolved)%s\n", use, id,
        static_cast<unsigned>(SCE_KERNEL_ERROR_ENOENT), files, id == INVALID_FILE_ID ? "; the id is the one a failed resolve writes (APS5_TRACE_APR=1 logs failed resolves)" : "");
    if (last) std::fprintf(stderr, "[apr] %zu distinct unknown file ids reported; further ones are not logged\n", MaxReportedIds);
    std::fflush(stderr);
}

// What a submission reports in its result buffer ({int32 result, uint32 errorOffset}, the layout of
// AmmSubmitResult too): the first failed command's error and its byte offset in the command buffer.
struct ExecutionResult {
    std::int32_t result = 0;
    std::uint32_t errorOffset = 0;
    // The guest's result buffer (sceKernelAprSubmitCommandBufferAndGetResult), or null. The first
    // failure is stored there at once, so it is in place before a later WriteKernelEventQueue or
    // WriteAddress of the same buffer tells the guest the buffer is done.
    std::uint32_t* sink = nullptr;
};

// Stores {result, errorOffset} in the guest's result buffer through GuestCopyOut (the buffer is guest
// memory, which the submission calls only probe once, at submission).
void _storeResult(std::uint32_t* sink, const ExecutionResult& status) {
    if (sink == nullptr) return;
    const std::uint32_t words[2] = {static_cast<std::uint32_t>(status.result), status.errorOffset};
    if (!GuestCopyOut_nid_no_patch(sink, words, sizeof(words))) {
        std::fprintf(stderr, "[apr] the result buffer 0x%llx is not writable guest memory; result 0x%08x not stored\n",
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(sink)), static_cast<unsigned>(status.result));
        std::fflush(stderr);
    }
}

// Fails a read of an unknown file id; false under APS5_APR_BAD_FILE_ID=abort (the read then throws).
bool _failUnknownFileRead(const Apr::ReadFileCommand& command, std::uint32_t commandOffset, ExecutionResult& status) {
    const auto policy = _badFileIdPolicy();
    if (policy == BadFileIdPolicy::Abort) return false;
    if (policy == BadFileIdPolicy::Zero && command.size != 0) {
        const GuestArena::HostWrite destination(reinterpret_cast<void*>(command.destination), command.size);
        if (destination.Open()) std::memset(reinterpret_cast<void*>(command.destination), 0, static_cast<std::size_t>(command.size));
    }
    if (status.result == 0) {
        status.result = SCE_KERNEL_ERROR_ENOENT;
        status.errorOffset = commandOffset;
        _storeResult(status.sink, status);
    }
    _reportUnknownFileId(command.fileId, "read");
    if (_traceApr()) {
        std::fprintf(stderr, "[apr] failed read: id=0x%08x offset=0x%llx size=0x%llx -> 0x%llx (command at 0x%x)\n", command.fileId,
            static_cast<unsigned long long>(command.offset), static_cast<unsigned long long>(command.size), static_cast<unsigned long long>(command.destination), commandOffset);
    }
    return true;
}

int _resolveForEach(const char* prefix, const char** paths, uint32_t count, uint32_t* ids, uint64_t* sizes, int* results) {
    if (!paths || !ids) return _fail(GUEST_EINVAL);
    std::uint32_t resolvedCount = 0;
    for (uint32_t index = 0; index < count; ++index) {
        const std::string path = prefix ? (paths[index] ? std::string(prefix) + paths[index] : std::string()) : (paths[index] ? std::string(paths[index]) : std::string());
        const bool resolved = !path.empty() && _resolve(path.c_str(), &ids[index], sizes ? &sizes[index] : nullptr);
        if (!resolved) {
            ids[index] = INVALID_FILE_ID;
            if (sizes) sizes[index] = 0;
        }
        if (resolved) ++resolvedCount;
        if (results) results[index] = resolved ? 0 : SCE_KERNEL_ERROR_ENOENT;
    }
    return static_cast<int>(resolvedCount);
}

void _readFile(const Apr::ReadFileCommand& command) {
    const auto file = _file(command.fileId);
    if (_traceApr()) std::fprintf(stderr, "[apr] read %s offset=0x%llx size=0x%llx -> 0x%llx\n", file.path.string().c_str(), static_cast<unsigned long long>(command.offset), static_cast<unsigned long long>(command.size), static_cast<unsigned long long>(command.destination));
    const auto hostPath = _hostPath(file.path);
    std::ifstream stream(hostPath, std::ios::binary);
    if (!stream) throw std::runtime_error("APR: cannot open " + file.path.string());
    stream.seekg(static_cast<std::streamoff>(command.offset));
    const GuestArena::HostWrite destination(reinterpret_cast<void*>(command.destination), command.size);
    if (!destination.Open()) throw std::runtime_error("APR: the read destination of " + file.path.string() + " is not writable guest memory");
#ifdef _WIN32
    std::vector<char> staging(static_cast<std::size_t>(std::min<std::uint64_t>(command.size, 64u * 1024u)));
    std::uint64_t read = 0;
    while (read < command.size) {
        const auto wanted = static_cast<std::streamsize>(std::min<std::uint64_t>(command.size - read, staging.size()));
        stream.read(staging.data(), wanted);
        const auto obtained = static_cast<std::uint64_t>(stream.gcount());
        if (obtained != 0) std::memcpy(reinterpret_cast<char*>(command.destination) + read, staging.data(), static_cast<std::size_t>(obtained));
        read += obtained;
        if (stream.bad()) throw std::runtime_error("APR: read failed for " + file.path.string());
        if (obtained != static_cast<std::uint64_t>(wanted)) break;
    }
#else
    stream.read(reinterpret_cast<char*>(command.destination), static_cast<std::streamsize>(command.size));
    if (stream.bad()) throw std::runtime_error("APR: read failed for " + file.path.string());
    const auto read = static_cast<std::uint64_t>(stream.gcount());
#endif
    if (read != command.size && (!stream.eof() || command.offset + read < std::filesystem::file_size(hostPath))) throw std::runtime_error("APR: read of " + file.path.string() + " at offset " + std::to_string(command.offset) + " returned " + std::to_string(read) + " of " + std::to_string(command.size) + " bytes");
    if (read != command.size) std::memset(reinterpret_cast<char*>(command.destination) + read, 0, static_cast<std::size_t>(command.size - read));
}

std::string _addressText(std::uint64_t address) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
    return text;
}

void _storeGuestAddress(std::uint64_t address, std::uint64_t value) {
    if (!GuestCopyOut_nid_no_patch(reinterpret_cast<void*>(address), &value, sizeof(value)))
        throw std::runtime_error("APR: the write address " + _addressText(address) + " is not writable guest memory");
    std::atomic_ref<std::uint64_t>(*reinterpret_cast<std::uint64_t*>(address)).store(value, std::memory_order_release);
}

std::uint64_t _loadGuestAddress(std::uint64_t address) {
    std::uint64_t value = 0;
    if (!GuestCopyIn_nid_no_patch(&value, reinterpret_cast<const void*>(address), sizeof(value)))
        throw std::runtime_error("APR: the wait address " + _addressText(address) + " is not readable guest memory");
    return value;
}

void _writeAddress(const Apr::WriteAddressCommand& command) {
    if (command.flags != 0) throw std::runtime_error("APR: WriteAddress flags " + std::to_string(command.flags) + " not implemented");
    _storeGuestAddress(command.address, command.value);
}

std::array<std::atomic<std::uint32_t>, 256> g_counters{};

std::mutex& g_counterWrites = *new std::mutex();

std::uint32_t _counter(std::uint32_t index) {
    return g_counters[index % g_counters.size()].load(std::memory_order_acquire);
}

struct CounterField {
    std::uint32_t bits;
    std::uint32_t shift;
};

CounterField _counterField(Apr::CounterAccess access) {
    const auto value = static_cast<std::uint32_t>(access);
    if (value == 0) return {64, 0};
    if (value == 1) return {32, 0};
    if (value < 4) return {16, (value - 2) * 16};
    if (value < 8) return {8, (value - 4) * 8};
    throw std::runtime_error("APR: counter access " + std::to_string(value) + " not implemented");
}

std::uint64_t _fieldMask(std::uint32_t bits) {
    return bits == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1u;
}

std::uint64_t _readCounter(std::uint32_t index, Apr::CounterAccess access) {
    const auto field = _counterField(access);
    if (field.bits == 64) return _counter(index) | static_cast<std::uint64_t>(_counter(index + 1u)) << 32u;
    return (_counter(index) >> field.shift) & _fieldMask(field.bits);
}

std::uint64_t _applyCounterOperation(Apr::CounterOperation operation, std::uint64_t current, std::uint64_t value) {
    switch (operation) {
        case Apr::CounterOperation::Store: return value;
        case Apr::CounterOperation::AtomicOr: return current | value;
        case Apr::CounterOperation::AtomicAndComplement: return current & ~value;
        case Apr::CounterOperation::AtomicXor: return current ^ value;
        case Apr::CounterOperation::AtomicAdd: return current + value;
    }
    throw std::runtime_error("APR: counter operation " + std::to_string(static_cast<std::uint32_t>(operation)) + " not implemented");
}

void _writeCounter(const Apr::WriteCounterCommand& command) {
    const auto field = _counterField(command.access);
    const std::lock_guard lock(g_counterWrites);
    const std::uint64_t current = _readCounter(command.counter, command.access);
    const std::uint64_t next = _applyCounterOperation(command.operation, current, command.value) & _fieldMask(field.bits);
    auto& low = g_counters[command.counter % g_counters.size()];
    if (field.bits == 64) {
        g_counters[(command.counter + 1u) % g_counters.size()].store(static_cast<std::uint32_t>(next >> 32u), std::memory_order_release);
        low.store(static_cast<std::uint32_t>(next), std::memory_order_release);
        return;
    }
    const auto mask = static_cast<std::uint32_t>(_fieldMask(field.bits) << field.shift);
    const auto bits = static_cast<std::uint32_t>(next << field.shift);
    low.store((low.load(std::memory_order_relaxed) & ~mask) | bits, std::memory_order_release);
}

bool _waitSatisfied(std::uint32_t compare, std::uint64_t value, std::uint64_t reference) {
    constexpr std::uint64_t sign = std::uint64_t{1} << 63u;
    switch (compare) {
        case 0: return value == reference;
        case 1: return value > reference;
        case 2: return value < reference;
        case 3: return value != reference;
        case 4: return value - reference < sign;
        case 5: return (value ^ sign) > (reference ^ sign);
        case 6: return (value ^ sign) < (reference ^ sign);
        default: throw std::runtime_error("APR: wait compare function " + std::to_string(compare) + " not implemented");
    }
}

template<class TCommand>
TCommand _read(const Apr::CommandBufferObject& buffer, std::uint32_t cursor) {
    Apr::CommandHeader header;
    std::memcpy(&header, buffer.base + cursor, sizeof(header));
    if (header.bytes < sizeof(TCommand)) throw std::runtime_error("APR: truncated command payload (opcode " + std::to_string(static_cast<std::uint32_t>(header.opcode)) + ", has " + std::to_string(header.bytes) + ", needs " + std::to_string(sizeof(TCommand)) + ")");
    TCommand command;
    std::memcpy(&command, buffer.base + cursor, sizeof(command));
    return command;
}

constexpr std::uint64_t AmmRangeBytes = 256ull << 30;
constexpr std::uint64_t AmmMultimapRangeBytes = 32ull << 30;
constexpr std::uint64_t AmmReservedBytes = AmmRangeBytes + AmmMultimapRangeBytes;
constexpr std::uint64_t AmmRangeAlignment = 64ull << 30;
constexpr int GuestMapFixed = 0x10;
constexpr int GuestMapNoOverwrite = 0x80;

static_assert(AmmPool::PageBytes == PS5_PAGE_SIZE);

using AmmPage = AmmPool::Page;

struct AmmState : AmmPool::Records {
    std::mutex lock;
    std::uintptr_t base = 0;
    std::set<std::uintptr_t> prt;
};

AmmState& _amm() {
    static auto& state = *new AmmState();
    return state;
}

int _ammHostProtection(std::int32_t protection);

// APS5_GPU_ENGINE=kyty with APS5_GPU_ENGINE_MEMORY=full: Kyty owns direct memory, reservations and
// mappings (DirectMemory/Export.cpp), so the AMM range and its pages go through the public calls,
// which forward to Kyty. The direct memory AMM maps comes from Kyty's sceKernelAllocateDirectMemory;
// mapped with AnyPS5's own primitives it would be AnyPS5 memory the GPU never sees.
bool _ammKyty() {
    static const bool enabled = [] {
        const char* engine = std::getenv("APS5_GPU_ENGINE");
        const char* memory = std::getenv("APS5_GPU_ENGINE_MEMORY");
        return engine != nullptr && std::strcmp(engine, "kyty") == 0 && memory != nullptr && std::strcmp(memory, "full") == 0;
    }();
    return enabled;
}

// PS5 protection for Kyty: CPU and GPU bits kept, AMM's extra read (0x140) / write (0x280) bits folded in.
int _ammKytyProtection(std::int32_t protection) {
    int prot = protection & 0x33;
    if ((protection & 0x140) != 0) prot |= 0x11;
    if ((protection & 0x280) != 0) prot |= 0x33;
    if ((prot & 0x02) != 0) prot |= 0x01;
    if ((prot & 0x20) != 0) prot |= 0x10;
    return prot;
}

int _ammProtectionKey(std::int32_t protection) {
    return _ammKyty() ? _ammKytyProtection(protection) : _ammHostProtection(protection);
}

int _ammReserveRange(void** address, std::size_t size, int flags, std::size_t alignment) {
    return _ammKyty() ? sceKernelReserveVirtualRange(address, size, flags, alignment) : DoReserveVirtual(address, size, flags, alignment);
}

int _ammMapDirectRange(void** address, std::size_t size, std::int32_t protection, std::uint64_t physical) {
    if (_ammKyty()) return sceKernelMapDirectMemory(address, size, _ammKytyProtection(protection), GuestMapFixed, static_cast<std::int64_t>(physical), 0);
    return DoMapDirect(address, size, _ammHostProtection(protection), GuestMapFixed, static_cast<std::int64_t>(physical), 0);
}

int _ammProtectRange(void* address, std::size_t size, std::int32_t protection) {
    if (_ammKyty()) return sceKernelMprotect(address, size, _ammKytyProtection(protection));
    return DoMprotect(address, size, _ammHostProtection(protection));
}

// Unbacked PRT pages read as zero on the GPU.
int _ammMapZeroRange(void** address, std::size_t size) {
    constexpr int CpuRead = 0x01;
    constexpr int GpuRead = 0x10;
    if (!_ammKyty()) return DoMapAnon(address, size, GpuRead, GuestMapFixed);
    void* target = *address;
    int status = sceKernelMapFlexibleMemory(&target, size, CpuRead | GpuRead, GuestMapFixed);
    if (status != 0) {
        target = *address;
        status = sceKernelMapFlexibleMemory(&target, size, CpuRead, GuestMapFixed);
    }
    if (status == 0) *address = target;
    return status;
}

std::uintptr_t _ammBase(AmmState& state) {
    if (state.base == 0 && _ammKyty()) {
        void* address = nullptr;
        const int status = sceKernelReserveVirtualRange(&address, AmmReservedBytes, 0, AmmRangeAlignment);
        if (status != 0) throw std::runtime_error("AMM: Kyty cannot reserve the virtual address range (" + std::to_string(status) + ")");
        state.base = reinterpret_cast<std::uintptr_t>(address);
        std::fprintf(stderr, "[amm] virtual address range 0x%llx + 0x%llx in Kyty's address space\n",
            static_cast<unsigned long long>(state.base), static_cast<unsigned long long>(AmmReservedBytes));
    }
    if (state.base == 0) {
        std::uintptr_t arenaBase = 0;
        std::size_t arenaBytes = 0;
        GuestArena::GuestArenaRange_nid_postfix(&arenaBase, &arenaBytes);
        if (arenaBase == 0 || arenaBytes < AmmReservedBytes) throw std::runtime_error("AMM: the guest address space arena cannot hold the virtual address range");
        const auto top = GuestArena::GuestArenaHighestFree_nid_no_patch(AmmReservedBytes, AmmRangeAlignment);
        if (top == 0) throw std::runtime_error("AMM: the guest address space arena has no free virtual address range");
        void* address = reinterpret_cast<void*>(top);
        if (DoReserveVirtual(&address, AmmReservedBytes, GuestMapFixed | GuestMapNoOverwrite, 0) != 0) throw std::runtime_error("AMM: cannot reserve the virtual address range");
        state.base = reinterpret_cast<std::uintptr_t>(address);
    }
    return state.base;
}

void _ammCheckRange(AmmState& state, std::uint64_t address, std::uint64_t size) {
    const auto base = _ammBase(state);
    if (address < base || size > AmmReservedBytes || address - base > AmmReservedBytes - size) throw std::runtime_error("AMM: range outside the AMM virtual address range");
}

int _ammHostProtection(std::int32_t protection) {
    int host = protection & 0x33;
    if ((protection & 0x140) != 0) host |= 1;
    if ((protection & 0x280) != 0) host |= 3;
    if ((host & 2) != 0) host |= 1;
    return host;
}

void _returnPoolPages(AmmState& state, std::uint64_t offset, std::uint64_t bytes) {
    AmmPool::ReturnPages(state, offset, bytes);
}

bool _tryTakePoolPages(AmmState& state, std::uint64_t bytes, std::uint64_t& offset) {
    return AmmPool::TryTakeRun(state, bytes, offset);
}

void _addPage(AmmState& state, std::uint64_t address, const AmmPage& page) {
    AmmPool::AddPage(state, address, page);
}

void _releasePage(AmmState& state, const AmmPage& page) {
    if (!page.pooled) return;
    const auto uses = state.pooledUses.find(page.physical);
    if (--uses->second == 0) {
        state.pooledUses.erase(uses);
        _returnPoolPages(state, page.physical, PS5_PAGE_SIZE);
    }
}

class AmmPageReferences {
public:
    AmmPageReferences(AmmState& state, const std::vector<AmmPage>& pages) : state(state), pages(pages) {
        for (const auto& page : pages) {
            if (page.pooled) ++state.pooledUses.find(page.physical)->second;
        }
    }

    AmmPageReferences(const AmmPageReferences&) = delete;
    AmmPageReferences& operator=(const AmmPageReferences&) = delete;

    ~AmmPageReferences() {
        for (const auto& page : pages) _releasePage(state, page);
    }

private:
    AmmState& state;
    const std::vector<AmmPage>& pages;
};

void _forgetPages(AmmState& state, std::uint64_t address, std::uint64_t size) {
    for (auto it = state.pages.lower_bound(address); it != state.pages.end() && it->first < address + size;) {
        _releasePage(state, it->second);
        it = state.pages.erase(it);
    }
}

std::vector<AmmPage> _mappedPages(AmmState& state, std::uint64_t address, std::uint64_t size) {
    std::vector<AmmPage> pages;
    for (std::uint64_t offset = 0; offset < size; offset += PS5_PAGE_SIZE) {
        const auto found = state.pages.find(address + offset);
        if (found == state.pages.end()) throw std::runtime_error("AMM: range is not fully mapped");
        pages.push_back(found->second);
    }
    return pages;
}

void _reserve(std::uint64_t address, std::uint64_t size) {
    void* target = reinterpret_cast<void*>(address);
    if (_ammReserveRange(&target, size, GuestMapFixed, 0) != 0) throw std::runtime_error("AMM: cannot return the range to the reservation");
}

void _mapRun(std::uint64_t address, std::uint64_t physical, std::uint64_t size, std::int32_t protection) {
    void* target = reinterpret_cast<void*>(address);
    if (_ammMapDirectRange(&target, size, protection, physical) != 0) throw std::runtime_error("AMM: map failed");
}

void _mapPages(AmmState& state, std::uint64_t address, const std::vector<AmmPage>& pages) {
    for (std::size_t first = 0; first < pages.size();) {
        std::size_t last = first + 1;
        while (last < pages.size() && pages[last].physical == pages[last - 1].physical + PS5_PAGE_SIZE && pages[last].protection == pages[first].protection) ++last;
        _mapRun(address + first * PS5_PAGE_SIZE, pages[first].physical, (last - first) * PS5_PAGE_SIZE, pages[first].protection);
        for (std::size_t index = first; index < last; ++index) _addPage(state, address + index * PS5_PAGE_SIZE, pages[index]);
        first = last;
    }
}

void _clearPrt(AmmState& state, std::uint64_t address, std::uint64_t size) {
    state.prt.erase(state.prt.lower_bound(address), state.prt.lower_bound(address + size));
}

void _requirePrt(AmmState& state, std::uint64_t address, std::uint64_t size) {
    for (std::uint64_t offset = 0; offset < size; offset += PS5_PAGE_SIZE) {
        if (!state.prt.contains(address + offset)) throw std::runtime_error("AMM: range is not a PRT range");
    }
}

void _zeroPrt(std::uint64_t address, std::uint64_t size) {
    void* target = reinterpret_cast<void*>(address);
    if (_ammMapZeroRange(&target, size) != 0) throw std::runtime_error("AMM: cannot map the unbacked PRT pages");
}

void _ammUnmap(const Apr::AmmUnmapCommand& command) {
    auto& state = _amm();
    const std::lock_guard lock(state.lock);
    _ammCheckRange(state, command.address, command.size);
    _forgetPages(state, command.address, command.size);
    _clearPrt(state, command.address, command.size);
    _reserve(command.address, command.size);
}

void _ammMapAsPrt(const Apr::AmmUnmapCommand& command) {
    auto& state = _amm();
    const std::lock_guard lock(state.lock);
    _ammCheckRange(state, command.address, command.size);
    _forgetPages(state, command.address, command.size);
    _zeroPrt(command.address, command.size);
    for (std::uint64_t offset = 0; offset < command.size; offset += PS5_PAGE_SIZE) state.prt.insert(command.address + offset);
}

void _ammUnmapToPrt(const Apr::AmmUnmapCommand& command) {
    auto& state = _amm();
    const std::lock_guard lock(state.lock);
    _ammCheckRange(state, command.address, command.size);
    _requirePrt(state, command.address, command.size);
    _forgetPages(state, command.address, command.size);
    _zeroPrt(command.address, command.size);
}

void _ammAllocatePrt(const Apr::AmmProtectCommand& command) {
    auto& state = _amm();
    const std::lock_guard lock(state.lock);
    _ammCheckRange(state, command.address, command.size);
    _requirePrt(state, command.address, command.size);
    for (std::uint64_t offset = 0; offset < command.size;) {
        const auto address = command.address + offset;
        const bool backed = state.pages.contains(address);
        std::uint64_t end = offset + PS5_PAGE_SIZE;
        while (end < command.size && state.pages.contains(command.address + end) == backed) end += PS5_PAGE_SIZE;
        const auto bytes = end - offset;
        if (backed) {
            for (std::uint64_t page = 0; page < bytes; page += PS5_PAGE_SIZE) {
                auto& entry = state.pages[address + page];
                entry.type = command.type;
                entry.protection = command.protection;
            }
            if (_ammProtectRange(reinterpret_cast<void*>(address), bytes, command.protection) != 0) throw std::runtime_error("AMM: protection change failed");
        } else {
            try {
                AmmPool::MapScattered(state, address, bytes, command.type, command.protection, [&](std::uint64_t at, std::uint64_t physical, std::uint64_t size) { _mapRun(at, physical, size, command.protection); });
            } catch (...) {
                _zeroPrt(address, bytes);
                throw;
            }
        }
        offset = end;
    }
}

void _ammMap(const Apr::AmmMapCommand& command, bool direct) {
    auto& state = _amm();
    const std::lock_guard lock(state.lock);
    _ammCheckRange(state, command.address, command.size);
    _reserve(command.address, command.size);
    _forgetPages(state, command.address, command.size);
    _clearPrt(state, command.address, command.size);
    std::uint64_t physical = direct ? command.directOffset : 0;
    if (!direct && !_tryTakePoolPages(state, command.size, physical)) {
        try {
            AmmPool::MapScattered(state, command.address, command.size, command.type, command.protection, [&](std::uint64_t address, std::uint64_t physicalRun, std::uint64_t size) { _mapRun(address, physicalRun, size, command.protection); });
        } catch (...) {
            _reserve(command.address, command.size);
            throw;
        }
        return;
    }
    try {
        _mapRun(command.address, physical, command.size, command.protection);
    } catch (...) {
        _reserve(command.address, command.size);
        if (!direct) _returnPoolPages(state, physical, command.size);
        throw;
    }
    for (std::uint64_t offset = 0; offset < command.size; offset += PS5_PAGE_SIZE) _addPage(state, command.address + offset, {physical + offset, command.type, command.protection, !direct});
}

void _ammRemap(const Apr::AmmRemapCommand& command, bool alias, bool intoPrt) {
    auto& state = _amm();
    const std::lock_guard lock(state.lock);
    _ammCheckRange(state, command.address, command.size);
    _ammCheckRange(state, command.source, command.size);
    if ((alias || intoPrt) && command.address < command.source + command.size && command.source < command.address + command.size) throw std::runtime_error("AMM: remap onto its own source range");
    if (intoPrt) _requirePrt(state, command.address, command.size);
    auto pages = _mappedPages(state, command.source, command.size);
    for (auto& page : pages) page.protection = command.protection;
    const AmmPageReferences references(state, pages);
    if (!alias) {
        _reserve(command.source, command.size);
        _forgetPages(state, command.source, command.size);
        _clearPrt(state, command.source, command.size);
    }
    _reserve(command.address, command.size);
    _forgetPages(state, command.address, command.size);
    if (!intoPrt) _clearPrt(state, command.address, command.size);
    _mapPages(state, command.address, pages);
}

void _ammProtect(const Apr::AmmProtectCommand& command, bool type) {
    auto& state = _amm();
    const std::lock_guard lock(state.lock);
    _ammCheckRange(state, command.address, command.size);
    auto pages = _mappedPages(state, command.address, command.size);
    for (std::size_t index = 0; index < pages.size(); ++index) {
        auto& page = pages[index];
        page.protection = (page.protection & ~command.mask) | (command.protection & command.mask);
        if (type) page.type = command.type;
        state.pages[command.address + index * PS5_PAGE_SIZE] = page;
    }
    for (std::size_t first = 0; first < pages.size();) {
        const int key = _ammProtectionKey(pages[first].protection);
        std::size_t last = first + 1;
        while (last < pages.size() && _ammProtectionKey(pages[last].protection) == key) ++last;
        if (_ammProtectRange(reinterpret_cast<void*>(command.address + first * PS5_PAGE_SIZE), (last - first) * PS5_PAGE_SIZE, pages[first].protection) != 0) throw std::runtime_error("AMM: protection change failed");
        first = last;
    }
}

struct ReadCursor {
    bool valid = false;
    std::uint32_t fileId = 0;
    std::uint64_t nextDestination = 0;
    std::uint64_t nextOffset = 0;
};

void _readResolved(Apr::Opcode opcode, Apr::ReadFileCommand command, ReadCursor& read, std::uint32_t commandOffset, ExecutionResult& status) {
    if (opcode != Apr::Opcode::ReadFile) {
        if (!read.valid) throw std::runtime_error("APR: gather or scatter read without a preceding read");
        command.fileId = read.fileId;
        if (opcode == Apr::Opcode::ReadFileGather) command.destination = read.nextDestination;
        if (opcode == Apr::Opcode::ReadFileScatter) command.offset = read.nextOffset;
    }
    // The gather/scatter state advances past a failed read too, so the reads that continue it fail
    // with the same id instead of losing their place.
    if (_knownFileId(command.fileId) || !_failUnknownFileRead(command, commandOffset, status)) _readFile(command);
    read = {true, command.fileId, command.destination + command.size, command.offset + command.size};
}

// Execution state of one submission: where it stopped (at a wait it deferred) and what it reports.
struct Deferred {
    std::uint32_t cursor = 0;
    ReadCursor read;
    ExecutionResult status;
};

// Runs the buffer from state.cursor. With defer, an unsatisfied wait returns true and leaves state at
// that wait, for the queue worker to resume.
bool _execute(const Apr::CommandBufferObject& buffer, Deferred& state, bool defer = false) {
    std::uint32_t& cursor = state.cursor;
    ReadCursor& read = state.read;
    while (cursor < buffer.offset) {
        if (buffer.offset - cursor < Apr::NopHeaderBytes) throw std::runtime_error("APR: truncated command buffer");
        std::uint32_t first;
        std::memcpy(&first, buffer.base + cursor, sizeof(first));
        if (Apr::IsNopHeader(first)) {
            const std::uint32_t bytes = Apr::NopDwords(first) * Apr::NopHeaderBytes;
            if (bytes > buffer.offset - cursor) throw std::runtime_error("APR: malformed command");
            cursor += bytes;
            continue;
        }
        if (cursor + sizeof(Apr::CommandHeader) > buffer.offset) throw std::runtime_error("APR: truncated command buffer");
        Apr::CommandHeader header;
        std::memcpy(&header, buffer.base + cursor, sizeof(header));
        if ((first & 0xffu) == static_cast<std::uint32_t>(Apr::Opcode::CompactReadFileGather)) {
            if (sizeof(Apr::CompactReadFileGatherCommand) > buffer.offset - cursor) throw std::runtime_error("APR: truncated compact gather command");
            Apr::CompactReadFileGatherCommand command;
            std::memcpy(&command, buffer.base + cursor, sizeof(command));
            const auto offset = static_cast<std::uint64_t>(command.opcodeAndOffsetHigh >> 8u) << 32u | command.offsetLow;
            if (offset >= 0x10000000000ull) throw std::runtime_error("APR: malformed compact gather offset");
            _readResolved(Apr::Opcode::ReadFileGather, Apr::ReadFileCommand{{}, 0, 0, 0, static_cast<std::uint64_t>(command.sizeMinusOne) + 1u, offset}, read, cursor, state.status);
            cursor += sizeof(command);
            continue;
        }
        if (header.bytes < sizeof(header) || header.bytes > buffer.offset - cursor) throw std::runtime_error("APR: malformed command");
        switch (header.opcode) {
        case Apr::Opcode::PushMarker:
        case Apr::Opcode::PopMarker:
        case Apr::Opcode::SetMarker:
            break;
        case Apr::Opcode::ReadFile:
        case Apr::Opcode::ReadFileGather:
        case Apr::Opcode::ReadFileScatter:
        case Apr::Opcode::ReadFileGatherScatter:
            _readResolved(header.opcode, _read<Apr::ReadFileCommand>(buffer, cursor), read, cursor, state.status);
            break;
        case Apr::Opcode::ResetGatherScatterState:
            read = {};
            break;
        case Apr::Opcode::AmmMap:
        case Apr::Opcode::AmmMapDirect:
            _ammMap(_read<Apr::AmmMapCommand>(buffer, cursor), header.opcode == Apr::Opcode::AmmMapDirect);
            break;
        case Apr::Opcode::AmmUnmap:
            _ammUnmap(_read<Apr::AmmUnmapCommand>(buffer, cursor));
            break;
        case Apr::Opcode::AmmRemap:
        case Apr::Opcode::AmmMultiMap:
            _ammRemap(_read<Apr::AmmRemapCommand>(buffer, cursor), header.opcode == Apr::Opcode::AmmMultiMap, false);
            break;
        case Apr::Opcode::AmmRemapIntoPrt:
            _ammRemap(_read<Apr::AmmRemapCommand>(buffer, cursor), false, true);
            break;
        case Apr::Opcode::AmmMapAsPrt:
            _ammMapAsPrt(_read<Apr::AmmUnmapCommand>(buffer, cursor));
            break;
        case Apr::Opcode::AmmUnmapToPrt:
            _ammUnmapToPrt(_read<Apr::AmmUnmapCommand>(buffer, cursor));
            break;
        case Apr::Opcode::AmmAllocatePaForPrt:
            _ammAllocatePrt(_read<Apr::AmmProtectCommand>(buffer, cursor));
            break;
        case Apr::Opcode::AmmModifyProtect:
        case Apr::Opcode::AmmModifyMtypeProtect:
            _ammProtect(_read<Apr::AmmProtectCommand>(buffer, cursor), header.opcode == Apr::Opcode::AmmModifyMtypeProtect);
            break;
        case Apr::Opcode::MapEnd:
            break;
        case Apr::Opcode::WriteAddress: {
            const auto command = _read<Apr::WriteAddressCommand>(buffer, cursor);
            _writeAddress(command);
            break;
        }
        case Apr::Opcode::WriteCounter: {
            _writeCounter(_read<Apr::WriteCounterCommand>(buffer, cursor));
            break;
        }
        case Apr::Opcode::WaitOnAddress:
        case Apr::Opcode::WaitOnCounter: {
            const auto command = _read<Apr::WaitCommand>(buffer, cursor);
            const bool counter = header.opcode == Apr::Opcode::WaitOnCounter;
            const std::uint32_t unused = counter ? 64u - _counterField(command.access).bits : 0u;
            if (!counter) static_cast<void>(_loadGuestAddress(command.address));
            const auto current = [&]() -> std::uint64_t {
                if (counter) return _readCounter(command.counter, command.access);
                return std::atomic_ref<std::uint64_t>(*reinterpret_cast<std::uint64_t*>(command.address)).load(std::memory_order_acquire);
            };
            const std::uint64_t reference = (command.reference & command.mask) << unused;
            if (defer && !_waitSatisfied(command.compare, (current() & command.mask) << unused, reference)) return true;
            while (!_waitSatisfied(command.compare, (current() & command.mask) << unused, reference)) PreciseSleepUs(50);
            break;
        }
        case Apr::Opcode::WriteKernelEventQueue: {
            const auto command = _read<Apr::WriteKernelEventQueueCommand>(buffer, cursor);
            EqueueTriggerEvent_nid_postfix(static_cast<KernelEqueue>(command.equeue), static_cast<uintptr_t>(command.ident), EVFILT_AMPR, reinterpret_cast<void*>(command.data));

            break;
        }
        case Apr::Opcode::WriteAddressFromTimeCounter: {
            const auto command = _read<Apr::WriteAddressFromCounterCommand>(buffer, cursor);
            _storeGuestAddress(command.address, sceKernelGetProcessTimeCounter());
            break;
        }
        case Apr::Opcode::WriteAddressFromCounter:
        case Apr::Opcode::WriteAddressFromCounterPair: {
            const auto command = _read<Apr::WriteAddressFromCounterCommand>(buffer, cursor);
            std::uint64_t value = _counter(command.counter0);
            if (header.opcode == Apr::Opcode::WriteAddressFromCounterPair) value |= static_cast<std::uint64_t>(_counter(command.counter1)) << 32u;
            _storeGuestAddress(command.address, value);
            break;
        }
        default:
            throw std::runtime_error("APR: unknown opcode " + std::to_string(static_cast<std::uint32_t>(header.opcode)));
        }
        cursor += header.bytes;
    }
    return false;
}

// Called when the whole buffer has run. sceKernelAprSubmitCommandBufferAndGetResult already wrote a
// zero result at submission; a failed command replaces it before the submission counts as completed,
// so a guest that waits on the id reads the final value.
void _reportResult(const ExecutionResult& status, std::uint32_t* result) {
    if (status.result == 0) return;
    _storeResult(result, status);
}

}

extern "C" {

int AmmGiveDirectMemory_nid_no_patch(std::int64_t searchStart, std::int64_t searchEnd, std::size_t size, std::size_t alignment, int usage, std::int64_t* offset) {
    if (!offset || (usage != 0 && usage != 1)) return SCE_KERNEL_ERROR_EINVAL;
    const int result = sceKernelAllocateDirectMemory(searchStart, searchEnd, size, alignment, 0, offset);
    if (result != 0 || usage == 0) return result;
    auto& state = _amm();
    const std::lock_guard lock(state.lock);
    _returnPoolPages(state, static_cast<std::uint64_t>(*offset), size);
    return 0;
}

void AmmVirtualAddressRanges_nid_no_patch(std::uint64_t* start, std::uint64_t* end, std::uint64_t* multimapStart, std::uint64_t* multimapEnd) {
    auto& state = _amm();
    const std::lock_guard lock(state.lock);
    const auto base = _ammBase(state);
    if (start) *start = base;
    if (end) *end = base + AmmRangeBytes;
    if (multimapStart) *multimapStart = base + AmmRangeBytes;
    if (multimapEnd) *multimapEnd = base + AmmReservedBytes;
}

struct AprQueue {
    std::mutex submit;
    std::mutex mutex;
    std::condition_variable ready;
    struct Work {
        std::vector<std::uint8_t> commands;
        Deferred resume;
        std::uint32_t id = 0;
        std::uint32_t* result = nullptr;
    };
    std::deque<Work> pending;
    bool running = false;
};

struct AprCompletion {
    std::mutex mutex;
    std::condition_variable done;
    std::set<std::uint32_t> outstanding;
    std::uint32_t issued = 0;
};

AprCompletion& _completion(bool amm) {
    static AprCompletion* const completions[2] = {new AprCompletion(), new AprCompletion()};
    return *completions[amm ? 1 : 0];
}

void _complete(bool amm, std::uint32_t id) {
    auto& completion = _completion(amm);
    {
        std::lock_guard lock(completion.mutex);
        completion.outstanding.erase(id);
    }
    completion.done.notify_all();
}

AprQueue* _startQueue(bool amm) {
    auto* created = new AprQueue();
    std::thread([created, amm] {
        for (;;) {
            AprQueue::Work work;
            {
                std::unique_lock lock(created->mutex);
                created->ready.wait(lock, [&] { return !created->pending.empty(); });
                work = std::move(created->pending.front());
                created->pending.pop_front();
                created->running = true;
            }
            try {
                const auto bytes = static_cast<std::uint32_t>(work.commands.size());
                _execute(Apr::CommandBufferObject{Apr::BufferType::Generic, 0, bytes, 0, bytes, work.commands.data()}, work.resume);
            } catch (const std::exception& error) {
                std::fprintf(stderr, "APR: %s\n", error.what());
                std::fflush(stderr);
                std::abort();
            }
            _reportResult(work.resume.status, work.result);
            {
                std::lock_guard lock(created->mutex);
                created->running = false;
            }
            _complete(amm, work.id);
        }
    }).detach();
    return created;
}

AprQueue& _queue(bool amm, std::uint32_t priority) {
    if (amm) {
        static AprQueue* const ammQueue = _startQueue(true);
        return *ammQueue;
    }
    static std::mutex& queuesLock = *new std::mutex();
    static std::map<std::uint32_t, AprQueue*>& queues = *new std::map<std::uint32_t, AprQueue*>();
    std::lock_guard lock(queuesLock);
    auto& queue = queues[priority];
    if (queue == nullptr) queue = _startQueue(false);
    return *queue;
}

std::optional<std::uint32_t> _submit(const std::uint8_t* base, std::uint32_t bytes, bool amm, std::uint32_t priority = 0, std::uint32_t* guestId = nullptr, std::uint32_t* guestResult = nullptr) {
    auto& queue = _queue(amm, priority);
    auto& completion = _completion(amm);
    std::lock_guard submitLock(queue.submit);
    std::uint32_t id;
    {
        std::lock_guard lock(completion.mutex);
        id = ++completion.issued;
        completion.outstanding.insert(id);
    }
    const std::uint32_t succeeded = 0;
    if ((guestId && !GuestCopyOut_nid_no_patch(guestId, &id, sizeof(id))) || (guestResult && !GuestCopyOut_nid_no_patch(guestResult, &succeeded, sizeof(succeeded)))) {
        _complete(amm, id);
        return std::nullopt;
    }
    bool queued;
    {
        std::lock_guard lock(queue.mutex);
        queued = queue.running || !queue.pending.empty();
    }
    const auto enqueue = [&](const Deferred& resume) {
        {
            std::lock_guard lock(queue.mutex);
            queue.pending.push_back({std::vector<std::uint8_t>(base, base + bytes), resume, id, guestResult});
        }
        queue.ready.notify_one();
    };
    if (queued) {
        Deferred fresh;
        fresh.status.sink = guestResult;
        enqueue(fresh);
        return id;
    }
    Deferred state;
    state.status.sink = guestResult;
    if (_execute(Apr::CommandBufferObject{Apr::BufferType::Generic, 0, bytes, 0, bytes, const_cast<std::uint8_t*>(base)}, state, true)) {
        enqueue(state);
        return id;
    }
    _reportResult(state.status, guestResult);
    _complete(amm, id);
    return id;
}

void _waitSubmitted(std::uint32_t id, bool amm) {
    auto& completion = _completion(amm);
    std::unique_lock lock(completion.mutex);
    completion.done.wait(lock, [&] { return id > completion.issued || !completion.outstanding.contains(id); });
}

std::uint32_t AmmSubmit_nid_no_patch(void* base, std::uint32_t bytes) {
    return *_submit(static_cast<const std::uint8_t*>(base), bytes, true);
}

bool AmmSubmitted_nid_no_patch(std::uint32_t id) {
    if (id == 0) return false;
    {
        auto& completion = _completion(true);
        std::lock_guard lock(completion.mutex);
        if (id > completion.issued) return false;
    }
    _waitSubmitted(id, true);
    return true;
}

int APS5_VABI sceKernelAprResolveFilepathsToIds(const char** paths, uint32_t count, uint32_t* ids, uint32_t* error_index) {
    if (!paths || !ids) return _fail(GUEST_EINVAL);
    for (uint32_t index = 0; index < count; ++index) {
        if (!_resolve(paths[index], &ids[index], nullptr)) {
            ids[index] = INVALID_FILE_ID;
            if (error_index) *error_index = index;
            return _fail(GUEST_ENOENT);
        }
    }
    return 0;
}

int APS5_VABI sceKernelAprResolveFilepathsToIdsAndFileSizes(const char** paths, uint32_t count, uint32_t* ids, uint64_t* sizes, uint32_t* error_index) {
    if (!paths || !ids || !sizes) return _fail(GUEST_EINVAL);
    for (uint32_t index = 0; index < count; ++index) {
        uint32_t id = INVALID_FILE_ID;
        uint64_t size = 0;
        const bool resolved = _resolve(paths[index], &id, &size);
        if (!resolved) {
            id = INVALID_FILE_ID;
            size = 0;
        }
        if (!GuestCopyOut_nid_no_patch(&ids[index], &id, sizeof(id)) || !GuestCopyOut_nid_no_patch(&sizes[index], &size, sizeof(size))) return _fail(GUEST_EFAULT);
        if (!resolved) {
            if (error_index && !GuestCopyOut_nid_no_patch(error_index, &index, sizeof(index))) return _fail(GUEST_EFAULT);
            return _fail(GUEST_ENOENT);
        }
    }
    return 0;
}

int APS5_VABI sceKernelAprResolveFilepathsWithPrefixToIdsAndFileSizes(const char* prefix, const char** paths, uint32_t count, uint32_t* ids, uint64_t* sizes, uint32_t* error_index) {
    if (!prefix || !paths || !ids) return _fail(GUEST_EINVAL);
    for (uint32_t index = 0; index < count; ++index) {
        const std::string path = paths[index] ? std::string(prefix) + paths[index] : std::string();
        uint32_t id = INVALID_FILE_ID;
        uint64_t size = 0;
        const bool resolved = !path.empty() && _resolve(path.c_str(), &id, sizes ? &size : nullptr);
        if (!resolved) {
            id = INVALID_FILE_ID;
            size = 0;
        }
        if (!GuestCopyOut_nid_no_patch(&ids[index], &id, sizeof(id)) || (sizes && !GuestCopyOut_nid_no_patch(&sizes[index], &size, sizeof(size)))) return _fail(GUEST_EFAULT);
        if (!resolved) {
            if (error_index && !GuestCopyOut_nid_no_patch(error_index, &index, sizeof(index))) return _fail(GUEST_EFAULT);
            return _fail(GUEST_ENOENT);
        }
    }
    return 0;
}

int APS5_VABI sceKernelAprResolveFilepathsWithPrefixToIds(const char* prefix, const char** paths, uint32_t count, uint32_t* ids, uint32_t* error_index) {
    return sceKernelAprResolveFilepathsWithPrefixToIdsAndFileSizes(prefix, paths, count, ids, nullptr, error_index);
}

int APS5_VABI sceKernelAprGetFileSize(uint32_t id, uint64_t* size) {
    if (!size) return _fail(GUEST_EINVAL);
    if (_badFileIdPolicy() != BadFileIdPolicy::Abort && !_knownFileId(id)) {
        _reportUnknownFileId(id, "size query");
        return _fail(GUEST_ENOENT);
    }
    const std::uint64_t bytes = _file(id).size;
    if (!GuestCopyOut_nid_no_patch(size, &bytes, sizeof(bytes))) return _fail(GUEST_EFAULT);
    return 0;
}

int APS5_VABI sceKernelAprGetFileStat(uint32_t id, FileStat* stat) {
    if (!stat) return _fail(GUEST_EINVAL);
    if (_badFileIdPolicy() != BadFileIdPolicy::Abort && !_knownFileId(id)) {
        _reportUnknownFileId(id, "stat");
        return _fail(GUEST_ENOENT);
    }
    FileStat result{};
    File::FillFileStat(_file(id).path, &result);
    if (!GuestCopyOut_nid_no_patch(stat, &result, sizeof(result))) return _fail(GUEST_EFAULT);
    return 0;
}

std::optional<std::vector<std::uint8_t>> _guestCommands(const Apr::CommandBufferObject& object) {
    if (object.offset > object.size) return std::nullopt;
    std::vector<std::uint8_t> commands;
    try {
        commands.resize(object.offset);
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
    if (object.offset != 0 && !GuestCopyIn_nid_no_patch(commands.data(), object.base, object.offset)) return std::nullopt;
    return commands;
}

int _submitGuest(const Apr::CommandBufferObject* buffer, uint32_t priority, uint32_t* id, uint32_t* result) {
    if (!buffer) return _fail(GUEST_EINVAL);
    Apr::CommandBufferObject object{};
    if (!GuestCopyIn_nid_no_patch(&object, buffer, sizeof(object))) return _fail(GUEST_EFAULT);
    const auto commands = _guestCommands(object);
    if (!commands) return _fail(GUEST_EFAULT);
    if (!_submit(commands->data(), static_cast<std::uint32_t>(commands->size()), false, priority, id, result)) return _fail(GUEST_EFAULT);
    return 0;
}

int APS5_VABI sceKernelAprSubmitCommandBuffer(const Apr::CommandBufferObject* buffer, uint32_t priority) {
    return _submitGuest(buffer, priority, nullptr, nullptr);
}

int APS5_VABI sceKernelAprSubmitCommandBufferAndGetId(const Apr::CommandBufferObject* buffer, uint32_t priority, uint32_t* id) {
    if (!id) return _fail(GUEST_EINVAL);
    return _submitGuest(buffer, priority, id, nullptr);
}

// result is the guest's {int32 result, uint32 errorOffset}. The zero result is written before the
// buffer runs; a failed command (an unknown file id) then stores its error and offset when the
// submission completes, before sceKernelAprWaitCommandBuffer on the id returns.
int APS5_VABI sceKernelAprSubmitCommandBufferAndGetResult(const Apr::CommandBufferObject* buffer, uint32_t priority, uint32_t* result, uint32_t* id) {
    if (!result) return _fail(GUEST_EINVAL);
    if (!id) return _fail(GUEST_EINVAL);
    return _submitGuest(buffer, priority, id, result);
}

int APS5_VABI sceKernelAprWaitCommandBuffer(uint32_t id) {
    _waitSubmitted(id, false);
    return 0;
}

int APS5_VABI sceKernelAprResolveFilepathsToIdsForEach(const char** paths, uint32_t count, uint32_t* ids, int* results) {
    return _resolveForEach(nullptr, paths, count, ids, nullptr, results);
}

int APS5_VABI sceKernelAprResolveFilepathsToIdsAndFileSizesForEach(const char** paths, uint32_t count, uint32_t* ids, uint64_t* sizes, int* results) {
    if (!sizes) return _fail(GUEST_EINVAL);
    return _resolveForEach(nullptr, paths, count, ids, sizes, results);
}

int APS5_VABI sceKernelAprResolveFilepathsWithPrefixToIdsForEach(const char* prefix, const char** paths, uint32_t count, uint32_t* ids, int* results) {
    if (!prefix) return _fail(GUEST_EINVAL);
    return _resolveForEach(prefix, paths, count, ids, nullptr, results);
}

int APS5_VABI sceKernelAprResolveFilepathsWithPrefixToIdsAndFileSizesForEach(const char* prefix, const char** paths, uint32_t count, uint32_t* ids, uint64_t* sizes, int* results) {
    if (!prefix || !sizes) return _fail(GUEST_EINVAL);
    return _resolveForEach(prefix, paths, count, ids, sizes, results);
}

bool _validWriteRange(std::uint64_t address, std::uint64_t size) {
    constexpr std::uint64_t pageMask = 0x3FFFu;
    return address != 0u && size != 0u && ((address | size) & pageMask) == 0u && address + size > address;
}

bool _validWriteProtection(std::uint64_t protection) {
    return (protection & ~0x3F3ULL) == 0u;
}

int APS5_VABI sceKernelWriteModifyMtypeProtectCommand(void* dst, std::uint64_t address, std::uint64_t size, std::uint64_t type, std::uint64_t protection, std::uint64_t mask, std::uint64_t* outSize) {
    if (!_validWriteRange(address, size) || !_validWriteProtection(protection) || !_validWriteProtection(mask)) return SCE_KERNEL_ERROR_EINVAL;
    const Apr::AmmProtectCommand command{{Apr::Opcode::AmmModifyMtypeProtect, sizeof(command)}, address, size, static_cast<std::int32_t>(type), static_cast<std::int32_t>(protection), static_cast<std::int32_t>(mask), 0};
    if (dst) std::memcpy(dst, &command, sizeof(command));
    if (outSize) *outSize = sizeof(command);
    return 0;
}

int APS5_VABI sceKernelWriteRemapIntoPrtCommand(std::uint32_t* dst, std::uint64_t address, std::uint64_t source, std::uint64_t size, int protection, int opcode, std::uint64_t* outSize) {
    if (!_validWriteRange(address, size) || !_validWriteRange(source, size) || !_validWriteProtection(static_cast<std::uint32_t>(protection))) return SCE_KERNEL_ERROR_EINVAL;
    const Apr::AmmRemapCommand command{{Apr::Opcode::AmmRemapIntoPrt, sizeof(command)}, address, source, size, protection, opcode ? opcode : 1011};
    if (dst) std::memcpy(dst, &command, sizeof(command));
    if (outSize) *outSize = sizeof(command);
    return 0;
}

}
