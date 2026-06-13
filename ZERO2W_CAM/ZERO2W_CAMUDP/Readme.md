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


