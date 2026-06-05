#include "mbm/mbm_profiler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
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
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <dxgi1_6.h>
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
    int interval_ms = 500;
    int workers = std::max(1u, std::thread::hardware_concurrency() / 2u);
    int duration_sec = 0;
    RunMode mode = RunMode::System;
    int top = 10;
    int probe_mb = 256;
    double theoretical_bandwidth_gbs = 51.2;
    std::string device = "cpu";
    int gpu_index = 0;
    bool list_devices = false;
    unsigned long pid = 0;
    std::string process_name;
};

struct ProcMemorySnapshot {
    double working_set_mb = 0.0;
    double private_mb = 0.0;
    std::uint64_t page_faults = 0;
    bool valid = false;
};

struct SystemMemorySnapshot {
    double total_mb = 0.0;
    double used_mb = 0.0;
    double available_mb = 0.0;
    double committed_mb = 0.0;
    double commit_limit_mb = 0.0;
    unsigned long memory_load_percent = 0;
    bool valid = false;
};

struct BandwidthProbeSnapshot {
    double bandwidth_mbs = 0.0;
    double seconds = 0.0;
    std::uint64_t bytes = 0;
};

struct GpuDevice {
    int index = 0;
    std::string name;
};

struct ProcessTarget {
    unsigned long pid = 0;
    std::string name;
#ifdef _WIN32
    HANDLE handle = nullptr;
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
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == INVALID_HANDLE_VALUE) {
        return;
    }

    DWORD mode = 0;
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
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

#ifdef _WIN32
std::string narrow(const wchar_t* text) {
    if (text == nullptr || text[0] == L'\0') {
        return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) {
        return {};
    }

    std::string out(static_cast<std::size_t>(required - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), required, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    const int required = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (required <= 1) {
        return {};
    }

    std::wstring out(static_cast<std::size_t>(required - 1), L'\0');
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
    IDXGIFactory1* factory = nullptr;
    if (CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)) != S_OK || factory == nullptr) {
        return devices;
    }

    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (adapter->GetDesc1(&desc) == S_OK && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            GpuDevice device;
            device.index = static_cast<int>(devices.size());
            device.name = narrow(desc.Description);
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

    const auto gpus = enumerateGpuDevices();
    if (opt.gpu_index >= 0 && opt.gpu_index < static_cast<int>(gpus.size())) {
        return "GPU[" + std::to_string(opt.gpu_index) + "] " + gpus[static_cast<std::size_t>(opt.gpu_index)].name;
    }

    return "GPU[" + std::to_string(opt.gpu_index) + "] unavailable";
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

std::string usageBar(double value, double scale, int width) {
    const double ratio = (scale <= 0.0) ? 0.0 : std::clamp(value / scale, 0.0, 1.0);
    const int filled = static_cast<int>(std::round(ratio * static_cast<double>(width)));

    std::string out = "\x1b[32m";
    for (int i = 0; i < width; ++i) {
        out += (i < filled) ? '|' : ' ';
    }
    out += "\x1b[0m";
    return out;
}

double roundedScale(double value, double floor_value) {
    const double base = std::max(value, floor_value);
    const double step = (base < 1024.0) ? 128.0 : 1024.0;
    return std::ceil(base / step) * step;
}

std::string formatNumber(double value, int precision) {
    std::ostringstream out;
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
    const double kb = static_cast<double>(bytes) / 1024.0;
    const double mb = kb / 1024.0;
    const double gb = mb / 1024.0;
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

    const auto gpus = enumerateGpuDevices();
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

ProcMemorySnapshot sampleProcessMemory(
#ifdef _WIN32
    HANDLE process
#endif
) {
    ProcMemorySnapshot snap;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)) != 0) {
        snap.working_set_mb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        snap.private_mb = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
        snap.page_faults = static_cast<std::uint64_t>(pmc.PageFaultCount);
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
    SystemMemorySnapshot snap;
#ifdef _WIN32
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem) != 0) {
        snap.total_mb = static_cast<double>(mem.ullTotalPhys) / (1024.0 * 1024.0);
        snap.available_mb = static_cast<double>(mem.ullAvailPhys) / (1024.0 * 1024.0);
        snap.used_mb = std::max(0.0, snap.total_mb - snap.available_mb);
        snap.committed_mb = static_cast<double>(mem.ullTotalPageFile - mem.ullAvailPageFile) / (1024.0 * 1024.0);
        snap.commit_limit_mb = static_cast<double>(mem.ullTotalPageFile) / (1024.0 * 1024.0);
        snap.memory_load_percent = mem.dwMemoryLoad;
        snap.valid = true;
    }
#endif
    return snap;
}

BandwidthProbeSnapshot measureSystemMemoryBandwidth(int workers, int probe_mb, int interval_ms) {
    const int worker_count = std::max(1, workers);
    const std::size_t total_bytes =
        static_cast<std::size_t>(std::max(16, probe_mb)) * 1024ULL * 1024ULL;
    const std::size_t bytes_per_worker =
        std::max<std::size_t>(4ULL * 1024ULL * 1024ULL, total_bytes / static_cast<std::size_t>(worker_count));
    const auto run_for = std::chrono::milliseconds(std::clamp(interval_ms / 3, 80, 250));

    std::atomic<std::uint64_t> touched_bytes{0};
    std::atomic<std::uint64_t> checksum_sink{0};
    const auto started = Clock::now();
    const auto deadline = started + run_for;
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(worker_count));

    for (int worker = 0; worker < worker_count; ++worker) {
        threads.emplace_back([bytes_per_worker, deadline, &touched_bytes, &checksum_sink, worker]() {
            const std::size_t words = std::max<std::size_t>(1024, bytes_per_worker / sizeof(std::uint64_t));
            std::vector<std::uint64_t> buffer(words, 0x9e3779b97f4a7c15ULL + static_cast<std::uint64_t>(worker));
            std::uint64_t local_bytes = 0;
            std::uint64_t local_checksum = 0;

            while (Clock::now() < deadline && g_running.load(std::memory_order_relaxed)) {
                for (std::size_t i = 0; i < buffer.size(); ++i) {
                    const std::uint64_t next = buffer[i] + 1U;
                    buffer[i] = next;
                    local_checksum += next;
                }
                local_bytes += static_cast<std::uint64_t>(buffer.size() * sizeof(std::uint64_t) * 2ULL);
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

    const auto ended = Clock::now();
    const double sec = std::max(1e-9, std::chrono::duration_cast<std::chrono::duration<double>>(ended - started).count());
    BandwidthProbeSnapshot snap;
    snap.bytes = touched_bytes.load(std::memory_order_relaxed);
    snap.seconds = sec;
    snap.bandwidth_mbs = toMb(snap.bytes) / sec;
    return snap;
}

#ifdef _WIN32
std::string processNameFromHandle(HANDLE process) {
    char path[MAX_PATH]{};
    DWORD size = static_cast<DWORD>(std::size(path));
    if (QueryFullProcessImageNameA(process, 0, path, &size) == 0) {
        return {};
    }

    std::string full(path, size);
    const std::size_t slash = full.find_last_of("\\/");
    if (slash == std::string::npos) {
        return full;
    }
    return full.substr(slash + 1);
}

unsigned long findProcessIdByName(const std::string& process_name) {
    const std::wstring wanted = widen(process_name);
    if (wanted.empty()) {
        return 0;
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    unsigned long pid = 0;
    if (Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            if (equalsIgnoreCase(entry.szExeFile, wanted)) {
                pid = static_cast<unsigned long>(entry.th32ProcessID);
                break;
            }
        } while (Process32NextW(snapshot, &entry) != FALSE);
    }

    CloseHandle(snapshot);
    return pid;
}

ProcessTarget openProcessTarget(const Options& opt) {
    ProcessTarget target;
    target.pid = opt.pid;
    if (target.pid == 0 && !opt.process_name.empty()) {
        target.pid = findProcessIdByName(opt.process_name);
        if (target.pid == 0) {
            std::cerr << "Process not found: " << opt.process_name << "\n";
            std::exit(1);
        }
    }

    target.handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, static_cast<DWORD>(target.pid));
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
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto readInt = [&](int& out) {
            if (i + 1 < argc) {
                out = std::max(1, std::atoi(argv[++i]));
            }
        };
        auto readNonNegativeInt = [&](int& out) {
            if (i + 1 < argc) {
                out = std::max(0, std::atoi(argv[++i]));
            }
        };

        if (a == "--interval-ms") {
            readInt(opt.interval_ms);
        } else if (a == "--workers") {
            readInt(opt.workers);
        } else if (a == "--duration-sec") {
            readInt(opt.duration_sec);
        } else if (a == "--top") {
            readInt(opt.top);
        } else if (a == "--probe-mb") {
            readInt(opt.probe_mb);
        } else if (a == "--theoretical-gbs" && i + 1 < argc) {
            opt.theoretical_bandwidth_gbs = std::max(1.0, std::atof(argv[++i]));
        } else if (a == "--device" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value == "cpu" || value == "gpu") {
                opt.device = value;
            } else {
                std::cerr << "Unknown device: " << value << " (use cpu or gpu)\n";
                std::exit(1);
            }
        } else if (a == "--gpu-index") {
            readNonNegativeInt(opt.gpu_index);
            opt.device = "gpu";
        } else if (a == "--list-devices") {
            opt.list_devices = true;
        } else if (a == "--pid" && i + 1 < argc) {
            opt.pid = std::strtoul(argv[++i], nullptr, 10);
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
    constexpr std::size_t dim = 256;
    const std::size_t rows = table.size() / dim;
    std::uniform_int_distribution<std::size_t> pick(0, rows - 1);

    std::size_t touched = 0;
    for (std::size_t i = 0; i < out.size() / dim; ++i) {
        const std::size_t row = pick(rng);
        const float* src = table.data() + row * dim;
        float* dst = out.data() + i * dim;
        std::memcpy(dst, src, dim * sizeof(float));
        touched += dim * sizeof(float) * 2ULL;
    }

    MBM_ADD_BYTES(touched);
}

void simulateLayerNorm(std::vector<float>& x, std::vector<float>& y) {
    MBM_SCOPE("simulate_layer_norm");
    const std::size_t n = x.size();
    double sum = 0.0;
    double sum2 = 0.0;
    for (float v : x) {
        sum += v;
        sum2 += static_cast<double>(v) * static_cast<double>(v);
    }
    const double mean = sum / static_cast<double>(n);
    const double var = std::max(1e-12, sum2 / static_cast<double>(n) - mean * mean);
    const double inv_std = 1.0 / std::sqrt(var + 1e-5);

    for (std::size_t i = 0; i < n; ++i) {
        y[i] = static_cast<float>((x[i] - mean) * inv_std);
    }

    MBM_ADD_BYTES(2ULL * n * sizeof(float));
}

void simulateAttentionScore(const std::vector<float>& q, const std::vector<float>& k, std::vector<float>& s, std::size_t d) {
    MBM_SCOPE("simulate_attention_score");
    const std::size_t tokens = q.size() / d;
    for (std::size_t i = 0; i < tokens; ++i) {
        float acc = 0.0f;
        const float* qi = q.data() + i * d;
        const float* ki = k.data() + i * d;
        for (std::size_t j = 0; j < d; ++j) {
            acc += qi[j] * ki[j];
        }
        s[i] = acc;
    }

    MBM_ADD_BYTES((2ULL * q.size() + s.size()) * sizeof(float));
}

void simulateMatmulTiled(const std::vector<float>& a, const std::vector<float>& b, std::vector<float>& c, std::size_t n) {
    MBM_SCOPE("simulate_matmul_tiled");
    constexpr std::size_t tile = 16;
    for (std::size_t ii = 0; ii < n; ii += tile) {
        for (std::size_t jj = 0; jj < n; jj += tile) {
            for (std::size_t kk = 0; kk < n; kk += tile) {
                const std::size_t i_max = std::min(n, ii + tile);
                const std::size_t j_max = std::min(n, jj + tile);
                const std::size_t k_max = std::min(n, kk + tile);
                for (std::size_t i = ii; i < i_max; ++i) {
                    for (std::size_t j = jj; j < j_max; ++j) {
                        float sum = c[i * n + j];
                        for (std::size_t kidx = kk; kidx < k_max; ++kidx) {
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

void demoWorker(unsigned seed) {
    std::mt19937 rng(seed);

    constexpr std::size_t embed_rows = 1 << 14;
    constexpr std::size_t embed_dim = 256;
    std::vector<float> embedding(embed_rows * embed_dim, 0.1f);
    std::vector<float> embed_out(128 * embed_dim, 0.0f);

    std::vector<float> ln_in(1 << 17, 0.2f);
    std::vector<float> ln_out(ln_in.size(), 0.0f);

    constexpr std::size_t tokens = 1024;
    constexpr std::size_t d = 128;
    std::vector<float> q(tokens * d, 0.3f);
    std::vector<float> k(tokens * d, 0.4f);
    std::vector<float> s(tokens, 0.0f);

    constexpr std::size_t n = 64;
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
                   int top_n,
                   const std::string& target_device,
                   const ProcMemorySnapshot& now_mem,
                   const ProcMemorySnapshot& prev_mem) {
    const double sec = std::max(1e-9,
                                std::chrono::duration_cast<std::chrono::duration<double>>(snap.to - snap.from).count());

    std::vector<mbm::FunctionWindowStat> rows = snap.functions;
    std::sort(rows.begin(), rows.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.bytes > rhs.bytes;
    });

    std::uint64_t total_bytes = 0;
    for (const auto& r : rows) {
        total_bytes += r.bytes;
    }

    const double total_mbs = toMb(total_bytes) / sec;
    const double page_fault_rate = static_cast<double>(now_mem.page_faults - prev_mem.page_faults) / sec;

    printTopLine("MBM live monitor", "device " + target_device);
    const double mem_scale = roundedScale(std::max(now_mem.working_set_mb, now_mem.private_mb), 512.0);
    const double bw_scale = roundedScale(total_mbs, 1024.0);
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

    const std::size_t lim = std::min(rows.size(), static_cast<std::size_t>(std::max(1, top_n)));
    for (std::size_t i = 0; i < lim; ++i) {
        const auto& r = rows[i];
        const double mb = toMb(r.bytes);
        const double mbs = mb / sec;
        const double avg_us = (r.calls == 0) ? 0.0 : static_cast<double>(r.total_ns) / static_cast<double>(r.calls) / 1000.0;

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
                         const SystemMemorySnapshot& mem,
                         const BandwidthProbeSnapshot& bw,
                         int workers,
                         int probe_mb,
                         double theoretical_bandwidth_gbs,
                         double window_sec) {
    const double theoretical_mbs = theoretical_bandwidth_gbs * 1024.0;
    const double bandwidth_ratio = (theoretical_mbs <= 0.0) ? 0.0 : bw.bandwidth_mbs / theoretical_mbs;

    printTopLine("MBM system memory monitor", "device " + target_device);
    std::cout << "Window: " << std::fixed << std::setprecision(0) << window_sec * 1000.0 << " ms"
              << "   Workers: " << workers
              << "   Probe: " << formatMegabytes(static_cast<double>(probe_mb))
              << "   Max: " << formatMegabytesPerSecond(theoretical_mbs)
              << "   Sample: " << std::setprecision(0) << bw.seconds * 1000.0 << " ms\n";

    if (!mem.valid) {
        std::cout << "System memory information is unavailable on this platform.\n";
        finishConsoleRedraw();
        return;
    }

    const double mem_scale = roundedScale(mem.total_mb, 1024.0);
    const double commit_scale = roundedScale(mem.commit_limit_mb, 1024.0);

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
    const double page_fault_rate =
        (now_mem.valid && prev_mem.valid) ? static_cast<double>(now_mem.page_faults - prev_mem.page_faults) / sec : 0.0;

    printTopLine("MBM process monitor", "device " + target_device);
    std::cout << "Target: " << target.name
              << "   PID: " << target.pid
              << "   Window: " << std::fixed << std::setprecision(0) << sec * 1000.0 << " ms\n";

    if (!now_mem.valid) {
        std::cout << "Process memory information is unavailable. The process may have exited, or access was denied.\n";
        std::cout << std::flush;
        return;
    }

    const double mem_scale = roundedScale(std::max(now_mem.working_set_mb, now_mem.private_mb), 128.0);
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

    const std::string selected_device = targetDeviceLabel(opt);

#ifdef _WIN32
    SetConsoleCtrlHandler(HandleCtrl, TRUE);
#endif

    if (hasExternalTarget(opt)) {
#ifdef _WIN32
        ProcessTarget target = openProcessTarget(opt);
        LiveConsole live_console;
        const auto started_at = Clock::now();
        auto last_sample_at = Clock::now();
        auto prev_mem = sampleProcessMemory(target.handle);

        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(opt.interval_ms));

            const auto now = Clock::now();
            const double sec = std::max(1e-9, std::chrono::duration_cast<std::chrono::duration<double>>(now - last_sample_at).count());
            const auto now_mem = sampleProcessMemory(target.handle);
            redrawConsoleView();
            printExternalProcessSnapshot(target, selected_device, now_mem, prev_mem, sec);
            prev_mem = now_mem;
            last_sample_at = now;

            DWORD exit_code = STILL_ACTIVE;
            if (GetExitCodeProcess(target.handle, &exit_code) == 0 || exit_code != STILL_ACTIVE) {
                g_running.store(false);
            }

            if (opt.duration_sec > 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started_at).count();
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
        const auto started_at = Clock::now();
        auto last_sample_at = Clock::now();

        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(opt.interval_ms));

            const auto bw = measureSystemMemoryBandwidth(opt.workers, opt.probe_mb, opt.interval_ms);
            const auto now = Clock::now();
            const auto mem = sampleSystemMemory();
            const double sec = std::max(1e-9, std::chrono::duration_cast<std::chrono::duration<double>>(now - last_sample_at).count());

            redrawConsoleView();
            printSystemSnapshot(selected_device, mem, bw, opt.workers, opt.probe_mb, opt.theoretical_bandwidth_gbs, sec);
            last_sample_at = now;

            if (opt.duration_sec > 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started_at).count();
                if (elapsed >= opt.duration_sec) {
                    g_running.store(false);
                }
            }
        }

        return 0;
    }

    std::vector<std::thread> workers;
    if (opt.mode == RunMode::Demo) {
        workers.reserve(static_cast<std::size_t>(opt.workers));
        for (int i = 0; i < opt.workers; ++i) {
            workers.emplace_back(demoWorker, 1234u + static_cast<unsigned>(i * 17));
        }
    }

    const auto started_at = Clock::now();
    auto prev_mem = sampleCurrentProcessMemory();

    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(opt.interval_ms));

        const auto snap = mbm::Profiler::instance().consumeWindow();
        const auto now_mem = sampleCurrentProcessMemory();
        redrawConsoleView();
        printSnapshot(snap, opt.top, selected_device, now_mem, prev_mem);
        prev_mem = now_mem;

        if (opt.duration_sec > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started_at).count();
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
