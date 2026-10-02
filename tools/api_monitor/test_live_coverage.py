"""Install the real Release Agent in an owned fixture; inspect actual coverage changes."""
import hashlib
from live_fixture_support import LiveFixture, ROOT

source = r'''
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdio>
int wmain(int argc, wchar_t** argv) {
 if(argc!=2)return 1;printf("%lu\n",GetCurrentProcessId());fflush(stdout);
 for(int c;(c=getchar())!=EOF;){
  if(c=='L'){if(!LoadLibraryW(argv[1]))return 2;}
  if(c=='W'){if(!LoadLibraryW(L"winhttp.dll"))return 3;}
  if(c=='Q')return 0;
 }
 return 0;
}
'''
configuration = {"enable_network": 1, "enable_loader": 0, "enable_raw_fallback": 1,
                 "raw_modules": "api_monitor_unloaded_fixture.dll", "fake_success_enabled": 1,
                 "fake_success_raw_fallback": 1,
                 "fake_success_rules": "KernelBase.dll|ApiMonitorMissingFixtureExport|bool|1|none|0"}
with LiveFixture(source, configuration) as fixture:
    fixture.wait_for(lambda f: bool(f.snapshots))
    initial_revision, rows = fixture.snapshots[-1]
    strong = [row for row in rows if row.hook_kind == 0]
    assert len([row for row in strong if row.api_id <= 614]) == 614
    assert len({row.api_id for row in strong}) == len(strong)
    assert any(row.state == 0 for row in strong), "real engine must install actual exports"
    assert any(row.state == 2 for row in strong), "disabled categories must be explicit"
    assert any(row.module.lower() == "winhttp.dll" and row.state == 3 for row in strong)
    assert any(row.hook_kind == 1 and row.state == 3 for row in rows), "unloaded Raw module must be explicit"
    assert any(row.hook_kind == 2 and row.state == 4 for row in rows), "missing Fake export must be explicit"
    digest = hashlib.sha256((ROOT / "APIMonitor_x64/api_monitor_definitions.json").read_bytes()).hexdigest().encode()
    assert all(row.sha256 == digest for row in rows)
    fixture.command("W")
    fixture.wait_for(lambda f: any(revision > initial_revision and
                     any(row.module.lower() == "winhttp.dll" and row.state in (0, 1) for row in snapshot)
                     for revision, snapshot in f.snapshots))
    assert any(event.api == "HooksInstalled" for event in fixture.events)
print("PASS: real Release Agent, 614 legacy definitions, installed/disabled/waiting/missing states, hash and late module installation")
