#include <arpa/inet.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static std::atomic<bool> g_running{true};
static FILE* g_camera_pipe = nullptr;

static const uint32_t MAGIC = 0x31474D4A; // "JMG1"

// Must match server struct layout.
struct __attribute__((packed)) UdpJpegHeader {
    uint32_t magic;
    uint32_t frame_id;
    uint32_t total_len;
    uint32_t offset;
    uint16_t chunk_len;
};

struct Config {
    std::string server_ip;
    int udp_port = 5005;
    int width = 640;
    int height = 480;
    int fps = 30;
};

static void signal_handler(int) {
    g_running = false;

    if (g_camera_pipe) {
        pclose(g_camera_pipe);
        g_camera_pipe = nullptr;
    }
}

static size_t find_marker(const std::vector<uint8_t>& buf,
                          size_t start,
                          uint8_t a,
                          uint8_t b) {
    if (buf.size() < 2 || start >= buf.size() - 1) {
        return std::string::npos;
    }

    for (size_t i = start; i + 1 < buf.size(); ++i) {
        if (buf[i] == a && buf[i + 1] == b) {
            return i;
        }
    }

    return std::string::npos;
}

static bool send_jpeg_udp(int sock,
                          const sockaddr_in& server_addr,
                          const std::vector<uint8_t>& jpeg,
                          uint32_t frame_id,
                          size_t max_payload_size) {
    if (jpeg.empty()) {
        return false;
    }

    if (jpeg.size() > 1024 * 1024) {
        std::cerr << "Frame too large for current server limit: "
                  << jpeg.size()
                  << " bytes. Server currently rejects frames > 1MB.\n";
        return false;
    }

    uint32_t total_len = static_cast<uint32_t>(jpeg.size());
    uint32_t offset = 0;

    while (offset < total_len && g_running) {
        uint32_t remaining = total_len - offset;
        uint16_t chunk_len = static_cast<uint16_t>(
            remaining > max_payload_size ? max_payload_size : remaining
        );

        UdpJpegHeader hdr {};
        hdr.magic = MAGIC;
        hdr.frame_id = frame_id;
        hdr.total_len = total_len;
        hdr.offset = offset;
        hdr.chunk_len = chunk_len;

        std::vector<uint8_t> packet(sizeof(UdpJpegHeader) + chunk_len);

        std::memcpy(packet.data(), &hdr, sizeof(UdpJpegHeader));
        std::memcpy(packet.data() + sizeof(UdpJpegHeader),
                    jpeg.data() + offset,
                    chunk_len);

        ssize_t sent = sendto(sock,
                              packet.data(),
                              packet.size(),
                              0,
                              reinterpret_cast<const sockaddr*>(&server_addr),
                              sizeof(server_addr));

        if (sent < 0) {
            std::cerr << "sendto failed: " << strerror(errno) << "\n";
            return false;
        }

        offset += chunk_len;
    }

    return true;
}

static void camera_udp_sender_thread(const std::string& cmd,
                                     int udp_sock,
                                     const sockaddr_in& server_addr,
                                     size_t max_payload_size) {
    g_camera_pipe = popen(cmd.c_str(), "r");

    if (!g_camera_pipe) {
        std::cerr << "Failed to start camera command\n";
        g_running = false;
        return;
    }

    std::vector<uint8_t> temp(8192);
    std::vector<uint8_t> accum;
    accum.reserve(1 << 20);

    uint32_t frame_id = 1;

    while (g_running) {
        size_t n = fread(temp.data(), 1, temp.size(), g_camera_pipe);

        if (n > 0) {
            accum.insert(accum.end(), temp.begin(), temp.begin() + n);

            while (true) {
                size_t soi = find_marker(accum, 0, 0xFF, 0xD8); // JPEG start

                if (soi == std::string::npos) {
                    if (accum.size() > 1) {
                        accum.erase(accum.begin(), accum.end() - 1);
                    }
                    break;
                }

                size_t eoi = find_marker(accum, soi + 2, 0xFF, 0xD9); // JPEG end

                if (eoi == std::string::npos) {
                    if (soi > 0) {
                        accum.erase(accum.begin(), accum.begin() + soi);
                    }
                    break;
                }

                std::vector<uint8_t> frame(accum.begin() + soi,
                                           accum.begin() + eoi + 2);

                bool ok = send_jpeg_udp(udp_sock,
                                        server_addr,
                                        frame,
                                        frame_id,
                                        max_payload_size);

                if (ok) {
                    std::cout << "Sent frame "
                              << frame_id
                              << ", size "
                              << frame.size()
                              << " bytes\n";
                }

                frame_id++;
                accum.erase(accum.begin(), accum.begin() + eoi + 2);
            }
        } else {
            if (feof(g_camera_pipe)) {
                std::cerr << "Camera pipe ended\n";
                break;
            }

            if (ferror(g_camera_pipe)) {
                std::cerr << "Camera pipe error\n";
                break;
            }
        }
    }

    if (g_camera_pipe) {
        pclose(g_camera_pipe);
        g_camera_pipe = nullptr;
    }

    g_running = false;
}

static bool load_config(const std::string& filename, Config& cfg) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Failed to open config file: " << filename << "\n";
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines
        if (line.empty()) {
            continue;
        }

        // Skip comment lines
        std::string trimmed = line;
        size_t first_non_ws = trimmed.find_first_not_of(" \t\r\n");
        if (first_non_ws == std::string::npos) {
            continue;
        }
        if (trimmed[first_non_ws] == '#') {
            continue;
        }

        std::istringstream iss(line);

        // server_ip is required
        if (!(iss >> cfg.server_ip)) {
            continue;
        }

        // Optional values
        if (!(iss >> cfg.udp_port)) {
            cfg.udp_port = 5005;
        }
        if (!(iss >> cfg.width)) {
            cfg.width = 640;
        }
        if (!(iss >> cfg.height)) {
            cfg.height = 480;
        }
        if (!(iss >> cfg.fps)) {
            cfg.fps = 30;
        }

        return true; // Successfully loaded first valid config line
    }

    std::cerr << "No valid configuration found in: " << filename << "\n";
    return false;
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::string config_file = "config.txt";
    if (argc >= 2) {
        config_file = argv[1];
    }

    Config cfg;

    if (!load_config(config_file, cfg)) {
        std::cerr << "Usage:\n"
                  << "  " << argv[0] << " [config_file]\n\n"
                  << "Config format:\n"
                  << "  <server_ip> [udp_port] [width] [height] [fps]\n\n"
                  << "Example config.txt:\n"
                  << "  192.168.1.100 5005 640 480 30\n";
        return 1;
    }

    // Keep below normal Ethernet MTU to avoid IP fragmentation.
    // Header is 18 bytes, so packet size is about 1418 bytes by default.
    size_t chunk_size = 1400;

    if (cfg.udp_port <= 0 || cfg.udp_port > 65535) {
        std::cerr << "Invalid UDP port: " << cfg.udp_port << "\n";
        return 1;
    }

    if (cfg.width <= 0 || cfg.height <= 0 || cfg.fps <= 0) {
        std::cerr << "Invalid width/height/fps in config file.\n";
        return 1;
    }

    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) {
        perror("socket");
        return 1;
    }

    int send_buf_size = 1024 * 1024;
    setsockopt(udp_sock,
               SOL_SOCKET,
               SO_SNDBUF,
               &send_buf_size,
               sizeof(send_buf_size));

    sockaddr_in server_addr {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(cfg.udp_port);

    if (inet_pton(AF_INET, cfg.server_ip.c_str(), &server_addr.sin_addr) != 1) {
        std::cerr << "Invalid server IP address: " << cfg.server_ip << "\n";
        close(udp_sock);
        return 1;
    }

    std::ostringstream cmd;
    cmd << "rpicam-vid"
        << " -t 0"
        << " --nopreview"
        << " --codec mjpeg"
        << " --width " << cfg.width
        << " --height " << cfg.height
        << " --framerate " << cfg.fps
        << " -o - 2>/tmp/pi_cam_udp.log";

    std::cout << "Loaded config from: " << config_file << "\n\n";

    std::cout << "Starting camera command:\n"
              << cmd.str() << "\n\n";

    std::cout << "Sending UDP MJPEG stream to "
              << cfg.server_ip
              << ":"
              << cfg.udp_port
              << "\n";

    std::cout << "Resolution: "
              << cfg.width
              << "x"
              << cfg.height
              << " @ "
              << cfg.fps
              << " FPS\n";

    std::cout << "UDP payload chunk size: "
              << chunk_size
              << " bytes\n\n";

    camera_udp_sender_thread(cmd.str(), udp_sock, server_addr, chunk_size);

    close(udp_sock);
    return 0;
}
