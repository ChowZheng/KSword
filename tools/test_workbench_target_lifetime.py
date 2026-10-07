"""Replay three production target-transition methods with synchronous owner deletion.

The real MemoryTargetTracker supplies identity changes. A small Qt/anchor adapter
replaces signal delivery and handles; no GUI, driver, or target write is used.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]
SOURCE_PATH = "Ksword5.1/Ksword5.1/UI/MemoryWorkbench/WorkbenchTarget.cpp"


def function_text(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for position in range(opening, len(source)):
        depth += (source[position] == "{") - (source[position] == "}")
        if depth == 0:
            return source[start:position + 1]
    raise AssertionError("Unclosed production method")


SHIM = r'''
#include "shared/evidence/memory_workbench/MemoryTargetTracker.h"
#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
static unsigned checks, sideEffects;
static std::shared_ptr<std::atomic_bool> currentLife;
static void check(bool condition) {
    ++checks;
    if (!condition) throw std::runtime_error("target transition accessed its destroyed owner");
}
template<class T> class QPointer {
    std::shared_ptr<std::atomic_bool> alive;
public:
    explicit QPointer(T* pointer) : alive(pointer->alive) {}
    explicit operator bool() const { return alive->load(); }
};
namespace ks::ui {
enum class NavStatus { Ok, Unavailable, TargetGone, TargetMismatch, LeaveRefused };
enum class LeaveReason { PinChange, DockAttachChange };
enum class LivenessState { Unknown, Alive, Exited };
struct IdentityRequest {
    std::optional<ksword::memwb::Scope> scope;
    std::optional<ksword::memwb::Channel> channel;
    std::optional<std::uint32_t> pinPid;
    std::uint64_t expectCreateTime = 0;
};
struct AnchorInfo {
    void* handle = nullptr;
    std::uint64_t createTime100ns = 99;
    std::uint32_t addressBits = 64;
    bool targetGone = false, identityWeak = false;
};
static void ReleaseAnchorHandle(void*) {}
static AnchorInfo AcquireAnchorFromDockHandle(void* handle) { AnchorInfo info; info.handle = handle; return info; }
static AnchorInfo AcquireAnchorForPid(std::uint32_t) { return AcquireAnchorFromDockHandle(reinterpret_cast<void*>(1)); }
static bool IsLegalScope(ksword::memwb::Scope) { return true; }
static bool IsLegalChannel(ksword::memwb::Channel) { return true; }
class WorkbenchTarget {
public:
    struct DockAttach { void* handle = nullptr; std::uint32_t pid = 0; std::uint64_t attachGeneration = 0; };
    struct Policy { bool lockToDock = false, allowKernelPhysical = true; } policy_;
    struct Services { std::uint64_t ddmaGeneration() { return 1; } };
    std::shared_ptr<std::atomic_bool> alive = std::make_shared<std::atomic_bool>(true);
    ksword::memwb::MemoryTargetTracker tracker_;
    std::shared_ptr<Services> services_ = std::make_shared<Services>();
    void* dockAnchorHandle_ = nullptr;
    void* pinnedAnchorHandle_ = nullptr;
    NavStatus lastIdentityFailure_ = NavStatus::Ok;
    LivenessState lastLiveness_ = LivenessState::Alive;
    std::function<void()> onLiveness;
    WorkbenchTarget() { currentLife = alive; }
    ~WorkbenchTarget() { alive->store(false); }
    void onDockAttached(const DockAttach& attach);
    void onDockDetached();
    bool requestIdentity(const IdentityRequest& request, LeaveReason reason);
    bool predictsIdentityChange(const IdentityRequest&, const AnchorInfo*) const { return true; }
    bool requestLeave(LeaveReason) { return true; }
    void updateLiveness(LivenessState state) {
        lastLiveness_ = state;
        const auto callback = onLiveness;
        if (callback) callback();
    }
    void applyMaskSideEffects(ksword::memwb::TargetChange mask) {
        // Production first reads tracker_.Session() here, then starts module enumeration.
        // Check the separate lifetime token before allowing any mock member access.
        check(currentLife->load());
        if (mask != ksword::memwb::TargetChange::None) {
            check(tracker_.Session().addressBits == 64); ++sideEffects;
        }
    }
};
}
'''


CASES = r'''
static int runCase(int scenario) {
    using namespace ks::ui;
    sideEffects = 0;
    if (scenario == 0) {
        // Ordinary transitions still update the real tracker and publish side effects.
        WorkbenchTarget target;
        target.onDockAttached({reinterpret_cast<void*>(1), 7, 1});
        check(target.tracker_.Session().pid == 7 && sideEffects == 1);
        target.onDockDetached();
        check(target.tracker_.Session().pid == 0 && sideEffects == 2);
        IdentityRequest request; request.pinPid = 8;
        check(target.requestIdentity(request, LeaveReason::PinChange));
        check(target.tracker_.Session().pid == 8 && sideEffects == 3);
    } else {
        auto target = std::make_unique<WorkbenchTarget>();
        target->tracker_.FollowAttach(7, 99, 1, 64);
        target->onLiveness = [&] { target.reset(); };
        WorkbenchTarget* const selected = target.get();
        if (scenario == 1) {
            selected->onDockAttached({reinterpret_cast<void*>(1), 8, 2});
        } else if (scenario == 2) {
            selected->onDockDetached();
        } else if (scenario == 3) {
            IdentityRequest request; request.pinPid = 8;
            check(!selected->requestIdentity(request, LeaveReason::PinChange));
        } else { check(false); }
        check(!target && !currentLife->load());
        check(sideEffects == 0);
    }
    std::cout << "PASS: target lifecycle scenario " << scenario << " (" << checks << " checks)\n";
    return 0;
}
int main(int argc, char** argv) {
    try { return runCase(argc == 2 ? std::atoi(argv[1]) : 0); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
'''


def bodies(source):
    signatures = (
        "void WorkbenchTarget::onDockAttached(",
        "void WorkbenchTarget::onDockDetached()",
        "bool WorkbenchTarget::requestIdentity(",
    )
    return "namespace ks::ui {\n" + "\n".join(function_text(source, signature) for signature in signatures) + "\n}\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prove-regression", action="store_true", help="Verify each unpatched HEAD method fails synchronous deletion")
    args = parser.parse_args()
    compiler = shutil.which("g++")
    if compiler is None:
        parser.error("A host g++ compiler is required; this does not build the Qt application")
    scratch = ROOT / ".codex-tmp/workbench-target-lifetime"
    scratch.mkdir(parents=True, exist_ok=True)
    source = scratch / "replay.cpp"
    executable = scratch / "replay.exe"
    environment = dict(os.environ, TEMP=str(scratch), TMP=str(scratch))
    environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
    command = [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-O2", "-I", str(ROOT), str(source),
               str(ROOT / "shared/evidence/memory_workbench/MemoryTargetTracker.cpp"),
               str(ROOT / "shared/evidence/memory_workbench/MemoryTargetSession.cpp"), "-o", str(executable)]
    source.write_text(SHIM + bodies((ROOT / SOURCE_PATH).read_text(encoding="utf-8-sig")) + CASES, encoding="utf-8")
    subprocess.run(command, check=True, env=environment, timeout=60)
    for scenario in range(4):
        subprocess.run([str(executable), str(scenario)], check=True, env=environment, timeout=10)
    if args.prove_regression:
        original = subprocess.check_output(["git", "show", "HEAD:" + SOURCE_PATH], cwd=ROOT).decode("utf-8-sig")
        source.write_text(SHIM + bodies(original) + CASES, encoding="utf-8")
        subprocess.run(command, check=True, env=environment, timeout=60)
        for scenario in range(1, 4):
            result = subprocess.run([str(executable), str(scenario)], capture_output=True, text=True, env=environment, timeout=10)
            if result.returncode != 1 or "destroyed owner" not in result.stderr:
                raise AssertionError(f"Unpatched method {scenario} did not expose the rejected lifetime access")
        print("Unpatched HEAD rejected in all 3 synchronous-deletion scenarios")


if __name__ == "__main__":
    main()
