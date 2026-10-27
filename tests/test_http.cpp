// The HTTP endpoint over real connections on the loopback interface.

#include <chrono>
#include <string>
#include <thread>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <netinet/in.h>
#include "mxl-monitoring/http.h"
#include "mxl-monitoring/prometheus.h"
#include "Support.hpp"

using namespace mxl::monitoring::tests;

namespace
{
    using Clock = std::chrono::steady_clock;

    int connectTo(std::uint16_t port)
    {
        auto const fd = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd >= 0);
        auto addr = sockaddr_in{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr const*>(&addr), sizeof(addr)) != 0)
        {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    // Sends request and reads until the endpoint closes, for at most 2 s.
    std::string exchange(std::uint16_t port, std::string const& request)
    {
        auto const fd = connectTo(port);
        REQUIRE(fd >= 0);
        REQUIRE(::send(fd, request.data(), request.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(request.size()));
        auto out = std::string{};
        char buffer[4096];
        auto const deadline = Clock::now() + std::chrono::seconds{2};
        while (Clock::now() < deadline)
        {
            auto pfd = pollfd{.fd = fd, .events = POLLIN, .revents = 0};
            if (::poll(&pfd, 1, 100) <= 0)
            {
                continue;
            }
            auto const n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0)
            {
                break;
            }
            out.append(buffer, static_cast<std::size_t>(n));
        }
        ::close(fd);
        return out;
    }

    std::string body(std::string const& response)
    {
        auto const end = response.find("\r\n\r\n");
        return end == std::string::npos ? std::string{} : response.substr(end + 4);
    }

    // A monitoring with one reader flow, served on a port the system picks.
    struct Endpoint
    {
        Endpoint()
        {
            REQUIRE(mxlMonitoringCreate(nullptr, &monitoring) == MXL_STATUS_OK);
            REQUIRE(mxlMonitoringAddFlow(monitoring, FlowId, MXL_MONITORING_ROLE_READER, &flow) == MXL_STATUS_OK);
            REQUIRE(mxlMonitoringHttpStart(monitoring, "127.0.0.1", 0, &http) == MXL_STATUS_OK);
            port = mxlMonitoringHttpPort(http);
            REQUIRE(port != 0);
        }

        ~Endpoint()
        {
            mxlMonitoringHttpStop(http);
            mxlMonitoringDestroy(monitoring);
        }

        Endpoint(Endpoint const&) = delete;
        Endpoint& operator=(Endpoint const&) = delete;

        mxlMonitoring monitoring = nullptr;
        mxlMonitoringFlow flow = nullptr;
        mxlMonitoringHttp http = nullptr;
        std::uint16_t port = 0;
    };
}

TEST_CASE("GET /metrics serves the Prometheus text of every flow", "[http]")
{
    auto e = Endpoint{};
    mxlMonitoringRecordRead(e.flow, MXL_STATUS_OK, 1, 1);
    auto size = std::size_t{0};
    REQUIRE(mxlMonitoringPrometheusRender(e.monitoring, nullptr, &size) == MXL_ERR_INVALID_ARG);
    auto text = std::string(size, '\0');
    REQUIRE(mxlMonitoringPrometheusRender(e.monitoring, text.data(), &size) == MXL_STATUS_OK);
    text.resize(size - 1);

    auto const response = exchange(e.port, "GET /metrics HTTP/1.1\r\nHost: test\r\n\r\n");
    CHECK(response.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(response.find("\r\nContent-Type: text/plain; version=0.0.4; charset=utf-8\r\n") != std::string::npos);
    CHECK(response.find("\r\nContent-Length: " + std::to_string(text.size()) + "\r\n") != std::string::npos);
    CHECK(body(response) == text);
    CHECK(text.find(std::string{"mxl_reader_reads_total{flow=\""} + FlowId + "\",result=\"ok\"} 1") != std::string::npos);
}

TEST_CASE("GET /healthz answers while the endpoint runs", "[http]")
{
    auto e = Endpoint{};
    auto const response = exchange(e.port, "GET /healthz HTTP/1.1\r\n\r\n");
    CHECK(response.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(body(response) == "ok\n");
}

TEST_CASE("Other paths are not found and other methods not allowed", "[http]")
{
    auto e = Endpoint{};
    CHECK(exchange(e.port, "GET /status HTTP/1.1\r\n\r\n").starts_with("HTTP/1.1 404 Not Found\r\n"));
    auto const post = exchange(e.port, "POST /metrics HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
    CHECK(post.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
    CHECK(post.find("\r\nAllow: GET\r\n") != std::string::npos);
    CHECK(exchange(e.port, "nonsense\r\n\r\n").starts_with("HTTP/1.1 400 Bad Request\r\n"));
}

TEST_CASE("A request header beyond 8 KiB is refused", "[http]")
{
    auto e = Endpoint{};
    auto const request = "GET /metrics HTTP/1.1\r\nX-Padding: " + std::string(9000, 'a') + "\r\n\r\n";
    CHECK(exchange(e.port, request).starts_with("HTTP/1.1 431 "));
}

TEST_CASE("A client that sends nothing does not hold up the next", "[http]")
{
    auto e = Endpoint{};
    auto const idle = connectTo(e.port);
    REQUIRE(idle >= 0);
    auto const started = Clock::now();
    auto const response = exchange(e.port, "GET /healthz HTTP/1.1\r\n\r\n");
    auto const took = Clock::now() - started;
    ::close(idle);
    CHECK(response.starts_with("HTTP/1.1 200 OK\r\n"));
    INFO("the idle client gets 200 ms before the next is answered");
    CHECK(took < std::chrono::seconds{1});
}

TEST_CASE("A port that is taken fails to start, and stopping frees it", "[http]")
{
    auto e = Endpoint{};
    mxlMonitoringHttp second = nullptr;
    CHECK(mxlMonitoringHttpStart(e.monitoring, "127.0.0.1", e.port, &second) == MXL_ERR_UNKNOWN);
    CHECK(mxlMonitoringHttpStart(nullptr, "127.0.0.1", 0, &second) == MXL_ERR_INVALID_ARG);

    INFO("stopping wakes the thread waiting for a connection at once");
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    auto const port = e.port;
    auto const started = Clock::now();
    mxlMonitoringHttpStop(e.http);
    e.http = nullptr;
    CHECK(Clock::now() - started < std::chrono::milliseconds{100});
    CHECK(connectTo(port) < 0);
}
