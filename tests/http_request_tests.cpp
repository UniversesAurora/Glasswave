#include <winsock2.h>
#include "netease/http_request.h"
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

using glasswave::netease::detail::HttpRequest;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Only a loopback listener. No public service or credentials participate in these tests.
class Server {
  public:
    enum class Mode {
        Success,
        HeadersBlocked,
        BodyBlocked,
        NotFound
    };
    explicit Server(Mode mode) {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        Check(listener != INVALID_SOCKET, "socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Check(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind");
        int length = sizeof(address);
        Check(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0, "getsockname");
        port = ntohs(address.sin_port);
        Check(listen(listener, 1) == 0, "listen");
        worker = std::thread([this, mode] {
            const SOCKET client = accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            DWORD timeout = 2000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                       sizeof(timeout));
            std::array<char, 4096> buffer{};
            if (recv(client, buffer.data(), static_cast<int>(buffer.size()), 0) > 0) {
                if (mode == Mode::Success || mode == Mode::NotFound) {
                    const std::string response =
                        std::string("HTTP/1.1 ") + (mode == Mode::Success ? "200 OK" : "404 Not Found") +
                        "\r\nContent-Length: 12\r\nConnection: close\r\n\r\nfixture body";
                    send(client, response.data(), static_cast<int>(response.size()), 0);
                } else if (mode == Mode::BodyBlocked) {
                    const std::string headers =
                        "HTTP/1.1 200 OK\r\nContent-Length: 12\r\nConnection: close\r\n\r\n";
                    send(client, headers.data(), static_cast<int>(headers.size()), 0);
                } else if (mode == Mode::HeadersBlocked) {
                    const std::string partial = "HTTP/1.1 200 OK\r\n";
                    send(client, partial.data(), static_cast<int>(partial.size()), 0);
                }
                if (mode == Mode::HeadersBlocked || mode == Mode::BodyBlocked)
                    recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
            }
            closesocket(client);
        });
    }
    ~Server() {
        closesocket(listener);
        if (worker.joinable()) worker.join();
    }
    INTERNET_PORT port = 0;

  private:
    SOCKET listener = INVALID_SOCKET;
    std::thread worker;
};

struct Event {
    HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Event() { CloseHandle(handle); }
};
struct Internet {
    HINTERNET handle = nullptr;
    ~Internet() {
        if (handle) WinHttpCloseHandle(handle);
    }
};

void Run(Server::Mode mode, bool timeout = false) {
    Server server(mode);
    Internet session{WinHttpOpen(L"Glasswave tests", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC)};
    Check(session.handle != nullptr, "WinHttpOpen");
    Internet connection{WinHttpConnect(session.handle, L"127.0.0.1", server.port, 0)};
    Check(connection.handle != nullptr, "WinHttpConnect");
    Event abort;
    std::array<char, 64> buffer{}; // Outlives the request's final callback.
    HttpRequest request(WinHttpOpenRequest(connection.handle, L"GET", L"/fixture", nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    Check(static_cast<bool>(request), "request callback setup");
    const bool blocked = mode == Server::Mode::HeadersBlocked || mode == Server::Mode::BodyBlocked;
    std::jthread cancel;
    if (blocked && !timeout)
        cancel = std::jthread([&] {
            Sleep(40);
            SetEvent(abort.handle);
        });
    const auto start = std::chrono::steady_clock::now();
    request.Begin();
    if (!request.Wait(WinHttpSendRequest(request.get(), nullptr, 0, nullptr, 0, 0,
                                         reinterpret_cast<DWORD_PTR>(&request)),
                      abort.handle, timeout ? 60 : 2000)) {
        if (blocked) {
            Check(request.Error() ==
                      static_cast<DWORD>(timeout ? ERROR_WINHTTP_TIMEOUT : ERROR_OPERATION_ABORTED),
                  "pending send interruption reason");
            Check(std::chrono::steady_clock::now() - start < std::chrono::seconds(1),
                  "prompt send cancellation");
            return;
        }
        throw std::runtime_error("send completion error " + std::to_string(request.Error()));
    }
    if (mode == Server::Mode::Success || mode == Server::Mode::NotFound) {
        request.Begin();
        Check(request.Wait(WinHttpReceiveResponse(request.get(), nullptr), abort.handle, 2000),
              "headers completion");
        DWORD status = 0, bytes = sizeof(status);
        Check(WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                  nullptr, &status, &bytes, nullptr) != FALSE,
              "status header");
        Check(status == (mode == Server::Mode::Success ? 200u : 404u), "HTTP status");
        request.Begin();
        Check(request.Wait(WinHttpQueryDataAvailable(request.get(), nullptr), abort.handle, 2000),
              "data completion");
        Check(request.Available() == 12, "available body size");
        request.Begin();
        Check(request.Wait(WinHttpReadData(request.get(), buffer.data(), request.Available(), nullptr),
                           abort.handle, 2000),
              "read completion");
        Check(std::string(buffer.data(), request.Read()) == "fixture body", "response body");
        return;
    }
    if (mode == Server::Mode::BodyBlocked) {
        request.Begin();
        Check(request.Wait(WinHttpReceiveResponse(request.get(), nullptr), abort.handle, 2000),
              "body fixture headers");
    }
    request.Begin();
    const BOOL operation =
        mode == Server::Mode::BodyBlocked
            ? WinHttpReadData(request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), nullptr)
            : WinHttpReceiveResponse(request.get(), nullptr);
    const bool completed = request.Wait(operation, abort.handle, timeout ? 60 : 2000);
    if (cancel.joinable()) cancel.join();
    Check(!completed, "pending operation must be interrupted");
    Check(request.Error() == static_cast<DWORD>(timeout ? ERROR_WINHTTP_TIMEOUT : ERROR_OPERATION_ABORTED),
          "interruption reason");
    Check(std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "prompt cancellation");
}

int main() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
    try {
        Run(Server::Mode::Success);
        Run(Server::Mode::NotFound);
        for (int i = 0; i < 10; ++i) {
            Run(Server::Mode::HeadersBlocked);
            Run(Server::Mode::BodyBlocked);
        }
        Run(Server::Mode::HeadersBlocked, true);
        WSACleanup();
        std::cout << "HTTP success, status, pending-header/read cancellation and timeout checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        WSACleanup();
        return 1;
    }
}
