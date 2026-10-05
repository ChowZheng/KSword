#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ks::file::detail
{
    enum class FilePathQueryStatus { Ready, NonDisk, Failed, TimedOut, Cancelled };

    struct FilePathQueryResult
    {
        FilePathQueryStatus status = FilePathQueryStatus::Failed;
        std::wstring path;
    };

    // Reuse one dedicated thread for name queries. A filesystem may ignore cancellation;
    // in that case retain only owned state and cap lingering workers across all scans.
    class FileHandleQueryWorker final
    {
    public:
        using Query = std::function<FilePathQueryResult(HANDLE)>;
        explicit FileHandleQueryWorker(Query query)
        {
            unsigned count = s_liveWorkers.load();
            do
            {
                if (count >= 4U) { return; }
            } while (!s_liveWorkers.compare_exchange_weak(count, count + 1U));
            try
            {
                m_state = std::make_shared<State>();
                m_thread = std::thread([state = m_state, query = std::move(query)]()
                {
                    for (;;)
                    {
                        std::unique_lock lock(state->mutex);
                        state->condition.wait(lock, [&]() { return state->stop || state->handle != nullptr; });
                        if (state->stop && state->handle == nullptr) { break; }
                        HANDLE handle = state->handle;
                        state->handle = nullptr;
                        const bool stopped = state->stop;
                        lock.unlock();
                        FilePathQueryResult result;
                        if (!stopped)
                        {
                            try { result = query(handle); }
                            catch (...) { result.status = FilePathQueryStatus::Failed; }
                        }
                        // Include CloseHandle in the bounded operation: cleanup can also block.
                        ::CloseHandle(handle);
                        lock.lock();
                        state->result = std::move(result);
                        state->done = true;
                        state->condition.notify_all();
                        if (state->stop) { break; }
                    }
                    s_liveWorkers.fetch_sub(1U);
                });
            }
            catch (...)
            {
                s_liveWorkers.fetch_sub(1U);
                throw;
            }
        }

        ~FileHandleQueryWorker()
        {
            if (!m_thread.joinable()) { return; }
            {
                std::lock_guard lock(m_state->mutex);
                m_state->stop = true;
            }
            m_state->condition.notify_all();
            if (m_abandoned)
            {
                // Do not kill a thread executing filesystem code or wait indefinitely.
                (void)::CancelSynchronousIo(m_thread.native_handle());
                m_thread.detach();
            }
            else { m_thread.join(); }
        }

        FileHandleQueryWorker(const FileHandleQueryWorker&) = delete;
        FileHandleQueryWorker& operator=(const FileHandleQueryWorker&) = delete;
        bool available() const { return m_thread.joinable() && !m_abandoned; }

        // Caller checks available() first. Ownership transfers to the worker, including
        // on timeout; no callbacks or references to the scan/UI escape into that thread.
        FilePathQueryResult run(HANDLE ownedHandle, const std::function<bool()>& cancelled,
            std::chrono::milliseconds timeout = std::chrono::milliseconds(250))
        {
            std::unique_lock lock(m_state->mutex);
            m_state->done = false;
            m_state->handle = ownedHandle;
            m_state->condition.notify_all();
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!m_state->done)
            {
                const bool cancel = cancelled && cancelled();
                if (cancel || std::chrono::steady_clock::now() >= deadline)
                {
                    m_abandoned = true;
                    m_state->stop = true;
                    return { cancel ? FilePathQueryStatus::Cancelled : FilePathQueryStatus::TimedOut, {} };
                }
                m_state->condition.wait_until(lock,
                    (std::min)(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(20)));
            }
            return std::move(m_state->result);
        }

    private:
        struct State
        {
            std::mutex mutex;
            std::condition_variable condition;
            HANDLE handle = nullptr;
            bool done = false;
            bool stop = false;
            FilePathQueryResult result;
        };
        inline static std::atomic<unsigned> s_liveWorkers{0};
        std::shared_ptr<State> m_state;
        std::thread m_thread;
        bool m_abandoned = false;
    };
}
