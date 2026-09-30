# USB Camera to UDP JPEG Sender

A beginner-friendly guide for streaming JPEG frames from a USB camera connected to a Raspberry Pi 3B+.

The program:

1. Searches `/dev/video0` through `/dev/video63` for a usable camera.
2. Checks that the selected device is a valid V4L2 video-capture device.
3. Starts FFmpeg to read video from the camera.
4. Extracts complete JPEG images from the FFmpeg output.
5. Splits every JPEG image into small UDP packets.
6. Sends those packets to a receiver/server on the network.

> **Important:** This repository contains the **sender only**. A compatible receiver must already be listening for the custom UDP JPEG packet format described below.

---

## 1. What You Need

### Hardware

- Raspberry Pi 3B+ or another Linux computer
- USB camera or USB webcam
- Network connection between the Raspberry Pi and the receiver/server

### Software

The Raspberry Pi needs:

- `g++` to compile the C++ program
- `ffmpeg` to capture and convert camera frames
- `v4l-utils` to inspect and test V4L2 camera devices

---

## 2. Project Files

Place these files in the same directory:

```text
usb-camera-udp/
├── usb_camera_udp.cpp
├── config.txt
└── README.md
```

- `usb_camera_udp.cpp`: the C++ sender program
- `config.txt`: the camera and server settings
- `README.md`: this guide

> **Do not copy the HTML-formatted source from a web page directly into the C++ file.** HTML text such as `&lt;`, `&gt;`, `<em>`, `<br>`, or other page markup will cause compilation errors. Use a clean, plain-text copy of `usb_camera_udp.cpp`.

---

## 3. Connect the Camera

Plug the USB camera into the Raspberry Pi.

Check whether Linux can see it:

```bash
v4l2-ctl --list-devices
```

You may see output similar to:

```text
USB Camera:
    /dev/video0
    /dev/video1
```

A single physical camera can create multiple `/dev/videoX` nodes. Not every node can capture normal video. The program checks each node and selects the first usable capture device when automatic detection is enabled.

---

## 4. Install the Required Packages

Open a terminal on the Raspberry Pi and run:

```bash
sudo apt update
sudo apt install -y g++ ffmpeg v4l-utils
```

Verify the programs were installed:

```bash
g++ --version
ffmpeg -version
v4l2-ctl --version
```

If all three commands print version information, continue to the next step.

---

## 5. Create `config.txt`

Create the configuration file:

```bash
nano config.txt
```

The configuration uses one line with the following values:

```text
server_ip udp_port width height fps camera_device input_format
```

### Recommended first configuration

```text
# server_ip udp_port width height fps camera_device input_format
192.168.1.100 5005 640 480 30 auto mjpeg
```

Replace `192.168.1.100` with the IPv4 address of your receiver/server.

Save the file in `nano`:

1. Press `Ctrl+O`.
2. Press `Enter`.
3. Press `Ctrl+X`.

### Meaning of each setting

| Position | Setting | Example | Meaning |
|---:|---|---|---|
| 1 | `server_ip` | `192.168.1.100` | IPv4 address of the receiving computer |
| 2 | `udp_port` | `5005` | UDP port used by the receiver |
| 3 | `width` | `640` | Requested video width in pixels |
| 4 | `height` | `480` | Requested video height in pixels |
| 5 | `fps` | `30` | Requested frames per second |
| 6 | `camera_device` | `auto` | Automatically finds a camera, or accepts a path such as `/dev/video2` |
| 7 | `input_format` | `mjpeg` | Uses native camera MJPEG, or `auto` for conversion by FFmpeg |

The receiver IP address and UDP port **must point to the machine running the receiver**.

---

## 6. Choose the Correct Camera Mode

### Option A: Camera supports native MJPEG

Use this first because it requires less CPU power:

```text
192.168.1.100 5005 640 480 30 auto mjpeg
```

In this mode, the camera already produces JPEG frames. FFmpeg copies them without decoding and encoding them again.

### Option B: Camera does not support MJPEG

Use the more compatible mode:

```text
192.168.1.100 5005 640 480 15 auto auto
```

The first `auto` means:

```text
Automatically find a usable /dev/videoX camera device.
```

The second `auto` means:

```text
Allow FFmpeg to select an available camera input format and convert it to MJPEG.
```

This mode uses more CPU, so `15 FPS` is a safer starting point for a Raspberry Pi 3B+.

### Option C: Select a camera device manually

If the correct camera is `/dev/video2`, use:

```text
192.168.1.100 5005 640 480 30 /dev/video2 mjpeg
```

The program will validate the specified device before starting FFmpeg.

---

## 7. Check the Camera's Supported Modes

List all detected camera devices:

```bash
v4l2-ctl --list-devices
```

Inspect the supported formats, resolutions, and frame rates for a particular node:

```bash
v4l2-ctl --device=/dev/video0 --list-formats-ext
```

If the output contains `MJPG`, `640x480`, and `30 FPS`, use:

```text
192.168.1.100 5005 640 480 30 auto mjpeg
```

If it does not, start with:

```text
192.168.1.100 5005 640 480 15 auto auto
```

Do not request a resolution or frame rate that the camera does not support.

---

## 8. Compile the Program

Go to the directory containing `usb_camera_udp.cpp`:

```bash
cd /path/to/usb-camera-udp
```

For example, if the project is in your home directory:

```bash
cd ~/usb-camera-udp
```

Compile it:

```bash
g++ -std=c++17 -O2 -Wall -Wextra -pedantic usb_camera_udp.cpp -o usb_camera_udp
```

If compilation succeeds, a new executable named `usb_camera_udp` will appear.

Check it:

```bash
ls -l usb_camera_udp
```

No FFmpeg development libraries are required. The program starts the installed `ffmpeg` command through `popen()`.

---

## 9. Run the Sender

Make sure the receiver/server is running first. Then start the sender:

```bash
./usb_camera_udp
```

The program uses `config.txt` by default.

To use another configuration file:

```bash
./usb_camera_udp my_config.txt
```

A successful startup should show information similar to:

```text
Searching for a V4L2 camera from /dev/video0 through /dev/video63...
Checking /dev/video0: ... [usable video capture device]
Automatically selected camera: /dev/video0
Loaded configuration from: config.txt
Camera device: /dev/video0
Resolution: 640x480
Frame rate: 30 FPS
UDP destination: 192.168.1.100:5005
Press Ctrl+C to stop.
Sent frame 1, size 42137 bytes
Sent frame 2, size 41982 bytes
```

Stop the program safely with:

```text
Ctrl+C
```

---

## 10. How Automatic Camera Detection Works

When `camera_device` is set to `auto`, the program:

1. Checks `/dev/video0`.
2. Continues through `/dev/video63`.
3. Ignores paths that do not exist.
4. Opens each existing device node.
5. Uses `VIDIOC_QUERYCAP` to read its V4L2 capabilities.
6. Rejects nodes that cannot capture video.
7. Rejects nodes without supported capture I/O.
8. Lists all usable nodes.
9. Selects the first usable node.

This prevents the program from blindly assuming that `/dev/video0` is the correct camera.

---

## 11. What the Program Sends

Each video frame is a complete JPEG image. Because a JPEG is normally too large for one small UDP datagram, the program divides it into chunks.

Each UDP datagram contains:

```text
18-byte UdpJpegHeader
+
Up to 1400 bytes of JPEG data
```

The packed header is:

```cpp
struct __attribute__((packed)) UdpJpegHeader {
    uint32_t magic;
    uint32_t frame_id;
    uint32_t total_len;
    uint32_t offset;
    uint16_t chunk_len;
};
```

Header fields:

- `magic`: packet signature, set to `0x31474D4A`
- `frame_id`: identifies the JPEG frame
- `total_len`: total size of the complete JPEG image
- `offset`: position of this chunk inside the JPEG image
- `chunk_len`: number of JPEG bytes in this datagram

The receiver must use the same:

- Header layout
- 18-byte packed header size
- Magic value
- Integer byte-order assumptions
- JPEG reassembly logic

> **Compatibility warning:** The current sender copies the packed C++ structure directly into the UDP packet. It does not convert the integer fields with `htonl()` or `htons()`. The existing receiver must interpret the fields in the same byte order. This will normally match when both sides use compatible little-endian systems and the same protocol implementation.

The sender rejects a JPEG frame larger than 1 MiB:

```text
1,048,576 bytes
```

---

## 12. Network Checklist

If the sender runs but the receiver shows no video, check all of the following:

1. The server IP in `config.txt` is correct.
2. The receiver is listening on the same UDP port.
3. The Raspberry Pi can reach the server.
4. A firewall is not blocking the UDP port.
5. Both sender and receiver use the same packet structure and magic value.
6. The receiver can reassemble packets using `frame_id`, `offset`, and `total_len`.

Test basic connectivity from the Raspberry Pi:

```bash
ping -c 4 192.168.1.100
```

Replace the address with the receiver's actual IP address.

On a Linux receiver using port `5005`, check whether the port is listening:

```bash
ss -lun | grep 5005
```

If a firewall is enabled on the receiver, allow the configured UDP port according to that system's firewall rules.

---

## 13. Run Automatically at Boot with systemd

Once the sender works when started manually, you can install it as a `systemd` service. This makes it start automatically whenever the Raspberry Pi boots and restarts it if it crashes.

> **Test the program manually before creating the service.** If `./usb_camera_udp` does not work from a terminal, running it through `systemd` will not fix it.

### 13.1 Find your Linux username

Run:

```bash
whoami
```

Remember the result. Common usernames include `pi`, `ubuntu`, or a username chosen during Raspberry Pi OS setup.

The examples below use `ubuntu`. If your username is different, replace `ubuntu` in the service file with your actual username.

### 13.2 Create the application directory

Create a permanent location for the program:

```bash
sudo mkdir -p /opt/usb-camera-udp
```

Copy the compiled program and configuration file into it:

```bash
sudo cp usb_camera_udp /opt/usb-camera-udp/
sudo cp config.txt /opt/usb-camera-udp/
```

Make sure the executable can be run:

```bash
sudo chmod +x /opt/usb-camera-udp/usb_camera_udp
```

Give your Linux user ownership of the application files. Replace `ubuntu` if necessary:

```bash
sudo chown -R ubuntu:ubuntu /opt/usb-camera-udp
```

Confirm that both required files are present:

```bash
ls -l /opt/usb-camera-udp
```

You should see:

```text
config.txt
usb_camera_udp
```

Test the installed copy before creating the service:

```bash
cd /opt/usb-camera-udp
./usb_camera_udp
```

Press `Ctrl+C` after confirming that frames are being sent.

### 13.3 Create the service file

Create the service definition:

```bash
sudo nano /etc/systemd/system/usb-camera-udp.service
```

Paste the following content:

```ini
[Unit]
Description=USB Camera UDP JPEG Sender
Wants=network-online.target
After=network-online.target

[Service]
Type=simple
User=ubuntu
WorkingDirectory=/opt/usb-camera-udp
ExecStart=/opt/usb-camera-udp/usb_camera_udp /opt/usb-camera-udp/config.txt
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Before saving, replace this line if your username is not `ubuntu`:

```ini
User=ubuntu
```

For example, if `whoami` printed `pi`, use:

```ini
User=pi
```

Save and exit from `nano`:

1. Press `Ctrl+O`.
2. Press `Enter`.
3. Press `Ctrl+X`.

### 13.4 Reload systemd

Tell `systemd` to load the new service file:

```bash
sudo systemctl daemon-reload
```

Run this command again whenever you edit `usb-camera-udp.service`.

### 13.5 Enable automatic startup

Enable the service so it starts during future boots:

```bash
sudo systemctl enable usb-camera-udp.service
```

A successful command normally creates a symbolic link for the service.

### 13.6 Start the service now

You do not need to reboot. Start it immediately:

```bash
sudo systemctl start usb-camera-udp.service
```

### 13.7 Check the service status

Run:

```bash
sudo systemctl status usb-camera-udp.service
```

If everything is working, the output should contain:

```text
Active: active (running)
```

Press `q` to leave the status screen.

### 13.8 View live service logs

The program's normal output and errors are recorded in the system journal. Follow the logs in real time with:

```bash
sudo journalctl -u usb-camera-udp.service -f
```

Press `Ctrl+C` to stop following the logs. This stops only the log viewer, not the camera service.

View logs from the current boot:

```bash
sudo journalctl -u usb-camera-udp.service -b
```

View the most recent 100 lines:

```bash
sudo journalctl -u usb-camera-udp.service -n 100 --no-pager
```

FFmpeg errors are still written to:

```bash
cat /tmp/usb_cam_udp_ffmpeg.log
```

### 13.9 Stop, start, or restart the service

Stop it:

```bash
sudo systemctl stop usb-camera-udp.service
```

Start it:

```bash
sudo systemctl start usb-camera-udp.service
```

Restart it after changing `config.txt` or replacing the executable:

```bash
sudo systemctl restart usb-camera-udp.service
```

Check whether it is running:

```bash
sudo systemctl is-active usb-camera-udp.service
```

Check whether automatic startup is enabled:

```bash
sudo systemctl is-enabled usb-camera-udp.service
```

### 13.10 Disable automatic startup

To stop the service and prevent it from starting at boot:

```bash
sudo systemctl disable --now usb-camera-udp.service
```

This does not delete the executable, configuration, or service file.

### 13.11 Edit the service later

Open the service file:

```bash
sudo nano /etc/systemd/system/usb-camera-udp.service
```

After saving any changes, reload and restart it:

```bash
sudo systemctl daemon-reload
sudo systemctl restart usb-camera-udp.service
sudo systemctl status usb-camera-udp.service
```

### 13.12 Common systemd problems

#### `status=217/USER`

The username in the service file does not exist. Check your username:

```bash
whoami
```

Then correct the `User=` line.

#### `status=203/EXEC`

The executable path is wrong, the file is missing, or it is not executable. Check it:

```bash
ls -l /opt/usb-camera-udp/usb_camera_udp
```

Restore executable permission if necessary:

```bash
sudo chmod +x /opt/usb-camera-udp/usb_camera_udp
```

#### The service cannot open `config.txt`

The provided service uses an absolute configuration path:

```ini
ExecStart=/opt/usb-camera-udp/usb_camera_udp /opt/usb-camera-udp/config.txt
```

Confirm the file exists and is readable:

```bash
ls -l /opt/usb-camera-udp/config.txt
```

#### The service cannot access the camera

Check whether the configured service user belongs to the `video` group. Replace `ubuntu` if necessary:

```bash
id ubuntu
```

Add the user to the group if `video` is not listed:

```bash
sudo usermod -aG video ubuntu
```

Then reboot, or restart the relevant user session and service:

```bash
sudo reboot
```

#### The service keeps restarting

Inspect both logs:

```bash
sudo journalctl -u usb-camera-udp.service -n 100 --no-pager
cat /tmp/usb_cam_udp_ffmpeg.log
```

Also test the exact service command manually as the configured user:

```bash
cd /opt/usb-camera-udp
./usb_camera_udp /opt/usb-camera-udp/config.txt
```

---

## 14. Troubleshooting

### Problem: `config.txt` cannot be opened

Example:

```text
Failed to open configuration file: config.txt
```

Make sure the file exists in the current directory:

```bash
ls -l config.txt
```

Alternatively, provide its full path:

```bash
./usb_camera_udp /home/pi/usb-camera-udp/config.txt
```

### Problem: No usable camera was detected

Check that the camera is connected:

```bash
ls -l /dev/video*
v4l2-ctl --list-devices
```

Check the current user's groups:

```bash
groups
```

If necessary, add the user to the `video` group:

```bash
sudo usermod -aG video "$USER"
```

Log out and log back in after changing group membership.

Also check whether another program is already using the camera:

```bash
fuser /dev/video0
```

### Problem: FFmpeg ends unexpectedly

Read the FFmpeg log:

```bash
cat /tmp/usb_cam_udp_ffmpeg.log
```

Common causes include:

- Unsupported resolution
- Unsupported frame rate
- Camera does not provide MJPEG
- Wrong `/dev/videoX` node
- Camera is already being used
- Permission denied

If `mjpeg` fails, change the final value in `config.txt` to `auto` and reduce the frame rate:

```text
192.168.1.100 5005 640 480 15 auto auto
```

### Problem: `Permission denied` for `/dev/videoX`

Check permissions:

```bash
ls -l /dev/video0
```

Add the user to the `video` group if needed:

```bash
sudo usermod -aG video "$USER"
```

Then log out and log back in.

### Problem: Requested MJPEG mode is unsupported

Inspect the camera modes:

```bash
v4l2-ctl --device=/dev/video0 --list-formats-ext
```

Use `auto auto` in the final two configuration fields if `MJPG` is unavailable:

```text
192.168.1.100 5005 640 480 15 auto auto
```

### Problem: Frames are larger than 1 MiB

Reduce one or more of the following:

- Resolution
- JPEG quality
- Scene complexity

Start with a lower resolution:

```text
192.168.1.100 5005 320 240 15 auto auto
```

### Problem: Video is corrupted or incomplete

UDP does not guarantee delivery, packet order, or retransmission. The receiver should:

- Group chunks by `frame_id`
- Place each chunk at its stated `offset`
- Verify all bytes up to `total_len` were received
- Discard incomplete frames after a short timeout
- Avoid displaying a frame until all chunks have arrived

Packet loss can be reduced by:

- Using Ethernet instead of Wi-Fi
- Lowering resolution
- Lowering frame rate
- Keeping the sender and receiver on the same local network

### Problem: Compilation produces many strange syntax errors

If the source contains text such as the following, it was copied with HTML formatting:

```text
&lt;
&gt;
<em>
<br>
```

Replace it with the clean, plain-text `usb_camera_udp.cpp` file. Do not compile the HTML-formatted text from a browser or chat transcript.

---

## 15. Quick Start

If the files already exist, this is the shortest setup procedure:

```bash
sudo apt update
sudo apt install -y g++ ffmpeg v4l-utils

v4l2-ctl --list-devices

g++ -std=c++17 -O2 -Wall -Wextra -pedantic usb_camera_udp.cpp -o usb_camera_udp

./usb_camera_udp
```

Example `config.txt`:

```text
192.168.1.100 5005 640 480 30 auto mjpeg
```

If that camera mode fails, try:

```text
192.168.1.100 5005 640 480 15 auto auto
```

Check FFmpeg errors with:

```bash
cat /tmp/usb_cam_udp_ffmpeg.log
```

---

## 16. Final Checklist

Before expecting video at the receiver, confirm:

- [ ] The USB camera is connected.
- [ ] `v4l2-ctl --list-devices` shows at least one camera.
- [ ] `g++`, `ffmpeg`, and `v4l-utils` are installed.
- [ ] `usb_camera_udp.cpp` is clean plain-text C++ source.
- [ ] `config.txt` exists.
- [ ] The server IP is correct.
- [ ] The UDP port matches the receiver.
- [ ] The requested resolution and FPS are supported.
- [ ] The receiver is running before the sender starts.
- [ ] The receiver understands the custom 18-byte UDP header.
- [ ] The firewall allows the selected UDP port.
- [ ] FFmpeg's log has been checked if capture fails.
- [ ] The installed program works manually from `/opt/usb-camera-udp`.
- [ ] The `User=` value in the service matches a real Linux username.
- [ ] `usb-camera-udp.service` is enabled and active.

---

## License

Add the appropriate license for your project here.
