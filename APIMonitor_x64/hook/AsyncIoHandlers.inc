        template<class T> T SafeIoValue(const T* value, T fallback = {})
        {
            __try { return value ? *value : fallback; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return fallback; }
        }
        struct IoCallbackContext
        {
            IoToken operation;
            LPOVERLAPPED_COMPLETION_ROUTINE fileCallback = nullptr;
            LPWSAOVERLAPPED_COMPLETION_ROUTINE wsaCallback = nullptr;
        };
        void CALLBACK FileIoCallback(IoCallbackContext* context, DWORD error, DWORD bytes, LPOVERLAPPED ov)
        {
            const DWORD saved = ::GetLastError();
            const int wsa = ::WSAGetLastError();
            CompleteIo(context->operation, error, bytes);
            ::WSASetLastError(wsa); ::SetLastError(saved);
            context->fileCallback(error, bytes, ov);
        }
        void CALLBACK WsaIoCallback(IoCallbackContext* context, DWORD error, DWORD bytes, LPWSAOVERLAPPED ov, DWORD flags)
        {
            const DWORD saved = ::GetLastError();
            const int wsa = ::WSAGetLastError();
            CompleteIo(context->operation, error, bytes);
            ::WSASetLastError(wsa); ::SetLastError(saved);
            context->wsaCallback(error, bytes, ov, flags);
        }
        void* PrepareIoCallback(const IoToken& operation, LPOVERLAPPED_COMPLETION_ROUTINE file,
            LPWSAOVERLAPPED_COMPLETION_ROUTINE wsa)
        {
            if (!operation || (!file && !wsa)) return nullptr;
            auto* context = new(std::nothrow) IoCallbackContext;
            if (!context) { AbortIo(operation); return nullptr; }
            context->operation = operation; context->fileCallback = file; context->wsaCallback = wsa;
            void* code = BuildContextThunk(context,
                wsa ? reinterpret_cast<void*>(&WsaIoCallback) : reinterpret_cast<void*>(&FileIoCallback), wsa ? 4 : 3);
            if (!code) { delete context; AbortIo(operation); }
            return code;
        }
        BOOL WINAPI HookedReadFileEx(HANDLE file, LPVOID buffer, DWORD length, LPOVERLAPPED ov,
            LPOVERLAPPED_COMPLETION_ROUTINE callback)
        {
            ScopedHookGuard guard;
            if (guard.bypass()) return g_readFileExOriginal(file, buffer, length, ov, callback);
            const DWORD incoming = ::GetLastError();
            auto op = BeginIo(reinterpret_cast<std::uintptr_t>(file), ov, L"KernelBase", L"ReadFileEx",
                ks::winapi_monitor::EventCategory::File, length, callback != nullptr);
            auto replacement = reinterpret_cast<LPOVERLAPPED_COMPLETION_ROUTINE>(PrepareIoCallback(op, callback, nullptr));
            if (callback && !replacement) op.reset();
            ::SetLastError(incoming);
            const BOOL result = g_readFileExOriginal(file, buffer, length, ov, replacement ? replacement : callback);
            const DWORD error = ::GetLastError();
            FinishIo(op, result != FALSE, result != FALSE, result ? 0 : error, 0);
            ::SetLastError(error); return result;
        }
        BOOL WINAPI HookedWriteFileEx(HANDLE file, LPCVOID buffer, DWORD length, LPOVERLAPPED ov,
            LPOVERLAPPED_COMPLETION_ROUTINE callback)
        {
            ScopedHookGuard guard;
            if (guard.bypass()) return g_writeFileExOriginal(file, buffer, length, ov, callback);
            const DWORD incoming = ::GetLastError();
            auto op = BeginIo(reinterpret_cast<std::uintptr_t>(file), ov, L"KernelBase", L"WriteFileEx",
                ks::winapi_monitor::EventCategory::File, length, callback != nullptr);
            auto replacement = reinterpret_cast<LPOVERLAPPED_COMPLETION_ROUTINE>(PrepareIoCallback(op, callback, nullptr));
            if (callback && !replacement) op.reset();
            ::SetLastError(incoming);
            const BOOL result = g_writeFileExOriginal(file, buffer, length, ov, replacement ? replacement : callback);
            const DWORD error = ::GetLastError();
            FinishIo(op, result != FALSE, result != FALSE, result ? 0 : error, 0);
            ::SetLastError(error); return result;
        }
        BOOL WINAPI HookedGetOverlappedResult(HANDLE file, LPOVERLAPPED ov, LPDWORD bytes, BOOL wait)
        {
            ScopedHookGuard guard;
            const BOOL result = g_getOverlappedResultOriginal(file, ov, bytes, wait);
            const DWORD error = ::GetLastError();
            if (!guard.bypass()) {
                ObserveIoResult(reinterpret_cast<std::uintptr_t>(file), ov,
                    result || (error != ERROR_IO_INCOMPLETE && error != ERROR_IO_PENDING), result ? 0 : error, SafeIoValue(bytes));
                if (ActiveConfig().enableFile) SendMonitorEventRaw(ks::winapi_monitor::EventCategory::File,L"KernelBase",L"GetOverlappedResult",result ? 0 : error,L"completion observation");
            }
            ::SetLastError(error); return result;
        }
        BOOL WINAPI HookedGetOverlappedResultEx(HANDLE file, LPOVERLAPPED ov, LPDWORD bytes, DWORD milliseconds, BOOL alertable)
        {
            ScopedHookGuard guard;
            const BOOL result = g_getOverlappedResultExOriginal(file, ov, bytes, milliseconds, alertable);
            const DWORD error = ::GetLastError();
            if (!guard.bypass()) {
                ObserveIoResult(reinterpret_cast<std::uintptr_t>(file), ov,
                    result || (error != ERROR_IO_INCOMPLETE && error != ERROR_IO_PENDING && error != WAIT_TIMEOUT && error != WAIT_IO_COMPLETION),
                    result ? 0 : error, SafeIoValue(bytes));
                if (ActiveConfig().enableFile) SendMonitorEventRaw(ks::winapi_monitor::EventCategory::File,L"KernelBase",L"GetOverlappedResultEx",result ? 0 : error,L"completion observation");
            }
            ::SetLastError(error); return result;
        }
        BOOL WINAPI HookedGetQueuedCompletionStatus(HANDLE port, LPDWORD bytes, PULONG_PTR key, LPOVERLAPPED* ov, DWORD timeout)
        {
            ScopedHookGuard guard;
            const BOOL result = g_getQueuedCompletionStatusOriginal(port, bytes, key, ov, timeout);
            const DWORD error = ::GetLastError();
            if (!guard.bypass()) {
                const auto completed = SafeIoValue(ov);
                if (completed) ObservePortCompletion(port, completed, result ? 0 : error, SafeIoValue(bytes));
                if (ActiveConfig().enableFile) SendMonitorEventRaw(ks::winapi_monitor::EventCategory::File,L"KernelBase",L"GetQueuedCompletionStatus",result ? 0 : error,L"IOCP observation");
            }
            ::SetLastError(error); return result;
        }
        DWORD IoStatusError(ULONG_PTR status)
        {
            using Convert = ULONG(NTAPI*)(NTSTATUS);
            const auto convert = reinterpret_cast<Convert>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
            return static_cast<NTSTATUS>(status) >= 0 ? 0 : convert ? convert(static_cast<NTSTATUS>(status)) : ERROR_GEN_FAILURE;
        }
        BOOL WINAPI HookedGetQueuedCompletionStatusEx(HANDLE port, LPOVERLAPPED_ENTRY entries, ULONG capacity, PULONG removed, DWORD timeout, BOOL alertable)
        {
            ScopedHookGuard guard;
            const BOOL result = g_getQueuedCompletionStatusExOriginal(port, entries, capacity, removed, timeout, alertable);
            const DWORD error = ::GetLastError();
            if (!guard.bypass()) {
                const ULONG count = result ? (std::min)((std::min)(SafeIoValue(removed), capacity), 8192UL) : 0;
                for (ULONG index = 0; entries && index < count; ++index) {
                    const auto entry = SafeIoValue(entries + index);
                    if (entry.lpOverlapped) ObservePortCompletion(port, entry.lpOverlapped, IoStatusError(entry.Internal), entry.dwNumberOfBytesTransferred);
                }
                if (ActiveConfig().enableFile) SendMonitorEventRaw(ks::winapi_monitor::EventCategory::File,L"KernelBase",L"GetQueuedCompletionStatusEx",result ? 0 : error,L"IOCP batch observation");
            }
            ::SetLastError(error); return result;
        }
        HANDLE WINAPI HookedCreateIoCompletionPort(HANDLE file, HANDLE port, ULONG_PTR key, DWORD concurrency)
        {
            ScopedHookGuard guard;
            const HANDLE result = g_createIoCompletionPortOriginal(file, port, key, concurrency);
            const DWORD error = ::GetLastError();
            if (!guard.bypass()) {
                if (result && file != INVALID_HANDLE_VALUE) BindIoPort(reinterpret_cast<std::uintptr_t>(file), result);
                if (ActiveConfig().enableFile) SendMonitorEventRaw(ks::winapi_monitor::EventCategory::File,L"KernelBase",L"CreateIoCompletionPort",result ? 0 : error,L"IOCP association");
            }
            ::SetLastError(error); return result;
        }
        BOOL WINAPI HookedSetFileCompletionNotificationModes(HANDLE file, UCHAR flags)
        {
            ScopedHookGuard guard;
            const BOOL result = g_setFileCompletionNotificationModesOriginal(file, flags);
            const DWORD error = ::GetLastError();
            if (!guard.bypass()) {
                if (result) SetIoNotificationMode(reinterpret_cast<std::uintptr_t>(file), flags);
                if (ActiveConfig().enableFile) SendMonitorEventRaw(ks::winapi_monitor::EventCategory::File,L"KernelBase",L"SetFileCompletionNotificationModes",result ? 0 : error,L"completion notification modes");
            }
            ::SetLastError(error); return result;
        }
        BOOL WSAAPI HookedWSAGetOverlappedResult(SOCKET socket, LPWSAOVERLAPPED ov, LPDWORD bytes, BOOL wait, LPDWORD flags)
        {
            ScopedHookGuard guard;
            const BOOL result = g_wSAGetOverlappedResultOriginal(socket, ov, bytes, wait, flags);
            const int error = ::WSAGetLastError(); const DWORD win32 = ::GetLastError();
            if (!guard.bypass()) {
                ObserveIoResult(socket, ov, result || (error != WSA_IO_INCOMPLETE && error != WSA_IO_PENDING), result ? 0 : error, SafeIoValue(bytes));
                SendMonitorEventRaw(ks::winapi_monitor::EventCategory::Network,L"Ws2_32",L"WSAGetOverlappedResult",result ? 0 : error,L"socket completion observation");
            }
            ::WSASetLastError(error); ::SetLastError(win32); return result;
        }
        BOOL WINAPI HookedCancelIo(HANDLE file)
        {
            ScopedHookGuard guard;
            const BOOL result = g_cancelIoOriginal(file); const DWORD error = ::GetLastError();
            if (!guard.bypass()) {
                ObserveIoCancellation(reinterpret_cast<std::uintptr_t>(file), nullptr, result ? 0 : error, true);
                if (ActiveConfig().enableFile) SendMonitorEventRaw(ks::winapi_monitor::EventCategory::File,L"KernelBase",L"CancelIo",result ? 0 : error,L"cancellation request");
            }
            ::SetLastError(error); return result;
        }
        BOOL WINAPI HookedCancelIoEx(HANDLE file, LPOVERLAPPED ov)
        {
            ScopedHookGuard guard;
            const BOOL result = g_cancelIoExOriginal(file, ov); const DWORD error = ::GetLastError();
            if (!guard.bypass()) {
                ObserveIoCancellation(reinterpret_cast<std::uintptr_t>(file), ov, result ? 0 : error);
                if (ActiveConfig().enableFile) SendMonitorEventRaw(ks::winapi_monitor::EventCategory::File,L"KernelBase",L"CancelIoEx",result ? 0 : error,L"cancellation request");
            }
            ::SetLastError(error); return result;
        }

        int WSAAPI HookedWSASendTo(SOCKET socketValue, LPWSABUF buffersPointer, DWORD bufferCount, LPDWORD bytesSentPointer, DWORD flagsValue, const sockaddr* toPointer, int toLength, LPWSAOVERLAPPED overlappedPointer, LPWSAOVERLAPPED_COMPLETION_ROUTINE completionRoutinePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) return g_wsaSendToOriginal(socketValue, buffersPointer, bufferCount, bytesSentPointer, flagsValue, toPointer, toLength, overlappedPointer, completionRoutinePointer);
            const DWORD incomingWin32 = ::GetLastError();
            const int incomingWsa = ::WSAGetLastError();
            const std::uint64_t requestLength = SumWsaBufferLength(buffersPointer, bufferCount);
            auto operation = BeginIo(socketValue, overlappedPointer, L"Ws2_32", L"WSASendTo",
                ks::winapi_monitor::EventCategory::Network, requestLength, completionRoutinePointer != nullptr);
            auto replacement = reinterpret_cast<LPWSAOVERLAPPED_COMPLETION_ROUTINE>(PrepareIoCallback(operation, nullptr, completionRoutinePointer));
            if (completionRoutinePointer && !replacement) operation.reset();
            ::WSASetLastError(incomingWsa); ::SetLastError(incomingWin32);
            const int resultValue = g_wsaSendToOriginal(socketValue, buffersPointer, bufferCount, bytesSentPointer, flagsValue, toPointer, toLength, overlappedPointer, replacement ? replacement : completionRoutinePointer);
            const int savedWsa = ::WSAGetLastError();
            const DWORD savedWin32 = ::GetLastError();
            const int errorValue = resultValue == 0 ? 0 : savedWsa;
            FinishIo(operation, resultValue == 0 || errorValue == WSA_IO_PENDING,
                resultValue != 0 && errorValue == WSA_IO_PENDING, errorValue, SafeIoValue(bytesSentPointer));
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSocketDetail(detailBuffer, L"datagram", socketValue, requestLength, SafeIoValue(bytesSentPointer), flagsValue);
            SendMonitorEventRaw(ks::winapi_monitor::EventCategory::Network,L"Ws2_32",L"WSASendTo",errorValue,detailBuffer);
            ::WSASetLastError(savedWsa); ::SetLastError(savedWin32);
            return resultValue;
        }
        int WSAAPI HookedWSARecvFrom(SOCKET socketValue, LPWSABUF buffersPointer, DWORD bufferCount, LPDWORD bytesReceivedPointer, LPDWORD flagsPointer, sockaddr* fromPointer, LPINT fromLengthPointer, LPWSAOVERLAPPED overlappedPointer, LPWSAOVERLAPPED_COMPLETION_ROUTINE completionRoutinePointer)
        {
            ScopedHookGuard guardValue;
            if (guardValue.bypass()) return g_wsaRecvFromOriginal(socketValue, buffersPointer, bufferCount, bytesReceivedPointer, flagsPointer, fromPointer, fromLengthPointer, overlappedPointer, completionRoutinePointer);
            const DWORD incomingWin32 = ::GetLastError();
            const int incomingWsa = ::WSAGetLastError();
            const std::uint64_t requestLength = SumWsaBufferLength(buffersPointer, bufferCount);
            auto operation = BeginIo(socketValue, overlappedPointer, L"Ws2_32", L"WSARecvFrom",
                ks::winapi_monitor::EventCategory::Network, requestLength, completionRoutinePointer != nullptr);
            auto replacement = reinterpret_cast<LPWSAOVERLAPPED_COMPLETION_ROUTINE>(PrepareIoCallback(operation, nullptr, completionRoutinePointer));
            if (completionRoutinePointer && !replacement) operation.reset();
            ::WSASetLastError(incomingWsa); ::SetLastError(incomingWin32);
            const int resultValue = g_wsaRecvFromOriginal(socketValue, buffersPointer, bufferCount, bytesReceivedPointer, flagsPointer, fromPointer, fromLengthPointer, overlappedPointer, replacement ? replacement : completionRoutinePointer);
            const int savedWsa = ::WSAGetLastError();
            const DWORD savedWin32 = ::GetLastError();
            const int errorValue = resultValue == 0 ? 0 : savedWsa;
            FinishIo(operation, resultValue == 0 || errorValue == WSA_IO_PENDING,
                resultValue != 0 && errorValue == WSA_IO_PENDING, errorValue, SafeIoValue(bytesReceivedPointer));
            wchar_t detailBuffer[ks::winapi_monitor::kMaxDetailChars] = {};
            BuildSocketDetail(detailBuffer, L"datagram", socketValue, requestLength, SafeIoValue(bytesReceivedPointer), SafeIoValue(flagsPointer));
            SendMonitorEventRaw(ks::winapi_monitor::EventCategory::Network,L"Ws2_32",L"WSARecvFrom",errorValue,detailBuffer);
            ::WSASetLastError(savedWsa); ::SetLastError(savedWin32);
            return resultValue;
        }
