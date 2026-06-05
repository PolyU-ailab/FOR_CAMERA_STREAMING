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
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static std::atomic<bool> g_running{true};
static std::mutex g_frame_mutex;
static std::condition_variable g_frame_cv;
static std::vector<uint8_t> g_latest_frame;
static uint64_t g_frame_id = 0;
static FILE* g_camera_pipe = nullptr;

static void signal_handler(int) {
    g_running = false;
    g_frame_cv.notify_all();
}

static ssize_t send_all(int fd, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return -1;
        sent += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(sent);
}

static size_t find_marker(const std::vector<uint8_t>& buf, size_t start, uint8_t a, uint8_t b) {
    if (buf.size() < 2 || start >= buf.size() - 1) return std::string::npos;
    for (size_t i = start; i + 1 < buf.size(); ++i) {
        if (buf[i] == a && buf[i + 1] == b) return i;
    }
    return std::string::npos;
}

static void camera_reader_thread(const std::string& cmd) {
    g_camera_pipe = popen(cmd.c_str(), "r");
    if (!g_camera_pipe) {
        std::cerr << "Failed to start camera command\n";
        g_running = false;
        g_frame_cv.notify_all();
        return;
    }

    std::vector<uint8_t> temp(8192);
    std::vector<uint8_t> accum;
    accum.reserve(1 << 20);

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

                std::vector<uint8_t> frame(accum.begin() + soi, accum.begin() + eoi + 2);
                {
                    std::lock_guard<std::mutex> lock(g_frame_mutex);
                    g_latest_frame = std::move(frame);
                    ++g_frame_id;
                }
                g_frame_cv.notify_all();

                accum.erase(accum.begin(), accum.begin() + eoi + 2);
            }
        } else {
            break;
        }
    }

    if (g_camera_pipe) {
        pclose(g_camera_pipe);
        g_camera_pipe = nullptr;
    }

    g_running = false;
    g_frame_cv.notify_all();
}

static void send_404(int fd) {
    const char* body = "404 Not Found\n";
    std::ostringstream oss;
    oss << "HTTP/1.1 404 Not Found\r\n"
        << "Content-Type: text/plain\r\n"
        << "Content-Length: " << strlen(body) << "\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    send_all(fd, oss.str().data(), oss.str().size());
}

static std::string parse_path(const std::string& req) {
    std::istringstream iss(req);
    std::string method, path, version;
    iss >> method >> path >> version;
    return path.empty() ? "/" : path;
}

static void stream_client(int fd) {
    const char* header =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"
        "Pragma: no-cache\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";

    if (send_all(fd, header, strlen(header)) < 0) return;

    uint64_t last_seen = 0;

    while (g_running) {
        std::vector<uint8_t> frame;
        uint64_t current_id = 0;

        {
            std::unique_lock<std::mutex> lock(g_frame_mutex);
            g_frame_cv.wait(lock, [&] {
                return g_frame_id != last_seen || !g_running.load();
            });

            if (!g_running) break;
            frame = g_latest_frame;
            current_id = g_frame_id;
        }

        if (frame.empty()) continue;

        std::ostringstream part;
        part << "--frame\r\n"
             << "Content-Type: image/jpeg\r\n"
             << "Content-Length: " << frame.size() << "\r\n\r\n";

        if (send_all(fd, part.str().data(), part.str().size()) < 0) break;
        if (send_all(fd, frame.data(), frame.size()) < 0) break;
        if (send_all(fd, "\r\n", 2) < 0) break;

        last_seen = current_id;
    }
}

static void handle_client(int client_fd) {
    char buf[2048];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        close(client_fd);
        return;
    }

    buf[n] = '\0';
    std::string req(buf);
    std::string path = parse_path(req);

    // Stream directly on "/" (and also allow "/stream")
    if (path == "/" || path == "/stream" || path == "/stream.mjpg") {
        stream_client(client_fd);
    } else {
        send_404(client_fd);
    }

    close(client_fd);
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    int port = 8080;
    int width = 640;
    int height = 480;
    int fps = 30;

    if (argc >= 2) port = std::atoi(argv[1]);
    if (argc >= 4) {
        width = std::atoi(argv[2]);
        height = std::atoi(argv[3]);
    }
    if (argc >= 5) fps = std::atoi(argv[4]);

    // rpicam-vid on Bookworm + MJPEG output
    std::ostringstream cmd;
    cmd << "rpicam-vid"
        << " -t 0"
        << " --nopreview"
        << " --codec mjpeg"
        << " --width " << width
        << " --height " << height
        << " --framerate " << fps
        << " -o - 2>/tmp/pi_cam_http.log";

    std::thread cam_thread(camera_reader_thread, cmd.str());

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        g_running = false;
        cam_thread.join();
        return 1;
    }

    int yes = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        g_running = false;
        cam_thread.join();
        return 1;
    }

    if (listen(server_fd, 8) < 0) {
        perror("listen");
        close(server_fd);
        g_running = false;
        cam_thread.join();
        return 1;
    }

    std::cerr << "Listening on port " << port << "\n";
    std::cerr << "Open: http://<PI-IP>:" << port << "/\n";

    while (g_running) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        std::thread(handle_client, client_fd).detach();
    }

    close(server_fd);
    g_running = false;
    g_frame_cv.notify_all();

    if (cam_thread.joinable()) cam_thread.join();
    return 0;
}

