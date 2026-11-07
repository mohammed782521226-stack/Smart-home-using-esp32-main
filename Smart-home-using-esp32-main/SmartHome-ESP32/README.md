# SmartHome-ESP32

ESP32 Smart Home Controller is a simple 8-channel smart home control project based on an ESP32 Dev Module. It combines physical wall switches, relay outputs, a web dashboard, Supabase cloud synchronization, and a local offline access point mode for basic control when internet service is unavailable.

This release folder is organized for a simple professional GitHub release. The original firmware and web interface were copied without changing their behavior or redesigning the UI.

## Features

- Controls 8 relay outputs from an ESP32 Dev Module.
- Reads 8 physical wall switches for local manual control.
- Provides a web dashboard for relay control and status viewing.
- Synchronizes relay and switch states with Supabase when internet is available.
- Keeps physical switch control working without WiFi or internet.
- Starts an ESP32 Access Point fallback for local control when connectivity is unavailable.
- Automatically retries WiFi and Supabase recovery.
- Pushes local state back to Supabase after reconnecting so the cloud state catches up.

## System Architecture

The project works in three layers:

1. **Physical switch layer**: Wall switches are scanned by the ESP32 and directly control the matching relay output. This layer does not require WiFi, internet, or Supabase.
2. **Local web control layer**: The ESP32 runs a local web server. When connected to the same network, users can open the ESP32 dashboard from a browser. If normal network/cloud access fails, the ESP32 can expose its own local Access Point.
3. **Cloud synchronization layer**: When internet is available, the firmware polls and updates Supabase so relay and switch states stay synchronized with the cloud dashboard.

Basic data flow:

```text
Wall Switches -> ESP32 GPIO Inputs -> Relay GPIO Outputs -> Relay Drivers -> Loads
                         |
                         +-> Local Web Server / Access Point
                         |
                         +-> Supabase REST API / Cloud Dashboard
```

## Hardware Description

The hardware is designed around an ESP32 Dev Module with 8 relay-controlled outputs and 8 physical wall switch inputs.

Main hardware blocks:

- ESP32 Dev Module, such as ESP32-WROOM-32 based development board.
- 8 relay channels for switching external lights or devices.
- 8 wall switch inputs connected to ESP32 GPIO pins.
- 2N2222 transistor drivers for relay coils where discrete relay driving is used.
- 560 ohm base resistors for transistor driver inputs.
- 10k ohm pull-up or pull-down resistors where required by the input/driver design.
- 100 nF capacitors for local decoupling/noise reduction.
- Protection components such as fuses, flyback protection, and suitable power protection devices.
- Screw terminals or connectors for relay outputs, switch inputs, and power wiring.

### GPIO Mapping

| Channel | Relay GPIO | Switch GPIO | Notes |
|---:|---:|---:|---|
| 1 | 25 | 32 | Relay 1 / Switch 1 |
| 2 | 26 | 12 | Relay 2 / Switch 2 |
| 3 | 22 | 33 | Relay 3 / Switch 3 |
| 4 | 14 | 4 | Relay 4 / Switch 4 |
| 5 | 27 | 19 | Relay 5 / Switch 5 |
| 6 | 21 | 18 | Relay 6 / Switch 6 |
| 7 | 13 | 5 | Relay 7 / Switch 7 |
| 8 | 23 | 36 | Relay 8 / Switch 8; GPIO 36 is input-only |

> Safety note: Mains wiring can be dangerous. Use proper isolation, fusing, enclosure protection, strain relief, and qualified electrical practices before connecting real AC loads.

## Software Requirements

- Arduino IDE 2.x or compatible ESP32 Arduino build environment.
- ESP32 board package installed in Arduino IDE.
- Board selection: **ESP32 Dev Module**.
- Required Arduino library:
  - ArduinoJson by Benoit Blanchon.
- Included with ESP32 Arduino core:
  - WiFi
  - WebServer
  - ESPmDNS
  - HTTPClient
  - WiFiClientSecure



## Installation Steps

1. Open Arduino IDE.
2. Install the ESP32 board package if it is not already installed.
3. Install **ArduinoJson** from Library Manager.
4. Open `Firmware/SmartHome_ESP32.ino`.
5. Select **ESP32 Dev Module** as the target board.
6. Select the correct USB/serial port.
7. Review and update WiFi and Supabase configuration values for your own environment.
8. Upload the sketch to the ESP32.
9. Open the Serial Monitor at 115200 baud to view connection status and local IP address.

## WiFi Configuration

In the firmware, configure your local WiFi network values before uploading:

```cpp
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
```

Do not publish real WiFi passwords in public repositories. For a public GitHub release, replace private credentials with example values before sharing.

## Supabase Configuration

The project uses Supabase to store and synchronize relay and switch states. Configure your own Supabase project values in the firmware and web interface:

- Supabase project URL.
- Supabase anonymous public key.
- REST endpoint for the `devices` table.
- Row ID used by the smart home controller.

Suggested table fields:

```text
id
relay1, relay2, relay3, relay4, relay5, relay6, relay7, relay8
switch1, switch2, switch3, switch4, switch5, switch6, switch7, switch8
```

Do not place service-role keys or private database credentials in firmware, browser code, screenshots, or documentation.

## Offline Mode

The firmware is designed to continue basic operation even when internet access fails:

- Physical wall switches continue to control their matching relays immediately.
- If Supabase is unreachable, relay changes are kept locally and queued for later synchronization.
- The ESP32 can start a fallback Access Point named `SmartHome` for local control.
- When internet access returns, the ESP32 pushes the current local state to Supabase first, then resumes normal cloud polling.

## Local Control Access

When connected to the same WiFi network as the ESP32, try one of these addresses:

- `http://smarthome.local`
- `http://<ESP32-IP-address>`

When the fallback Access Point is active:

1. Connect your phone or computer to the ESP32 WiFi network.
2. Open a browser.
3. Visit `http://192.168.4.1`.
4. Use the local dashboard to control relays.

## Web Interface

The web dashboard in `Web_Interface/index.html` is the original browser-based interface copied from the source workspace. It uses Supabase for cloud status and control. The firmware also contains an embedded local dashboard served directly by the ESP32 for offline/local control.

## Hardware Documents

- `Hardware/schematic.pdf` is currently a placeholder to be replaced with the final schematic.
- `Hardware/wiring_diagram.pdf` is currently a placeholder to be replaced with the final wiring diagram.
- `Hardware/images/` is reserved for board photos, enclosure photos, and wiring images.

## Future Improvements

- Replace placeholder hardware PDFs with final schematic and wiring diagrams.
- Move WiFi and Supabase settings to a separate configuration workflow before public release.
- Add screenshots of the web dashboard and local fallback dashboard.
- Add enclosure photos and PCB/prototype board photos.
- Add versioned release notes.
- Add a test checklist for relay outputs, switch inputs, offline mode, and reconnect behavior.

## License

This project is released under the MIT License. See `LICENSE` for details.
