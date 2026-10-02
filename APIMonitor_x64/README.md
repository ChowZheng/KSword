# API Monitor x64 definitions

`api_monitor_definitions.json` is the build-time source for API identities,
signatures, binding records, original pointers and ordinary wrappers. IDs 1–614
belong to the original catalog; never renumber or reuse a removed ID.

`tools/api_monitor/generate_definitions.py` uses only the Python standard library.
MSBuild validates and generates before compiling into `$(IntDir)generated`.
The Python executable can be overridden using `/p:ApiMonitorPython=...`.
Unchanged output retains its timestamp. The successful Release build copies the
source bytes to `profiles/api_monitor_definitions.json` next to the published DLL.
The main executable has a non-linking dependency on this project.

The published JSON is for inspection. Editing it does not enable runtime hooks.
Rebuild the DLL after changing the source definitions. `SessionReady` reports the
SHA256 embedded during compilation; the UI compares it with the published file.

Ordinary wrappers call named C++ formatters in `hook/ApiCaptureFormatters.inc`.
Special behavior (injection, clipboard policy, asynchronous callbacks) remains in
compiled C++ handlers. `hook/ApiHandlers.json` registers available handler names;
the generator also verifies their presence in C++ source. New ABI types require
review and inclusion in the generator's supported Windows/native type set.
No C++ bodies or arbitrary expressions are accepted as catalog values.

Capture metadata records direction, encoding and an optional bounded length
source (parameter, unit, indirect flag). Structured legacy capture is delegated
to its named formatter. A null length does not authorize unbounded reads.
Generated formatters are protected by an SEH boundary; unreadable data produces
`capture=unreadable`. Wrappers preserve the original return and last-error value.

Run `python tools/api_monitor/test_definitions.py` after building, together with
the production hook, protocol and UI regression fixtures in that directory.
