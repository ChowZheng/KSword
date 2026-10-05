#include "../Ksword5.1/Ksword5.1/ksword/file/file_handle_query_worker.h"
#include <iostream>
#include <vector>

using ks::file::detail::FileHandleQueryWorker;
using ks::file::detail::FilePathQueryResult;
using ks::file::detail::FilePathQueryStatus;

int main()
{
    const auto started = std::chrono::steady_clock::now();
    // Normal queries reuse the worker and release each transferred handle.
    {
        FileHandleQueryWorker worker([](HANDLE)
        { return FilePathQueryResult{FilePathQueryStatus::Ready, L"occupied.dat"}; });
        for (int index = 0; index < 100; ++index)
        {
            HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (worker.run(handle, {}).path != L"occupied.dat") { return 1; }
            DWORD flags = 0;
            if (GetHandleInformation(handle, &flags)) { return 2; }
        }
    }
    // Simulate four drivers ignoring cancellation. Timeout must not join them,
    // and subsequent scans must not create an unbounded number of threads.
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<unsigned> entered{0};
    std::atomic<unsigned> finished{0};
    std::vector<HANDLE> handles;
    const auto blockedQuery = [release, &entered, &finished](HANDLE)
    {
        entered.fetch_add(1U);
        WaitForSingleObject(release, INFINITE);
        finished.fetch_add(1U);
        return FilePathQueryResult{FilePathQueryStatus::Ready, L"late"};
    };
    for (int index = 0; index < 4; ++index)
    {
        FileHandleQueryWorker worker(blockedQuery);
        if (!worker.available()) { return 3; }
        handles.push_back(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (worker.run(handles.back(), {}, std::chrono::milliseconds(100)).status !=
            FilePathQueryStatus::TimedOut) { return 4; }
        DWORD flags = 0;
        if (!GetHandleInformation(handles.back(), &flags)) { return 5; }
    }
    {
        FileHandleQueryWorker exhausted(blockedQuery);
        if (exhausted.available()) { return 6; }
    }
    if (entered.load() != 4U || std::chrono::steady_clock::now() - started > std::chrono::seconds(3))
        return 7;
    SetEvent(release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;)
    {
        bool allClosed = finished.load() == 4U;
        for (HANDLE handle : handles)
        {
            DWORD flags = 0;
            if (GetHandleInformation(handle, &flags)) { allClosed = false; }
        }
        if (allClosed) { break; }
        if (std::chrono::steady_clock::now() >= deadline) { return 8; }
        Sleep(5);
    }
    // Wait for the slots to be released as well as the transferred handles.
    for (;;)
    {
        FileHandleQueryWorker recovered([](HANDLE) { return FilePathQueryResult{}; });
        if (recovered.available()) { break; }
        if (std::chrono::steady_clock::now() >= deadline) { return 9; }
        Sleep(5);
    }
    // Cancellation while the query is blocked returns without using the scan
    // callback after run() returns. The worker retains only its own state.
    ResetEvent(release);
    HANDLE cancelledHandle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    {
        FileHandleQueryWorker worker(blockedQuery);
        if (!worker.available()) { return 9; }
        const auto cancelTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(40);
        if (worker.run(cancelledHandle, [cancelTime]() { return std::chrono::steady_clock::now() >= cancelTime; }).status
            != FilePathQueryStatus::Cancelled) { return 10; }
    }
    SetEvent(release);
    const auto finishDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    DWORD cancelledFlags = 0;
    while (GetHandleInformation(cancelledHandle, &cancelledFlags))
    {
        if (std::chrono::steady_clock::now() >= finishDeadline) { return 11; }
        Sleep(5);
    }
    CloseHandle(release);
    std::cout << "PASS: reuse, timeout, cancellation, worker cap, late completion, handle ownership\n";
}
