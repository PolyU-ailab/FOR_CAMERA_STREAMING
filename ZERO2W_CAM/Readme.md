# To build a program 
sudo apt update
sudo apt install -y build-essential rpicam-apps


g++ -std=c++17 -O2 -pthread -static -static-libstdc++ -static-libgcc pi_cam_http_min.cpp -o pi_cam_http_min
