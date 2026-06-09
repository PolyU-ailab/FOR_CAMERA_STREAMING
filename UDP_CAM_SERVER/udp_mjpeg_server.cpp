#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static const uint32_t MAGIC = 0x31474D4A; // "JMG1"

// Must match ESP32 struct layout.
struct __attribute__((packed)) UdpJpegHeader {
    uint32_t magic;
    uint32_t frame_id;
    uint32_t total_len;
    uint32_t offset;
    uint16_t chunk_len;
};

struct FrameBuffer {
    std::mutex mtx;

    // Latest complete JPEG frame.
    std::vector<uint8_t> latestJpeg;

    // Current reassembly buffer.
    uint32_t currentFrameId = 0;
    uint32_t expectedSize = 0;
    uint32_t receivedBytes = 0;
    std::vector<uint8_t> assembling;
    std::vector<uint8_t> receivedMap;

    bool hasFrame = false;
};

static std::atomic<bool> running(true);
static std::map<int, FrameBuffer*> cameras;

void handleSignal(int) {
    running = false;
}

bool sendAll(int sock, const uint8_t* data, size_t len) {
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(sock, data + sent, len - sent, 0);

        if (n <= 0) {
            return false;
        }

        sent += static_cast<size_t>(n);
    }

    return true;
}

bool sendString(int sock, const std::string& s) {
    return sendAll(sock, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

void udpReceiverThread(int udpPort, FrameBuffer* fb) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        std::cerr << "Failed to create UDP socket for port " << udpPort << "\n";
        return;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(udpPort);

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "Failed to bind UDP port " << udpPort << "\n";
        close(sock);
        return;
    }

    std::cout << "UDP listening on port " << udpPort << "\n";

    std::vector<uint8_t> packet(2048);

    while (running) {
        sockaddr_in src {};
        socklen_t srcLen = sizeof(src);

        ssize_t n = recvfrom(sock,
                             packet.data(),
                             packet.size(),
                             0,
                             reinterpret_cast<sockaddr*>(&src),
                             &srcLen);

        if (n <= 0) {
            continue;
        }

        if (n < static_cast<ssize_t>(sizeof(UdpJpegHeader))) {
            continue;
        }

        UdpJpegHeader hdr;
        std::memcpy(&hdr, packet.data(), sizeof(hdr));

        if (hdr.magic != MAGIC) {
            continue;
        }

        if (hdr.chunk_len == 0) {
            continue;
        }

        if (sizeof(UdpJpegHeader) + hdr.chunk_len > static_cast<size_t>(n)) {
            continue;
        }

        if (hdr.total_len == 0 || hdr.total_len > 1024 * 1024) {
            // Prevent huge invalid allocations.
            continue;
        }

        if (hdr.offset + hdr.chunk_len > hdr.total_len) {
            continue;
        }

        const uint8_t* payload = packet.data() + sizeof(UdpJpegHeader);

        std::lock_guard<std::mutex> lock(fb->mtx);

        // New frame detected.
        if (hdr.frame_id != fb->currentFrameId || hdr.total_len != fb->expectedSize) {
            fb->currentFrameId = hdr.frame_id;
            fb->expectedSize = hdr.total_len;
            fb->receivedBytes = 0;
            fb->assembling.assign(hdr.total_len, 0);
            fb->receivedMap.assign(hdr.total_len, 0);
        }

        // Copy only bytes not already received.
        for (uint32_t i = 0; i < hdr.chunk_len; ++i) {
            uint32_t pos = hdr.offset + i;

            if (!fb->receivedMap[pos]) {
                fb->assembling[pos] = payload[i];
                fb->receivedMap[pos] = 1;
                fb->receivedBytes++;
            }
        }

        // Complete JPEG received.
        if (fb->receivedBytes == fb->expectedSize) {
            fb->latestJpeg = fb->assembling;
            fb->hasFrame = true;

            std::cout << "Port " << udpPort
                      << " got complete frame "
                      << fb->currentFrameId
                      << ", size "
                      << fb->latestJpeg.size()
                      << " bytes\n";
        }
    }

    close(sock);
}

int parseCamNumber(const std::string& path) {
    // Expected path: /cam1, /cam2, /cam3, etc.
    if (path.rfind("/cam", 0) != 0) {
        return -1;
    }

    std::string numStr = path.substr(4);

    if (numStr.empty()) {
        return -1;
    }

    for (char c : numStr) {
        if (c < '0' || c > '9') {
            return -1;
        }
    }

    return std::stoi(numStr);
}

void handleHttpClient(int clientSock) {
    char buffer[2048];
    ssize_t n = recv(clientSock, buffer, sizeof(buffer) - 1, 0);

    if (n <= 0) {
        close(clientSock);
        return;
    }

    buffer[n] = '\0';
    std::string request(buffer);

    std::istringstream iss(request);
    std::string method;
    std::string path;
    std::string version;

    iss >> method >> path >> version;

    if (method != "GET") {
        sendString(clientSock,
                   "HTTP/1.1 405 Method Not Allowed\r\n"
                   "Connection: close\r\n\r\n");
        close(clientSock);
        return;
    }

    int camNumber = parseCamNumber(path);

    if (camNumber <= 0) {
        sendString(clientSock,
                   "HTTP/1.1 404 Not Found\r\n"
                   "Connection: close\r\n\r\n");
        close(clientSock);
        return;
    }

    int udpPort = 5004 + camNumber;

    if (cameras.find(udpPort) == cameras.end()) {
        sendString(clientSock,
                   "HTTP/1.1 404 Not Found\r\n"
                   "Connection: close\r\n\r\n");
        close(clientSock);
        return;
    }

    FrameBuffer* fb = cameras[udpPort];

    std::string header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-cache\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n\r\n";

    if (!sendString(clientSock, header)) {
        close(clientSock);
        return;
    }

    std::vector<uint8_t> lastSentFrame;
    uint32_t idleCount = 0;

    while (running) {
        std::vector<uint8_t> jpg;

        {
            std::lock_guard<std::mutex> lock(fb->mtx);

            if (fb->hasFrame && fb->latestJpeg != lastSentFrame) {
                jpg = fb->latestJpeg;
                lastSentFrame = fb->latestJpeg;
            }
        }

        if (!jpg.empty()) {
            std::ostringstream partHeader;
            partHeader << "--frame\r\n";
            partHeader << "Content-Type: image/jpeg\r\n";
            partHeader << "Content-Length: " << jpg.size() << "\r\n\r\n";

            if (!sendString(clientSock, partHeader.str())) {
                break;
            }

            if (!sendAll(clientSock, jpg.data(), jpg.size())) {
                break;
            }

            if (!sendString(clientSock, "\r\n")) {
                break;
            }

            idleCount = 0;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            idleCount++;
        }

        // Keep connection alive but avoid spinning forever if no frames.
        if (idleCount > 30000) {
            break;
        }
    }

    close(clientSock);
}

void httpServerThread(int httpPort) {
    int serverSock = socket(AF_INET, SOCK_STREAM, 0);

    if (serverSock < 0) {
        std::cerr << "Failed to create HTTP socket\n";
        return;
    }

    int reuse = 1;
    setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(httpPort);

    if (bind(serverSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "Failed to bind HTTP port " << httpPort << "\n";
        close(serverSock);
        return;
    }

    if (listen(serverSock, 16) < 0) {
        std::cerr << "Failed to listen on HTTP port " << httpPort << "\n";
        close(serverSock);
        return;
    }

    std::cout << "HTTP server listening on port " << httpPort << "\n";

    while (running) {
        sockaddr_in clientAddr {};
        socklen_t clientLen = sizeof(clientAddr);

        int clientSock = accept(serverSock,
                                reinterpret_cast<sockaddr*>(&clientAddr),
                                &clientLen);

        if (clientSock < 0) {
            continue;
        }

        std::thread(handleHttpClient, clientSock).detach();
    }

    close(serverSock);
}

int main(int argc, char* argv[]) {
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    int httpPort = 8080;

    // Number of cameras to listen for.
    // Default:
    // cam1 = UDP 5005
    // cam2 = UDP 5006
    // cam3 = UDP 5007
    int cameraCount = 3;

    if (argc >= 2) {
        cameraCount = std::stoi(argv[1]);
    }

    if (argc >= 3) {
        httpPort = std::stoi(argv[2]);
    }

    std::vector<std::thread> threads;
    std::vector<FrameBuffer*> buffers;

    for (int cam = 1; cam <= cameraCount; ++cam) {
        int udpPort = 5004 + cam;

        FrameBuffer* fb = new FrameBuffer();
        buffers.push_back(fb);
        cameras[udpPort] = fb;

        threads.emplace_back(udpReceiverThread, udpPort, fb);

        std::cout << "Mapping UDP " << udpPort
                  << " -> /cam" << cam << "\n";
    }

    std::thread httpThread(httpServerThread, httpPort);

    std::cout << "\nOpen streams:\n";
    for (int cam = 1; cam <= cameraCount; ++cam) {
        std::cout << "  http://SERVER_IP:" << httpPort
                  << "/cam" << cam << "\n";
    }
    std::cout << "\nPress Ctrl+C to stop.\n";

    httpThread.join();

    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    for (FrameBuffer* fb : buffers) {
        delete fb;
    }

    return 0;
}