#pragma once

#include <windows.h>
#include <winhttp.h>
#include <atomic>

namespace glasswave::net::detail {

// Owns an asynchronous WinHTTP request and its callback context. Callers can expose a
// blocking interface by waiting on completion plus an abort event. Context storage stays
// alive through HANDLE_CLOSING, including cancellation while a read buffer is in use.
class HttpRequest {
  public:
    explicit HttpRequest(HINTERNET request) : m_request(request) {
        if (!m_request) {
            m_error.store(GetLastError());
            return;
        }
        m_completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_completed) {
            m_error.store(GetLastError());
            return;
        }
        m_closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_closed) {
            m_error.store(GetLastError());
            return;
        }
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(this);
        if (!WinHttpSetOption(m_request, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) {
            m_error.store(GetLastError());
            return;
        }
        m_callbacks =
            WinHttpSetStatusCallback(m_request, Callback,
                                     WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES,
                                     0) != WINHTTP_INVALID_STATUS_CALLBACK;
        if (!m_callbacks) m_error.store(GetLastError());
    }
    ~HttpRequest() {
        if (m_request) {
            WinHttpCloseHandle(m_request);
            if (m_callbacks) WaitForSingleObject(m_closed, INFINITE);
        }
        if (m_completed) CloseHandle(m_completed);
        if (m_closed) CloseHandle(m_closed);
    }
    HttpRequest(const HttpRequest&) = delete;
    HttpRequest& operator=(const HttpRequest&) = delete;
    explicit operator bool() const { return m_callbacks; }
    HINTERNET get() const { return m_request; }
    DWORD Error() const { return m_error.load(); }
    DWORD Available() const { return m_available.load(); }
    DWORD Read() const { return m_read.load(); }

    void Begin() {
        ResetEvent(m_completed);
        m_error.store(ERROR_SUCCESS);
    }
    bool Wait(BOOL started, HANDLE abortEvent, int timeoutMs) {
        if (!started && GetLastError() != ERROR_IO_PENDING) {
            m_error.store(GetLastError());
            return false;
        }
        const HANDLE events[] = {abortEvent, m_completed};
        const DWORD result = WaitForMultipleObjects(2, events, FALSE, static_cast<DWORD>(timeoutMs));
        if (result == WAIT_OBJECT_0 + 1) return Error() == ERROR_SUCCESS;
        m_error.store(result == WAIT_OBJECT_0  ? ERROR_OPERATION_ABORTED
                      : result == WAIT_TIMEOUT ? ERROR_WINHTTP_TIMEOUT
                                               : GetLastError());
        return false;
    }

  private:
    static void CALLBACK Callback(HINTERNET, DWORD_PTR context, DWORD status, void* information, DWORD size) {
        auto* self = reinterpret_cast<HttpRequest*>(context);
        if (!self) return;
        switch (status) {
        case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
            SetEvent(self->m_closed);
            return;
        case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
            self->m_available.store(*static_cast<DWORD*>(information));
            break;
        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
            self->m_read.store(size);
            break;
        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
            self->m_error.store(static_cast<WINHTTP_ASYNC_RESULT*>(information)->dwError);
            break;
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
            break;
        default:
            return;
        }
        SetEvent(self->m_completed);
    }
    HINTERNET m_request = nullptr;
    HANDLE m_completed = nullptr, m_closed = nullptr;
    bool m_callbacks = false;
    std::atomic<DWORD> m_error = 0, m_available = 0, m_read = 0;
};
} // namespace glasswave::net::detail
