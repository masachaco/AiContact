#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace aicontact {

class Rack;

// Tiny HTTP/1.1 server bound to 127.0.0.1 that exposes Rack::handleRpc:
//   GET  /health -> {"ok":true,"name":...,"port":...,"gateway":...}
//   POST /rpc    -> body {"method":..., "params":{...}, "rack"?: name|port}
//
// Discovery without files: every rack listens on its own port in
// (gatewayPort, gatewayPort + kPortRange), and one of them also holds the
// well-known gateway port (47860). Racks register with the gateway every
// second; the gateway answers "racks.list" and relays requests carrying
// "rack" to the rack they name, so clients only ever talk to the gateway port.
// When the gateway's rack closes, the next rack to notice takes the port over
// and the others re-register with it.
class ControlServer {
public:
    static constexpr int kDefaultGatewayPort = 47860;
    static constexpr int kPortRange = 40;
    // Returns the current API token (called for every request, so changes apply at once).
    using TokenProvider = std::function<std::string()>;

    ControlServer(Rack& rack, TokenProvider token) : rack_(rack), token_(std::move(token)) {}
    ~ControlServer() { stop(); }

    // The gateway port can be moved with the AICONTACT_PORT environment variable
    // (tests use that to stay away from racks that are already running).
    bool start();
    void stop();
    int port() const { return port_.load(); }  // this rack's own port
    int gatewayPort() const { return gatewayPort_; }
    bool isGateway() const { return gatewaySocket_.load() >= 0; }

private:
    struct Peer {
        std::string name, os, version;
        int pid = 0;
        double lastSeen = 0;
    };

    void serve();
    void registrar();
    void handleClient(intptr_t client);
    std::string handleRpc(const std::string& body, const std::string& token, int& status);
    bool tryBecomeGateway();
    std::string selfInfo() const;  // JSON object

    Rack& rack_;
    TokenProvider token_;
    std::thread server_, registrar_;
    std::atomic<bool> running_{false};
    std::atomic<int> port_{0};
    int gatewayPort_ = kDefaultGatewayPort;
    intptr_t ownSocket_ = -1;
    std::atomic<intptr_t> gatewaySocket_{-1};
    std::atomic<int> activeClients_{0};

    std::mutex peersMutex_;
    std::map<int, Peer> peers_;  // other racks registered with this gateway, by port

    std::mutex wakeMutex_;
    std::condition_variable wake_;
    bool warnedToken_ = false;
};

}  // namespace aicontact
