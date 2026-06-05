#include "mbm/mbm_profiler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    int interval_ms = 500;
    int workers = std::max(1u, std::thread::hardware_concurrency() / 2u);
    int duration_sec = 0;
    bool demo = true;
    int top = 10;
};

struct ProcMemorySnapshot {
    double working_set_mb = 0.0;
    double private_mb = 0.0;
    std::uint64_t page_faults = 0;
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

double toMb(std::uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

ProcMemorySnapshot sampleProcessMemory() {
    ProcMemorySnapshot snap;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)) != 0) {
        snap.working_set_mb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        snap.private_mb = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
        snap.page_faults = static_cast<std::uint64_t>(pmc.PageFaultCount);
    }
#endif
    return snap;
}

void parseArgs(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto readInt = [&](int& out) {
            if (i + 1 < argc) {
                out = std::max(1, std::atoi(argv[++i]));
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
        } else if (a == "--demo") {
            opt.demo = true;
        } else if (a == "--no-demo") {
            opt.demo = false;
        } else if (a == "--help" || a == "-h") {
            std::cout
                << "Usage: mbm_cli [options]\n"
                << "  --interval-ms N    Refresh interval in milliseconds (default: 500)\n"
                << "  --workers N        Number of demo worker threads (default: half cores)\n"
                << "  --duration-sec N   Stop automatically after N seconds (default: 0 = run until Ctrl+C)\n"
                << "  --top N            Show top N functions by bytes/window (default: 10)\n"
                << "  --demo             Run built-in AI-like demo workload (default)\n"
                << "  --no-demo          Disable demo workload\n"
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

    std::cout << "window=" << std::fixed << std::setprecision(0) << sec * 1000.0 << "ms"
              << " total=" << std::setprecision(1) << total_mbs << " MB/s"
              << " ws=" << now_mem.working_set_mb << " MB"
              << " private=" << now_mem.private_mb << " MB"
              << " pf_rate=" << std::max(0.0, page_fault_rate) << "/s\n";

    std::cout << "--------------------------------------------------------------------------------\n";
    std::cout << std::left
              << std::setw(34) << "Function"
              << std::right
              << std::setw(10) << "Calls"
              << std::setw(12) << "MB/s"
              << std::setw(12) << "MB"
              << std::setw(12) << "Avg us"
              << "\n";

    const std::size_t lim = std::min(rows.size(), static_cast<std::size_t>(std::max(1, top_n)));
    for (std::size_t i = 0; i < lim; ++i) {
        const auto& r = rows[i];
        const double mb = toMb(r.bytes);
        const double mbs = mb / sec;
        const double avg_us = (r.calls == 0) ? 0.0 : static_cast<double>(r.total_ns) / static_cast<double>(r.calls) / 1000.0;

        std::cout << std::left << std::setw(34) << r.name
                  << std::right << std::setw(10) << r.calls
                  << std::setw(12) << std::fixed << std::setprecision(1) << mbs
                  << std::setw(12) << std::fixed << std::setprecision(1) << mb
                  << std::setw(12) << std::fixed << std::setprecision(2) << avg_us
                  << "\n";
    }

    if (rows.empty()) {
        std::cout << "(no instrumented function activity in this window)\n";
    }
    std::cout << std::flush;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    parseArgs(argc, argv, opt);

#ifdef _WIN32
    SetConsoleCtrlHandler(HandleCtrl, TRUE);
#endif

    std::vector<std::thread> workers;
    if (opt.demo) {
        workers.reserve(static_cast<std::size_t>(opt.workers));
        for (int i = 0; i < opt.workers; ++i) {
            workers.emplace_back(demoWorker, 1234u + static_cast<unsigned>(i * 17));
        }
    }

    const auto started_at = Clock::now();
    auto prev_mem = sampleProcessMemory();

    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(opt.interval_ms));

        const auto snap = mbm::Profiler::instance().consumeWindow();
        const auto now_mem = sampleProcessMemory();
        printSnapshot(snap, opt.top, now_mem, prev_mem);
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
