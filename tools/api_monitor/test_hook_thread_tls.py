"""Production guards must bypass safely before TLS initialization and after TLS teardown."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
targets = (root / "APIMonitor_x64/hook/HookTargets.cpp").read_text(encoding="utf-8-sig")
guard = targets[targets.index("        thread_local bool g_hookReentryGuard"):targets.index("        using CoveragePacket")]
source = r'''
#include <cstdio>
#include "ENGINE"
namespace apimon { namespace { GUARD } }
using namespace apimon;
bool MissingTlsProbe() {
 ScopedInlineHookInternalBypass internal;
 ScopedHookGuard guard;
 return guard.bypass() && IsInlineHookInternalBypassActive();
}
void SetSlots(void** slots) {
#ifdef _M_IX86
 __writefsdword(0x2C, reinterpret_cast<unsigned long>(slots));
#else
 __writegsqword(0x58, reinterpret_cast<unsigned long long>(slots));
#endif
}
int main() {
 if(IsInlineHookInternalBypassActive()) return 1;
 { ScopedHookGuard first;ScopedHookGuard second;if(first.bypass()||!second.bypass()) return 2; }
 if(g_hookReentryGuard) return 3;
#ifdef _M_IX86
 auto** slots=reinterpret_cast<void**>(__readfsdword(0x2C));
#else
 auto** slots=reinterpret_cast<void**>(__readgsqword(0x58));
#endif
 if(!slots||!slots[_tls_index])return 4;
 auto* storage=slots[_tls_index];
 slots[_tls_index]=nullptr;const bool missingBlock=MissingTlsProbe();slots[_tls_index]=storage;
 SetSlots(nullptr);const bool missingVector=MissingTlsProbe();SetSlots(slots);
 if(!missingBlock||!missingVector||IsInlineHookInternalBypassActive()||g_hookReentryGuard)return 5;
 { ScopedInlineHookInternalBypass internal;if(!IsInlineHookInternalBypassActive())return 6; }
 auto gdi=LoadLibraryW(L"gdi32.dll");auto nativeAlias=gdi?GetProcAddress(gdi,"EngAcquireSemaphore"):nullptr;
 if(!nativeAlias||!IsNativeRuntimeHookTarget(reinterpret_cast<void*>(nativeAlias)))return 7;
 if(IsNativeRuntimeHookTarget(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"GetComputerNameW"))))return 8;
 puts("PASS: real guard with missing TLS vector/block, restored nested guards and native-runtime Raw alias");return 0;
}
'''.replace("ENGINE", (root / "APIMonitor_x64/hook/HookEngine.cpp").as_posix()).replace("GUARD", guard)
with tempfile.TemporaryDirectory(prefix="ksword_tls_lifetime_") as temporary:
    folder = Path(temporary)
    cpp = folder / "fixture.cpp"
    cpp.write_text(source, encoding="utf-8")
    subprocess.run(["cl", "/nologo", "/EHsc", "/std:c++17", "/utf-8", "/I"+str(root/"APIMonitor_x64"),
                    str(cpp), "/Fe:"+str(folder/"fixture.exe"), "/Fo:"+str(folder/"fixture.obj")], check=True)
    subprocess.run([str(folder/"fixture.exe")], check=True, timeout=30)
