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
    g_scope_stack.push_back({.name = function_name, .start = Clock::now()});
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

    auto frame{g_scope_stack.back()};
    g_scope_stack.pop_back();

    const auto end{Clock::now()};
    const auto elapsed_ns{std::chrono::duration_cast<std::chrono::nanoseconds>(end - frame.start).count()};

    auto& state{impl()};
    std::scoped_lock lock{state.mutex};
    auto& stat{state.window_stats[frame.name]};
    stat.calls += 1;
    stat.bytes += frame.bytes;
    stat.total_ns += elapsed_ns;
}

WindowSnapshot Profiler::consumeWindow() {
    WindowSnapshot snapshot{};
    auto& state{impl()};
    std::scoped_lock lock{state.mutex};

    snapshot.from = state.window_start;
    snapshot.to = Clock::now();
    snapshot.functions.reserve(state.window_stats.size());
    for (const auto& [name, stat] : state.window_stats) {
        snapshot.functions.push_back({name, stat.calls, stat.bytes, stat.total_ns});
    }

    state.window_stats.clear();
    state.window_start = snapshot.to;
    return snapshot;
}

ScopeGuard::ScopeGuard(const char* function_name) {
    Profiler::instance().enterScope(function_name);
}

ScopeGuard::~ScopeGuard() {
    Profiler::instance().leaveScope();
}

} // namespace mbm
