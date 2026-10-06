"""Exercise the production AsyncSnapshotTask with an isolated message queue.

Uses real worker threads and the real header, replacing only PostMessageW.
Creates no window, starts no hook, and accesses no driver or target process.
--prove-regression also runs the same fixture against the pre-fix Git header.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import uuid


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "KswordARKLight/Ui/AsyncTask.h"

FIXTURE = r'''
#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

struct Message { HWND owner; UINT message; WPARAM task; LPARAM generation; };
static std::mutex queueMutex;
static std::condition_variable queueCondition;
static std::deque<Message> messages;
static unsigned postFailures;
static bool pausePost, enteredPost, releasePost;
static BOOL TestPostMessageW(HWND owner, UINT message, WPARAM task, LPARAM generation) {
    std::unique_lock lock(queueMutex);
    if (pausePost) {
        enteredPost = true;
        queueCondition.notify_all();
        if (!queueCondition.wait_for(lock, std::chrono::seconds(3), [] { return releasePost; }))
            throw std::runtime_error("publish gate timed out");
        pausePost = false;
    }
    if (postFailures) { --postFailures; return FALSE; }
    messages.push_back({owner, message, task, generation});
    queueCondition.notify_all();
    return TRUE;
}
#define PostMessageW TestPostMessageW
#include "@HEADER@"
#undef PostMessageW

using Ksword::Ui::AsyncSnapshotTask;
static unsigned checks;
static void check(bool condition, const char* label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}
static Message nextMessage() {
    std::unique_lock lock(queueMutex);
    if (!queueCondition.wait_for(lock, std::chrono::seconds(3), [] { return !messages.empty(); }))
        throw std::runtime_error("completion timed out");
    const auto message = messages.front(); messages.pop_front(); return message;
}
template<class Result> static void consume(AsyncSnapshotTask<Result>& task, const Message& message) {
    check(task.consume(message.owner, message.task, message.generation), "message handled");
}
template<class Predicate> static void await(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("worker timed out");
        std::this_thread::yield();
    }
}
struct Gate {
    std::mutex mutex; std::condition_variable condition; bool entered = false, ready = false;
    void block() {
        std::unique_lock lock(mutex); entered = true; condition.notify_all();
        if (!condition.wait_for(lock, std::chrono::seconds(3), [this] { return ready; }))
            throw std::runtime_error("work gate timed out");
    }
    void wait() {
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, std::chrono::seconds(3), [this] { return entered; }))
            throw std::runtime_error("work did not start");
    }
    void release() { std::lock_guard lock(mutex); ready = true; condition.notify_all(); }
};
static std::atomic_int liveResults, wrongDestructors;
struct FirstResult {
    int marker = 1;
    FirstResult() { ++liveResults; }
    FirstResult(const FirstResult&) = delete;
    FirstResult(FirstResult&& other) noexcept : marker(other.marker) { other.marker = 0; }
    ~FirstResult() { if (marker) --liveResults; }
};
struct DifferentResult {
    int marker = 0;
    ~DifferentResult() { if (marker) ++wrongDestructors; }
};
static const HWND owner = reinterpret_cast<HWND>(static_cast<UINT_PTR>(0x1234));
static constexpr UINT completionMessage = WM_APP + 670; // USB/connection pages share this ID.

static void normalAndCoalesced() {
    AsyncSnapshotTask<int> task(owner, completionMessage);
    unsigned deliveries = 0;
    task.request([] { return 42; }, [&](auto generation, auto&& result, auto error) {
        check(generation == 1 && result && *result == 42 && !error, "normal result"); ++deliveries;
    });
    const auto message = nextMessage(); consume(task, message);
    check(deliveries == 1 && !task.running(), "normal task retires");

    task.request([] { return 43; }, [&](auto generation, auto&& result, auto error) {
        check(generation == 2 && result && *result == 43 && !error, "new generation result"); ++deliveries;
    });
    const auto newer = nextMessage();
    consume(task, message); // Delayed duplicate of an already consumed generation.
    check(deliveries == 1 && task.running(), "old message cannot consume a newer result");
    consume(task, newer);
    check(deliveries == 2 && !task.running(), "new generation survives stale message");

    Gate gate;
    task.request([&] { gate.block(); return 7; }, [&](auto, auto&&, auto) { ++deliveries; });
    gate.wait();
    task.request([] { return 8; }, [&](auto generation, auto&& result, auto error) {
        check(generation == 5 && result && *result == 9 && !error, "latest result"); ++deliveries;
    });
    task.request([] { return 9; }, [&](auto generation, auto&& result, auto error) {
        check(generation == 5 && result && *result == 9 && !error, "latest coalesced result"); ++deliveries;
    });
    gate.release(); consume(task, nextMessage());
    check(deliveries == 2, "superseded result suppressed");
    consume(task, nextMessage());
    check(deliveries == 3 && !task.running(), "coalesced task progresses");
}
static void destroyedWindowAndReusedHandle() {
    auto first = std::make_unique<AsyncSnapshotTask<FirstResult>>(owner, completionMessage);
    first->request([] { return FirstResult{}; }, [](auto, auto&&, auto) {});
    const auto stale = nextMessage();
    first->cancel(); first.reset();
    AsyncSnapshotTask<DifferentResult> replacement(owner, completionMessage);
    consume(replacement, stale);
    check(wrongDestructors.load() == 0, "reused HWND must not run another Result destructor");
    check(liveResults.load() == 0, "cancel releases queued result");
    unsigned deliveries = 0;
    replacement.request([] { return DifferentResult{}; }, [&](auto, auto&& result, auto error) {
        check(result.has_value() && !error, "replacement result valid"); ++deliveries;
    });
    consume(replacement, nextMessage()); check(deliveries == 1, "replacement remains usable");
}
static void droppedMessageAndCancelInFlight() {
    {
        AsyncSnapshotTask<FirstResult> task(owner, completionMessage);
        task.request([] { return FirstResult{}; }, [](auto, auto&&, auto) {});
        (void)nextMessage(); // DestroyWindow may discard the notification entirely.
        task.cancel();
        check(liveResults.load() == 0, "dropped notification must not leak result");
    }
    Gate gate; unsigned deliveries = 0;
    AsyncSnapshotTask<FirstResult> task(owner, completionMessage);
    task.request([&] { gate.block(); return FirstResult{}; }, [&](auto, auto&&, auto) { ++deliveries; });
    gate.wait(); task.cancel(); gate.release();
    await([&] { return !task.running(); });
    await([] { return liveResults.load() == 0; });
    std::lock_guard lock(queueMutex);
    check(messages.empty() && deliveries == 0, "cancelled worker cannot publish");
}
static void postFailureStillProgresses() {
    Gate gate; unsigned deliveries = 0;
    AsyncSnapshotTask<int> task(owner, completionMessage);
    task.request([&] { gate.block(); return 1; }, [](auto, auto&&, auto) {});
    gate.wait(); task.request([] { return 2; }, [&](auto, auto&& result, auto error) {
        check(result && *result == 2 && !error, "pending request after post failure"); ++deliveries;
    });
    { std::lock_guard lock(queueMutex); postFailures = 1; }
    gate.release(); consume(task, nextMessage());
    check(deliveries == 1 && !task.running(), "failed post does not stall pending request");
}
static void cancelSerializesWithPublish() {
    { std::lock_guard lock(queueMutex); pausePost = true; enteredPost = releasePost = false; }
    AsyncSnapshotTask<FirstResult> task(owner, completionMessage);
    task.request([] { return FirstResult{}; }, [](auto, auto&&, auto) {});
    {
        std::unique_lock lock(queueMutex);
        check(queueCondition.wait_for(lock, std::chrono::seconds(3), [] { return enteredPost; }), "publisher reached gate");
    }
    std::atomic_bool cancelStarted = false, cancelReturned = false;
    std::thread canceller([&] { cancelStarted = true; task.cancel(); cancelReturned = true; });
    await([&] { return cancelStarted.load(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const bool cancelWaitedForPublisher = !cancelReturned.load();
    { std::lock_guard lock(queueMutex); releasePost = true; queueCondition.notify_all(); }
    canceller.join(); consume(task, nextMessage());
    check(cancelWaitedForPublisher, "cancel serializes with the publishing critical section");
    check(cancelReturned.load() && liveResults.load() == 0, "serialized cancel reclaims published result");
}
static void exceptionAndDeleteFromDelivery() {
    auto task = std::make_unique<AsyncSnapshotTask<int>>(owner, completionMessage);
    task->request([]() -> int { throw std::runtime_error("expected"); }, [&](auto, auto&& result, auto error) {
        check(!result && error != nullptr, "worker exception delivered"); task.reset();
    });
    const auto message = nextMessage(); consume(*task, message);
    check(!task, "delivery can destroy owner");
}
int main() {
    try {
        destroyedWindowAndReusedHandle(); normalAndCoalesced();
        droppedMessageAndCancelInFlight(); postFailureStillProgresses();
        cancelSerializesWithPublish(); exceptionAndDeleteFromDelivery();
        std::cout << "PASS " << checks << " checks, 6 production async lifetime scenarios\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
'''


def run_fixture(header, scratch, compiler, expect_failure=False):
    source = scratch / ("old_fixture.cpp" if expect_failure else "fixture.cpp")
    executable = source.with_suffix(".exe")
    source.write_text(FIXTURE.replace("@HEADER@", header.as_posix()), encoding="utf-8")
    environment = os.environ.copy()
    environment["TEMP"] = environment["TMP"] = str(scratch)
    subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-O2",
                    str(source), "-o", str(executable)], cwd=ROOT, env=environment, check=True)
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
    if expect_failure:
        if result.returncode == 0 or "another Result destructor" not in result.stderr:
            raise AssertionError(f"Old header did not expose type confusion: {result.stdout}{result.stderr}")
        print("PASS old header rejected: " + result.stderr.strip())
    else:
        if result.returncode != 0:
            raise AssertionError(result.stdout + result.stderr)
        print(result.stdout.strip())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--prove-regression", action="store_true")
    parser.add_argument("--old-ref", default="b2154a0d")
    args = parser.parse_args()
    compiler = shutil.which("g++")
    if not compiler:
        raise SystemExit("g++ is required for the isolated production-header regression")
    scratch = ROOT / ".codex-tmp" / ("arklight-async-lifetime-" + uuid.uuid4().hex)
    scratch.mkdir(parents=True)  # Inherit workspace ACLs; no private temp-directory ACL.
    run_fixture(HEADER, scratch, compiler)
    if args.prove_regression:
        old = scratch / "old_AsyncTask.h"
        source = subprocess.check_output(["git", "show", args.old_ref + ":KswordARKLight/Ui/AsyncTask.h"], cwd=ROOT, text=True)
        old.write_text(source.replace('#include "../Core/Win32Lean.h"', '#include <windows.h>'), encoding="utf-8")
        run_fixture(old, scratch, compiler, expect_failure=True)


if __name__ == "__main__":
    main()
