#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "../shared/KswordArkLogProtocol.h"
#include "../shared/driver/KswordArkCallbackIoctl.h"
#include "../shared/driver/KswordArkProcessIoctl.h"
#include "../shared/driver/KswordArkThreadIoctl.h"

// Legacy CLI core: shared transport state, IOCTL wrappers and error mapping.
// Inputs: the legacy CLI translation units that issue KswordARK IOCTLs.
// Processing: declares the mutable transport status globals plus the
//             DriverHandle/IoctlResult types and the send/open helpers.
// Returns: no behavior; see CliCore.cpp for definitions.
//
// The three globals below are a single instance across every legacy CLI
// translation unit on purpose: wmain maps its exit code from them and
// dispatchNested restores them to isolate nested commands.
namespace ks::cli::legacy
{
    constexpr DWORD kDefaultShareMode = FILE_SHARE_READ | FILE_SHARE_WRITE;
    constexpr DWORD kDefaultDesiredAccess = GENERIC_READ | GENERIC_WRITE;
    constexpr std::size_t kSmallResponseBytes = 64U * 1024U;
    constexpr std::size_t kLargeResponseBytes = 2U * 1024U * 1024U;
    constexpr std::size_t kHugeResponseBytes = 4U * 1024U * 1024U;
    constexpr std::size_t kMaxCommandBytes = 64U * 1024U * 1024U;
    constexpr std::size_t kMaxHexBytes = 256U;
    extern DWORD lastIoctlError;
    extern DWORD lastOpenError;
    extern int responseFailureRc;

    // Transport success does not imply that the operation in the packet succeeded.
    int operationStatusRc(long status);

    // IoctlResult mirrors the Win32 DeviceIoControl completion state.
    // Inputs: fields are assigned by sendIoctl after each kernel request.
    // Processing: no ownership is stored here; this is a plain result packet.
    // Returns: no behavior; callers inspect ok, win32Error, and bytesReturned.
    struct IoctlResult
    {
        bool ok = false;
        DWORD win32Error = ERROR_SUCCESS;
        DWORD bytesReturned = 0;
    };

    // DriverHandle owns a Win32 HANDLE returned by CreateFileW.
    // Inputs: constructed from a raw HANDLE or default-invalid state.
    // Processing: closes the handle in reset/destructor and supports moves.
    // Returns: native() exposes the raw handle for Win32 calls without transfer.
    class DriverHandle
    {
    public:
        DriverHandle() noexcept = default;

        explicit DriverHandle(HANDLE handle) noexcept
            : handle_(handle)
        {
        }

        ~DriverHandle()
        {
            reset();
        }

        DriverHandle(const DriverHandle&) = delete;
        DriverHandle& operator=(const DriverHandle&) = delete;

        DriverHandle(DriverHandle&& other) noexcept
            : handle_(other.release())
        {
        }

        DriverHandle& operator=(DriverHandle&& other) noexcept
        {
            if (this != &other)
            {
                reset(other.release());
            }
            return *this;
        }

        bool valid() const noexcept
        {
            return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
        }

        HANDLE native() const noexcept
        {
            return handle_;
        }

        HANDLE release() noexcept
        {
            HANDLE detached = handle_;
            handle_ = INVALID_HANDLE_VALUE;
            return detached;
        }

        void reset(HANDLE next = INVALID_HANDLE_VALUE) noexcept
        {
            if (valid())
            {
                ::CloseHandle(handle_);
            }
            handle_ = next;
        }

    private:
        HANDLE handle_ = INVALID_HANDLE_VALUE;
    };

    // printWin32Error renders a failed Win32 operation in a compact form.
    // Inputs: operation describes the API/IOCTL; error is GetLastError output.
    // Processing: prints decimal and hexadecimal forms for quick triage.
    // Returns: no value; output goes to stderr.
    void printWin32Error(const wchar_t* operation, DWORD error);

    // openDriver opens the shared KswordARK control/log device.
    // Inputs: desiredAccess usually includes read/write because some IOCTLs are gated.
    // Processing: calls CreateFileW on the path exported by shared protocol headers.
    // Returns: RAII DriverHandle; caller checks valid() before issuing requests.
    DriverHandle openDriver(DWORD desiredAccess = kDefaultDesiredAccess);

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
        DWORD outputBytes);

    // currentUtc100ns returns the current UTC FILETIME tick count.
    // Inputs: none.
    // Processing: converts GetSystemTimePreciseAsFileTime into a 64-bit scalar.
    // Returns: UTC time in 100ns units.
    std::uint64_t currentUtc100ns();

    // formatGuid128 renders the callback GUID packet in the same shape as UI code.
    // Inputs: fixed 16-byte GUID packet.
    // Processing: prints uppercase hexadecimal with 8-4-4-4-12 grouping.
    // Returns: formatted GUID text.
    std::wstring formatGuid128(const KSWORD_ARK_GUID128& guid);

    // parseGuid128 decodes a 32-hex-digit callback GUID string.
    // Inputs: GUID text with or without dashes.
    // Processing: strips separators and decodes byte pairs in order.
    // Returns: GUID packet or throws on malformed text.
    KSWORD_ARK_GUID128 parseGuid128(const std::wstring& text);

    // openDriverOrReport opens the shared control device and emits a uniform error.
    // Inputs: desired access for the pending IOCTL.
    // Processing: caller receives a possibly-invalid handle and may stop on failure.
    // Returns: DriverHandle RAII wrapper.
    DriverHandle openDriverOrReport(DWORD desiredAccess = kDefaultDesiredAccess);

    // runNoOutputIoctl issues a control code whose success is mostly transport-level.
    // Inputs: control code and optional request buffer.
    // Processing: prints bytesReturned and Win32 completion information.
    // Returns: CLI exit code.
    int runNoOutputIoctl(
        const wchar_t* label,
        DWORD code,
        void* input,
        DWORD inputBytes,
        DWORD desiredAccess = kDefaultDesiredAccess);

    // printSimpleProcessEntry renders one process row with the most useful stable fields.
    // Inputs: shared process entry from a variable response.
    // Processing: prints IDs, field flags, object pointers and decoded names.
    // Returns: no value.
    void printSimpleProcessEntry(const KSWORD_ARK_PROCESS_ENTRY& entry);

    // printSimpleThreadEntry renders one thread row with cross-view relevant fields.
    // Inputs: shared thread entry.
    // Processing: prints ID, flags, stack info availability and counter sources.
    // Returns: no value.
    void printSimpleThreadEntry(const KSWORD_ARK_THREAD_ENTRY& entry);

    // copyBytesToFixed copies a byte vector into a fixed protocol array.
    // Inputs: destination array, destination capacity, and source bytes.
    // Processing: zero-fills destination and copies at most capacity bytes.
    // Returns: no value; destination remains deterministic for METHOD_BUFFERED.
    void copyBytesToFixed(unsigned char* destination, std::size_t capacity, const std::vector<std::uint8_t>& source);

    // checkedDwordSize converts vector sizes into Win32 DWORD byte counts.
    // Inputs: byte count in size_t.
    // Processing: rejects values that DeviceIoControl cannot represent.
    // Returns: DWORD byte count.
    DWORD checkedDwordSize(std::size_t bytes);

    // boundedPathLength validates a fixed WCHAR path protocol field.
    // Inputs: command line path and target field capacity including terminator.
    // Processing: rejects empty or over-capacity paths before copying.
    // Returns: character length excluding NUL.
    unsigned short boundedPathLength(const std::wstring& pathText, std::size_t capacity);

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
        DWORD desiredAccess = kDefaultDesiredAccess);

    // validateVariable validates header + variable entries before parsing rows.
    // Inputs: returned bytes, header size, entry size and minimum row size.
    // Processing: checks short buffers and invalid row sizes.
    // Returns: available row count in the returned buffer.
    std::size_t validateVariable(DWORD bytesReturned, std::size_t headerSize, std::uint32_t entrySize, std::size_t minEntrySize, const wchar_t* label);

    // isUnsupportedTransportError recognizes old-driver or missing-IOCTL results.
    // Inputs: Win32 error returned by DeviceIoControl.
    // Processing: maps common unsupported/unimplemented transport statuses.
    // Returns: true when the caller should print "unsupported / unavailable".
    bool isUnsupportedTransportError(DWORD error);

    // normalizeIoctlRc keeps new audit commands graceful on older drivers.
    // Inputs: feature label, IoctlResult and the raw helper return code.
    // Processing: rewrites missing IOCTL failures into a clear audit message.
    // Returns: CLI exit code; 5 means supported by CLI but unavailable in driver.
    int normalizeIoctlRc(const wchar_t* feature, const IoctlResult& io, int rc);

    // commandUnsupported reports a known command whose protocol is not present.
    // Inputs: feature and reason strings.
    // Processing: prints a stable phrase required by acceptance checks.
    // Returns: non-zero CLI status to let scripts detect degraded support.
    int commandUnsupported(const wchar_t* feature, const wchar_t* reason);

    // Fixed-response helpers are templates: their definitions must stay visible
    // to every caller, so they live in this header instead of CliCore.cpp.
    template <typename TResponse>
    bool sendFixedNoInput(
        DWORD code,
        const wchar_t* label,
        TResponse& response,
        IoctlResult& io,
        DWORD desiredAccess = kDefaultDesiredAccess)
    {
        DriverHandle handle = openDriverOrReport(desiredAccess);
        if (!handle.valid())
        {
            io.win32Error = ::GetLastError();
            return false;
        }

        io = sendIoctl(handle, code, nullptr, 0U, &response, static_cast<DWORD>(sizeof(response)));
        if (!io.ok)
        {
            printWin32Error(label, io.win32Error);
            return false;
        }
        if (io.bytesReturned < sizeof(response))
        {
            std::wcerr << L"error: " << label << L" response too small: " << io.bytesReturned << L" bytes\n";
            io.ok = false;
            io.win32Error = ERROR_INSUFFICIENT_BUFFER;
            return false;
        }
        return true;
    }

    template <typename TRequest, typename TResponse>
    bool sendFixedRequestResponse(
        DWORD code,
        const wchar_t* label,
        const TRequest& request,
        TResponse& response,
        IoctlResult& io,
        DWORD desiredAccess = kDefaultDesiredAccess)
    {
        DriverHandle handle = openDriverOrReport(desiredAccess);
        if (!handle.valid())
        {
            io.win32Error = ::GetLastError();
            return false;
        }

        io = sendIoctl(
            handle,
            code,
            const_cast<TRequest*>(&request),
            static_cast<DWORD>(sizeof(request)),
            &response,
            static_cast<DWORD>(sizeof(response)));
        if (!io.ok)
        {
            printWin32Error(label, io.win32Error);
            return false;
        }
        if (io.bytesReturned < sizeof(response))
        {
            std::wcerr << L"error: " << label << L" response too small: " << io.bytesReturned << L" bytes\n";
            io.ok = false;
            io.win32Error = ERROR_INSUFFICIENT_BUFFER;
            return false;
        }
        return true;
    }

    // sendBlobFixedResponse sends a raw input blob and receives one fixed response.
    // Inputs: IOCTL label/code, blob bytes, response object, result packet, access.
    // Processing: performs METHOD_BUFFERED DeviceIoControl with size validation.
    // Returns: true on a transport-level success with a full fixed response.
    template <typename TResponse>
    bool sendBlobFixedResponse(
        DWORD code,
        const wchar_t* label,
        std::vector<std::uint8_t>& input,
        TResponse& response,
        IoctlResult& io,
        DWORD desiredAccess = kDefaultDesiredAccess)
    {
        DriverHandle handle = openDriverOrReport(desiredAccess);
        if (!handle.valid())
        {
            io.win32Error = ::GetLastError();
            return false;
        }

        io = sendIoctl(handle, code, input.data(), checkedDwordSize(input.size()), &response, static_cast<DWORD>(sizeof(response)));
        if (!io.ok)
        {
            printWin32Error(label, io.win32Error);
            return false;
        }
        if (io.bytesReturned < sizeof(response))
        {
            std::wcerr << L"error: " << label << L" response too small: " << io.bytesReturned << L" bytes\n";
            io.ok = false;
            io.win32Error = ERROR_INSUFFICIENT_BUFFER;
            return false;
        }
        return true;
    }
}
