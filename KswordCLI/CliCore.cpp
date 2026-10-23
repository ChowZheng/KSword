#include "CliCore.h"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace ks::cli::legacy
{
    // Declarations for legacy CLI text/parse helpers that still live in
    // KswordCLI.cpp. They are defined in this same namespace, so the
    // declarations only need to match the definitions exactly.
    std::vector<std::uint8_t> parseHexBytes(const std::wstring& text);
    std::string fixedAnsi(const char* text, std::size_t maxBytes);
    std::wstring fixedUtf16(const unsigned short* text, std::size_t maxChars);
    std::wstring hex64(std::uint64_t value);
    void dumpWideText(const wchar_t* label, const std::wstring& value);

    // Single shared instance for the whole legacy CLI: wmain maps the process
    // exit code from these values and dispatchNested saves/restores them.
    DWORD lastIoctlError = ERROR_SUCCESS;
    DWORD lastOpenError = ERROR_SUCCESS;
    int responseFailureRc = 0;

    // Transport success does not imply that the operation in the packet succeeded.
    int operationStatusRc(long status)
    {
        const auto value = static_cast<std::uint32_t>(status);
        if (value == 0xC00000BBU || value == 0xC0000002U || value == 0xC000007AU) return 5;
        return status < 0 || value == 0x102U ? 3 : 0;
    }

    // printWin32Error renders a failed Win32 operation in a compact form.
    // Inputs: operation describes the API/IOCTL; error is GetLastError output.
    // Processing: prints decimal and hexadecimal forms for quick triage.
    // Returns: no value; output goes to stderr.
    void printWin32Error(const wchar_t* operation, DWORD error)
    {
        std::wcerr << L"error: " << operation << L" failed, win32=" << error
                   << L" (0x" << std::hex << error << std::dec << L")\n";
        if ((error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) &&
            std::wcsstr(operation, L"CreateFileW(") == operation)
            std::wcerr << L"hint: run 'sc start KswordARK' for an installed service, "
                       << L"or run Launcher.exe / Ksword5.1.exe as administrator to load the driver.\n";
        // Some callers retain GetLastError after diagnostics; stream writes
        // must not replace the original failed operation's error.
        ::SetLastError(error);
    }

    // openDriver opens the shared KswordARK control/log device.
    // Inputs: desiredAccess usually includes read/write because some IOCTLs are gated.
    // Processing: calls CreateFileW on the path exported by shared protocol headers.
    // Returns: RAII DriverHandle; caller checks valid() before issuing requests.
    DriverHandle openDriver(DWORD desiredAccess)
    {
        DriverHandle handle(::CreateFileW(
            KSWORD_ARK_LOG_WIN32_PATH,
            desiredAccess,
            kDefaultShareMode,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
        lastOpenError = handle.valid() ? ERROR_SUCCESS : ::GetLastError();
        return handle;
    }

    // sendIoctl wraps synchronous DeviceIoControl for fixed and variable buffers.
    // Inputs: open handle, control code, optional input and output buffers.
    // Processing: forwards arguments without interpreting protocol contents.
    // Returns: IoctlResult with Win32 success bit, error code, and returned byte count.
    IoctlResult sendIoctl(
        DriverHandle& handle,
        DWORD code,
        void* input,
        DWORD inputBytes,
        void* output,
        DWORD outputBytes)
    {
        IoctlResult result{};
        if (!handle.valid())
        {
            result.ok = false;
            result.win32Error = ERROR_INVALID_HANDLE;
            return result;
        }

        DWORD bytesReturned = 0;
        const BOOL ok = ::DeviceIoControl(
            handle.native(),
            code,
            input,
            inputBytes,
            output,
            outputBytes,
            &bytesReturned,
            nullptr);
        result.ok = (ok != FALSE);
        result.win32Error = result.ok ? ERROR_SUCCESS : ::GetLastError();
        lastIoctlError = result.win32Error;
        result.bytesReturned = bytesReturned;
        return result;
    }

    // currentUtc100ns returns the current UTC FILETIME tick count.
    // Inputs: none.
    // Processing: converts GetSystemTimePreciseAsFileTime into a 64-bit scalar.
    // Returns: UTC time in 100ns units.
    std::uint64_t currentUtc100ns()
    {
        FILETIME fileTime{};
        ::GetSystemTimePreciseAsFileTime(&fileTime);
        ULARGE_INTEGER value{};
        value.HighPart = fileTime.dwHighDateTime;
        value.LowPart = fileTime.dwLowDateTime;
        return static_cast<std::uint64_t>(value.QuadPart);
    }

    // formatGuid128 renders the callback GUID packet in the same shape as UI code.
    // Inputs: fixed 16-byte GUID packet.
    // Processing: prints uppercase hexadecimal with 8-4-4-4-12 grouping.
    // Returns: formatted GUID text.
    std::wstring formatGuid128(const KSWORD_ARK_GUID128& guid)
    {
        std::wostringstream stream;
        const unsigned char* bytes = guid.bytes;
        stream << std::hex << std::uppercase << std::setfill(L'0')
               << std::setw(2) << static_cast<unsigned int>(bytes[0])
               << std::setw(2) << static_cast<unsigned int>(bytes[1])
               << std::setw(2) << static_cast<unsigned int>(bytes[2])
               << std::setw(2) << static_cast<unsigned int>(bytes[3]) << L"-"
               << std::setw(2) << static_cast<unsigned int>(bytes[4])
               << std::setw(2) << static_cast<unsigned int>(bytes[5]) << L"-"
               << std::setw(2) << static_cast<unsigned int>(bytes[6])
               << std::setw(2) << static_cast<unsigned int>(bytes[7]) << L"-"
               << std::setw(2) << static_cast<unsigned int>(bytes[8])
               << std::setw(2) << static_cast<unsigned int>(bytes[9]) << L"-"
               << std::setw(2) << static_cast<unsigned int>(bytes[10])
               << std::setw(2) << static_cast<unsigned int>(bytes[11])
               << std::setw(2) << static_cast<unsigned int>(bytes[12])
               << std::setw(2) << static_cast<unsigned int>(bytes[13])
               << std::setw(2) << static_cast<unsigned int>(bytes[14])
               << std::setw(2) << static_cast<unsigned int>(bytes[15]);
        return stream.str();
    }

    // parseGuid128 decodes a 32-hex-digit callback GUID string.
    // Inputs: GUID text with or without dashes.
    // Processing: strips separators and decodes byte pairs in order.
    // Returns: GUID packet or throws on malformed text.
    KSWORD_ARK_GUID128 parseGuid128(const std::wstring& text)
    {
        const std::vector<std::uint8_t> bytes = parseHexBytes(text);
        if (bytes.size() != 16U)
        {
            throw std::invalid_argument("guid");
        }

        KSWORD_ARK_GUID128 guid{};
        std::memcpy(guid.bytes, bytes.data(), 16U);
        return guid;
    }

    // openDriverOrReport opens the shared control device and emits a uniform error.
    // Inputs: desired access for the pending IOCTL.
    // Processing: caller receives a possibly-invalid handle and may stop on failure.
    // Returns: DriverHandle RAII wrapper.
    DriverHandle openDriverOrReport(DWORD desiredAccess)
    {
        DriverHandle handle = openDriver(desiredAccess);
        if (!handle.valid())
        {
            printWin32Error(L"CreateFileW(" KSWORD_ARK_LOG_WIN32_PATH L")", ::GetLastError());
        }
        return handle;
    }

    // runNoOutputIoctl issues a control code whose success is mostly transport-level.
    // Inputs: control code and optional request buffer.
    // Processing: prints bytesReturned and Win32 completion information.
    // Returns: CLI exit code.
    int runNoOutputIoctl(
        const wchar_t* label,
        DWORD code,
        void* input,
        DWORD inputBytes,
        DWORD desiredAccess)
    {
        DriverHandle handle = openDriverOrReport(desiredAccess);
        if (!handle.valid())
        {
            return 2;
        }

        IoctlResult io = sendIoctl(handle, code, input, inputBytes, nullptr, 0U);
        if (!io.ok)
        {
            printWin32Error(label, io.win32Error);
            return 3;
        }

        std::wcout << L"bytesReturned=" << io.bytesReturned
                   << L" win32Error=" << io.win32Error << L"\n";
        return 0;
    }

    // printSimpleProcessEntry renders one process row with the most useful stable fields.
    // Inputs: shared process entry from a variable response.
    // Processing: prints IDs, field flags, object pointers and decoded names.
    // Returns: no value.
    void printSimpleProcessEntry(const KSWORD_ARK_PROCESS_ENTRY& entry)
    {
        std::wcout << L"  pid=" << entry.processId
                   << L" ppid=" << entry.parentProcessId
                   << L" flags=0x" << std::hex << entry.flags
                   << L" fieldFlags=0x" << entry.fieldFlags
                   << L" r0Status=" << std::dec << entry.r0Status
                   << L" image='" << fixedAnsi(entry.imageName, sizeof(entry.imageName)).c_str() << L"'\n";
        const std::wstring imagePath = fixedUtf16(entry.imagePath, KSWORD_ARK_PROCESS_IMAGE_PATH_CHARS);
        dumpWideText(L"imagePath", imagePath);
    }

    // printSimpleThreadEntry renders one thread row with cross-view relevant fields.
    // Inputs: shared thread entry.
    // Processing: prints ID, flags, stack info availability and counter sources.
    // Returns: no value.
    void printSimpleThreadEntry(const KSWORD_ARK_THREAD_ENTRY& entry)
    {
        std::wcout << L"  tid=" << entry.threadId
                   << L" pid=" << entry.processId
                   << L" flags=0x" << std::hex << entry.flags
                   << L" fieldFlags=0x" << entry.fieldFlags
                   << L" r0Status=" << std::dec << entry.r0Status
                   << L" stackBase=" << hex64(entry.stackBase)
                   << L" kernelStack=" << hex64(entry.kernelStack) << L"\n";
    }

    // copyBytesToFixed copies a byte vector into a fixed protocol array.
    // Inputs: destination array, destination capacity, and source bytes.
    // Processing: zero-fills destination and copies at most capacity bytes.
    // Returns: no value; destination remains deterministic for METHOD_BUFFERED.
    void copyBytesToFixed(unsigned char* destination, std::size_t capacity, const std::vector<std::uint8_t>& source)
    {
        if (destination == nullptr || capacity == 0U)
        {
            return;
        }
        std::fill(destination, destination + capacity, 0U);
        const std::size_t copyBytes = std::min<std::size_t>(capacity, source.size());
        if (copyBytes != 0U)
        {
            std::copy(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(copyBytes), destination);
        }
    }

    // checkedDwordSize converts vector sizes into Win32 DWORD byte counts.
    // Inputs: byte count in size_t.
    // Processing: rejects values that DeviceIoControl cannot represent.
    // Returns: DWORD byte count.
    DWORD checkedDwordSize(std::size_t bytes)
    {
        if (bytes > std::numeric_limits<DWORD>::max())
        {
            throw std::out_of_range("buffer too large");
        }
        return static_cast<DWORD>(bytes);
    }

    // boundedPathLength validates a fixed WCHAR path protocol field.
    // Inputs: command line path and target field capacity including terminator.
    // Processing: rejects empty or over-capacity paths before copying.
    // Returns: character length excluding NUL.
    unsigned short boundedPathLength(const std::wstring& pathText, std::size_t capacity)
    {
        if (pathText.empty() || pathText.size() >= capacity)
        {
            throw std::out_of_range("path length");
        }
        return static_cast<unsigned short>(pathText.size());
    }

    // sendRawIoctl opens the driver and performs one synchronous IOCTL call.
    // Inputs: label/code, optional input buffer, output buffer vector and access.
    // Processing: handles CreateFile/DeviceIoControl transport errors uniformly.
    // Returns: CLI-style exit code; io/output receive raw completion data.
    int sendRawIoctl(
        const wchar_t* label,
        DWORD code,
        void* input,
        DWORD inputBytes,
        std::vector<std::uint8_t>& output,
        IoctlResult& io,
        DWORD desiredAccess)
    {
        DriverHandle handle = openDriverOrReport(desiredAccess);
        if (!handle.valid())
        {
            // 打不开设备时也要把真实错误码留在 io 里：调用方普遍会在失败分支上
            // 打印 io.win32Error，不填就会印出一个 0，把"设备不存在(2)"说成
            // "没有错误"。姊妹函数 sendFixedRequestResponse 本来就是这么做的。
            io.win32Error = ::GetLastError();
            io.ok = false;
            return 2;
        }
        io = sendIoctl(
            handle,
            code,
            input,
            inputBytes,
            output.empty() ? nullptr : output.data(),
            checkedDwordSize(output.size()));
        if (!io.ok)
        {
            printWin32Error(label, io.win32Error);
            return 3;
        }
        return 0;
    }

    // validateVariable validates header + variable entries before parsing rows.
    // Inputs: returned bytes, header size, entry size and minimum row size.
    // Processing: checks short buffers and invalid row sizes.
    // Returns: available row count in the returned buffer.
    std::size_t validateVariable(DWORD bytesReturned, std::size_t headerSize, std::uint32_t entrySize, std::size_t minEntrySize, const wchar_t* label)
    {
        if (bytesReturned < headerSize)
        {
            std::wcerr << L"error: " << label << L" response too small: " << bytesReturned << L" bytes\n";
            throw std::runtime_error("small response");
        }
        if (entrySize < minEntrySize)
        {
            std::wcerr << L"error: " << label << L" invalid entrySize=" << entrySize << L"\n";
            throw std::runtime_error("entry size");
        }
        return (static_cast<std::size_t>(bytesReturned) - headerSize) / static_cast<std::size_t>(entrySize);
    }

    // isUnsupportedTransportError recognizes old-driver or missing-IOCTL results.
    // Inputs: Win32 error returned by DeviceIoControl.
    // Processing: maps common unsupported/unimplemented transport statuses.
    // Returns: true when the caller should print "unsupported / unavailable".
    bool isUnsupportedTransportError(DWORD error)
    {
        return error == ERROR_INVALID_FUNCTION ||
               error == ERROR_NOT_SUPPORTED ||
               error == ERROR_CALL_NOT_IMPLEMENTED ||
               error == ERROR_PROC_NOT_FOUND;
    }

    // normalizeIoctlRc keeps new audit commands graceful on older drivers.
    // Inputs: feature label, IoctlResult and the raw helper return code.
    // Processing: rewrites missing IOCTL failures into a clear audit message.
    // Returns: CLI exit code; 5 means supported by CLI but unavailable in driver.
    int normalizeIoctlRc(const wchar_t* feature, const IoctlResult& io, int rc)
    {
        if (rc == 3 && isUnsupportedTransportError(io.win32Error))
        {
            std::wcerr << L"unsupported / unavailable: " << feature
                       << L" (driver does not expose this read-only IOCTL)\n";
            return 5;
        }
        return rc;
    }

    // commandUnsupported reports a known command whose protocol is not present.
    // Inputs: feature and reason strings.
    // Processing: prints a stable phrase required by acceptance checks.
    // Returns: non-zero CLI status to let scripts detect degraded support.
    int commandUnsupported(const wchar_t* feature, const wchar_t* reason)
    {
        std::wcerr << L"unsupported / unavailable: " << feature
                   << L" (" << reason << L")\n";
        return 5;
    }
}
