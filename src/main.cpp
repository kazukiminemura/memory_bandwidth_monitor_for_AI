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
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
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
    double bandwidth_mbs{0.0};
    double seconds{0.0};
    std::uint64_t bytes{0};
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

struct ProcessTarget {
    unsigned long pid{0};
    std::string name;
#ifdef _WIN32
    HANDLE handle{nullptr};
#endif
};

std::atomic<bool> g_running{true};

#ifdef _WIN32
BOOL WINAPI HandleCtrl(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        g_running.store(false);
        return TRUE;
    }
    return FALSE;
}
#endif

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

double toMb(std::uint64_t bytes) {
    return bytes / (1024.0 * 1024.0);
}

double bytesToMb(double bytes) {
    return bytes / (1024.0 * 1024.0);
}

template <typename T>
double numericToDouble(T value) {
    return value / 1.0;
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
    IDXGIFactory1* factory{nullptr};
    if (CreateDXGIFactory1(IID_PPV_ARGS(&factory)) != S_OK || factory == nullptr) {
        return devices;
    }

    IDXGIAdapter1* adapter{nullptr};
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (adapter->GetDesc1(&desc) == S_OK && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            GpuDevice device{};
            device.index = devices.size();
            device.name = narrow(desc.Description);
            device.dedicated_memory_mb = desc.DedicatedVideoMemory / (1024.0 * 1024.0);
            devices.push_back(std::move(device));
        }
        adapter->Release();
        adapter = nullptr;
    }

    factory->Release();
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
    PDH_HQUERY query{nullptr};
    PDH_HCOUNTER dedicated_counter{nullptr};
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) {
        snap.valid = snap.dedicated_total_mb > 0.0;
        return snap;
    }

    const wchar_t* counter_path{L"\\GPU Adapter Memory(*)\\Dedicated Usage"};
    if (PdhAddEnglishCounterW(query, counter_path, 0, &dedicated_counter) != ERROR_SUCCESS &&
        PdhAddCounterW(query, counter_path, 0, &dedicated_counter) != ERROR_SUCCESS) {
        PdhCloseQuery(query);
        snap.valid = snap.dedicated_total_mb > 0.0;
        return snap;
    }

    if (PdhCollectQueryData(query) == ERROR_SUCCESS) {
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
                snap.dedicated_used_mb = total_bytes / (1024.0 * 1024.0);
            }
        }
    }

    PdhCloseQuery(query);
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

void readPositiveInt(int argc, char** argv, int& i, int& out) {
    if (i + 1 >= argc) {
        return;
    }

    int value{out};
    if (parseValue(argv[++i], value)) {
        out = std::max(1, value);
    }
}

void readPositiveSize(int argc, char** argv, int& i, std::size_t& out) {
    if (i + 1 >= argc) {
        return;
    }

    std::size_t value{out};
    if (parseValue(argv[++i], value)) {
        out = std::max<std::size_t>(1, value);
    }
}

void readNonNegativeSize(int argc, char** argv, int& i, std::size_t& out) {
    if (i + 1 >= argc) {
        return;
    }

    std::size_t value{out};
    if (parseValue(argv[++i], value)) {
        out = value;
    }
}

void readPositiveDouble(int argc, char** argv, int& i, double& out) {
    if (i + 1 >= argc) {
        return;
    }

    double value{out};
    if (parseValue(argv[++i], value)) {
        out = std::max(1.0, value);
    }
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

    std::atomic<std::uint64_t> touched_bytes{0};
    std::atomic<std::uint64_t> checksum_sink{0};
    const auto started{Clock::now()};
    const auto deadline{started + run_for};
    std::vector<std::thread> threads;
    threads.reserve(worker_count);

    for (std::size_t worker{0}; worker < worker_count; ++worker) {
        threads.emplace_back([bytes_per_worker, deadline, &touched_bytes, &checksum_sink, worker]() {
            const std::size_t words{std::max<std::size_t>(1024, bytes_per_worker / sizeof(std::uint64_t))};
            std::vector<std::uint64_t> buffer(words, 0x9e3779b97f4a7c15ULL + worker);
            std::uint64_t local_bytes{0};
            std::uint64_t local_checksum{0};

            while (Clock::now() < deadline && g_running.load(std::memory_order_relaxed)) {
                for (std::size_t i{0}; i < buffer.size(); ++i) {
                    const std::uint64_t next{buffer[i] + 1U};
                    buffer[i] = next;
                    local_checksum += next;
                }
                local_bytes += buffer.size() * sizeof(std::uint64_t) * 2ULL;
            }

            touched_bytes.fetch_add(local_bytes, std::memory_order_relaxed);
            checksum_sink.fetch_xor(local_checksum, std::memory_order_relaxed);
        });
    }

    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    const auto ended{Clock::now()};
    const double sec{std::max(1e-9, std::chrono::duration_cast<std::chrono::duration<double>>(ended - started).count())};
    BandwidthProbeSnapshot snap{};
    snap.bytes = touched_bytes.load(std::memory_order_relaxed);
    snap.seconds = sec;
    snap.bandwidth_mbs = toMb(snap.bytes) / sec;
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

    HANDLE snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    unsigned long pid{0};
    if (Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            if (equalsIgnoreCase(entry.szExeFile, wanted)) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry) != FALSE);
    }

    CloseHandle(snapshot);
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

    target.handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, target.pid);
    if (target.handle == nullptr) {
        std::cerr << "Could not open process PID " << target.pid << " (error " << GetLastError() << ")\n";
        std::exit(1);
    }

    target.name = processNameFromHandle(target.handle);
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
            readPositiveInt(argc, argv, i, opt.interval_ms);
        } else if (a == "--workers") {
            readPositiveSize(argc, argv, i, opt.workers);
        } else if (a == "--duration-sec") {
            readPositiveInt(argc, argv, i, opt.duration_sec);
        } else if (a == "--top") {
            readPositiveSize(argc, argv, i, opt.top);
        } else if (a == "--probe-mb") {
            readPositiveSize(argc, argv, i, opt.probe_mb);
        } else if (a == "--theoretical-gbs" && i + 1 < argc) {
            readPositiveDouble(argc, argv, i, opt.theoretical_bandwidth_gbs);
        } else if (a == "--gpu-vram-gbs" && i + 1 < argc) {
            readPositiveDouble(argc, argv, i, opt.gpu_vram_bandwidth_gbs);
        } else if (a == "--device" && i + 1 < argc) {
            const std::string value{argv[++i]};
            if (value == "cpu" || value == "gpu") {
                opt.device = value;
            } else {
                std::cerr << "Unknown device: " << value << " (use cpu or gpu)\n";
                std::exit(1);
            }
        } else if (a == "--gpu-index") {
            readNonNegativeSize(argc, argv, i, opt.gpu_index);
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
                << "  --pid N            Monitor another process by PID instead of demo instrumentation\n"
                << "  --process-name EXE Monitor the first process with this executable name\n"
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
    const double count{numericToDouble(n)};
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

    const double total_mbs{toMb(total_bytes) / sec};
    const double page_fault_delta{numericToDouble(now_mem.page_faults - prev_mem.page_faults)};
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
        const double mb{toMb(r.bytes)};
        const double mbs{mb / sec};
        const double total_ns{numericToDouble(r.total_ns)};
        const double calls{numericToDouble(r.calls)};
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
                         double window_sec) {
    const double theoretical_mbs{theoretical_bandwidth_gbs * 1024.0};
    const double gpu_vram_mbs{gpu_vram_bandwidth_gbs * 1024.0};
    const double bandwidth_ratio{(theoretical_mbs <= 0.0) ? 0.0 : bw.bandwidth_mbs / theoretical_mbs};
    const double gpu_memory_ratio{
        (gpu_mem.dedicated_total_mb <= 0.0) ? 0.0 : gpu_mem.dedicated_used_mb / gpu_mem.dedicated_total_mb};

    printTopLine("MBM system memory monitor", "device " + target_device);
    std::cout << "Window: " << std::fixed << std::setprecision(0) << window_sec * 1000.0 << " ms"
              << "   Workers: " << workers
              << "   Probe: " << formatMegabytes(probe_mb)
              << "   Max: " << formatMegabytesPerSecond(theoretical_mbs)
              << "   Sample: " << std::setprecision(0) << bw.seconds * 1000.0 << " ms\n";

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
    std::cout << " BW   [" << usageBar(bw.bandwidth_mbs, theoretical_mbs, 40) << "] "
              << std::right << std::setw(10) << formatMegabytesPerSecond(bw.bandwidth_mbs)
              << " / " << std::setw(10) << formatMegabytesPerSecond(theoretical_mbs)
              << "  " << std::setw(4) << formatPercent(bandwidth_ratio)
              << " measured read+write\n\n";
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
    std::cout << std::left << std::setw(24) << "memory_bandwidth"
              << std::right << std::setw(18) << formatMegabytesPerSecond(bw.bandwidth_mbs)
              << "  probe read+write\n";
    std::cout << std::left << std::setw(24) << "theoretical_max"
              << std::right << std::setw(18) << formatMegabytesPerSecond(theoretical_mbs)
              << "  DDR4-3200 dual-channel\n";
    std::cout << std::left << std::setw(24) << "bandwidth_utilization"
              << std::right << std::setw(18) << formatPercent(bandwidth_ratio)
              << "  measured / theoretical\n";
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
    std::cout << std::left << std::setw(24) << "bandwidth_bytes"
              << std::right << std::setw(18) << formatBytes(bw.bytes)
              << "  touched by probe\n";
    std::cout << std::left << std::setw(24) << "physical_used"
              << std::right << std::setw(18) << formatMegabytes(mem.used_mb)
              << "  system\n";
    std::cout << std::left << std::setw(24) << "physical_available"
              << std::right << std::setw(18) << formatMegabytes(mem.available_mb)
              << "  system\n";
    std::cout << std::left << std::setw(24) << "committed"
              << std::right << std::setw(18) << formatMegabytes(mem.committed_mb)
              << "  system\n\n";
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
    std::cout << std::left << std::setw(24) << "memory_bandwidth"
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
        auto prev_mem{sampleProcessMemory(target.handle)};

        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(opt.interval_ms));

            const auto now{Clock::now()};
            const double sec{std::max(1e-9, std::chrono::duration_cast<std::chrono::duration<double>>(now - last_sample_at).count())};
            const auto now_mem{sampleProcessMemory(target.handle)};
            redrawConsoleView();
            printExternalProcessSnapshot(target, selected_device, now_mem, prev_mem, sec);
            prev_mem = now_mem;
            last_sample_at = now;

            DWORD exit_code{STILL_ACTIVE};
            if (GetExitCodeProcess(target.handle, &exit_code) == 0 || exit_code != STILL_ACTIVE) {
                g_running.store(false);
            }

            if (opt.duration_sec > 0) {
                const auto elapsed{std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started_at).count()};
                if (elapsed >= opt.duration_sec) {
                    g_running.store(false);
                }
            }
        }

        CloseHandle(target.handle);
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

    std::vector<std::thread> workers;
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

    for (auto& t : workers) {
        if (t.joinable()) {
            t.join();
        }
    }

    return 0;
}
