## Compile on Raspberry Pi



```bash
sudo apt install rpicam-apps

g++ -O3 -std=c++17 -static pi_udp_camera.cpp -o pi_udp_camera -pthread
```

***

## Run examples

### Send as camera 1

Your server maps:

```text
UDP 5005 -> cam1 
```

Run on the Pi:

```bash
./pi_udp_camera SERVER_IP 5005 640 480 30
```

Then open on the server side:

```text
http://SERVER_IP:8080/cam1
```

***

### Send as camera 2

```bash
./pi_udp_camera 192.168.1.100 5006 640 480 30
```

Open:

```text
http://SERVER_IP:8080/cam2
```

***

### Send as camera 3

```bash
./pi_udp_camera 192.168.1.100 5007 640 480 30
```

Open:

```text
http://SERVER_IP:8080/cam3
```

***

## Important notes

Your server currently rejects JPEG frames larger than:

```cpp
1024 * 1024
```

So if you use high resolution, for example `1920x1080`, the server may discard the frame.

For stable UDP streaming, start with:

```bash
./pi_udp_camera SERVER_IP 5005 640 480 15
```


The **best way** to make a C++ program run automatically on startup in **Ubuntu Server** is to create a **systemd service**.

This is reliable, standard, and works well for servers.

***

# Option 1 (Recommended): Create a `systemd` service

## 1) Compile your C++ program

For example:

```bash
g++ main.cpp -o myapp
```

Test it first:

```bash
./myapp
```

If it works, move it to a permanent location, for example:

```bash
sudo mkdir -p /opt/myapp
sudo cp myapp /opt/myapp/
```

Make sure it is executable:

```bash
sudo chmod +x /opt/myapp/myapp
```

***

## 2) Create a service file

Create this file:

```bash
sudo nano /etc/systemd/system/myapp.service
```

Paste this:

```ini
[Unit]
Description=My C++ App
After=network.target

[Service]
Type=simple
ExecStart=/opt/myapp/myapp
WorkingDirectory=/opt/myapp
Restart=always
RestartSec=5
User=ubuntu

[Install]
WantedBy=multi-user.target
```

***

## 3) Reload systemd

After creating the service:

```bash
sudo systemctl daemon-reload
```

***

## 4) Enable it at boot

```bash
sudo systemctl enable myapp.service
```

This makes it start automatically whenever the server boots.

***

## 5) Start it now (without rebooting)

```bash
sudo systemctl start myapp.service
```

***

## 6) Check status

```bash
sudo systemctl status myapp.service
```

If everything is okay, you should see it as **active (running)**.

***

# Useful service settings explained

Here is what the important lines mean:

* **`ExecStart=/opt/myapp/myapp`**  
  Full path to your compiled executable.

* **`WorkingDirectory=/opt/myapp`**  
  Important if your program reads or writes files using relative paths.

* **`Restart=always`**  
  If your program crashes, systemd will restart it.

* **`User=ubuntu`**  
  Runs the program as the `ubuntu` user instead of root.  
  Replace `ubuntu` with the correct user on your server.

* **`After=network.target`**  
  Starts after basic networking is up.  
  If your app requires full network connectivity, use:

```ini
After=network-online.target
Wants=network-online.target
```

***

# Example for a program that needs network

```ini
[Unit]
Description=My C++ Network App
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/opt/myapp/myapp
WorkingDirectory=/opt/myapp
Restart=always
RestartSec=5
User=pi

[Install]
WantedBy=multi-user.target
```

***

# How to view logs

If your app doesn’t start correctly, check logs with:

```bash
journalctl -u myapp.service -b
```

To follow logs live:

```bash
journalctl -u myapp.service -f
```

This is usually the fastest way to debug startup problems.

***

# Common problems

## 1) Using a relative path in `ExecStart`

This will fail:

```ini
ExecStart=./myapp
```

Use the **full absolute path**:

```ini
ExecStart=/opt/myapp/myapp
```

***

## 2) Missing execute permission

Fix with:

```bash
sudo chmod +x /opt/myapp/myapp
```

***

## 3) Program depends on files but cannot find them

Set:

```ini
WorkingDirectory=/opt/myapp
```

***

## 4) Program works manually but fails as a service

This often happens because:

* environment variables are missing
* service runs as a different user
* working directory is different
* system PATH is different

If needed, you can add environment variables:

```ini
Environment="MY_VAR=value"
```

or

```ini
Environment="PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
```

***

## 5) App exits immediately

If your program is supposed to keep running but exits, systemd may repeatedly restart it.  
Make sure your app either:

* stays running (for daemon/server style apps), or
* you configure the service differently if it is a one-time script/tool.

For long-running apps, `Type=simple` is usually correct.

***

# Commands summary

Replace `myapp` with your real name:

```bash
g++ main.cpp -o myapp
sudo mkdir -p /opt/myapp
sudo cp myapp /opt/myapp/
sudo chmod +x /opt/myapp/myapp

sudo nano /etc/systemd/system/myapp.service
sudo systemctl daemon-reload
sudo systemctl enable myapp.service
sudo systemctl start myapp.service
sudo systemctl status myapp.service
```

***

# If you want it to run only after reboot, not immediately

Just enable it:

```bash
sudo systemctl enable myapp.service
```

Then reboot:

```bash
sudo reboot
```

After boot:

```bash
systemctl status myapp.service
```

***

# Alternative methods (not recommended for most servers)

You *can* also use:

* `cron @reboot`
* `/etc/rc.local` (older method)

But on modern Ubuntu Server, **systemd is the correct approach**.

***

