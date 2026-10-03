#include "SystemInfo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

#if defined(__APPLE__)
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/processor_info.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>
#elif defined(__linux__)
#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>
#else
#error "Simple Task Manager supports macOS and Linux."
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct RawProcess {
    int pid = 0;
    std::uint64_t startToken = 0;
    std::uint64_t cpuTime = 0;
    std::uint64_t memoryBytes = 0;
    std::string name;
};

struct CpuCounters {
    std::uint64_t total = 0;
    std::uint64_t idle = 0;
};

#if defined(__APPLE__)
bool readCpuCounters(CpuCounters& counters) {
    natural_t processorCount = 0;
    processor_info_array_t processorInfo = nullptr;
    mach_msg_type_number_t infoCount = 0;

    const kern_return_t result = host_processor_info(
        mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &processorCount,
        &processorInfo, &infoCount);
    if (result != KERN_SUCCESS || processorInfo == nullptr) {
        return false;
    }

    const auto* states = reinterpret_cast<const integer_t*>(processorInfo);
    const std::size_t availableStates =
        static_cast<std::size_t>(infoCount) / CPU_STATE_MAX;
    const std::size_t count = std::min<std::size_t>(processorCount, availableStates);

    for (std::size_t processor = 0; processor < count; ++processor) {
        const std::size_t base = processor * CPU_STATE_MAX;
        std::uint64_t processorTotal = 0;
        for (std::size_t state = 0; state < CPU_STATE_MAX; ++state) {
            processorTotal += static_cast<std::uint32_t>(states[base + state]);
        }
        counters.total += processorTotal;
        counters.idle += static_cast<std::uint32_t>(states[base + CPU_STATE_IDLE]);
    }

    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(processorInfo),
                  static_cast<vm_size_t>(infoCount) * sizeof(integer_t));
    return count > 0;
}

std::vector<RawProcess> readProcesses() {
    std::vector<RawProcess> processes;
    int requiredBytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (requiredBytes <= 0) {
        return processes;
    }

    std::vector<pid_t> pids;
    int copiedBytes = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const std::size_t capacity =
            static_cast<std::size_t>(requiredBytes) / sizeof(pid_t) + 32U;
        pids.resize(capacity);
        const auto bufferBytes = static_cast<int>(pids.size() * sizeof(pid_t));
        copiedBytes = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), bufferBytes);
        if (copiedBytes < 0) {
            return processes;
        }
        if (copiedBytes < bufferBytes) {
            break;
        }
        requiredBytes = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
        if (requiredBytes <= 0) {
            return processes;
        }
    }

    const std::size_t pidCount = std::min<std::size_t>(
        static_cast<std::size_t>(std::max(copiedBytes, 0)) / sizeof(pid_t), pids.size());
    processes.reserve(pidCount);
    for (std::size_t index = 0; index < pidCount; ++index) {
        const pid_t pid = pids[index];
        if (pid <= 0) {
            continue;
        }

        char nameBuffer[PROC_PIDPATHINFO_MAXSIZE] = {};
        if (proc_name(pid, nameBuffer, sizeof(nameBuffer)) <= 0) {
            continue;
        }

        rusage_info_v4 usage{};
        if (proc_pid_rusage(pid, RUSAGE_INFO_V4,
                            reinterpret_cast<rusage_info_t*>(&usage)) != 0) {
            continue;
        }

        proc_bsdinfo bsdInfo{};
        const int bsdInfoBytes = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0,
                                              &bsdInfo, sizeof(bsdInfo));
        const std::uint64_t startToken = bsdInfoBytes == sizeof(bsdInfo)
            ? static_cast<std::uint64_t>(bsdInfo.pbi_start_tvsec) * 1000000ULL
                + static_cast<std::uint64_t>(bsdInfo.pbi_start_tvusec)
            : 0ULL;

        RawProcess process;
        process.pid = static_cast<int>(pid);
        process.startToken = startToken;
        process.cpuTime = usage.ri_user_time + usage.ri_system_time;
        process.memoryBytes = usage.ri_phys_footprint;
        process.name = nameBuffer;
        processes.push_back(std::move(process));
    }
    return processes;
}

std::uint64_t totalPhysicalMemory() {
    std::uint64_t bytes = 0;
    std::size_t size = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) != 0) {
        return 0;
    }
    return bytes;
}

std::uint64_t usedPhysicalMemory() {
    vm_statistics64_data_t statistics{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          reinterpret_cast<host_info64_t>(&statistics), &count)
        != KERN_SUCCESS) {
        return 0;
    }

    vm_size_t pageSize = 0;
    if (host_page_size(mach_host_self(), &pageSize) != KERN_SUCCESS) {
        return 0;
    }
    const std::uint64_t usedPages = static_cast<std::uint64_t>(statistics.active_count)
        + static_cast<std::uint64_t>(statistics.wire_count)
        + static_cast<std::uint64_t>(statistics.compressor_page_count);
    return usedPages * static_cast<std::uint64_t>(pageSize);
}

constexpr double processCpuTimeScale = 1.0e-9;

#elif defined(__linux__)
bool readCpuCounters(CpuCounters& counters) {
    std::ifstream statFile("/proc/stat");
    std::string line;
    if (!std::getline(statFile, line)) {
        return false;
    }

    std::istringstream values(line);
    std::string label;
    std::array<std::uint64_t, 10> ticks{};
    values >> label;
    if (label != "cpu") {
        return false;
    }
    for (auto& tick : ticks) {
        if (!(values >> tick)) {
            break;
        }
    }

    for (const auto tick : ticks) {
        counters.total += tick;
    }
    // Linux reports user, nice, system, idle, iowait, irq, softirq and steal.
    // Idle and iowait are both time when a CPU is not doing useful work.
    counters.idle = ticks[3] + ticks[4];
    return counters.total > 0;
}

bool parsePositivePid(const std::string& name, int& pid) {
    if (name.empty()) {
        return false;
    }
    for (const char character : name) {
        if (character < '0' || character > '9') {
            return false;
        }
    }
    try {
        const long parsed = std::stol(name);
        if (parsed <= 0 || parsed > std::numeric_limits<int>::max()) {
            return false;
        }
        pid = static_cast<int>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<RawProcess> readProcesses() {
    std::vector<RawProcess> processes;
    DIR* directory = opendir("/proc");
    if (directory == nullptr) {
        return processes;
    }

    const long pageSizeResult = sysconf(_SC_PAGESIZE);
    const std::uint64_t pageSize = pageSizeResult > 0
        ? static_cast<std::uint64_t>(pageSizeResult) : 0ULL;
    dirent* entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        int pid = 0;
        if (!parsePositivePid(entry->d_name, pid)) {
            continue;
        }

        std::ifstream statFile("/proc/" + std::to_string(pid) + "/stat");
        std::string statLine;
        if (!std::getline(statFile, statLine)) {
            continue;
        }

        // The comm field is parenthesized and may itself contain spaces or ')'.
        const std::size_t openParen = statLine.find('(');
        const std::size_t closeParen = statLine.rfind(')');
        if (openParen == std::string::npos || closeParen == std::string::npos
            || closeParen <= openParen) {
            continue;
        }

        std::istringstream fields(statLine.substr(closeParen + 1));
        std::array<std::string, 22> values{};
        bool complete = true;
        for (auto& value : values) {
            if (!(fields >> value)) {
                complete = false;
                break;
            }
        }
        if (!complete) {
            continue;
        }

        try {
            // After comm: index 11/12 are utime/stime, 19 is starttime,
            // and 21 is resident pages (proc(5), fields 14/15/22/24).
            const std::uint64_t userTicks = std::stoull(values[11]);
            const std::uint64_t systemTicks = std::stoull(values[12]);
            const std::uint64_t startTime = std::stoull(values[19]);
            const long long residentPages = std::stoll(values[21]);

            RawProcess process;
            process.pid = pid;
            process.startToken = startTime;
            process.cpuTime = userTicks + systemTicks;
            process.memoryBytes = residentPages > 0 && pageSize > 0
                ? static_cast<std::uint64_t>(residentPages) * pageSize : 0ULL;
            process.name = statLine.substr(openParen + 1, closeParen - openParen - 1);
            if (process.name.empty()) {
                process.name = std::to_string(pid);
            }
            processes.push_back(std::move(process));
        } catch (...) {
            continue;
        }
    }
    closedir(directory);
    return processes;
}

bool readMemoryInfo(std::uint64_t& totalBytes, std::uint64_t& availableBytes) {
    std::ifstream memoryFile("/proc/meminfo");
    if (!memoryFile) {
        return false;
    }

    std::uint64_t freeKb = 0;
    std::uint64_t buffersKb = 0;
    std::uint64_t cachedKb = 0;
    std::uint64_t reclaimableKb = 0;
    std::uint64_t sharedKb = 0;
    std::string line;
    while (std::getline(memoryFile, line)) {
        std::istringstream fields(line);
        std::string key;
        std::uint64_t value = 0;
        if (!(fields >> key >> value)) {
            continue;
        }
        if (key == "MemTotal:") totalBytes = value * 1024ULL;
        else if (key == "MemAvailable:") availableBytes = value * 1024ULL;
        else if (key == "MemFree:") freeKb = value;
        else if (key == "Buffers:") buffersKb = value;
        else if (key == "Cached:") cachedKb = value;
        else if (key == "SReclaimable:") reclaimableKb = value;
        else if (key == "Shmem:") sharedKb = value;
    }

    if (availableBytes == 0 && totalBytes > 0) {
        const std::uint64_t fallbackKb = freeKb + buffersKb + cachedKb
            + reclaimableKb > sharedKb
            ? freeKb + buffersKb + cachedKb + reclaimableKb - sharedKb : freeKb;
        availableBytes = fallbackKb * 1024ULL;
    }
    return totalBytes > 0;
}

double linuxClockTicksPerSecond() {
    static const double ticks = [] {
        const long value = sysconf(_SC_CLK_TCK);
        return value > 0 ? static_cast<double>(value) : 100.0;
    }();
    return ticks;
}

#endif

} // namespace

SystemInfo::SystemInfo()
    : snapshot_(std::make_shared<SystemSnapshot>()) {
    worker_ = std::thread(&SystemInfo::run, this);
}

SystemInfo::~SystemInfo() {
    {
        std::lock_guard<std::mutex> lock(waitMutex_);
        stopping_ = true;
    }
    waitCondition_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

std::shared_ptr<const SystemSnapshot> SystemInfo::getSnapshot() const {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    return snapshot_;
}

void SystemInfo::run() {
    constexpr auto sampleInterval = std::chrono::seconds(1);
    while (true) {
        {
            std::lock_guard<std::mutex> lock(waitMutex_);
            if (stopping_) {
                return;
            }
        }

        auto next = std::make_shared<SystemSnapshot>(sample());
        {
            std::lock_guard<std::mutex> lock(snapshotMutex_);
            snapshot_ = std::move(next);
        }

        std::unique_lock<std::mutex> lock(waitMutex_);
        if (waitCondition_.wait_for(lock, sampleInterval, [this] { return stopping_; })) {
            return;
        }
    }
}

SystemSnapshot SystemInfo::sample() {
    SystemSnapshot snapshot;
    const auto now = Clock::now();
    CpuCounters currentCounters;

    if (readCpuCounters(currentCounters)) {
        if (hasPreviousSystemSample_ && currentCounters.total >= previousSystemTotalTicks_
            && currentCounters.idle >= previousSystemIdleTicks_) {
            const std::uint64_t totalDelta = currentCounters.total - previousSystemTotalTicks_;
            const std::uint64_t idleDelta = currentCounters.idle - previousSystemIdleTicks_;
            if (totalDelta > 0) {
                const std::uint64_t busyDelta = totalDelta > idleDelta
                    ? totalDelta - idleDelta : 0;
                snapshot.cpuUsage = 100.0 * static_cast<double>(busyDelta)
                    / static_cast<double>(totalDelta);
            }
        }
        previousSystemTotalTicks_ = currentCounters.total;
        previousSystemIdleTicks_ = currentCounters.idle;
        hasPreviousSystemSample_ = true;
    }

#if defined(__APPLE__)
    snapshot.totalMemoryBytes = totalPhysicalMemory();
    snapshot.usedMemoryBytes = usedPhysicalMemory();
    snapshot.logicalProcessorCount = std::thread::hardware_concurrency();
#elif defined(__linux__)
    std::uint64_t availableMemoryBytes = 0;
    readMemoryInfo(snapshot.totalMemoryBytes, availableMemoryBytes);
    snapshot.usedMemoryBytes = snapshot.totalMemoryBytes > availableMemoryBytes
        ? snapshot.totalMemoryBytes - availableMemoryBytes : 0;
    snapshot.logicalProcessorCount = std::thread::hardware_concurrency();
#endif

    if (snapshot.totalMemoryBytes > 0) {
        snapshot.usedMemoryBytes = std::min(snapshot.usedMemoryBytes,
                                            snapshot.totalMemoryBytes);
        snapshot.ramUsage = 100.0 * static_cast<double>(snapshot.usedMemoryBytes)
            / static_cast<double>(snapshot.totalMemoryBytes);
    }

    std::unordered_map<int, PreviousProcessSample> currentProcessSamples;
    auto rawProcesses = readProcesses();
    currentProcessSamples.reserve(rawProcesses.size());
    snapshot.processes.reserve(rawProcesses.size());

#if defined(__APPLE__)
    constexpr double cpuTimeScale = processCpuTimeScale;
#elif defined(__linux__)
    const double cpuTimeScale = 1.0 / linuxClockTicksPerSecond();
#endif

    for (auto& raw : rawProcesses) {
        ProcessInfo process;
        process.pid = raw.pid;
        process.startToken = raw.startToken;
        process.name = std::move(raw.name);
        process.memoryBytes = raw.memoryBytes;

        const auto previous = previousProcesses_.find(raw.pid);
        if (raw.startToken != 0 && previous != previousProcesses_.end()
            && previous->second.startToken == raw.startToken
            && raw.cpuTime >= previous->second.cpuTime) {
            const double processElapsed = std::chrono::duration<double>(
                now - previous->second.sampledAt).count();
            if (processElapsed > 0.0) {
                const double cpuSeconds = static_cast<double>(
                    raw.cpuTime - previous->second.cpuTime) * cpuTimeScale;
                process.cpuUsage = 100.0 * cpuSeconds / processElapsed;
                if (!std::isfinite(process.cpuUsage) || process.cpuUsage < 0.0) {
                    process.cpuUsage = 0.0;
                }
            }
        }

        currentProcessSamples.emplace(raw.pid,
            PreviousProcessSample{raw.startToken, raw.cpuTime, now});
        snapshot.processes.push_back(std::move(process));
    }
    previousProcesses_ = std::move(currentProcessSamples);

    std::sort(snapshot.processes.begin(), snapshot.processes.end(),
        [](const ProcessInfo& left, const ProcessInfo& right) {
            if (left.cpuUsage != right.cpuUsage) {
                return left.cpuUsage > right.cpuUsage;
            }
            if (left.memoryBytes != right.memoryBytes) {
                return left.memoryBytes > right.memoryBytes;
            }
            if (left.name != right.name) {
                return left.name < right.name;
            }
            return left.pid < right.pid;
        });

    return snapshot;
}
