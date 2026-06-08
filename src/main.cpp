#include "mbm/mbm_profiler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <dxgi1_6.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <tlhelp32.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
constexpr double bytes_per_mb{1024.0 * 1024.0};

enum class RunMode {
    System,
    Demo,
};

struct Options {
    int interval_ms{500};
    std::size_t workers{std::max(1u, std::thread::hardware_concurrency() / 2u)};
    int duration_sec{0};
    RunMode mode{RunMode::System};
    std::size_t top{10};
    std::size_t probe_mb{256};
    double theoretical_bandwidth_gbs{51.2};
    double gpu_vram_bandwidth_gbs{456.0};
    std::string device{"cpu"};
    std::size_t gpu_index{0};
    bool list_devices{false};
    unsigned long pid{0};
    std::string process_name;
};

struct ProcMemorySnapshot {
    double working_set_mb{0.0};
    double private_mb{0.0};
    std::uint64_t page_faults{0};
    bool valid{false};
};

struct SystemMemorySnapshot {
    double total_mb{0.0};
    double used_mb{0.0};
    double available_mb{0.0};
    double committed_mb{0.0};
    double commit_limit_mb{0.0};
    unsigned long memory_load_percent{0};
    bool valid{false};
};

struct BandwidthProbeSnapshot {
    double read_bandwidth_mbs{0.0};
    double write_bandwidth_mbs{0.0};
    double total_bandwidth_mbs{0.0};
    double seconds{0.0};
    std::uint64_t read_bytes{0};
    std::uint64_t write_bytes{0};
};

struct GpuMemorySnapshot {
    double dedicated_used_mb{0.0};
    double dedicated_total_mb{0.0};
    bool valid{false};
};

struct GpuDevice {
    std::size_t index{0};
    std::string name;
    double dedicated_memory_mb{0.0};
};

std::atomic<bool> g_running{true};

#ifdef _WIN32
template <typename T>
struct ComReleaser {
    void operator()(T* ptr) const {
        if (ptr != nullptr) {
            ptr->Release();
        }
    }
};

template <typename T>
using ComPtr = std::unique_ptr<T, ComReleaser<T>>;

struct HandleCloser {
    void operator()(HANDLE handle) const {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};

using UniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleCloser>;

struct PdhQueryCloser {
    void operator()(PDH_HQUERY query) const {
        if (query != nullptr) {
            PdhCloseQuery(query);
        }
    }
};

using UniquePdhQuery = std::unique_ptr<std::remove_pointer_t<PDH_HQUERY>, PdhQueryCloser>;

BOOL WINAPI HandleCtrl(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        g_running.store(false);
        return TRUE;
    }
    return FALSE;
}
#endif

struct ProcessTarget {
    unsigned long pid{0};
    std::string name;
#ifdef _WIN32
    UniqueHandle handle;
#endif
};

void enableVirtualTerminalOutput() {
#ifdef _WIN32
    const HANDLE out{GetStdHandle(STD_OUTPUT_HANDLE)};
    if (out == INVALID_HANDLE_VALUE) {
        return;
    }

    DWORD mode{0};
    if (GetConsoleMode(out, &mode) == 0) {
        return;
    }

    SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
}

void resetConsoleView() {
    std::cout << "\x1b[H";
}

class LiveConsole {
public:
    LiveConsole() {
        std::cout << "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H" << std::flush;
    }

    ~LiveConsole() {
        std::cout << "\x1b[?25h\x1b[0m\x1b[?1049l" << std::flush;
    }

    LiveConsole(const LiveConsole&) = delete;
    LiveConsole& operator=(const LiveConsole&) = delete;
};

void redrawConsoleView() {
    resetConsoleView();
}

void finishConsoleRedraw() {
    std::cout << "\x1b[J" << std::flush;
}

template <typename T>
double toDouble(T value) {
    return static_cast<double>(value);
}

template <typename T>
double bytesToMb(T bytes) {
    return toDouble(bytes) / bytes_per_mb;
}

#ifdef _WIN32
std::string narrow(const wchar_t* text) {
    if (text == nullptr || text[0] == L'\0') {
        return {};
    }

    const int required{WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr)};
    if (required <= 1) {
        return {};
    }

    std::string out(required - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), required, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    const int required{MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0)};
    if (required <= 1) {
        return {};
    }

    std::wstring out(required - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), required);
    return out;
}

bool equalsIgnoreCase(const wchar_t* lhs, const std::wstring& rhs) {
    return _wcsicmp(lhs, rhs.c_str()) == 0;
}
#endif

std::vector<GpuDevice> enumerateGpuDevices() {
    std::vector<GpuDevice> devices;
#ifdef _WIN32
    IDXGIFactory1* raw_factory{nullptr};
    if (CreateDXGIFactory1(IID_PPV_ARGS(&raw_factory)) != S_OK || raw_factory == nullptr) {
        return devices;
    }
    ComPtr<IDXGIFactory1> factory{raw_factory};

    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* raw_adapter{nullptr};
        if (factory->EnumAdapters1(i, &raw_adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        ComPtr<IDXGIAdapter1> adapter{raw_adapter};
        DXGI_ADAPTER_DESC1 desc{};
        if (adapter->GetDesc1(&desc) == S_OK && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            devices.push_back({devices.size(), narrow(desc.Description), bytesToMb(desc.DedicatedVideoMemory)});
        }
    }
#endif
    return devices;
}

std::string targetDeviceLabel(const Options& opt) {
    if (opt.device == "cpu") {
        return "CPU";
    }

    const auto gpus{enumerateGpuDevices()};
    if (opt.gpu_index < gpus.size()) {
        return "GPU[" + std::to_string(opt.gpu_index) + "] " + gpus[opt.gpu_index].name;
    }

    return "GPU[" + std::to_string(opt.gpu_index) + "] unavailable";
}

std::string gpuDeviceLabel(std::size_t gpu_index) {
    const auto gpus{enumerateGpuDevices()};
    if (gpu_index < gpus.size()) {
        return "GPU[" + std::to_string(gpu_index) + "] " + gpus[gpu_index].name;
    }

    return "GPU[" + std::to_string(gpu_index) + "]";
}

double gpuDedicatedMemoryMb(std::size_t gpu_index) {
    const auto gpus{enumerateGpuDevices()};
    if (gpu_index < gpus.size()) {
        return gpus[gpu_index].dedicated_memory_mb;
    }

    return 0.0;
}

GpuMemorySnapshot sampleGpuMemory(std::size_t gpu_index) {
    GpuMemorySnapshot snap{};
    snap.dedicated_total_mb = gpuDedicatedMemoryMb(gpu_index);
#ifdef _WIN32
    PDH_HQUERY raw_query{nullptr};
    PDH_HCOUNTER dedicated_counter{nullptr};
    if (PdhOpenQueryW(nullptr, 0, &raw_query) != ERROR_SUCCESS) {
        snap.valid = snap.dedicated_total_mb > 0.0;
        return snap;
    }
    UniquePdhQuery query{raw_query};

    const wchar_t* counter_path{L"\\GPU Adapter Memory(*)\\Dedicated Usage"};
    if (PdhAddEnglishCounterW(query.get(), counter_path, 0, &dedicated_counter) != ERROR_SUCCESS &&
        PdhAddCounterW(query.get(), counter_path, 0, &dedicated_counter) != ERROR_SUCCESS) {
        snap.valid = snap.dedicated_total_mb > 0.0;
        return snap;
    }

    if (PdhCollectQueryData(query.get()) == ERROR_SUCCESS) {
        DWORD buffer_size{0};
        DWORD item_count{0};
        PDH_STATUS status{PdhGetFormattedCounterArrayW(
            dedicated_counter, PDH_FMT_DOUBLE, &buffer_size, &item_count, nullptr)};
        if (status == PDH_MORE_DATA && buffer_size > 0 && item_count > 0) {
            std::vector<unsigned char> buffer(buffer_size);
            auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
            status = PdhGetFormattedCounterArrayW(dedicated_counter, PDH_FMT_DOUBLE, &buffer_size, &item_count, items);
            if (status == ERROR_SUCCESS) {
                double total_bytes{0.0};
                for (DWORD i{0}; i < item_count; ++i) {
                    if (items[i].FmtValue.CStatus == ERROR_SUCCESS) {
                        total_bytes += std::max(0.0, items[i].FmtValue.doubleValue);
                    }
                }
                snap.dedicated_used_mb = bytesToMb(total_bytes);
            }
        }
    }
#endif
    snap.valid = snap.dedicated_total_mb > 0.0;
    return snap;
}

std::string fitText(const std::string& text, std::size_t width) {
    if (text.size() <= width) {
        return text;
    }
    if (width <= 1) {
        return text.substr(0, width);
    }
    return text.substr(0, width - 1) + "~";
}

std::string usageBar(double value, double scale, std::size_t width) {
    const double ratio{(scale <= 0.0) ? 0.0 : std::clamp(value / scale, 0.0, 1.0)};
    const double filled{std::round(ratio * width)};

    std::string out{"\x1b[32m"};
    for (std::size_t i{0}; i < width; ++i) {
        out += (i < filled) ? '|' : ' ';
    }
    out += "\x1b[0m";
    return out;
}

double roundedScale(double value, double floor_value) {
    const double base{std::max(value, floor_value)};
    const double step{(base < 1024.0) ? 128.0 : 1024.0};
    return std::ceil(base / step) * step;
}

std::string formatNumber(double value, int precision) {
    std::ostringstream out{};
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

std::string formatMegabytes(double mb) {
    if (std::abs(mb) >= 1024.0) {
        return formatNumber(mb / 1024.0, 1) + " GB";
    }
    if (std::abs(mb) >= 10.0) {
        return formatNumber(mb, 0) + " MB";
    }
    return formatNumber(mb, 1) + " MB";
}

std::string formatMegabytesPerSecond(double mbs) {
    if (std::abs(mbs) >= 1024.0) {
        return formatNumber(mbs / 1024.0, 1) + " GB/s";
    }
    if (std::abs(mbs) >= 100.0) {
        return formatNumber(mbs, 0) + " MB/s";
    }
    return formatNumber(mbs, 1) + " MB/s";
}

std::string formatBytes(std::uint64_t bytes) {
    const double kb{bytes / 1024.0};
    const double mb{kb / 1024.0};
    const double gb{mb / 1024.0};
    if (gb >= 1.0) {
        return formatNumber(gb, 1) + " GB";
    }
    if (mb >= 1.0) {
        return formatNumber(mb, 1) + " MB";
    }
    if (kb >= 1.0) {
        return formatNumber(kb, 1) + " KB";
    }
    return std::to_string(bytes) + " B";
}

std::string formatPercent(double ratio) {
    return formatNumber(std::clamp(ratio, 0.0, 999.0) * 100.0, 0) + "%";
}

void printTopLine(const std::string& title, const std::string& right) {
    std::cout << "\x1b[30;46m " << std::left << std::setw(42) << fitText(title, 42)
              << "\x1b[0m"
              << "  " << right << std::right << "\n";
}

void printDevices() {
    std::cout << "Available devices:\n";
    std::cout << "  CPU\n";

    const auto gpus{enumerateGpuDevices()};
    if (gpus.empty()) {
        std::cout << "  (no GPU devices found by DXGI)\n";
        return;
    }

    for (const auto& gpu : gpus) {
        std::cout << "  GPU[" << gpu.index << "] " << gpu.name << "\n";
    }
}

bool hasExternalTarget(const Options& opt) {
    return opt.pid != 0 || !opt.process_name.empty();
}

template <typename T>
bool parseValue(const char* text, T& out) {
    const std::string_view view{text};
    const auto* first{view.data()};
    const auto* last{view.data() + view.size()};
    const auto result{std::from_chars(first, last, out)};
    return result.ec == std::errc{} && result.ptr == last;
}

template <typename T, typename Clamp>
void readArg(int argc, char** argv, int& i, T& out, Clamp clamp) {
    if (i + 1 >= argc) {
        return;
    }

    T value{out};
    if (parseValue(argv[++i], value)) {
        out = clamp(value);
    }
}

template <typename T>
void readPositive(int argc, char** argv, int& i, T& out) {
    readArg(argc, argv, i, out, [](T value) { return std::max<T>(1, value); });
}

template <typename T>
void readValue(int argc, char** argv, int& i, T& out) {
    readArg(argc, argv, i, out, [](T value) { return value; });
}

ProcMemorySnapshot sampleProcessMemory(
#ifdef _WIN32
    HANDLE process
#endif
) {
    ProcMemorySnapshot snap{};
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)) != 0) {
        snap.working_set_mb = bytesToMb(pmc.WorkingSetSize);
        snap.private_mb = bytesToMb(pmc.PrivateUsage);
        snap.page_faults = pmc.PageFaultCount;
        snap.valid = true;
    }
#endif
    return snap;
}

ProcMemorySnapshot sampleCurrentProcessMemory() {
#ifdef _WIN32
    return sampleProcessMemory(GetCurrentProcess());
#else
    return {};
#endif
}

SystemMemorySnapshot sampleSystemMemory() {
    SystemMemorySnapshot snap{};
#ifdef _WIN32
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem) != 0) {
        snap.total_mb = bytesToMb(mem.ullTotalPhys);
        snap.available_mb = bytesToMb(mem.ullAvailPhys);
        snap.used_mb = std::max(0.0, snap.total_mb - snap.available_mb);
        snap.committed_mb = bytesToMb(mem.ullTotalPageFile - mem.ullAvailPageFile);
        snap.commit_limit_mb = bytesToMb(mem.ullTotalPageFile);
        snap.memory_load_percent = mem.dwMemoryLoad;
        snap.valid = true;
    }
#endif
    return snap;
}

BandwidthProbeSnapshot measureSystemMemoryBandwidth(std::size_t workers, std::size_t probe_mb, int interval_ms) {
    const std::size_t worker_count{std::max<std::size_t>(1, workers)};
    const std::size_t total_bytes{std::max<std::size_t>(16, probe_mb) * 1024ULL * 1024ULL};
    const std::size_t bytes_per_worker{
        std::max<std::size_t>(4ULL * 1024ULL * 1024ULL, total_bytes / worker_count)};
    const auto run_for{std::chrono::milliseconds(std::clamp(interval_ms / 3, 80, 250))};

    std::atomic<std::uint64_t> read_bytes{0};
    std::atomic<std::uint64_t> write_bytes{0};
    std::atomic<std::uint64_t> checksum_sink{0};
    const auto started{Clock::now()};

    auto run_probe_phase = [&](bool write_phase) {
        const auto deadline{Clock::now() + run_for};
        const auto phase_started{Clock::now()};
        {
            std::vector<std::jthread> threads;
            threads.reserve(worker_count);

            for (std::size_t worker{0}; worker < worker_count; ++worker) {
                threads.emplace_back([bytes_per_worker,
                                      deadline,
                                      &read_bytes,
                                      &write_bytes,
                                      &checksum_sink,
                                      worker,
                                      write_phase]() {
                    const std::size_t words{std::max<std::size_t>(1024, bytes_per_worker / sizeof(std::uint64_t))};
                    std::vector<std::uint64_t> buffer(words, 0x9e3779b97f4a7c15ULL + worker);
                    volatile std::uint64_t* memory{buffer.data()};
                    std::uint64_t local_bytes{0};
                    std::uint64_t local_checksum{0};
                    std::uint64_t value{0xbf58476d1ce4e5b9ULL + worker};

                    while (Clock::now() < deadline && g_running.load(std::memory_order_relaxed)) {
                        if (write_phase) {
                            for (std::size_t i{0}; i < buffer.size(); ++i) {
                                value += 0x9e3779b97f4a7c15ULL;
                                memory[i] = value;
                            }
                        } else {
                            for (std::size_t i{0}; i < buffer.size(); ++i) {
                                local_checksum += memory[i];
                            }
                        }
                        local_bytes += buffer.size() * sizeof(std::uint64_t);
                    }

                    if (write_phase) {
                        write_bytes.fetch_add(local_bytes, std::memory_order_relaxed);
                        checksum_sink.fetch_xor(value, std::memory_order_relaxed);
                    } else {
                        read_bytes.fetch_add(local_bytes, std::memory_order_relaxed);
                        checksum_sink.fetch_xor(local_checksum, std::memory_order_relaxed);
                    }
                });
            }
        }
        return std::max(1e-9, std::chrono::duration_cast<std::chrono::duration<double>>(Clock::now() - phase_started).count());
    };

    const double read_sec{run_probe_phase(false)};
    const double write_sec{run_probe_phase(true)};

    const auto ended{Clock::now()};
    const double sec{std::max(1e-9, std::chrono::duration_cast<std::chrono::duration<double>>(ended - started).count())};
    BandwidthProbeSnapshot snap{};
    snap.read_bytes = read_bytes.load(std::memory_order_relaxed);
    snap.write_bytes = write_bytes.load(std::memory_order_relaxed);
    snap.seconds = sec;
    snap.read_bandwidth_mbs = bytesToMb(snap.read_bytes) / read_sec;
    snap.write_bandwidth_mbs = bytesToMb(snap.write_bytes) / write_sec;
    snap.total_bandwidth_mbs = snap.read_bandwidth_mbs + snap.write_bandwidth_mbs;
    return snap;
}

#ifdef _WIN32
std::string processNameFromHandle(HANDLE process) {
    char path[MAX_PATH]{};
    DWORD size{MAX_PATH};
    if (QueryFullProcessImageNameA(process, 0, path, &size) == 0) {
        return {};
    }

    std::string full(path, size);
    const std::size_t slash{full.find_last_of("\\/")};
    if (slash == std::string::npos) {
        return full;
    }
    return full.substr(slash + 1);
}

unsigned long findProcessIdByName(const std::string& process_name) {
    const std::wstring wanted{widen(process_name)};
    if (wanted.empty()) {
        return 0;
    }

    UniqueHandle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (snapshot.get() == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    unsigned long pid{0};
    if (Process32FirstW(snapshot.get(), &entry) != FALSE) {
        do {
            if (equalsIgnoreCase(entry.szExeFile, wanted)) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot.get(), &entry) != FALSE);
    }

    return pid;
}

ProcessTarget openProcessTarget(const Options& opt) {
    ProcessTarget target{};
    target.pid = opt.pid;
    if (target.pid == 0 && !opt.process_name.empty()) {
        target.pid = findProcessIdByName(opt.process_name);
        if (target.pid == 0) {
            std::cerr << "Process not found: " << opt.process_name << "\n";
            std::exit(1);
        }
    }

    target.handle.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, target.pid));
    if (target.handle == nullptr) {
        std::cerr << "Could not open process PID " << target.pid << " (error " << GetLastError() << ")\n";
        std::exit(1);
    }

    target.name = processNameFromHandle(target.handle.get());
    if (target.name.empty()) {
        target.name = opt.process_name.empty() ? "pid " + std::to_string(target.pid) : opt.process_name;
    }
    return target;
}
#endif

void parseArgs(int argc, char** argv, Options& opt) {
    for (int i{1}; i < argc; ++i) {
        const std::string a{argv[i]};

        if (a == "--interval-ms") {
            readPositive(argc, argv, i, opt.interval_ms);
        } else if (a == "--workers") {
            readPositive(argc, argv, i, opt.workers);
        } else if (a == "--duration-sec") {
            readPositive(argc, argv, i, opt.duration_sec);
        } else if (a == "--top") {
            readPositive(argc, argv, i, opt.top);
        } else if (a == "--probe-mb") {
            readPositive(argc, argv, i, opt.probe_mb);
        } else if (a == "--theoretical-gbs" && i + 1 < argc) {
            readPositive(argc, argv, i, opt.theoretical_bandwidth_gbs);
        } else if (a == "--gpu-vram-gbs" && i + 1 < argc) {
            readPositive(argc, argv, i, opt.gpu_vram_bandwidth_gbs);
        } else if (a == "--device" && i + 1 < argc) {
            const std::string value{argv[++i]};
            if (value == "cpu" || value == "gpu") {
                opt.device = value;
            } else {
                std::cerr << "Unknown device: " << value << " (use cpu or gpu)\n";
                std::exit(1);
            }
        } else if (a == "--gpu-index") {
            readValue(argc, argv, i, opt.gpu_index);
            opt.device = "gpu";
        } else if (a == "--list-devices") {
            opt.list_devices = true;
        } else if (a == "--pid" && i + 1 < argc) {
            parseValue(argv[++i], opt.pid);
            opt.mode = RunMode::System;
        } else if (a == "--process-name" && i + 1 < argc) {
            opt.process_name = argv[++i];
            opt.mode = RunMode::System;
        } else if (a == "--demo") {
            opt.mode = RunMode::Demo;
        } else if (a == "--system") {
            opt.mode = RunMode::System;
        } else if (a == "--no-demo") {
            opt.mode = RunMode::System;
        } else if (a == "--help" || a == "-h") {
            std::cout
                << "Usage: mbm_cli [options]\n"
                << "  --interval-ms N    Refresh interval in milliseconds (default: 500)\n"
                << "  --workers N        Number of bandwidth/demo worker threads (default: half cores)\n"
                << "  --duration-sec N   Stop automatically after N seconds (default: 0 = run until Ctrl+C)\n"
                << "  --top N            Show top N functions by bytes/window (default: 10)\n"
                << "  --probe-mb N       Total memory used by the system bandwidth probe (default: 256)\n"
                << "  --theoretical-gbs N  Theoretical RAM bandwidth used as 100% in system mode (default: 51.2)\n"
                << "  --gpu-vram-gbs N   Theoretical GPU VRAM bandwidth to show in system mode (default: 456.0)\n"
                << "  --device cpu|gpu   Select target device label (default: cpu)\n"
                << "  --gpu-index N      Select GPU adapter index when --device gpu is used (default: 0)\n"
                << "  --list-devices     Show CPU/GPU devices and exit\n"
                << "  --pid N            Add process memory stats for this PID to the system monitor\n"
                << "  --process-name EXE Add process memory stats for this executable name\n"
                << "  --system           Monitor system memory and probe memory bandwidth (default)\n"
                << "  --demo             Run built-in AI-like demo workload\n"
                << "  --no-demo          Alias for --system\n"
                << "  --help             Show this help\n";
            std::exit(0);
        }
    }
}

void simulateEmbeddingLookup(std::vector<float>& table, std::vector<float>& out, std::mt19937& rng) {
    MBM_SCOPE("simulate_embedding_lookup");
    constexpr std::size_t dim{256};
    const std::size_t rows{table.size() / dim};
    std::uniform_int_distribution<std::size_t> pick(0, rows - 1);

    std::size_t touched{0};
    for (std::size_t i{0}; i < out.size() / dim; ++i) {
        const std::size_t row{pick(rng)};
        const float* src = table.data() + row * dim;
        float* dst = out.data() + i * dim;
        std::memcpy(dst, src, dim * sizeof(float));
        touched += dim * sizeof(float) * 2ULL;
    }

    MBM_ADD_BYTES(touched);
}

void simulateLayerNorm(std::vector<float>& x, std::vector<float>& y) {
    MBM_SCOPE("simulate_layer_norm");
    const std::size_t n{x.size()};
    double sum{0.0};
    double sum2{0.0};
    for (float v : x) {
        const double sample{v};
        sum += sample;
        sum2 += sample * sample;
    }
    const double count{toDouble(n)};
    const double mean{sum / count};
    const double var{std::max(1e-12, sum2 / count - mean * mean)};
    const double inv_std{1.0 / std::sqrt(var + 1e-5)};

    for (std::size_t i{0}; i < n; ++i) {
        y[i] = (x[i] - mean) * inv_std;
    }

    MBM_ADD_BYTES(2ULL * n * sizeof(float));
}

void simulateAttentionScore(const std::vector<float>& q, const std::vector<float>& k, std::vector<float>& s, std::size_t d) {
    MBM_SCOPE("simulate_attention_score");
    const std::size_t tokens{q.size() / d};
    for (std::size_t i{0}; i < tokens; ++i) {
        float acc{0.0f};
        const float* qi = q.data() + i * d;
        const float* ki = k.data() + i * d;
        for (std::size_t j{0}; j < d; ++j) {
            acc += qi[j] * ki[j];
        }
        s[i] = acc;
    }

    MBM_ADD_BYTES((2ULL * q.size() + s.size()) * sizeof(float));
}

void simulateMatmulTiled(const std::vector<float>& a, const std::vector<float>& b, std::vector<float>& c, std::size_t n) {
    MBM_SCOPE("simulate_matmul_tiled");
    constexpr std::size_t tile{16};
    for (std::size_t ii{0}; ii < n; ii += tile) {
        for (std::size_t jj{0}; jj < n; jj += tile) {
            for (std::size_t kk{0}; kk < n; kk += tile) {
                const std::size_t i_max{std::min(n, ii + tile)};
                const std::size_t j_max{std::min(n, jj + tile)};
                const std::size_t k_max{std::min(n, kk + tile)};
                for (std::size_t i{ii}; i < i_max; ++i) {
                    for (std::size_t j{jj}; j < j_max; ++j) {
                        float sum{c[i * n + j]};
                        for (std::size_t kidx{kk}; kidx < k_max; ++kidx) {
                            sum += a[i * n + kidx] * b[kidx * n + j];
                        }
                        c[i * n + j] = sum;
                    }
                }
            }
        }
    }

    MBM_ADD_BYTES((2ULL * n * n * n + n * n) * sizeof(float));
}

void demoWorker(std::size_t seed) {
    std::mt19937 rng(seed);

    constexpr std::size_t embed_rows{1 << 14};
    constexpr std::size_t embed_dim{256};
    std::vector<float> embedding(embed_rows * embed_dim, 0.1f);
    std::vector<float> embed_out(128 * embed_dim, 0.0f);

    std::vector<float> ln_in(1 << 17, 0.2f);
    std::vector<float> ln_out(ln_in.size(), 0.0f);

    constexpr std::size_t tokens{1024};
    constexpr std::size_t d{128};
    std::vector<float> q(tokens * d, 0.3f);
    std::vector<float> k(tokens * d, 0.4f);
    std::vector<float> s(tokens, 0.0f);

    constexpr std::size_t n{64};
    std::vector<float> a(n * n, 0.1f);
    std::vector<float> b(n * n, 0.1f);
    std::vector<float> c(n * n, 0.0f);

    while (g_running.load(std::memory_order_relaxed)) {
        simulateEmbeddingLookup(embedding, embed_out, rng);
        simulateLayerNorm(ln_in, ln_out);
        simulateAttentionScore(q, k, s, d);
        simulateMatmulTiled(a, b, c, n);
    }
}

void printSnapshot(const mbm::WindowSnapshot& snap,
                   std::size_t top_n,
                   const std::string& target_device,
                   const ProcMemorySnapshot& now_mem,
                   const ProcMemorySnapshot& prev_mem) {
    const double sec{std::max(1e-9,
                              std::chrono::duration_cast<std::chrono::duration<double>>(snap.to - snap.from).count())};

    std::vector<mbm::FunctionWindowStat> rows = snap.functions;
    std::sort(rows.begin(), rows.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.bytes > rhs.bytes;
    });

    std::uint64_t total_bytes{0};
    for (const auto& r : rows) {
        total_bytes += r.bytes;
    }

    const double total_mbs{bytesToMb(total_bytes) / sec};
    const double page_fault_delta{toDouble(now_mem.page_faults - prev_mem.page_faults)};
    const double page_fault_rate{page_fault_delta / sec};

    printTopLine("MBM live monitor", "device " + target_device);
    const double mem_scale{roundedScale(std::max(now_mem.working_set_mb, now_mem.private_mb), 512.0)};
    const double bw_scale{roundedScale(total_mbs, 1024.0)};
    std::cout << "Window: " << std::fixed << std::setprecision(0) << sec * 1000.0 << " ms"
              << "   Top: " << top_n
              << "   Scale: mem " << formatMegabytes(mem_scale) << ", bw " << formatMegabytesPerSecond(bw_scale) << "\n";
    std::cout << " Mem  [" << usageBar(now_mem.working_set_mb, mem_scale, 32) << "] "
              << std::right << std::setw(10) << formatMegabytes(now_mem.working_set_mb) << " working set\n";
    std::cout << " Priv [" << usageBar(now_mem.private_mb, mem_scale, 32) << "] "
              << std::right << std::setw(10) << formatMegabytes(now_mem.private_mb) << " private\n";
    std::cout << " BW   [" << usageBar(total_mbs, bw_scale, 32) << "] "
              << std::right << std::setw(10) << formatMegabytesPerSecond(total_mbs) << " total"
              << "   PF " << std::max(0.0, page_fault_rate) << "/s\n\n";

    std::cout << "\x1b[7m"
              << std::left << std::setw(4) << "#"
              << std::setw(34) << "Function"
              << std::right << std::setw(10) << "Calls"
              << std::setw(12) << "Rate"
              << std::setw(12) << "Bytes"
              << std::setw(12) << "Avg us"
              << "\x1b[0m\n";

    const std::size_t lim{std::min(rows.size(), std::max<std::size_t>(1, top_n))};
    for (std::size_t i{0}; i < lim; ++i) {
        const auto& r = rows[i];
        const double mb{bytesToMb(r.bytes)};
        const double mbs{mb / sec};
        const double total_ns{toDouble(r.total_ns)};
        const double calls{toDouble(r.calls)};
        const double avg_us{(r.calls == 0) ? 0.0 : total_ns / calls / 1000.0};

        std::cout << std::left << std::setw(4) << (i + 1)
                  << std::setw(34) << fitText(r.name, 33)
                  << std::right << std::setw(10) << r.calls
                  << std::setw(12) << formatMegabytesPerSecond(mbs)
                  << std::setw(12) << formatMegabytes(mb)
                  << std::setw(12) << std::fixed << std::setprecision(2) << avg_us
                  << "\n";
    }

    if (rows.empty()) {
        std::cout << "(no instrumented function activity in this window)\n";
    }
    std::cout << "\nCtrl+C to quit\n";
    finishConsoleRedraw();
}

void printSystemSnapshot(const std::string& target_device,
                         const std::string& gpu_label,
                         const SystemMemorySnapshot& mem,
                         const GpuMemorySnapshot& gpu_mem,
                         const BandwidthProbeSnapshot& bw,
                         std::size_t workers,
                         std::size_t probe_mb,
                         double theoretical_bandwidth_gbs,
                         double gpu_vram_bandwidth_gbs,
                         double window_sec,
                         const ProcessTarget* process_target = nullptr,
                         const ProcMemorySnapshot* process_mem = nullptr,
                         const ProcMemorySnapshot* prev_process_mem = nullptr,
                         double process_window_sec = 0.0) {
    const double theoretical_mbs{theoretical_bandwidth_gbs * 1024.0};
    const double gpu_vram_mbs{gpu_vram_bandwidth_gbs * 1024.0};
    const double read_bandwidth_ratio{(theoretical_mbs <= 0.0) ? 0.0 : bw.read_bandwidth_mbs / theoretical_mbs};
    const double write_bandwidth_ratio{(theoretical_mbs <= 0.0) ? 0.0 : bw.write_bandwidth_mbs / theoretical_mbs};
    const double total_bandwidth_ratio{(theoretical_mbs <= 0.0) ? 0.0 : bw.total_bandwidth_mbs / theoretical_mbs};
    const double gpu_memory_ratio{
        (gpu_mem.dedicated_total_mb <= 0.0) ? 0.0 : gpu_mem.dedicated_used_mb / gpu_mem.dedicated_total_mb};

    printTopLine("MBM system memory monitor", "device " + target_device);
    std::cout << "Window: " << std::fixed << std::setprecision(0) << window_sec * 1000.0 << " ms"
              << "   Workers: " << workers
              << "   Probe: " << formatMegabytes(probe_mb)
              << "   Max: " << formatMegabytesPerSecond(theoretical_mbs)
              << "   Sample: " << std::setprecision(0) << bw.seconds * 1000.0 << " ms\n";
    if (process_target != nullptr) {
        std::cout << "Target: " << process_target->name
                  << "   PID: " << process_target->pid << "\n";
    }

    if (!mem.valid) {
        std::cout << "System memory information is unavailable on this platform.\n";
        finishConsoleRedraw();
        return;
    }

    const double mem_scale{roundedScale(mem.total_mb, 1024.0)};
    const double commit_scale{roundedScale(mem.commit_limit_mb, 1024.0)};

    std::cout << " RAM  [" << usageBar(mem.used_mb, mem_scale, 40) << "] "
              << std::right << std::setw(10) << formatMegabytes(mem.used_mb)
              << " / " << std::setw(10) << formatMegabytes(mem.total_mb) << "  load " << mem.memory_load_percent << "%\n";
    std::cout << " Free [" << usageBar(mem.available_mb, mem_scale, 40) << "] "
              << std::right << std::setw(10) << formatMegabytes(mem.available_mb)
              << " available\n";
    std::cout << " Cmit [" << usageBar(mem.committed_mb, commit_scale, 40) << "] "
              << std::right << std::setw(10) << formatMegabytes(mem.committed_mb)
              << " / " << std::setw(10) << formatMegabytes(mem.commit_limit_mb) << " committed\n";
    std::cout << " Read [" << usageBar(bw.read_bandwidth_mbs, theoretical_mbs, 40) << "] "
              << std::right << std::setw(10) << formatMegabytesPerSecond(bw.read_bandwidth_mbs)
              << " / " << std::setw(10) << formatMegabytesPerSecond(theoretical_mbs)
              << "  " << std::setw(4) << formatPercent(read_bandwidth_ratio)
              << " measured read\n";
    std::cout << "Write [" << usageBar(bw.write_bandwidth_mbs, theoretical_mbs, 40) << "] "
              << std::right << std::setw(10) << formatMegabytesPerSecond(bw.write_bandwidth_mbs)
              << " / " << std::setw(10) << formatMegabytesPerSecond(theoretical_mbs)
              << "  " << std::setw(4) << formatPercent(write_bandwidth_ratio)
              << " measured write\n\n";
    if (gpu_mem.valid) {
        std::cout << " VRAM [" << usageBar(gpu_mem.dedicated_used_mb, gpu_mem.dedicated_total_mb, 40) << "] "
                  << std::right << std::setw(10) << formatMegabytes(gpu_mem.dedicated_used_mb)
                  << " / " << std::setw(10) << formatMegabytes(gpu_mem.dedicated_total_mb)
                  << "  " << std::setw(4) << formatPercent(gpu_memory_ratio)
                  << " dedicated usage\n";
    } else {
        std::cout << " VRAM [" << usageBar(0.0, 1.0, 40) << "] "
                  << std::right << std::setw(10) << "--"
                  << " / " << std::setw(10) << "--"
                  << "  usage unavailable\n";
    }
    std::cout << " VBW  [" << usageBar(gpu_vram_mbs, gpu_vram_mbs, 40) << "] "
              << std::right << std::setw(10) << formatMegabytesPerSecond(gpu_vram_mbs)
              << " theoretical " << fitText(gpu_label, 28) << "\n\n";

    std::cout << "\x1b[7m"
              << std::left << std::setw(24) << "Metric"
              << std::right << std::setw(18) << "Value"
              << "  Status"
              << "\x1b[0m\n";
    std::cout << std::left << std::setw(24) << "memory_read_bandwidth"
              << std::right << std::setw(18) << formatMegabytesPerSecond(bw.read_bandwidth_mbs)
              << "  probe read\n";
    std::cout << std::left << std::setw(24) << "memory_write_bandwidth"
              << std::right << std::setw(18) << formatMegabytesPerSecond(bw.write_bandwidth_mbs)
              << "  probe write\n";
    std::cout << std::left << std::setw(24) << "memory_total_bandwidth"
              << std::right << std::setw(18) << formatMegabytesPerSecond(bw.total_bandwidth_mbs)
              << "  probe read + write\n";
    std::cout << std::left << std::setw(24) << "theoretical_max"
              << std::right << std::setw(18) << formatMegabytesPerSecond(theoretical_mbs)
              << "  DDR4-3200 dual-channel\n";
    std::cout << std::left << std::setw(24) << "bandwidth_utilization"
              << std::right << std::setw(18) << formatPercent(total_bandwidth_ratio)
              << "  total / theoretical\n";
    std::cout << std::left << std::setw(24) << "gpu_vram_bandwidth"
              << std::right << std::setw(18) << formatMegabytesPerSecond(gpu_vram_mbs)
              << "  theoretical " << fitText(gpu_label, 24) << "\n";
    std::cout << std::left << std::setw(24) << "gpu_vram_used"
              << std::right << std::setw(18)
              << (gpu_mem.valid ? formatMegabytes(gpu_mem.dedicated_used_mb) : "--")
              << "  dedicated usage\n";
    std::cout << std::left << std::setw(24) << "gpu_vram_total"
              << std::right << std::setw(18)
              << (gpu_mem.valid ? formatMegabytes(gpu_mem.dedicated_total_mb) : "--")
              << "  dedicated capacity\n";
    std::cout << std::left << std::setw(24) << "gpu_vram_utilization"
              << std::right << std::setw(18)
              << (gpu_mem.valid ? formatPercent(gpu_memory_ratio) : "--")
              << "  used / dedicated\n";
    std::cout << std::left << std::setw(24) << "read_bytes"
              << std::right << std::setw(18) << formatBytes(bw.read_bytes)
              << "  touched by probe\n";
    std::cout << std::left << std::setw(24) << "write_bytes"
              << std::right << std::setw(18) << formatBytes(bw.write_bytes)
              << "  touched by probe\n";
    std::cout << std::left << std::setw(24) << "physical_used"
              << std::right << std::setw(18) << formatMegabytes(mem.used_mb)
              << "  system\n";
    std::cout << std::left << std::setw(24) << "physical_available"
              << std::right << std::setw(18) << formatMegabytes(mem.available_mb)
              << "  system\n";
    std::cout << std::left << std::setw(24) << "committed"
              << std::right << std::setw(18) << formatMegabytes(mem.committed_mb)
              << "  system\n";
    if (process_target != nullptr && process_mem != nullptr) {
        if (process_mem->valid) {
            const double process_sec{std::max(1e-9, process_window_sec)};
            const double page_fault_rate{
                (prev_process_mem != nullptr && prev_process_mem->valid)
                    ? (process_mem->page_faults - prev_process_mem->page_faults) / process_sec
                    : 0.0};
            std::cout << std::left << std::setw(24) << "target_working_set"
                      << std::right << std::setw(18) << formatMegabytes(process_mem->working_set_mb)
                      << "  " << fitText(process_target->name, 24) << "\n";
            std::cout << std::left << std::setw(24) << "target_private_bytes"
                      << std::right << std::setw(18) << formatMegabytes(process_mem->private_mb)
                      << "  " << fitText(process_target->name, 24) << "\n";
            std::cout << std::left << std::setw(24) << "target_page_fault_rate"
                      << std::right << std::setw(14) << std::fixed << std::setprecision(1) << std::max(0.0, page_fault_rate)
                      << " /s  sampled\n";
        } else {
            std::cout << std::left << std::setw(24) << "target_process"
                      << std::right << std::setw(18) << "--"
                      << "  unavailable\n";
        }
    }
    std::cout << "\n";
    std::cout << "Ctrl+C to quit\n";
    finishConsoleRedraw();
}

void printExternalProcessSnapshot(const ProcessTarget& target,
                                  const std::string& target_device,
                                  const ProcMemorySnapshot& now_mem,
                                  const ProcMemorySnapshot& prev_mem,
                                  double sec) {
    const double page_fault_rate{
        (now_mem.valid && prev_mem.valid) ? (now_mem.page_faults - prev_mem.page_faults) / sec : 0.0};

    printTopLine("MBM process monitor", "device " + target_device);
    std::cout << "Target: " << target.name
              << "   PID: " << target.pid
              << "   Window: " << std::fixed << std::setprecision(0) << sec * 1000.0 << " ms\n";

    if (!now_mem.valid) {
        std::cout << "Process memory information is unavailable. The process may have exited, or access was denied.\n";
        std::cout << std::flush;
        return;
    }

    const double mem_scale{roundedScale(std::max(now_mem.working_set_mb, now_mem.private_mb), 128.0)};
    std::cout << "Scale: " << formatMegabytes(mem_scale) << "\n";
    std::cout << " Mem  [" << usageBar(now_mem.working_set_mb, mem_scale, 40) << "] "
              << std::right << std::setw(10) << formatMegabytes(now_mem.working_set_mb) << " working set\n";
    std::cout << " Priv [" << usageBar(now_mem.private_mb, mem_scale, 40) << "] "
              << std::right << std::setw(10) << formatMegabytes(now_mem.private_mb) << " private bytes\n";
    std::cout << " PF   [" << usageBar(std::min(std::max(0.0, page_fault_rate), 1000.0), 1000.0, 40) << "] "
              << std::right << std::setw(8) << std::fixed << std::setprecision(1) << std::max(0.0, page_fault_rate) << " /s page faults\n\n";

    std::cout << "\x1b[7m"
              << std::left << std::setw(24) << "Metric"
              << std::right << std::setw(18) << "Value"
              << "  Status"
              << "\x1b[0m\n";
    std::cout << std::left << std::setw(24) << "working_set"
              << std::right << std::setw(18) << formatMegabytes(now_mem.working_set_mb)
              << "  sampled\n";
    std::cout << std::left << std::setw(24) << "private_bytes"
              << std::right << std::setw(18) << formatMegabytes(now_mem.private_mb)
              << "  sampled\n";
    std::cout << std::left << std::setw(24) << "page_fault_rate"
              << std::right << std::setw(14) << std::fixed << std::setprecision(1) << std::max(0.0, page_fault_rate)
              << " /s  sampled\n";
    std::cout << std::left << std::setw(24) << "memory_read_bandwidth"
              << std::right << std::setw(18) << "--"
              << "  unsupported without hardware counters\n";
    std::cout << std::left << std::setw(24) << "memory_write_bandwidth"
              << std::right << std::setw(18) << "--"
              << "  unsupported without hardware counters\n\n";
    std::cout << "Ctrl+C to quit\n";
    finishConsoleRedraw();
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    parseArgs(argc, argv, opt);
    enableVirtualTerminalOutput();

    if (opt.list_devices) {
        printDevices();
        return 0;
    }

    const std::string selected_device{targetDeviceLabel(opt)};
    const std::string selected_gpu{gpuDeviceLabel(opt.gpu_index)};

#ifdef _WIN32
    SetConsoleCtrlHandler(HandleCtrl, TRUE);
#endif

    if (hasExternalTarget(opt)) {
#ifdef _WIN32
        ProcessTarget target{openProcessTarget(opt)};
        LiveConsole live_console;
        const auto started_at{Clock::now()};
        auto last_sample_at{Clock::now()};
        auto prev_mem{sampleProcessMemory(target.handle.get())};

        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(opt.interval_ms));

            const auto bw{measureSystemMemoryBandwidth(opt.workers, opt.probe_mb, opt.interval_ms)};
            const auto now{Clock::now()};
            const double sec{std::max(1e-9, std::chrono::duration_cast<std::chrono::duration<double>>(now - last_sample_at).count())};
            const auto mem{sampleSystemMemory()};
            const auto gpu_mem{sampleGpuMemory(opt.gpu_index)};
            const auto now_mem{sampleProcessMemory(target.handle.get())};
            redrawConsoleView();
            printSystemSnapshot(selected_device,
                                selected_gpu,
                                mem,
                                gpu_mem,
                                bw,
                                opt.workers,
                                opt.probe_mb,
                                opt.theoretical_bandwidth_gbs,
                                opt.gpu_vram_bandwidth_gbs,
                                sec,
                                &target,
                                &now_mem,
                                &prev_mem,
                                sec);
            prev_mem = now_mem;
            last_sample_at = now;

            DWORD exit_code{STILL_ACTIVE};
            if (GetExitCodeProcess(target.handle.get(), &exit_code) == 0 || exit_code != STILL_ACTIVE) {
                g_running.store(false);
            }

            if (opt.duration_sec > 0) {
                const auto elapsed{std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started_at).count()};
                if (elapsed >= opt.duration_sec) {
                    g_running.store(false);
                }
            }
        }

        return 0;
#else
        std::cerr << "External process monitoring is currently implemented on Windows only.\n";
        return 1;
#endif
    }

    LiveConsole live_console;
    if (opt.mode == RunMode::System) {
        const auto started_at{Clock::now()};
        auto last_sample_at{Clock::now()};

        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(opt.interval_ms));

            const auto bw{measureSystemMemoryBandwidth(opt.workers, opt.probe_mb, opt.interval_ms)};
            const auto now{Clock::now()};
            const auto mem{sampleSystemMemory()};
            const auto gpu_mem{sampleGpuMemory(opt.gpu_index)};
            const double sec{std::max(1e-9, std::chrono::duration_cast<std::chrono::duration<double>>(now - last_sample_at).count())};

            redrawConsoleView();
            printSystemSnapshot(selected_device,
                                selected_gpu,
                                mem,
                                gpu_mem,
                                bw,
                                opt.workers,
                                opt.probe_mb,
                                opt.theoretical_bandwidth_gbs,
                                opt.gpu_vram_bandwidth_gbs,
                                sec);
            last_sample_at = now;

            if (opt.duration_sec > 0) {
                const auto elapsed{std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started_at).count()};
                if (elapsed >= opt.duration_sec) {
                    g_running.store(false);
                }
            }
        }

        return 0;
    }

    std::vector<std::jthread> workers;
    if (opt.mode == RunMode::Demo) {
        workers.reserve(opt.workers);
        for (std::size_t i{0}; i < opt.workers; ++i) {
            workers.emplace_back(demoWorker, 1234u + i * 17u);
        }
    }

    const auto started_at{Clock::now()};
    auto prev_mem{sampleCurrentProcessMemory()};

    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(opt.interval_ms));

        const auto snap{mbm::Profiler::instance().consumeWindow()};
        const auto now_mem{sampleCurrentProcessMemory()};
        redrawConsoleView();
        printSnapshot(snap, opt.top, selected_device, now_mem, prev_mem);
        prev_mem = now_mem;

        if (opt.duration_sec > 0) {
            const auto elapsed{std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started_at).count()};
            if (elapsed >= opt.duration_sec) {
                g_running.store(false);
            }
        }
    }

    return 0;
}
