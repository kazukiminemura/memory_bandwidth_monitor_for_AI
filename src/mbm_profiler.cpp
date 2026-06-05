#include "mbm/mbm_profiler.hpp"

#include <mutex>
#include <unordered_map>

namespace mbm {
namespace {

using Clock = std::chrono::steady_clock;

struct ScopeFrame {
    const char* name{nullptr};
    Clock::time_point start;
    std::uint64_t bytes{0};
};

struct MutableStat {
    std::uint64_t calls{0};
    std::uint64_t bytes{0};
    std::uint64_t total_ns{0};
};

thread_local std::vector<ScopeFrame> g_scope_stack;

class ProfilerImpl {
public:
    Clock::time_point window_start{Clock::now()};
    std::unordered_map<std::string, MutableStat> window_stats;
    std::mutex mutex;
};

ProfilerImpl& impl() {
    static ProfilerImpl state;
    return state;
}

} // namespace

Profiler& Profiler::instance() {
    static Profiler profiler;
    return profiler;
}

Profiler::Profiler() = default;
Profiler::~Profiler() = default;

void Profiler::enterScope(const char* function_name) {
    ScopeFrame frame{};
    frame.name = function_name;
    frame.start = Clock::now();
    g_scope_stack.push_back(frame);
}

void Profiler::addBytes(std::uint64_t bytes) {
    if (g_scope_stack.empty()) {
        return;
    }
    g_scope_stack.back().bytes += bytes;
}

void Profiler::leaveScope() {
    if (g_scope_stack.empty()) {
        return;
    }

    ScopeFrame frame{g_scope_stack.back()};
    g_scope_stack.pop_back();

    const auto end{Clock::now()};
    const auto elapsed_ns{std::chrono::duration_cast<std::chrono::nanoseconds>(end - frame.start).count()};

    auto& state{impl()};
    std::lock_guard<std::mutex> lock(state.mutex);
    auto& stat{state.window_stats[frame.name]};
    stat.calls += 1;
    stat.bytes += frame.bytes;
    stat.total_ns += elapsed_ns;
}

WindowSnapshot Profiler::consumeWindow() {
    WindowSnapshot snapshot{};
    auto& state{impl()};
    std::lock_guard<std::mutex> lock(state.mutex);

    snapshot.from = state.window_start;
    snapshot.to = Clock::now();
    snapshot.functions.reserve(state.window_stats.size());
    for (const auto& [name, stat] : state.window_stats) {
        FunctionWindowStat out{};
        out.name = name;
        out.calls = stat.calls;
        out.bytes = stat.bytes;
        out.total_ns = stat.total_ns;
        snapshot.functions.push_back(std::move(out));
    }

    state.window_stats.clear();
    state.window_start = snapshot.to;
    return snapshot;
}

ScopeGuard::ScopeGuard(const char* function_name) : active_{true} {
    Profiler::instance().enterScope(function_name);
}

ScopeGuard::~ScopeGuard() {
    if (active_) {
        Profiler::instance().leaveScope();
    }
}

} // namespace mbm
