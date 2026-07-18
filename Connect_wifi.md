### 1) Install NetworkManager

`nmcli` is the CLI for NetworkManager, and it is explicitly intended for **servers, headless machines, and terminals**.

```bash
sudo apt update
sudo apt install -y network-manager
sudo systemctl enable --now NetworkManager
```

***

### 2) Change Netplan to use NetworkManager

Netplan reads YAML from `/etc/netplan/*.yaml`, and the simplest way to hand over control is to set:

```yaml
network:
  version: 2
  renderer: NetworkManager
```

That makes Netplan hand control to NetworkManager for devices it manages. `netplan generate`, `netplan apply`, and `netplan try` are the correct commands to render/apply the backend config.

Create a new override file (safer than editing installer/cloud-generated files directly):

```bash
sudo tee /etc/netplan/01-network-manager-all.yaml >/dev/null <<'EOF'
network:
  version: 2
  renderer: NetworkManager
EOF
```

Then generate/test/apply:

```bash
sudo netplan generate
sudo netplan try
# if all looks good:
sudo netplan apply
```

***

### 3) If this is a cloud image, disable cloud-init network rewrites

On many Ubuntu Server cloud images, `cloud-init` can re-generate network config on boot. Cloud-init documents that you can disable its network configuration with:

```yaml
network:
  config: disabled
```

in `/etc/cloud/cloud.cfg.d/*`.

Run:

```bash
sudo mkdir -p /etc/cloud/cloud.cfg.d
sudo tee /etc/cloud/cloud.cfg.d/99-disable-network-config.cfg >/dev/null <<'EOF'
network: {config: disabled}
EOF
```

***

### 4) Stop/disable/mask `systemd-networkd`

`systemd` socket units can activate matching services on demand, so if you want `systemd-networkd` fully out of the way, disable/mask **both** the service and its socket. The `systemd-networkd` service is commonly shown as being triggered by `systemd-networkd.socket`, and socket units are specifically used for on-demand activation.

```bash
sudo systemctl stop systemd-networkd.service systemd-networkd.socket
sudo systemctl disable systemd-networkd.service systemd-networkd.socket
sudo systemctl mask systemd-networkd.service systemd-networkd.socket

sudo systemctl stop systemd-networkd-wait-online.service
sudo systemctl disable systemd-networkd-wait-online.service
sudo systemctl mask systemd-networkd-wait-online.service
```

***

## 5) Use `nmcli` to manage Wi-Fi

The official `nmcli` examples show:

* list Wi-Fi networks with `nmcli device wifi list`
* connect with `nmcli device wifi connect "SSID" password "PASSWORD"` or `nmcli --ask device wifi connect "SSID"`

### Check your devices

```bash
nmcli general status
nmcli device status
```

### Turn Wi-Fi radio on

```bash
sudo nmcli radio wifi on
```

### Scan available networks

```bash
nmcli device wifi list
```

### Connect to Wi-Fi

```bash
sudo nmcli --ask device wifi connect "YourSSID"
```

Or non-interactively:

```bash
sudo nmcli device wifi connect "YourSSID" password "YourPassword"
```

***

## 6) Verify NetworkManager really owns the Wi-Fi interface

`nmcli` can show whether a device is managed by NetworkManager, including the `GENERAL.NM-MANAGED: yes` field in device details.

First find your Wi-Fi interface name:

```bash
nmcli device status
```

Then inspect it:

```bash
nmcli -p -f general,wifi-properties device show wlp2s0
```

You want to see something like:

```text
GENERAL.NM-MANAGED: yes
```

***

## Quick end-to-end command set

If you want the shortest working sequence:

```bash
sudo apt update
sudo apt install -y network-manager
sudo systemctl enable --now NetworkManager

sudo tee /etc/netplan/01-network-manager-all.yaml >/dev/null <<'EOF'
network:
  version: 2
  renderer: NetworkManager
EOF

sudo mkdir -p /etc/cloud/cloud.cfg.d
sudo tee /etc/cloud/cloud.cfg.d/99-disable-network-config.cfg >/dev/null <<'EOF'
network: {config: disabled}
EOF

sudo netplan generate
sudo netplan try
sudo netplan apply

sudo systemctl stop systemd-networkd.service systemd-networkd.socket
sudo systemctl disable systemd-networkd.service systemd-networkd.socket
sudo systemctl mask systemd-networkd.service systemd-networkd.socket
sudo systemctl stop systemd-networkd-wait-online.service
sudo systemctl disable systemd-networkd-wait-online.service
sudo systemctl mask systemd-networkd-wait-online.service

nmcli radio wifi on
nmcli device wifi list
sudo nmcli --ask device wifi connect "YourSSID"
```

***

## Useful checks if it does not work

### Is NetworkManager running?

```bash
systemctl status NetworkManager
```

`nmcli` is the command-line client for controlling NetworkManager and reporting status.

### Is `systemd-networkd` still active?

```bash
systemctl status systemd-networkd
systemctl status systemd-networkd.socket
```

### What renderer does Netplan currently show?

```bash
sudo netplan get
```

Netplan’s active config should show `renderer: NetworkManager` when NM is the chosen backend.

### Is the Wi-Fi device managed by NM?

```bash
nmcli device status
nmcli -p -f general,wifi-properties device show wlp2s0
```

`GENERAL.NM-MANAGED: yes` indicates NetworkManager is handling it.


Got it, you want a Raspberry Pi CLI setup for **WPA2-Enterprise** using **PEAP + MSCHAPv2**, with **CA certificate set to “Do not validate.”** Here’s the practical `nmcli` way.

> ⚠️ Security note: “Do not validate CA certificate” can work, but it is less secure because your Pi may send credentials to a rogue/fake access point. PEAP/MSCHAPv2 normally relies on validating the RADIUS server certificate to prevent that. [\[security.s...change.com\]](https://security.stackexchange.com/questions/182530/is-it-possible-for-an-802-1x-network-peap-mschapv2-to-have-no-certificate), [\[community.cisco.com\]](https://community.cisco.com/t5/network-access-control/about-peap-need-to-install-certificates/td-p/4768871)

## 1. Check your Wi-Fi interface name

```bash
nmcli device status
```

Usually on Raspberry Pi it is:

```bash
wlan0
```

## 2. Create the WPA2-Enterprise connection

Replace:

* `YOUR_SSID`
* `YOUR_USERNAME`
* `YOUR_PASSWORD`

```bash
sudo nmcli connection add type wifi \
  ifname wlan0 \
  con-name "YOUR_SSID" \
  ssid "YOUR_SSID"
```

Then configure PEAP/MSCHAPv2:

```bash
sudo nmcli connection modify "YOUR_SSID" \
  wifi-sec.key-mgmt wpa-eap \
  802-1x.eap peap \
  802-1x.phase2-auth mschapv2 \
  802-1x.identity "YOUR_USERNAME" \
  802-1x.password "YOUR_PASSWORD" \
  802-1x.system-ca-certs no \
  802-1x.ca-cert ""
```

This matches the usual NetworkManager WPA2-Enterprise settings for PEAP/MSCHAPv2, including setting `wifi-sec.key-mgmt` to `wpa-eap`. [\[askubuntu.com\]](https://askubuntu.com/questions/262491/connect-to-a-wpa2-enterprise-connection-via-cli-no-desktop), [\[kitsugo.com\]](https://kitsugo.com/guide/mschapv2-on-networkmanager/)

## 3. Connect

```bash
sudo nmcli connection up "YOUR_SSID"
```

## 4. Full example

```bash
sudo nmcli connection add type wifi \
  ifname wlan0 \
  con-name "SchoolWiFi" \
  ssid "SchoolWiFi"

sudo nmcli connection modify "SchoolWiFi" \
  wifi-sec.key-mgmt wpa-eap \
  802-1x.eap peap \
  802-1x.phase2-auth mschapv2 \
  802-1x.identity "student123" \
  802-1x.password "mypassword" \
  802-1x.system-ca-certs no \
  802-1x.ca-cert ""

sudo nmcli connection up "SchoolWiFi"
```

## If it keeps asking for password or fails

Try restarting NetworkManager:

```bash
sudo systemctl restart NetworkManager
sudo nmcli connection up "YOUR_SSID"
```

If your Raspberry Pi OS does not have NetworkManager enabled, check:

```bash
systemctl status NetworkManager
```

Install/enable if needed:

```bash
sudo apt update
sudo apt install network-manager
sudo systemctl enable NetworkManager
sudo systemctl start NetworkManager
```

## Debug command

```bash
journalctl -u NetworkManager -f
```

That will show why authentication fails in real time.

