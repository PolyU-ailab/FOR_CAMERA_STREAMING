#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <linux/videodev2.h>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#include <atomic>
#include <cstdint>

/*
 * ============================================================================
 * Global state
 * ============================================================================
 */

static std::atomic<bool> g_running{true};
static FILE* g_camera_pipe = nullptr;

/*
 * This value must match the receiver/server.
 *
 * On a little-endian system, the value is stored in memory as:
 * 4A 4D 47 31
 *
 * This corresponds to "JMG1".
 */
static constexpr uint32_t MAGIC = 0x31474D4A;

/*
 * The original server rejects frames larger than 1 MB.
 */
static constexpr uint32_t MAX_JPEG_SIZE = 1024U * 1024U;

/*
 * Search /dev/video0 through /dev/video63.
 */
static constexpr int MAX_VIDEO_DEVICE_NUMBER = 63;

/*
 * Keep UDP packets below the usual Ethernet MTU.
 *
 * UDP JPEG header: 18 bytes
 * JPEG payload:    1400 bytes
 * UDP payload:     1418 bytes
 */
static constexpr size_t UDP_CHUNK_SIZE = 1400;

/*
 * ============================================================================
 * UDP packet header
 * ============================================================================
 *
 * This structure must match the server structure exactly.
 */

struct __attribute__((packed)) UdpJpegHeader {
    uint32_t magic;
    uint32_t frame_id;
    uint32_t total_len;
    uint32_t offset;
    uint16_t chunk_len;
};

static_assert(
    sizeof(UdpJpegHeader) == 18,
    "UdpJpegHeader must be exactly 18 bytes"
);

/*
 * ============================================================================
 * Configuration
 * ============================================================================
 *
 * Configuration format:
 *
 * server_ip udp_port width height fps camera_device input_format
 *
 * Example:
 *
 * 192.168.1.100 5005 640 480 30 auto mjpeg
 *
 * camera_device:
 *   auto          Automatically scan /dev/video0 through /dev/video63
 *   /dev/video0   Use a specific video device
 *
 * input_format:
 *   mjpeg         Camera provides native MJPEG
 *   auto          Let FFmpeg select the input format and encode to MJPEG
 */

struct Config {
    std::string server_ip;

    int udp_port = 5005;
    int width = 640;
    int height = 480;
    int fps = 30;

    std::string camera_device = "auto";
    std::string input_format = "mjpeg";
};

/*
 * ============================================================================
 * Signal handling
 * ============================================================================
 */

static void signal_handler(int signal_number) {
    (void)signal_number;

    /*
     * Do not call pclose() from the signal handler.
     * pclose() is not async-signal-safe.
     */
    g_running.store(false);
}

/*
 * ============================================================================
 * String utilities
 * ============================================================================
 */

static std::string shell_quote(const std::string& value) {
    std::string quoted = "'";

    for (char character : value) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted += character;
        }
    }

    quoted += "'";
    return quoted;
}

/*
 * ============================================================================
 * V4L2 camera detection
 * ============================================================================
 */

static bool path_exists(const std::string& path) {
    return access(path.c_str(), F_OK) == 0;
}

static bool is_v4l2_capture_device(
    const std::string& device_path,
    bool print_information = true
) {
    struct stat device_stat {};

    if (stat(device_path.c_str(), &device_stat) < 0) {
        return false;
    }

    if (!S_ISCHR(device_stat.st_mode)) {
        if (print_information) {
            std::cout
                << "Skipping " << device_path
                << ": not a character device\n";
        }

        return false;
    }

    /*
     * Open read-only first. Some devices do not allow O_RDWR when another
     * application has configured an output interface.
     */
    int camera_fd = open(
        device_path.c_str(),
        O_RDONLY | O_NONBLOCK
    );

    if (camera_fd < 0) {
        if (print_information) {
            std::cerr
                << "Found " << device_path
                << ", but it could not be opened: "
                << std::strerror(errno)
                << "\n";
        }

        return false;
    }

    v4l2_capability capability {};

    if (ioctl(camera_fd, VIDIOC_QUERYCAP, &capability) < 0) {
        if (print_information) {
            std::cerr
                << "Found " << device_path
                << ", but VIDIOC_QUERYCAP failed: "
                << std::strerror(errno)
                << "\n";
        }

        close(camera_fd);
        return false;
    }

    uint32_t capabilities = capability.capabilities;

    /*
     * When V4L2_CAP_DEVICE_CAPS is present, device_caps describes the
     * capabilities of this specific /dev/videoX node.
     */
    if ((capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U) {
        capabilities = capability.device_caps;
    }

    const bool supports_single_plane_capture =
        (capabilities & V4L2_CAP_VIDEO_CAPTURE) != 0U;

    const bool supports_multi_plane_capture =
        (capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0U;

    const bool supports_video_capture =
        supports_single_plane_capture ||
        supports_multi_plane_capture;

    const bool supports_streaming =
        (capabilities & V4L2_CAP_STREAMING) != 0U;

    const bool supports_read_write =
        (capabilities & V4L2_CAP_READWRITE) != 0U;

    const bool supports_capture_io =
        supports_streaming ||
        supports_read_write;

    if (print_information) {
        std::cout
            << "Checking " << device_path
            << ": card=\"" << capability.card
            << "\", driver=\"" << capability.driver
            << "\", bus=\"" << capability.bus_info
            << "\"";
    }

    if (!supports_video_capture) {
        if (print_information) {
            std::cout << " [not a video capture node]\n";
        }

        close(camera_fd);
        return false;
    }

    if (!supports_capture_io) {
        if (print_information) {
            std::cout << " [no supported capture I/O]\n";
        }

        close(camera_fd);
        return false;
    }

    if (print_information) {
        std::cout << " [usable video capture device]\n";
    }

    close(camera_fd);
    return true;
}

static std::string find_usb_camera(
    int maximum_device_number = MAX_VIDEO_DEVICE_NUMBER
) {
    std::cout
        << "Searching for a V4L2 camera from /dev/video0"
        << " through /dev/video"
        << maximum_device_number
        << "...\n\n";

    std::vector<std::string> usable_devices;

    for (
        int device_number = 0;
        device_number <= maximum_device_number;
        ++device_number
    ) {
        const std::string device_path =
            "/dev/video" + std::to_string(device_number);

        /*
         * Do not print errors for device paths that do not exist.
         */
        if (!path_exists(device_path)) {
            continue;
        }

        if (is_v4l2_capture_device(device_path, true)) {
            usable_devices.push_back(device_path);
        }
    }

    if (usable_devices.empty()) {
        std::cerr
            << "\nNo usable V4L2 capture device was found between "
            << "/dev/video0 and /dev/video"
            << maximum_device_number
            << ".\n";

        return "";
    }

    std::cout << "\nUsable camera nodes:\n";

    for (size_t index = 0; index < usable_devices.size(); ++index) {
        std::cout
            << "  [" << index << "] "
            << usable_devices[index]
            << "\n";
    }

    /*
     * Select the first usable capture node.
     */
    std::cout
        << "\nAutomatically selected camera: "
        << usable_devices.front()
        << "\n\n";

    return usable_devices.front();
}

/*
 * ============================================================================
 * JPEG marker search
 * ============================================================================
 */

static size_t find_marker(
    const std::vector<uint8_t>& buffer,
    size_t start,
    uint8_t first_byte,
    uint8_t second_byte
) {
    if (buffer.size() < 2 || start >= buffer.size() - 1) {
        return std::string::npos;
    }

    for (size_t index = start; index + 1 < buffer.size(); ++index) {
        if (
            buffer[index] == first_byte &&
            buffer[index + 1] == second_byte
        ) {
            return index;
        }
    }

    return std::string::npos;
}

/*
 * ============================================================================
 * UDP JPEG sender
 * ============================================================================
 */

static bool send_jpeg_udp(
    int socket_fd,
    const sockaddr_in& server_address,
    const std::vector<uint8_t>& jpeg,
    uint32_t frame_id,
    size_t maximum_payload_size
) {
    if (jpeg.empty()) {
        std::cerr
            << "Frame " << frame_id
            << " is empty\n";

        return false;
    }

    if (jpeg.size() > MAX_JPEG_SIZE) {
        std::cerr
            << "Frame " << frame_id
            << " is too large: "
            << jpeg.size()
            << " bytes. Maximum supported size is "
            << MAX_JPEG_SIZE
            << " bytes.\n";

        return false;
    }

    if (
        maximum_payload_size == 0 ||
        maximum_payload_size > UINT16_MAX
    ) {
        std::cerr
            << "Invalid UDP payload size: "
            << maximum_payload_size
            << "\n";

        return false;
    }

    const uint32_t total_length =
        static_cast<uint32_t>(jpeg.size());

    uint32_t offset = 0;

    while (offset < total_length && g_running.load()) {
        const uint32_t remaining = total_length - offset;

        const uint16_t chunk_length =
            static_cast<uint16_t>(
                remaining > maximum_payload_size
                    ? maximum_payload_size
                    : remaining
            );

        UdpJpegHeader header {};

        header.magic = MAGIC;
        header.frame_id = frame_id;
        header.total_len = total_length;
        header.offset = offset;
        header.chunk_len = chunk_length;

        std::vector<uint8_t> packet(
            sizeof(UdpJpegHeader) + chunk_length
        );

        std::memcpy(
            packet.data(),
            &header,
            sizeof(UdpJpegHeader)
        );

        std::memcpy(
            packet.data() + sizeof(UdpJpegHeader),
            jpeg.data() + offset,
            chunk_length
        );

        const ssize_t bytes_sent = sendto(
            socket_fd,
            packet.data(),
            packet.size(),
            0,
            reinterpret_cast<const sockaddr*>(&server_address),
            sizeof(server_address)
        );

        if (bytes_sent < 0) {
            if (errno == EINTR && !g_running.load()) {
                return false;
            }

            std::cerr
                << "sendto() failed for frame "
                << frame_id
                << " at offset "
                << offset
                << ": "
                << std::strerror(errno)
                << "\n";

            return false;
        }

        if (static_cast<size_t>(bytes_sent) != packet.size()) {
            std::cerr
                << "Incomplete UDP datagram. Expected "
                << packet.size()
                << " bytes, but sendto() returned "
                << bytes_sent
                << " bytes.\n";

            return false;
        }

        offset += chunk_length;
    }

    return offset == total_length;
}

/*
 * ============================================================================
 * FFmpeg command generation
 * ============================================================================
 */

static std::string create_ffmpeg_command(const Config& config) {
    std::ostringstream command;

    command
        << "ffmpeg"
        << " -hide_banner"
        << " -loglevel warning"
        << " -f v4l2";

    /*
     * If input_format is mjpeg, request native MJPEG from the camera.
     *
     * If input_format is auto, do not specify an input format. FFmpeg
     * will select an available camera format and encode it to MJPEG.
     */
    if (config.input_format != "auto") {
        command
            << " -input_format "
            << shell_quote(config.input_format);
    }

    command
        << " -video_size "
        << config.width
        << "x"
        << config.height

        << " -framerate "
        << config.fps

        << " -i "
        << shell_quote(config.camera_device)

        << " -an";

    if (config.input_format == "mjpeg") {
        /*
         * Camera already provides JPEG images. Do not decode and re-encode.
         */
        command << " -c:v copy";
    } else {
        /*
         * Camera provides another format such as YUYV.
         * Encode frames as MJPEG.
         */
        command
            << " -c:v mjpeg"
            << " -q:v 5";
    }

    /*
     * Write consecutive JPEG images to standard output.
     */
    command
        << " -f mjpeg"
        << " pipe:1"
        << " 2>/tmp/usb_cam_udp_ffmpeg.log";

    return command.str();
}

/*
 * ============================================================================
 * Camera reader
 * ============================================================================
 */

static void camera_udp_sender(
    const std::string& command,
    int udp_socket,
    const sockaddr_in& server_address,
    size_t maximum_payload_size
) {
    g_camera_pipe = popen(command.c_str(), "r");

    if (g_camera_pipe == nullptr) {
        std::cerr
            << "Failed to start FFmpeg: "
            << std::strerror(errno)
            << "\n";

        g_running.store(false);
        return;
    }

    std::vector<uint8_t> temporary_buffer(8192);
    std::vector<uint8_t> accumulated_data;

    accumulated_data.reserve(MAX_JPEG_SIZE);

    uint32_t frame_id = 1;

    while (g_running.load()) {
        const size_t bytes_read = fread(
            temporary_buffer.data(),
            1,
            temporary_buffer.size(),
            g_camera_pipe
        );

        if (bytes_read > 0) {
            accumulated_data.insert(
                accumulated_data.end(),
                temporary_buffer.begin(),
                temporary_buffer.begin() + bytes_read
            );

            /*
             * Protect against unlimited memory use if invalid data is
             * received or JPEG end markers are missing.
             */
            if (accumulated_data.size() > MAX_JPEG_SIZE * 2U) {
                std::cerr
                    << "Camera buffer exceeded "
                    << MAX_JPEG_SIZE * 2U
                    << " bytes. Discarding buffered data.\n";

                accumulated_data.clear();
                continue;
            }

            while (g_running.load()) {
                /*
                 * JPEG Start Of Image marker: FF D8
                 */
                const size_t start_of_image = find_marker(
                    accumulated_data,
                    0,
                    0xFF,
                    0xD8
                );

                if (start_of_image == std::string::npos) {
                    /*
                     * Keep the last byte in case it is FF and the next
                     * fread() starts with D8.
                     */
                    if (accumulated_data.size() > 1) {
                        accumulated_data.erase(
                            accumulated_data.begin(),
                            accumulated_data.end() - 1
                        );
                    }

                    break;
                }

                /*
                 * JPEG End Of Image marker: FF D9
                 */
                const size_t end_of_image = find_marker(
                    accumulated_data,
                    start_of_image + 2,
                    0xFF,
                    0xD9
                );

                if (end_of_image == std::string::npos) {
                    /*
                     * Remove any invalid bytes before the JPEG.
                     */
                    if (start_of_image > 0) {
                        accumulated_data.erase(
                            accumulated_data.begin(),
                            accumulated_data.begin() + start_of_image
                        );
                    }

                    break;
                }

                const size_t frame_end = end_of_image + 2;

                std::vector<uint8_t> jpeg_frame(
                    accumulated_data.begin() + start_of_image,
                    accumulated_data.begin() + frame_end
                );

                const bool success = send_jpeg_udp(
                    udp_socket,
                    server_address,
                    jpeg_frame,
                    frame_id,
                    maximum_payload_size
                );

                if (success) {
                    std::cout
                        << "Sent frame "
                        << frame_id
                        << ", size "
                        << jpeg_frame.size()
                        << " bytes\n";
                } else if (g_running.load()) {
                    std::cerr
                        << "Failed to send frame "
                        << frame_id
                        << "\n";
                }

                ++frame_id;

                accumulated_data.erase(
                    accumulated_data.begin(),
                    accumulated_data.begin() + frame_end
                );
            }
        } else {
            if (feof(g_camera_pipe)) {
                std::cerr
                    << "FFmpeg ended unexpectedly.\n"
                    << "Check the FFmpeg log:\n"
                    << "  cat /tmp/usb_cam_udp_ffmpeg.log\n";

                break;
            }

            if (ferror(g_camera_pipe)) {
                if (!g_running.load()) {
                    break;
                }

                std::cerr
                    << "Error while reading from the FFmpeg pipe\n";

                break;
            }
        }
    }

    FILE* pipe_to_close = g_camera_pipe;
    g_camera_pipe = nullptr;

    if (pipe_to_close != nullptr) {
        const int close_status = pclose(pipe_to_close);

        if (close_status == -1) {
            std::cerr
                << "pclose() failed: "
                << std::strerror(errno)
                << "\n";
        }
    }

    g_running.store(false);
}

/*
 * ============================================================================
 * Configuration loader
 * ============================================================================
 */

static bool load_config(
    const std::string& filename,
    Config& config
) {
    std::ifstream file(filename);

    if (!file.is_open()) {
        std::cerr
            << "Failed to open configuration file: "
            << filename
            << "\n";

        return false;
    }

    std::string line;

    while (std::getline(file, line)) {
        const size_t first_non_whitespace =
            line.find_first_not_of(" \t\r\n");

        /*
         * Skip empty lines.
         */
        if (first_non_whitespace == std::string::npos) {
            continue;
        }

        /*
         * Skip comment lines.
         */
        if (line[first_non_whitespace] == '#') {
            continue;
        }

        std::istringstream input(line);

        /*
         * server_ip is required.
         */
        if (!(input >> config.server_ip)) {
            continue;
        }

        /*
         * All remaining fields are optional.
         */
        if (!(input >> config.udp_port)) {
            config.udp_port = 5005;
            return true;
        }

        if (!(input >> config.width)) {
            config.width = 640;
            return true;
        }

        if (!(input >> config.height)) {
            config.height = 480;
            return true;
        }

        if (!(input >> config.fps)) {
            config.fps = 30;
            return true;
        }

        if (!(input >> config.camera_device)) {
            config.camera_device = "auto";
            return true;
        }

        if (!(input >> config.input_format)) {
            config.input_format = "mjpeg";
        }

        return true;
    }

    std::cerr
        << "No valid configuration line was found in: "
        << filename
        << "\n";

    return false;
}

/*
 * ============================================================================
 * Main program
 * ============================================================================
 */

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::string config_filename = "config.txt";

    if (argc >= 2) {
        config_filename = argv[1];
    }

    Config config;

    if (!load_config(config_filename, config)) {
        std::cerr
            << "\nUsage:\n"
            << "  " << argv[0] << " [config_file]\n\n"

            << "Configuration format:\n"
            << "  <server_ip> [udp_port] [width] [height] "
            << "[fps] [camera_device] [input_format]\n\n"

            << "Automatic camera detection:\n"
            << "  192.168.1.100 5005 640 480 30 auto mjpeg\n\n"

            << "Specific camera device:\n"
            << "  192.168.1.100 5005 640 480 30 "
            << "/dev/video2 mjpeg\n\n"

            << "Camera without native MJPEG:\n"
            << "  192.168.1.100 5005 640 480 15 auto auto\n";

        return EXIT_FAILURE;
    }

    if (config.server_ip.empty()) {
        std::cerr << "Server IP address cannot be empty.\n";
        return EXIT_FAILURE;
    }

    if (config.udp_port <= 0 || config.udp_port > 65535) {
        std::cerr
            << "Invalid UDP port: "
            << config.udp_port
            << "\n";

        return EXIT_FAILURE;
    }

    if (
        config.width <= 0 ||
        config.height <= 0 ||
        config.fps <= 0
    ) {
        std::cerr
            << "Width, height, and FPS must be positive values.\n";

        return EXIT_FAILURE;
    }

    if (config.camera_device.empty()) {
        config.camera_device = "auto";
    }

    if (
        config.input_format != "mjpeg" &&
        config.input_format != "auto"
    ) {
        std::cerr
            << "Unsupported input format: "
            << config.input_format
            << "\n"
            << "Use either \"mjpeg\" or \"auto\".\n";

        return EXIT_FAILURE;
    }

    /*
     * Automatically search all /dev/videoX paths.
     */
    if (config.camera_device == "auto") {
        config.camera_device = find_usb_camera(
            MAX_VIDEO_DEVICE_NUMBER
        );

        if (config.camera_device.empty()) {
            std::cerr
                << "\nUnable to continue because no usable "
                << "camera was detected.\n";

            return EXIT_FAILURE;
        }
    } else {
        std::cout
            << "Using camera specified in configuration: "
            << config.camera_device
            << "\n";

        if (!is_v4l2_capture_device(config.camera_device, true)) {
            std::cerr
                << "The configured camera is not a usable "
                << "V4L2 capture device: "
                << config.camera_device
                << "\n";

            return EXIT_FAILURE;
        }

        std::cout << "\n";
    }

    /*
     * Create UDP socket.
     */
    const int udp_socket = socket(
        AF_INET,
        SOCK_DGRAM,
        0
    );

    if (udp_socket < 0) {
        std::cerr
            << "socket() failed: "
            << std::strerror(errno)
            << "\n";

        return EXIT_FAILURE;
    }

    /*
     * Increase the UDP send buffer.
     */
    int send_buffer_size = 1024 * 1024;

    if (setsockopt(
            udp_socket,
            SOL_SOCKET,
            SO_SNDBUF,
            &send_buffer_size,
            sizeof(send_buffer_size)
        ) < 0) {
        std::cerr
            << "Warning: failed to increase UDP send buffer: "
            << std::strerror(errno)
            << "\n";
    }

    sockaddr_in server_address {};

    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(
        static_cast<uint16_t>(config.udp_port)
    );

    if (inet_pton(
            AF_INET,
            config.server_ip.c_str(),
            &server_address.sin_addr
        ) != 1) {
        std::cerr
            << "Invalid IPv4 server address: "
            << config.server_ip
            << "\n";

        close(udp_socket);
        return EXIT_FAILURE;
    }

    const std::string ffmpeg_command =
        create_ffmpeg_command(config);

    std::cout
        << "Loaded configuration from: "
        << config_filename
        << "\n\n"

        << "Camera device: "
        << config.camera_device
        << "\n"

        << "Camera input format: "
        << config.input_format
        << "\n"

        << "Resolution: "
        << config.width
        << "x"
        << config.height
        << "\n"

        << "Frame rate: "
        << config.fps
        << " FPS\n"

        << "UDP destination: "
        << config.server_ip
        << ":"
        << config.udp_port
        << "\n"

        << "UDP JPEG chunk size: "
        << UDP_CHUNK_SIZE
        << " bytes\n\n"

        << "Starting FFmpeg command:\n"
        << ffmpeg_command
        << "\n\n"

        << "Press Ctrl+C to stop.\n\n";

    camera_udp_sender(
        ffmpeg_command,
        udp_socket,
        server_address,
        UDP_CHUNK_SIZE
    );

    close(udp_socket);

    std::cout << "USB camera UDP sender stopped.\n";

    return EXIT_SUCCESS;
}
