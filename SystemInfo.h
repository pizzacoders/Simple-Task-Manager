#ifndef SYSTEMINFO_H
#define SYSTEMINFO_H

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct ProcessInfo {
    int pid = 0;
    std::uint64_t startToken = 0;
    std::string name;
    double cpuUsage = 0.0;
    std::uint64_t memoryBytes = 0;
};

struct SystemSnapshot {
    double cpuUsage = 0.0;
    double ramUsage = 0.0;
    std::uint64_t usedMemoryBytes = 0;
    std::uint64_t totalMemoryBytes = 0;
    unsigned int logicalProcessorCount = 0;
    std::vector<ProcessInfo> processes;
};

// Samples operating-system statistics on a background thread. UI code can
// keep a shared snapshot without copying the process list on every frame.
class SystemInfo {
public:
    SystemInfo();
    ~SystemInfo();

    SystemInfo(const SystemInfo&) = delete;
    SystemInfo& operator=(const SystemInfo&) = delete;

    std::shared_ptr<const SystemSnapshot> getSnapshot() const;

private:
    struct PreviousProcessSample {
        std::uint64_t startToken = 0;
        std::uint64_t cpuTime = 0;
        std::chrono::steady_clock::time_point sampledAt{};
    };

    void run();
    SystemSnapshot sample();

    mutable std::mutex snapshotMutex_;
    std::shared_ptr<const SystemSnapshot> snapshot_;
    std::mutex waitMutex_;
    std::condition_variable waitCondition_;
    bool stopping_ = false;

    std::unordered_map<int, PreviousProcessSample> previousProcesses_;
    std::uint64_t previousSystemTotalTicks_ = 0;
    std::uint64_t previousSystemIdleTicks_ = 0;
    bool hasPreviousSystemSample_ = false;
    std::thread worker_;
};

#endif
