#include "aicontact/ControlServer.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "aicontact/Auth.h"
#include "aicontact/Rack.h"
#include "aicontact/Util.h"

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <process.h>
#  pragma comment(lib, "ws2_32.lib")
using socklen_t = int;
using socket_t = SOCKET;
#  define HR_CLOSE closesocket
#  define HR_GETPID _getpid
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  define HR_CLOSE ::close
using socket_t = int;
#  define HR_GETPID ::getpid
#endif

namespace aicontact {

using json = nlohmann::json;

namespace {

constexpr size_t kMaxBody = 64 * 1024 * 1024;
constexpr int kMaxClients = 16;           // concurrent requests (each runs on its own thread)
constexpr double kPeerTimeoutSeconds = 6.0; // a rack that missed this many heartbeats is dropped
constexpr int kRelayTimeoutMs = 600000;   // relayed requests may be long (validate renders audio)

#ifdef _WIN32
struct WinsockInit {
    WinsockInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
    ~WinsockInit() { WSACleanup(); }
};
#endif

const char* osName() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

void setTimeouts(socket_t s, int ms) {
#ifdef _WIN32
    DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof(t));
#else
    timeval t{ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof(t));
#endif
}

bool sendAll(intptr_t s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = (int)::send((socket_t)s, data.data() + sent, (int)(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

void respond(intptr_t s, int status, const std::string& body) {
    const char* text = status == 200   ? "OK"
                       : status == 401 ? "Unauthorized"
                       : status == 404 ? "Not Found"
                       : status == 413 ? "Payload Too Large"
                       : status == 502 ? "Bad Gateway"
                       : status == 503 ? "Service Unavailable"
                                       : "Bad Request";
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + text +
                       "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\n\r\n";
    sendAll(s, head + body);
}

// We may answer (401, 413, ...) before reading the request body. Closing a
// socket with unread input makes Windows send RST, and the client then loses
// our response. So: stop sending, discard what is left (bounded), then close.
void closeGracefully(intptr_t client) {
    auto c = (socket_t)client;
#ifdef _WIN32
    shutdown(c, SD_SEND);
#else
    shutdown(c, SHUT_WR);
#endif
    setTimeouts(c, 200);
    char buf[16384];
    size_t drained = 0;
    while (drained < 1024 * 1024) {
        int n = (int)recv(c, buf, sizeof(buf), 0);
        if (n <= 0) break;
        drained += (size_t)n;
    }
    HR_CLOSE(c);
}

std::string lower(std::string v) {
    for (auto& c : v) c = (char)std::tolower((unsigned char)c);
    return v;
}

intptr_t listenOn(int port) {
    auto s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if ((intptr_t)s < 0) return -1;
#ifndef _WIN32
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
#else
    BOOL excl = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof(excl));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) == 0 && listen(s, 16) == 0) return (intptr_t)s;
    HR_CLOSE(s);
    return -1;
}

struct HttpResult {
    bool connected = false;
    int status = 0;
    std::string body;
};

// Minimal HTTP/1.1 client for rack-to-rack calls on 127.0.0.1.
HttpResult httpPost(int port, const std::string& token, const std::string& body, int timeoutMs) {
    HttpResult r;
    auto s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if ((intptr_t)s < 0) return r;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        HR_CLOSE(s);
        return r;
    }
    r.connected = true;
    setTimeouts(s, timeoutMs);
    const std::string req = "POST /rpc HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                            "\r\nAuthorization: Bearer " + token +
                            "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                            std::to_string(body.size()) + "\r\n\r\n" + body;
    std::string data;
    if (sendAll((intptr_t)s, req)) {
        char buf[16384];
        for (;;) {
            int n = (int)recv(s, buf, sizeof(buf), 0);
            if (n <= 0) break;
            data.append(buf, (size_t)n);
        }
    }
    HR_CLOSE(s);
    const size_t sp = data.find(' '), end = data.find("\r\n\r\n");
    if (sp != std::string::npos) r.status = std::atoi(data.c_str() + sp + 1);
    if (end != std::string::npos) r.body = data.substr(end + 4);
    return r;
}

}  // namespace

bool ControlServer::start() {
    if (running_) return true;
#ifdef _WIN32
    static WinsockInit winsock;
#endif
    if (const char* env = std::getenv("AICONTACT_PORT"))
        if (std::atoi(env) > 0) gatewayPort_ = std::atoi(env);
    for (int p = gatewayPort_ + 1; p < gatewayPort_ + kPortRange && ownSocket_ < 0; ++p) {
        ownSocket_ = listenOn(p);
        if (ownSocket_ >= 0) port_ = p;
    }
    if (ownSocket_ < 0) {
        rack_.log().log("error", "", "control server: no free port in " + std::to_string(gatewayPort_ + 1) + "-" +
                                         std::to_string(gatewayPort_ + kPortRange - 1));
        return false;
    }
    running_ = true;
    rack_.log().log("info", "", "control server listening on 127.0.0.1:" + std::to_string(port_.load()));
    tryBecomeGateway();
    server_ = std::thread([this] { serve(); });
    registrar_ = std::thread([this] { registrar(); });
    return true;
}

void ControlServer::stop() {
    if (!running_.exchange(false)) return;
    wake_.notify_all();
    if (registrar_.joinable()) registrar_.join();
    if (server_.joinable()) server_.join();
    if (!isGateway() && token_)  // leave the directory at once instead of timing out
        httpPost(gatewayPort_, token_(), json{{"method", "racks.unregister"}, {"params", {{"port", port_.load()}}}}.dump(), 300);
    HR_CLOSE((socket_t)ownSocket_);
    ownSocket_ = -1;
    const intptr_t gw = gatewaySocket_.exchange(-1);
    if (gw >= 0) HR_CLOSE((socket_t)gw);
    // Request threads use this object; let them finish (relayed requests are bounded by their timeout).
    while (activeClients_.load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    port_ = 0;
}

bool ControlServer::tryBecomeGateway() {
    const intptr_t s = listenOn(gatewayPort_);
    if (s < 0) return false;
    {
        std::lock_guard<std::mutex> lock(peersMutex_);
        peers_.clear();
    }
    gatewaySocket_ = s;
    rack_.log().log("info", "", "serving the rack directory on 127.0.0.1:" + std::to_string(gatewayPort_));
    return true;
}

std::string ControlServer::selfInfo() const {
    return json{{"name", rack_.name()}, {"port", port_.load()}, {"pid", (int)HR_GETPID()}, {"os", osName()},
                {"version", kVersion}}
        .dump();
}

void ControlServer::serve() {
    while (running_) {
        fd_set fds;
        FD_ZERO(&fds);
        const auto own = (socket_t)ownSocket_;
        const intptr_t gwRaw = gatewaySocket_.load();
        FD_SET(own, &fds);
        socket_t maxFd = own;
        if (gwRaw >= 0) {
            FD_SET((socket_t)gwRaw, &fds);
            if ((socket_t)gwRaw > maxFd) maxFd = (socket_t)gwRaw;
        }
        timeval tv{0, 200 * 1000};
        if (select((int)maxFd + 1, &fds, nullptr, nullptr, &tv) <= 0) continue;
        for (intptr_t ls : {(intptr_t)own, gwRaw}) {
            if (ls < 0 || !FD_ISSET((socket_t)ls, &fds)) continue;
            sockaddr_in peer{};
            socklen_t len = sizeof(peer);
            auto c = accept((socket_t)ls, (sockaddr*)&peer, &len);
            if ((intptr_t)c < 0) continue;
            if (activeClients_.load() >= kMaxClients) {
                respond((intptr_t)c, 503, R"({"error":"too many concurrent requests"})");
                closeGracefully((intptr_t)c);
                continue;
            }
            ++activeClients_;
            std::thread([this, c] {
                handleClient((intptr_t)c);
                closeGracefully((intptr_t)c);
                --activeClients_;
            }).detach();
        }
    }
}

void ControlServer::registrar() {
    while (running_) {
        {
            std::unique_lock<std::mutex> lock(wakeMutex_);
            wake_.wait_for(lock, std::chrono::seconds(1), [this] { return !running_.load(); });
        }
        if (!running_) break;
        if (isGateway()) {
            const double now = nowSeconds();
            std::lock_guard<std::mutex> lock(peersMutex_);
            for (auto it = peers_.begin(); it != peers_.end();)
                it = now - it->second.lastSeen > kPeerTimeoutSeconds ? peers_.erase(it) : std::next(it);
            continue;
        }
        const std::string token = token_ ? token_() : std::string();
        const auto r = httpPost(gatewayPort_, token, json{{"method", "racks.register"}, {"params", json::parse(selfInfo())}}.dump(), 2000);
        if (!r.connected) {
            tryBecomeGateway();  // nobody holds the gateway port (its rack was closed): take it over
        } else if (r.status == 401) {
            if (!warnedToken_)
                rack_.log().log("warn", "", "the rack directory on 127.0.0.1:" + std::to_string(gatewayPort_) +
                                                " uses a different API token; this rack is reachable on port " +
                                                std::to_string(port_.load()) + " only");
            warnedToken_ = true;
        } else {
            warnedToken_ = false;
        }
    }
}

void ControlServer::handleClient(intptr_t client) {
    auto c = (socket_t)client;
    setTimeouts(c, 5000);
    std::string data;
    char buf[16384];
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        int n = (int)recv(c, buf, sizeof(buf), 0);
        if (n <= 0) return;
        data.append(buf, (size_t)n);
        headerEnd = data.find("\r\n\r\n");
        if (data.size() > 65536 && headerEnd == std::string::npos) return respond(client, 400, R"({"error":"header too large"})");
    }
    const std::string head = data.substr(0, headerEnd);
    std::string body = data.substr(headerEnd + 4);

    const size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return respond(client, 400, R"({"error":"bad request"})");
    const std::string method = head.substr(0, sp1);
    const std::string path = head.substr(sp1 + 1, sp2 - sp1 - 1);

    // Reject browser-originated requests: a web page must not be able to drive the
    // rack, neither directly (Origin header) nor via DNS rebinding (foreign Host).
    const std::string lhead = lower(head);
    if (lhead.find("\r\norigin:") != std::string::npos) return respond(client, 400, R"({"error":"cross-origin requests are not allowed"})");
    size_t hostPos = lhead.find("\r\nhost:");
    if (hostPos != std::string::npos) {
        size_t start = lhead.find_first_not_of(' ', hostPos + 7);
        std::string host = lhead.substr(start, lhead.find("\r\n", start) - start);
        host = host.substr(0, host.rfind(':') == std::string::npos ? host.size() : host.rfind(':'));
        if (host != "127.0.0.1" && host != "localhost" && host != "[::1]")
            return respond(client, 400, R"({"error":"bad Host header"})");
    }

    // Every endpoint needs the API token (see Auth.h), so other local processes
    // and users cannot deploy instruments or read files through us.
    std::string presented;
    {
        size_t authPos = lhead.find("\r\nauthorization:");
        if (authPos != std::string::npos) {
            size_t start = head.find_first_not_of(' ', authPos + 16);
            std::string value = head.substr(start, head.find("\r\n", start) - start);
            if (lower(value.substr(0, 7)) == "bearer ") presented = value.substr(7);
        }
        const std::string expected = token_ ? token_() : std::string();
        if (expected.empty() || presented.empty() || !constantTimeEquals(presented, expected))
            return respond(client, 401, json{{"error", "unauthorized: send 'Authorization: Bearer <token>' "
                                                       "(copy it from the plugin's \"API Token\" button)"}}.dump());
    }

    size_t contentLength = 0;
    size_t pos = lhead.find("\r\ncontent-length:");
    if (pos != std::string::npos) contentLength = (size_t)std::strtoull(lhead.c_str() + pos + 17, nullptr, 10);
    if (contentLength > kMaxBody) return respond(client, 413, R"({"error":"body too large"})");
    setTimeouts(c, 30000);
    while (body.size() < contentLength) {
        int n = (int)recv(c, buf, sizeof(buf), 0);
        if (n <= 0) return;
        body.append(buf, (size_t)n);
    }
    body.resize(contentLength);

    if (method == "GET" && path == "/health") {
        json j = json::parse(selfInfo());
        j["ok"] = true;
        j["gateway"] = isGateway();
        j["gateway_port"] = gatewayPort_;
        j["aci_abi"] = kAciAbiVersion;
        return respond(client, 200, j.dump());
    }
    if (method == "POST" && path == "/rpc") {
        int status = 200;
        const std::string out = handleRpc(body, presented, status);
        return respond(client, status, out);
    }
    respond(client, 404, R"({"error":"not found"})");
}

std::string ControlServer::handleRpc(const std::string& body, const std::string& token, int& status) {
    json req;
    try {
        req = json::parse(body);
    } catch (const std::exception& e) {
        status = 400;
        return json{{"error", std::string("invalid JSON: ") + e.what()}}.dump();
    }
    if (!req.is_object()) return rack_.handleRpc(req).dump();
    auto fail = [](const std::string& message) { return json{{"error", message}}.dump(); };
    const std::string method = req.value("method", std::string());
    const json params = req.value("params", json::object());
    const std::string notGateway = "this rack is not the rack directory; send requests to 127.0.0.1:" +
                                   std::to_string(gatewayPort_);

    // ---- directory (gateway only) ----
    if (method == "racks.list") {
        if (!isGateway()) return fail(notGateway);
        json self = json::parse(selfInfo());
        self["gateway"] = true;
        json racks = json::array({self});
        const double now = nowSeconds();
        std::lock_guard<std::mutex> lock(peersMutex_);
        for (const auto& [port, p] : peers_)
            if (now - p.lastSeen <= kPeerTimeoutSeconds)
                racks.push_back({{"name", p.name}, {"port", port}, {"pid", p.pid}, {"os", p.os}, {"version", p.version}, {"gateway", false}});
        return json{{"result", {{"racks", racks}}}}.dump();
    }
    if (method == "racks.register" || method == "racks.unregister") {
        if (!isGateway()) return fail(notGateway);
        const int port = params.value("port", 0);
        // Only ports racks may use: the gateway relays to registered ports, nothing else.
        if (port <= gatewayPort_ || port >= gatewayPort_ + kPortRange || port == port_.load())
            return fail("port must be in " + std::to_string(gatewayPort_ + 1) + "-" + std::to_string(gatewayPort_ + kPortRange - 1));
        std::lock_guard<std::mutex> lock(peersMutex_);
        if (method == "racks.unregister") {
            peers_.erase(port);
        } else {
            Peer& p = peers_[port];
            p.name = params.value("name", std::string()).substr(0, 200);
            p.os = params.value("os", std::string()).substr(0, 20);
            p.version = params.value("version", std::string()).substr(0, 40);
            p.pid = params.value("pid", 0);
            p.lastSeen = nowSeconds();
        }
        return json{{"result", {{"ok", true}}}}.dump();
    }

    // ---- requests addressed to a rack: handle here or relay ----
    if (req.contains("rack") && !req["rack"].is_null()) {
        const json& r = req["rack"];
        int target = -1;
        std::string wanted;
        if (r.is_number_integer()) {
            target = r.get<int>();
        } else if (r.is_string()) {
            wanted = r.get<std::string>();
            if (!wanted.empty() && wanted.find_first_not_of("0123456789") == std::string::npos) target = std::atoi(wanted.c_str());
        } else {
            return fail("'rack' must be a rack name or port");
        }
        if (target < 0) {  // by name: exact match first, then case-insensitive
            std::vector<std::pair<int, std::string>> all{{port_.load(), rack_.name()}};
            {
                std::lock_guard<std::mutex> lock(peersMutex_);
                for (const auto& [port, p] : peers_) all.push_back({port, p.name});
            }
            for (bool exact : {true, false}) {
                std::vector<int> hits;
                for (const auto& [port, name] : all)
                    if (exact ? name == wanted : lower(name) == lower(wanted)) hits.push_back(port);
                if (hits.size() > 1) return fail("several racks are named '" + wanted + "'; pick one by port (racks.list)");
                if (hits.size() == 1) { target = hits[0]; break; }
            }
            if (target < 0) return fail("no rack named '" + wanted + "' (racks.list shows the open racks)");
        }
        if (target != port_.load()) {
            if (!isGateway()) return fail(notGateway);
            {
                std::lock_guard<std::mutex> lock(peersMutex_);
                if (!peers_.count(target)) return fail("no rack on port " + std::to_string(target) + " (racks.list shows the open racks)");
            }
            const auto res = httpPost(target, token, body, kRelayTimeoutMs);
            if (!res.connected) {
                status = 502;
                return fail("rack on port " + std::to_string(target) + " is not reachable");
            }
            status = res.status ? res.status : 502;
            return res.body;
        }
    }
    return rack_.handleRpc(req).dump();
}

}  // namespace aicontact
