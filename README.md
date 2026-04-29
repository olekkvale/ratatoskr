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
| GetBaseMac | string | Base station BT MAC |
| GetBattery | (int32, bool) | Percent + charging |
| GetBatteryCharging | int32 | 1=charging, 0=not |
| GetBatteryPercent | int32 | Percent only |
| GetBluetoothAddress | string | Connected BT device MAC |
| GetBluetoothName | string | Connected BT device name |
| GetBluetoothStatus | bool | Connected |
| GetDeviceInfo | string | HW ID + build date |
| GetDeviceName | string | "A50" |
| GetFirmwareShort | string | "1.3.24" |
| GetFirmwareVersion | string | "1.3.24 main" |
| GetLedBrightness | int32 | 0-100 |
| GetNotificationSound | int32 | 0=None, 1=Minimal, 2=All |
| GetPower | int32 | 1=on, 0=off, -1=unknown (live query) |
| GetProtocolId | string | "ah v15" |
| GetSerialMeta | string | Serial via metadata |
| GetSerialNumber | string | ASCII serial |
| GetSleepMode | int32 | 0/15/30/60 min |
| GetUptime | int32 | Base station uptime (seconds) |

#### SET

| Method | Args | Description |
|--------|------|-------------|
| FactoryReset | string | Serial number (12 chars) |
| SetInactiveTime | int32 | 0/15/30/60 min (auto-sleep) |
| SetLedBrightness | int32 | 0-100 |
| SetNotificationSound | int32 | 0=None, 1=Minimal, 2=All |
| StartBluetoothPairing | -- | Trigger BT search |

#### Signals

| Signal | Args | Trigger |
|--------|------|---------|
| BatteryChanged | int32, bool | Every 2-5 min |
| BluetoothChanged | bool | BT connect/disconnect |
| PowerChanged | int32 | Power on/off (raw byte: 0x00=on, 0x05=off). Suppressed at startup -- fires only on actual state change |

### Volume

Headset audio output, sidetone, microphone, and Game/Voice mix.

#### GET

| Method | Return | Description |
|--------|--------|-------------|
| GetChatmix | int32 | 0-12 (Voice to Game) |
| GetMicMute | bool | Flip-to-mute state |
| GetSidetone | int32 | 0-6 |
| GetVolume | int32 | 0-31 |

#### SET

| Method | Args | Description |
|--------|------|-------------|
| SetMicMute | bool | SW mute |
| SetMixamp | int32 | 0-12 |
| SetSidetone | int32 | 0-6 |
| SetVolume | int32 | 0-21 |

#### Signals

| Signal | Args | Trigger |
|--------|------|---------|
| MicMuteChanged | bool | Flip-to-mute lever |
| MixampChanged | int32 | MixAmp dial |
| VolumeChanged | int32 | Volume wheel |

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
| GetActiveEqualizerData | int32 → ay | Active EQ data for type (0=mic, 1=hp). 50 bytes in canonical SET-compatible layout. Lazy-cached, invalidated by external changes. |
| GetEqualizerPresetCount | int32 → int32 | Number of built-in presets per type (3 in firmware 1.3.24). |
| GetEqualizerPresetData | int32, int32 → ay | Built-in preset EQ data (type, index 0..count-1). 50 bytes, cached for session. |
| GetNoiseGate | int32 | 0x00=Disabled, 0x01=Home, 0x02=Night, 0x04=Tournament (device-internal 0x40 normalized to 0x00) |

#### SET

| Method | Args | Description |
|--------|------|-------------|
| SaveEqualizerPreset | -- | Persist active EQ to headset |
| SetCustomEqualizer | int32, ay | Type (0=mic, 1=headphone) + 50 bytes band data |
| SetEqualizerActive | int32 | Preset number |
| SetEqualizerPreset | int32 | 0=Standard, 1=Gaming, 2=Media |
| SetNoiseGate | int32 | 0=Home, 1=Night, 2=Tournament, 3=Disabled |

#### Signals

| Signal | Args | Description |
|--------|------|-------------|
| EqualizerChanged | uint32 | Fires whenever the device's EQ checksum changes (own SET, external G HUB session, etc.). Argument is the firmware checksum hash. Subscribers should re-fetch via GetActiveEqualizerData — the call is cache-warm after a self-initiated SET so it's free. |

## License

GPL-3.0. See [LICENSE](LICENSE).
