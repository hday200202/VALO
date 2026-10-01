#include "ThreadPool.hpp"

namespace valo {

ThreadPool::ThreadPool(uint32_t threads) {
    if (threads == 0) threads = 1;
    for (uint32_t i = 1; i < threads; ++i)
        m_workers.emplace_back([this] { workerLoop(); });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    for (auto& t : m_workers) t.join();
}

void ThreadPool::workerLoop() {
    uint64_t seen = 0;

    while (true) {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [&] { return m_stop || m_generation != seen; });
            if (m_stop) return;
            seen = m_generation;
            ++m_active;
        }

        while (true) {
            uint32_t i = m_next.fetch_add(1);
            if (i >= m_count) break;
            (*m_job)(i);
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (--m_active == 0) m_done.notify_one();
        }
    }
}

void ThreadPool::dispatch(uint32_t count, const Job& job) {
    if (count == 0) return;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_job = &job;
        m_count = count;
        m_next = 0;
        ++m_generation;
    }
    m_wake.notify_all();

    while (true) {
        uint32_t i = m_next.fetch_add(1);
        if (i >= m_count) break;
        job(i);
    }

    std::unique_lock<std::mutex> lock(m_mutex);
    m_done.wait(lock, [&] { return m_active == 0; });
    m_job = nullptr; 
}

}