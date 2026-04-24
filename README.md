# Ratatoskr

Open source hardware control for Linux -- headsets, keyboards, and more.

Ratatoskr is a daemon that communicates with USB HID devices and exposes control via D-Bus. 

Named after the squirrel that carries messages between worlds on Yggdrasil.

## Supported devices

- **Logitech Astro A50 Gen 5** -- 25 GET + 15 SET + 6 real-time signals

### Planned (udev rules staged, driver not yet implemented)

- **Keychron Q6 HE** (USB wired + 2.4 GHz dongle)

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
# Recommended: use /usr prefix for consistent paths
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build

# Enable and start daemon
sudo systemctl daemon-reload
sudo systemctl enable --now ratatoskrd
```

Binary path follows `CMAKE_INSTALL_PREFIX`. System integration files (systemd,
D-Bus, udev) are installed to absolute paths because those subsystems do not
search under `/usr/local`. Defaults can be overridden:
`-DSYSTEMD_UNIT_DIR=...`, `-DDBUS_SYSTEM_POLICY_DIR=...`, `-DUDEV_RULES_DIR=...`.

Files installed (with `-DCMAKE_INSTALL_PREFIX=/usr`):
- `/usr/bin/ratatoskrd` -- daemon binary
- `/usr/lib/systemd/system/ratatoskrd.service` -- systemd service
- `/usr/share/dbus-1/system.d/org.ratatoskr.conf` -- D-Bus policy
- `/usr/lib/udev/rules.d/90-ratatoskr.rules` -- udev rules

## Uninstallation

```bash
sudo systemctl disable --now ratatoskrd
sudo cmake --build build --target uninstall
```

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

The API is grouped by feature area, mirroring the tabs in `ratatoskr-gui` and the
Noctalia plugin: **Settings**, **Volume**, **Stream (Routing)**, and **EQ**.

### Settings

Device identity, power, battery, Bluetooth, idle behaviour, LED, and notifications.

#### GET

| Method | Return | Description |
|--------|--------|-------------|
| GetPower | int32 | 1=on, 0=off, -1=unknown (live query) |
| GetBattery | (int32, bool) | Percent + charging |
| GetBatteryPercent | int32 | Percent only |
| GetBatteryCharging | int32 | 1=charging, 0=not |
| GetDeviceName | string | "A50" |
| GetSerialNumber | string | ASCII serial |
| GetFirmwareVersion | string | "1.3.24 main" |
| GetFirmwareShort | string | "1.3.24" |
| GetDeviceInfo | string | HW ID + build date |
| GetProtocolId | string | "ah v15" |
| GetSerialMeta | string | Serial via metadata |
| GetUptime | int32 | Base station uptime (seconds) |
| GetBaseMac | string | Base station BT MAC |
| GetBluetoothStatus | bool | Connected |
| GetBluetoothName | string | Connected BT device name |
| GetBluetoothAddress | string | Connected BT device MAC |
| GetSleepMode | int32 | 0/15/30/60 min |
| GetNotificationSound | int32 | 0=None, 1=Minimal, 2=All |
| GetLedBrightness | int32 | 0-100 |

#### SET

| Method | Args | Description |
|--------|------|-------------|
| SetInactiveTime | int32 | 0/15/30/60 min (auto-sleep) |
| SetNotificationSound | int32 | 0=None, 1=Minimal, 2=All |
| SetLedBrightness | int32 | 0-100 |
| StartBluetoothPairing | -- | Trigger BT search |
| FactoryReset | string | Serial number (12 chars) |

#### Signals

| Signal | Args | Trigger |
|--------|------|---------|
| BatteryChanged | int32, bool | Every 2-5 min |
| PowerChanged | int32 | Power on/off (raw byte: 0x00=on, 0x05=off). Suppressed at startup -- fires only on actual state change |
| BluetoothChanged | bool | BT connect/disconnect |

### Volume

Headset audio output, sidetone, microphone, and Game/Voice mix.

#### GET

| Method | Return | Description |
|--------|--------|-------------|
| GetVolume | int32 | 0-31 |
| GetSidetone | int32 | 0-6 |
| GetChatmix | int32 | 0-12 (Voice to Game) |
| GetMicMute | bool | Flip-to-mute state |

#### SET

| Method | Args | Description |
|--------|------|-------------|
| SetVolume | int32 | 0-21 |
| SetSidetone | int32 | 0-6 |
| SetMixamp | int32 | 0-12 |
| SetMicMute | bool | SW mute |

#### Signals

| Signal | Args | Trigger |
|--------|------|---------|
| VolumeChanged | int32 | Volume wheel |
| MicMuteChanged | bool | Flip-to-mute lever |
| MixampChanged | int32 | MixAmp dial |

### Stream (Routing)

Five-channel stream mixer: stream master + mic out + game + bluetooth + voice.

#### GET

| Method | Return | Description |
|--------|--------|-------------|
| GetRouting | (i,b,i,b,i,b,i,b,i,b) | 5 channels × (vol, mute) |

#### SET

| Method | Args | Description |
|--------|------|-------------|
| SetRouting | i,b,i,b,i,b,i,b,i,b | stream/mic/game/bt/voice × (vol, mute) |

### EQ

Headphone/microphone equalizer and noise gate.

#### GET

| Method | Return | Description |
|--------|--------|-------------|
| GetNoiseGate | int32 | 0x01=Home, 0x02=Night, 0x04=Tournament |

#### SET

| Method | Args | Description |
|--------|------|-------------|
| SetEqualizerPreset | int32 | 0=Standard, 1=Gaming, 2=Media |
| SetCustomEqualizer | int32, ay | Type (0=mic, 1=headphone) + 50 bytes band data |
| SetEqualizerActive | int32 | Preset number |
| SaveEqualizerPreset | -- | Persist active EQ to headset |
| SetNoiseGate | int32 | 0=Home, 1=Night, 2=Tournament |

## License

GPL-3.0. See [LICENSE](LICENSE).
