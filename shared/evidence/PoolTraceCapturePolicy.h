#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ksword::pool_trace {
enum class Action { None, Start, Status, Stop, Cancel };
enum class Command { None, Help, Profiles, ProfileDetails, Start, Status, Stop, Cancel };
enum class Completion { Success, Failed, TimedOut, NoSession };
enum class Session { Idle, Recording, Uncertain };

// Production UI and tests share these transitions. Terminating the recorder
// and successfully saving an ETL are separate outcomes.
struct CaptureState {
    Session session = Session::Idle;
    Command pending = Command::None;
    bool mayOwnSession = false, saved = false, stoppedKnown = true, shuttingDown = false;
    bool canStart() const { return !shuttingDown && pending == Command::None && !mayOwnSession; }
    bool canFinish() const { return !shuttingDown && pending == Command::None && mayOwnSession; }
    bool recording() const { return session == Session::Recording; }
    bool begin(Command command) {
        if (shuttingDown || pending != Command::None || command == Command::None) { return false; }
        if (command == Command::Start && mayOwnSession) { return false; }
        if ((command == Command::Help || command == Command::Profiles || command == Command::ProfileDetails) && mayOwnSession) { return false; }
        if ((command == Command::Stop || command == Command::Cancel || command == Command::Status) && !mayOwnSession) { return false; }
        pending = command;
        if (command == Command::Start) { mayOwnSession = true; stoppedKnown = false; saved = false; session = Session::Uncertain; }
        return true;
    }
    void finish(Completion completion, bool outputVerified = false) {
        if (shuttingDown || pending == Command::None) { return; }
        const auto command = pending; pending = Command::None;
        if (command == Command::Start) {
            if (completion == Completion::NoSession) {
                session = Session::Idle; mayOwnSession = false; stoppedKnown = true;
            } else { session = completion == Completion::Success ? Session::Recording : Session::Uncertain; }
        } else if (command == Command::Stop || command == Command::Cancel) {
            if (completion == Completion::Success || completion == Completion::NoSession) {
                mayOwnSession = false; stoppedKnown = true; session = Session::Idle;
                saved = command == Command::Stop && completion == Completion::Success && outputVerified;
            } else { session = Session::Uncertain; stoppedKnown = false; }
        }
    }
    Action shutdown() {
        const auto cleanup = !mayOwnSession ? Action::None
            : (recording() && pending != Command::Stop && pending != Command::Cancel ? Action::Stop : Action::Cancel);
        shuttingDown = true; pending = Command::None;
        return cleanup;
    }
};
inline int commandTimeoutMs(Command command) {
    return command == Command::Stop ? 120000 : (command == Command::Start ? 30000 : 15000);
}
inline bool noSessionExit(std::uint32_t exitCode) { return exitCode == 0xC5583000U; }
inline bool verifiedNewOutput(bool isFile, std::uint64_t size, std::int64_t modified,
    bool existed, std::uint64_t previousSize, std::int64_t previousModified)
{
    return isFile && size > 0 && (!existed || size != previousSize || modified != previousModified);
}
constexpr unsigned requestedBufferKiB = 128, requestedBuffers = 256;
constexpr std::uint64_t requestedBufferBytes = std::uint64_t{requestedBufferKiB} * requestedBuffers * 1024;

// Reduced standard profile based on WPR -exportprofile Pool: pool allocation
// and free stacks plus process/image metadata. No unrelated event collectors.
inline const char* profileXml() {
    return R"wpr(<WindowsPerformanceRecorder Version="1.0">
  <Profiles>
    <SystemCollector Id="KSwordPoolSystem" Name="KSword Pool System Collector">
      <BufferSize Value="128" />
      <Buffers Value="256" />
    </SystemCollector>
    <SystemProvider Id="KSwordPoolProvider">
      <Keywords>
        <Keyword Value="CpuConfig" />
        <Keyword Value="Loader" />
        <Keyword Value="Pool" />
        <Keyword Value="ProcessThread" />
      </Keywords>
      <Stacks>
        <Stack Value="PoolAllocation" />
        <Stack Value="PoolAllocationSession" />
        <Stack Value="PoolFree" />
        <Stack Value="PoolFreeSession" />
      </Stacks>
    </SystemProvider>
    <Profile Id="KSwordPool.Verbose.Memory" Name="KSwordPool" Description="KSword pool allocation history" LoggingMode="Memory" DetailLevel="Verbose">
      <Collectors>
        <SystemCollectorId Value="KSwordPoolSystem"><SystemProviderId Value="KSwordPoolProvider" /></SystemCollectorId>
      </Collectors>
      <TraceMergeProperties>
        <TraceMergeProperty Id="KSwordPoolMerge" Name="KSwordPoolMerge">
          <CustomEvents><CustomEvent Value="ImageId" /><CustomEvent Value="BuildInfo" /><CustomEvent Value="VolumeMapping" /><CustomEvent Value="EventMetadata" /></CustomEvents>
        </TraceMergeProperty>
      </TraceMergeProperties>
    </Profile>
  </Profiles>
</WindowsPerformanceRecorder>
)wpr";
}
inline bool validInstance(const std::wstring& instance)
{
    if (instance.compare(0, 13, L"KSwordMemory_") != 0 || instance.size() <= 13 || instance.size() > 96) { return false; }
    for (const auto c : instance) {
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
            (c >= L'0' && c <= L'9') || c == L'_')) { return false; }
    }
    return true;
}
inline std::vector<std::wstring> arguments(Action action, const std::wstring& instance,
    const std::wstring& output = {}, const std::wstring& profile = {})
{
    if (!validInstance(instance)) { return {}; }
    std::vector<std::wstring> result;
    switch (action) {
    case Action::None: return {};
    case Action::Start:
        if (profile.empty()) { return {}; }
        result = {L"-start", profile}; break; // custom bounded memory mode
    case Action::Status: result = {L"-status", L"collectors", L"-details"}; break;
    case Action::Stop:
        if (output.empty()) { return {}; }
        result = {L"-stop", output, L"-skipPdbGen", L"-compress"}; break;
    case Action::Cancel: result = {L"-cancel"}; break;
    }
    result.push_back(L"-instancename");
    result.push_back(instance); // required final option: never mutate a global session
    return result;
}
}
