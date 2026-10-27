#include "mxl-monitoring/http.h"
#include <cerrno>
#include <chrono>
#include <atomic>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "mxl-monitoring/prometheus.h"

struct mxlMonitoringHttp_t
{
    mxlMonitoring monitoring = nullptr;
    int listener = -1;
    std::uint16_t port = 0;
    std::atomic<bool> stopping{false};
    std::thread thread;
};

namespace
{
    using Clock = std::chrono::steady_clock;

    constexpr auto Budget = std::chrono::milliseconds{200};
    // Stopping shuts the listening socket down, which ends the poll at once;
    // the timeout only bounds a stop that shutdown did not wake.
    constexpr auto StopPoll = 1000;
    constexpr std::size_t MaxHeader = 8 * 1024;

    bool ready(int fd, short events, Clock::time_point deadline)
    {
        auto const left = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
        auto pfd = pollfd{.fd = fd, .events = events, .revents = 0};
        return left > 0 && ::poll(&pfd, 1, static_cast<int>(left)) > 0;
    }

    // The request up to the end of its headers, "" when the budget passes
    // first or the peer closes, and what was read when it grows past
    // MaxHeader.
    std::string readHead(int client)
    {
        auto const deadline = Clock::now() + Budget;
        auto head = std::string{};
        char buffer[1024];
        while (head.size() <= MaxHeader && ready(client, POLLIN, deadline))
        {
            auto const n = ::recv(client, buffer, sizeof(buffer), MSG_DONTWAIT);
            if (n <= 0)
            {
                return {};
            }
            // The blank line may straddle two reads.
            auto const from = head.size() < 3 ? 0 : head.size() - 3;
            head.append(buffer, static_cast<std::size_t>(n));
            if (head.find("\r\n\r\n", from) != std::string::npos)
            {
                return head;
            }
        }
        return head.size() > MaxHeader ? head : std::string{};
    }

    void sendAll(int client, std::string_view data)
    {
        auto const deadline = Clock::now() + Budget;
        while (!data.empty() && ready(client, POLLOUT, deadline))
        {
            auto const n = ::send(client, data.data(), data.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n <= 0)
            {
                return;
            }
            data.remove_prefix(static_cast<std::size_t>(n));
        }
    }

    std::string response(std::string_view status, std::string_view type, std::string const& body, std::string_view extra = {})
    {
        auto out = std::string{"HTTP/1.1 "};
        out.append(status).append("\r\nContent-Type: ").append(type);
        out.append("\r\nContent-Length: ").append(std::to_string(body.size()));
        out.append("\r\nConnection: close\r\n").append(extra).append("\r\n");
        return out + body;
    }

    std::string metrics(mxlMonitoring monitoring)
    {
        // Flows may be added between asking for the size and rendering.
        auto size = std::size_t{0};
        auto text = std::string{};
        for (auto attempt = 0; attempt < 4; ++attempt)
        {
            text.resize(size);
            auto const status = mxlMonitoringPrometheusRender(monitoring, size == 0 ? nullptr : text.data(), &size);
            if (status == MXL_STATUS_OK)
            {
                text.resize(size == 0 ? 0 : size - 1);
                return text;
            }
        }
        return {};
    }

    std::string answer(mxlMonitoring monitoring, std::string const& head)
    {
        if (head.size() > MaxHeader)
        {
            return response("431 Request Header Fields Too Large", "text/plain", "request header too large\n");
        }
        auto const line = std::string_view{head}.substr(0, head.find("\r\n"));
        auto const sp1 = line.find(' ');
        auto const sp2 = sp1 == std::string_view::npos ? std::string_view::npos : line.find(' ', sp1 + 1);
        if (sp2 == std::string_view::npos)
        {
            return response("400 Bad Request", "text/plain", "malformed request\n");
        }
        auto const method = line.substr(0, sp1);
        auto const target = line.substr(sp1 + 1, sp2 - sp1 - 1);
        auto const path = target.substr(0, target.find('?'));
        if (path != "/metrics" && path != "/healthz")
        {
            return response("404 Not Found", "text/plain", "not found\n");
        }
        if (method != "GET")
        {
            return response("405 Method Not Allowed", "text/plain", "only GET\n", "Allow: GET\r\n");
        }
        if (path == "/healthz")
        {
            return response("200 OK", "text/plain", "ok\n");
        }
        return response("200 OK", "text/plain; version=0.0.4; charset=utf-8", metrics(monitoring));
    }

    void serve(mxlMonitoringHttp http)
    {
        while (!http->stopping.load(std::memory_order_relaxed))
        {
            auto pfd = pollfd{.fd = http->listener, .events = POLLIN, .revents = 0};
            if (::poll(&pfd, 1, StopPoll) <= 0)
            {
                continue;
            }
            auto const client = ::accept4(http->listener, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0)
            {
                continue;
            }
            if (auto const head = readHead(client); !head.empty())
            {
                sendAll(client, answer(http->monitoring, head));
            }
            ::close(client);
        }
    }

    int listen(char const* address, std::uint16_t port)
    {
        auto hints = addrinfo{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;
        addrinfo* found = nullptr;
        if (auto const err = ::getaddrinfo(address, std::to_string(port).c_str(), &hints, &found); err != 0)
        {
            errno = err == EAI_SYSTEM ? errno : EINVAL;
            return -1;
        }
        auto fd = -1;
        auto error = 0;
        for (auto* a = found; a != nullptr && fd < 0; a = a->ai_next)
        {
            fd = ::socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
            if (fd < 0)
            {
                error = errno;
                continue;
            }
            auto const on = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
            if (::bind(fd, a->ai_addr, a->ai_addrlen) != 0 || ::listen(fd, 16) != 0)
            {
                error = errno;
                ::close(fd);
                fd = -1;
            }
        }
        ::freeaddrinfo(found);
        if (fd < 0)
        {
            errno = error;
        }
        return fd;
    }

    std::uint16_t boundPort(int fd)
    {
        auto addr = sockaddr_storage{};
        auto len = socklen_t{sizeof(addr)};
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        {
            return 0;
        }
        if (addr.ss_family == AF_INET6)
        {
            return ntohs(reinterpret_cast<sockaddr_in6 const*>(&addr)->sin6_port);
        }
        return ntohs(reinterpret_cast<sockaddr_in const*>(&addr)->sin_port);
    }
}

extern "C"
{
    mxlStatus mxlMonitoringHttpStart(mxlMonitoring monitoring, char const* address, uint16_t port, mxlMonitoringHttp* http)
    {
        if (monitoring == nullptr || http == nullptr)
        {
            return MXL_ERR_INVALID_ARG;
        }
        auto const fd = listen(address, port);
        if (fd < 0)
        {
            return MXL_ERR_UNKNOWN;
        }
        auto* const h = new (std::nothrow) mxlMonitoringHttp_t{};
        if (h == nullptr)
        {
            ::close(fd);
            return MXL_ERR_UNKNOWN;
        }
        h->monitoring = monitoring;
        h->listener = fd;
        h->port = boundPort(fd);
        try
        {
            h->thread = std::thread{serve, h};
        }
        catch (...)
        {
            ::close(fd);
            delete h;
            return MXL_ERR_UNKNOWN;
        }
        *http = h;
        return MXL_STATUS_OK;
    }

    uint16_t mxlMonitoringHttpPort(mxlMonitoringHttp http)
    {
        return http != nullptr ? http->port : 0;
    }

    void mxlMonitoringHttpStop(mxlMonitoringHttp http)
    {
        if (http == nullptr)
        {
            return;
        }
        http->stopping.store(true, std::memory_order_relaxed);
        ::shutdown(http->listener, SHUT_RDWR);
        if (http->thread.joinable())
        {
            http->thread.join();
        }
        ::close(http->listener);
        delete http;
    }
}
