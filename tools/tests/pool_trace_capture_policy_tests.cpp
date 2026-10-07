#include "../../shared/evidence/PoolTraceCapturePolicy.h"
#include <cassert>
#include <iostream>
int main()
{
    using namespace ksword::pool_trace;
    const std::wstring instance = L"KSwordMemory_0123456789abcdef";
    assert(!validInstance(L"Global"));
    assert(!validInstance(L"KSwordMemory_"));
    assert(!validInstance(L"KSwordMemory_bad -cancel"));
    assert(arguments(Action::Start, L"Global").empty());
    assert(arguments(Action::Start, instance).empty());
    assert(arguments(Action::Stop, instance).empty());
    for (const auto action : {Action::Start, Action::Status, Action::Stop, Action::Cancel}) {
        const auto args = arguments(action, instance, L"C:\\trace output\\pool trace.etl", L"C:\\profile path\\pool.wprp!KSwordPool");
        assert(args[args.size() - 2] == L"-instancename" && args.back() == instance);
        for (const auto& arg : args) { assert(arg != L"-filemode" && arg != L"-disablepagingexecutive"); }
    }
    const auto stop = arguments(Action::Stop, instance, L"C:\\trace output\\pool trace.etl");
    assert(stop[1] == L"C:\\trace output\\pool trace.etl"); // one argv, no shell
    assert(arguments(Action::Start, instance, {}, L"C:\\profile path\\pool.wprp!KSwordPool")[1] == L"C:\\profile path\\pool.wprp!KSwordPool");
    assert(requestedBufferBytes == 32ULL * 1024 * 1024);
    const std::string xml(profileXml());
    assert(xml.find("LoggingMode=\"Memory\"") != std::string::npos);
    for (const auto event : {"PoolAllocation", "PoolFree", "ProcessThread", "Loader", "ImageId", "BuildInfo"}) { assert(xml.find(event) != std::string::npos); }

    CaptureState state;
    assert(state.canStart() && !state.canFinish());
    assert(state.begin(Command::Help) && !state.canStart());
    assert(!state.begin(Command::Start)); // no overlapping commands
    state.finish(Completion::Failed);
    assert(state.canStart() && state.stoppedKnown);
    assert(state.begin(Command::Start));
    state.finish(Completion::Success);
    assert(state.recording() && state.canFinish() && !state.canStart());
    assert(!state.begin(Command::Help)); // do not disturb an owned capture
    assert(state.begin(Command::Status));
    state.finish(Completion::Failed);
    assert(state.recording() && state.canFinish());
    assert(state.begin(Command::Stop));
    state.finish(Completion::Success, false);
    assert(state.canStart() && state.stoppedKnown && !state.saved); // stopped, missing ETL
    assert(state.begin(Command::Start)); state.finish(Completion::Success);
    assert(state.begin(Command::Stop)); state.finish(Completion::Success, true);
    assert(state.canStart() && state.saved && !state.mayOwnSession);

    for (const auto failure : {Completion::Failed, Completion::TimedOut}) {
        CaptureState failedStart;
        assert(failedStart.begin(Command::Start)); failedStart.finish(failure);
        assert(failedStart.canFinish() && !failedStart.canStart() && !failedStart.stoppedKnown);
        assert(failedStart.begin(Command::Cancel)); failedStart.finish(Completion::Failed);
        assert(failedStart.canFinish() && !failedStart.canStart()); // cleanup retry remains usable
        assert(failedStart.begin(Command::Cancel)); failedStart.finish(Completion::NoSession);
        assert(failedStart.canStart() && !failedStart.canFinish() && failedStart.stoppedKnown);
    }
    CaptureState neverStarted;
    assert(neverStarted.begin(Command::Start)); neverStarted.finish(Completion::NoSession);
    assert(neverStarted.canStart() && !neverStarted.mayOwnSession);
    CaptureState timedStop;
    assert(timedStop.begin(Command::Start)); timedStop.finish(Completion::Success);
    assert(timedStop.begin(Command::Status)); timedStop.finish(Completion::TimedOut);
    assert(timedStop.recording());
    assert(timedStop.begin(Command::Stop)); timedStop.finish(Completion::TimedOut);
    assert(timedStop.canFinish() && !timedStop.recording());
    assert(timedStop.begin(Command::Cancel)); timedStop.finish(Completion::Success);
    assert(timedStop.canStart() && !timedStop.saved);
    CaptureState noSession;
    assert(noSession.begin(Command::Start)); noSession.finish(Completion::Success);
    assert(noSession.begin(Command::Stop)); noSession.finish(Completion::NoSession, true);
    assert(noSession.canStart() && !noSession.saved); // old file cannot make absent capture saved

    CaptureState pendingStart;
    assert(pendingStart.begin(Command::Start));
    assert(pendingStart.shutdown() == Action::Cancel);
    pendingStart.finish(Completion::Success); // late callbacks cannot advance a destroyed page
    assert(!pendingStart.canStart() && !pendingStart.canFinish() && !pendingStart.recording());
    CaptureState recording;
    assert(recording.begin(Command::Start)); recording.finish(Completion::Success);
    assert(recording.shutdown() == Action::Stop);
    CaptureState querying;
    assert(querying.begin(Command::Start)); querying.finish(Completion::Success);
    assert(querying.begin(Command::Status)); assert(querying.shutdown() == Action::Stop);
    CaptureState stopping;
    assert(stopping.begin(Command::Start)); stopping.finish(Completion::Success);
    assert(stopping.begin(Command::Stop)); assert(stopping.shutdown() == Action::Cancel);
    CaptureState idle;
    assert(idle.begin(Command::Profiles)); assert(idle.shutdown() == Action::None);

    assert(!verifiedNewOutput(true, 100, 123, true, 100, 123));
    assert(!verifiedNewOutput(true, 0, 124, false, 0, 0));
    assert(!verifiedNewOutput(false, 100, 124, false, 0, 0));
    assert(verifiedNewOutput(true, 100, 124, true, 100, 123));
    assert(verifiedNewOutput(true, 101, 123, true, 100, 123));
    assert(verifiedNewOutput(true, 100, 123, false, 0, 0));
    assert(noSessionExit(0xC5583000U) && !noSessionExit(5));
    assert(commandTimeoutMs(Command::Start) == 30000 && commandTimeoutMs(Command::Stop) == 120000);
    std::cout << "POOL_TRACE_CAPTURE_POLICY_TESTS=PASS\n";
}
