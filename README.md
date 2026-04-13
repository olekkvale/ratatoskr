# Ratatoskr

Open source hardware control for Linux -- headsets, keyboards, and more.

Ratatoskr is a daemon that communicates with USB HID devices and exposes control via D-Bus. 

Named after the squirrel that carries messages between worlds on Yggdrasil.

## Supported devices

- **Logitech Astro A50 Gen 5** -- 24 GET + 15 SET + 6 real-time signals

## Features

- D-Bus API on system bus (`org.ratatoskr.Device`)
- Background listener for spontaneous HID reports (battery, volume, mic mute, etc.)
- Auto-reconnect on USB re-enumeration
- Modular driver architecture -- one driver per device

## Building

Requires: `hidapi`, `sdbus-cpp` (v2+), `libudev`, `cmake` (3.16+), C++17 compiler.

```bash
# Arch Linux
sudo pacman -S hidapi sdbus-cpp cmake

mkdir build && cd build
cmake ..
make
```

## Installation

```bash
sudo cmake --install build

# Enable and start
sudo systemctl enable --now ratatoskrd
```

Files installed:
- `/usr/bin/ratatoskrd` -- daemon binary
- `/usr/lib/systemd/system/ratatoskrd.service` -- systemd service
- `/usr/share/dbus-1/system.d/org.ratatoskr.conf` -- D-Bus policy
- `/usr/lib/udev/rules.d/90-ratatoskr.rules` -- udev rules

## Usage

```bash
# Query battery
busctl call org.ratatoskr /org/ratatoskr/devices/a50_gen5 \
  org.ratatoskr.Device GetBattery

# Set sidetone (0-6)
busctl call org.ratatoskr /org/ratatoskr/devices/a50_gen5 \
  org.ratatoskr.Device SetSidetone i 3

# Monitor real-time signals
dbus-monitor --system "type='signal',sender='org.ratatoskr'"
```

## D-Bus API

### GET methods

| Method | Return | Description |
|--------|--------|-------------|
| GetBattery | (int32, bool) | Percent + charging |
| GetBatteryPercent | int32 | Percent only |
| GetBatteryCharging | int32 | 1=charging, 0=not |
| GetVolume | int32 | 0-31 |
| GetSidetone | int32 | 0-6 |
| GetMicMute | bool | Flip-to-mute state |
| GetChatmix | int32 | 0-12 (Voice to Game) |
| GetDeviceName | string | "A50" |
| GetSerialNumber | string | ASCII serial |
| GetFirmwareVersion | string | "1.3.24 main" |
| GetUptime | int32 | Base station uptime (seconds) |
| GetNoiseGate | int32 | 0x01/0x02/0x04 |
| GetSleepMode | int32 | 0/15/30/60 min |
| GetNotificationSound | int32 | 0/1/2 |
| GetLedBrightness | int32 | 0-100 |
| GetBaseMac | string | BT MAC address |
| GetBluetoothStatus | bool | Connected |
| GetBluetoothName | string | BT device name |
| GetBluetoothAddress | string | BT device MAC |
| GetFirmwareShort | string | "1.3.24" |
| GetDeviceInfo | string | HW ID + build date |
| GetProtocolId | string | "ah v15" |
| GetSerialMeta | string | Serial via metadata |
| GetRouting | (i,b,i,b,i,b,i,b,i,b) | Stream routing: 5 channels × (vol, mute) |

### SET methods

| Method | Args | Description |
|--------|------|-------------|
| SetSidetone | int32 | 0-6 |
| SetVolume | int32 | 0-21 |
| SetMixamp | int32 | 0-12 |
| SetEqualizerPreset | int32 | 0=Standard, 1=Gaming, 2=Media |
| SetNoiseGate | int32 | 0=Home, 1=Night, 2=Tournament |
| SetNotificationSound | int32 | 0=None, 1=Minimal, 2=All |
| SetLedBrightness | int32 | 0-100 |
| SetInactiveTime | int32 | 0/15/30/60 min |
| SetMicMute | bool | SW mute |
| SetCustomEqualizer | int32, ay | Type + 50 bytes band data |
| SetEqualizerActive | int32 | Preset number |
| SaveEqualizerPreset | -- | Save to headset |
| FactoryReset | string | Serial number (12 chars) |
| StartBluetoothPairing | -- | Trigger BT search |
| SetRouting | i,b,i,b,i,b,i,b,i,b | Stream routing: stream/mic/game/bt/voice × (vol, mute) |

### Signals

| Signal | Args | Trigger |
|--------|------|---------|
| BatteryChanged | int32, bool | Every 2-5 min |
| VolumeChanged | int32 | Volume wheel |
| MicMuteChanged | bool | Flip-to-mute |
| MixampChanged | int32 | MixAmp dial |
| PowerChanged | int32 | Power on/off |
| BluetoothChanged | bool | BT connect/disconnect |

## License

GPL-3.0. See [LICENSE](LICENSE).
