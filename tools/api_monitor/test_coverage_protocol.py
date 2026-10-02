"""Production snapshot receiver: interruption, reordering, stale sessions and revisions."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
code = r'''
#include "COVERAGE_HEADER"
#include <cstdio>
using namespace ks::winapi_monitor;
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d\n", __LINE__); return 1; } } while (0)
ApiMonitorEventPacket frame(EventKind kind, unsigned revision, unsigned index=0, unsigned count=2) {
 ApiMonitorEventPacket packet; packet.pid=42;packet.sessionIdentity=123;packet.snapshotRevision=revision;
 packet.snapshotIndex=index;packet.snapshotCount=count;packet.eventKind=static_cast<unsigned>(kind);
 packet.apiId=index+1;return packet;
}
int main() {
 CHECK(sizeof(ApiMonitorEventPacket)==1000);
 CoverageSnapshotReceiver receiver(42,123);
 CHECK(!receiver.consume(frame(EventKind::CoverageBegin,1))&&receiver.stale());
 CHECK(!receiver.consume(frame(EventKind::CoverageItem,1,0)));
 CHECK(!receiver.consume(frame(EventKind::CoverageEnd,1,2))&&receiver.stale()); // interrupted
 receiver.consume(frame(EventKind::CoverageBegin,2));
 receiver.consume(frame(EventKind::CoverageItem,2,1)); // missing first item
 CHECK(!receiver.consume(frame(EventKind::CoverageEnd,2,2))&&receiver.stale());
 receiver.consume(frame(EventKind::CoverageBegin,3));
 receiver.consume(frame(EventKind::CoverageItem,3,0));receiver.consume(frame(EventKind::CoverageItem,3,1));
 CHECK(receiver.consume(frame(EventKind::CoverageEnd,3,2))&&!receiver.stale());
 CHECK(receiver.items().size()==2&&receiver.revision()==3);
 auto old=frame(EventKind::CoverageBegin,99);old.sessionIdentity=456;
 CHECK(!receiver.consume(old)&&!receiver.stale()&&receiver.revision()==3);
 old.sessionIdentity=123;old.pid=43;CHECK(!receiver.consume(old)&&!receiver.stale());
 receiver.consume(frame(EventKind::CoverageBegin,2));CHECK(!receiver.stale()&&receiver.revision()==3);
 receiver.consume(frame(EventKind::CoverageBegin,5));receiver.consume(frame(EventKind::CoverageItem,5,0));
 receiver.consume(frame(EventKind::CoverageBegin,4)); // delayed older begin must not destroy current assembly
 receiver.consume(frame(EventKind::CoverageItem,5,1));
 CHECK(receiver.consume(frame(EventKind::CoverageEnd,5,2))&&receiver.revision()==5);
 receiver.interrupt();CHECK(receiver.stale()&&receiver.items().size()==2);
 receiver.consume(frame(EventKind::CoverageBegin,6,0,0));
 CHECK(receiver.consume(frame(EventKind::CoverageEnd,6,0,0))&&!receiver.stale()&&receiver.items().empty());
 CHECK(sessionIdentity(L"session A")!=sessionIdentity(L"session B")&&sessionIdentity(L"")==0);
 printf("PASS: coverage ordering, incomplete snapshots, disconnect, PID/session/revision isolation and empty full snapshot\n");
}
'''.replace("COVERAGE_HEADER", (root / "shared/ApiMonitorCoverage.h").as_posix())
with tempfile.TemporaryDirectory(prefix="ksword_coverage_") as temporary:
    directory = Path(temporary)
    source = directory / "fixture.cpp"
    source.write_text(code, encoding="utf-8")
    subprocess.run(["cl", "/nologo", "/EHsc", "/std:c++17", "/utf-8", str(source),
                    "/Fe:" + str(directory / "fixture.exe"), "/Fo:" + str(directory / "fixture.obj")], check=True)
    subprocess.run([str(directory / "fixture.exe")], check=True, timeout=30)
