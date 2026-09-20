#pragma once

#include <thread>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <cstdint>

namespace valo {

class ThreadPool {
public:
    using Job = std::function<void(uint32_t i)>;

    explicit ThreadPool(uint32_t threads = std::thread::hardware_concurrency());
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void dispatch(uint32_t count, const Job& job);
    uint32_t size() const { return uint32_t(m_workers.size()) + 1; }

private:
    void workerLoop();

    std::vector<std::thread>    m_workers;
    std::mutex                  m_mutex;
    std::condition_variable     m_wake;
    std::condition_variable     m_done;

    const Job*                  m_job           = nullptr;
    std::atomic<uint32_t>       m_count         = 0;
    std::atomic<uint32_t>       m_next          = 0;
    uint32_t                    m_active        = 0;
    uint64_t                    m_generation    = 0;
    bool                        m_stop          = false;
};

}