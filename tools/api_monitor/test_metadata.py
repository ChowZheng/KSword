"""SAL directions and simple length references; omit executable size expressions."""
import tempfile
from pathlib import Path
from audit_definition_metadata import declaration_directions, parameter_length, audit

with tempfile.TemporaryDirectory(prefix="ksword_metadata_") as temp:
    folder = Path(temp)
    (folder / "fixture.h").write_text("""
    BOOL ReadFixture(_In_ HANDLE file, _Out_writes_bytes_to_(length, *returned) PVOID buffer,
                     _In_ DWORD length, _Out_ LPDWORD returned);
    typedef int (WSAAPI *LPFN_WSARECVMSG)(_In_ SOCKET socket, _Inout_ LPWSAMSG message,
                                        _Out_opt_ LPDWORD bytes, _Inout_opt_ LPOVERLAPPED ov,
                                        _In_opt_ LPWSAOVERLAPPED_COMPLETION_ROUTINE callback);
    """, encoding="utf-8")
    declarations = declaration_directions(folder, {"ReadFixture", "WSARecvMsg"})
    assert declarations["ReadFixture"] == ["in", "out", "in", "out"]
    assert declarations["WSARecvMsg"] == ["in", "inout", "out", "inout", "in"]
    checked, differences = audit({"apis": [{"export": "ReadFixture", "parameters": [
        {"name": "file", "direction": "in"}, {"name": "buffer", "direction": "inout"},
        {"name": "length", "direction": "in"}, {"name": "returned", "direction": "out"}
    ]}]}, declarations)
    assert checked == 4 and len(differences) == 1 and "ReadFixture.buffer" in differences[0]
assert parameter_length("_Out_writes_bytes_to_(length, *returned) PVOID buffer") == {
    "parameter": "length", "indirect": False, "unit": "bytes"}
assert parameter_length("_Inout_updates_(*count) DWORD* values") == {
    "parameter": "count", "indirect": True, "unit": "elements"}
assert parameter_length("_In_reads_bytes_(length + extra) PVOID buffer") is None
print("PASS: declaration directions, extension typedefs, drift audit, bounded simple references and expression exclusion")
