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

// a worker that wakes late must not take an index from the next dispatch, so an
// index is only handed out while the generation it woke for is still current
bool ThreadPool::claim(const uint32_t generation, const uint32_t count, uint32_t& index) {
    uint64_t t = m_ticket.load();
    while (true) {
        if (uint32_t(t >> 32) != generation || uint32_t(t) >= count) return false;
        if (m_ticket.compare_exchange_weak(t, t + 1)) { index = uint32_t(t); return true; }
    }
}

void ThreadPool::workerLoop() {
    uint64_t seen = 0;

    while (true) {
        const Job* job   = nullptr;
        uint32_t   count = 0;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [&] { return m_stop || m_generation != seen; });
            if (m_stop) return;
            seen  = m_generation;
            job   = m_job;
            count = m_count;
            ++m_active;
        }

        uint32_t i = 0;
        while (job && claim(uint32_t(seen), count, i)) (*job)(i);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (--m_active == 0) m_done.notify_one();
        }
    }
}

void ThreadPool::dispatch(uint32_t count, const Job& job) {
    if (count == 0) return;

    uint32_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_job      = &job;
        m_count    = count;
        ++m_generation;
        generation = uint32_t(m_generation);
        m_ticket   = uint64_t(generation) << 32;
    }
    m_wake.notify_all();

    uint32_t i = 0;
    while (claim(generation, count, i)) job(i);

    std::unique_lock<std::mutex> lock(m_mutex);
    m_done.wait(lock, [&] { return m_active == 0; });
    m_job = nullptr; 
}

}