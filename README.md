# Wireless HP LaserJet 1020 Print Server

Turns a USB-only HP LaserJet 1020 into a wireless cloud-connected printer using an ESP32-S3 as a WiFi-to-USB bridge and a Flask web server on AWS.

## Architecture

```
Browser (anywhere) → AWS Flask Server → ESP32-S3 (home WiFi) → HP 1020 (USB)
```

1. Upload a document through the web portal (PDF, image, text)
2. Server renders it to 600 DPI monochrome PBM
3. ESP32 downloads the rendered pages over HTTPS
4. ESP32 JBIG-compresses, wraps in ZjStream, sends to printer via USB Host

## Hardware

- ESP32-S3-DevKitC-1 N16R8 (16 MB flash, 8 MB PSRAM)
- USB OTG adapter (USB-C/Micro-USB to USB-A female)
- Standard USB-A to USB-B printer cable
- HP LaserJet 1020 (self-powered)

Connect the OTG port (GPIO19/20) to the HP 1020. Use the UART port for programming.

## Quick Start

### 1. ESP32 Firmware

```bash
cd firmware

# Configure WiFi and server URL
pio run -t menuconfig
# → Print Server Configuration → set SSID, password, cloud URL, API key

# Download HP 1020 firmware blob (see firmware/data/README.md)

# Build and flash
pio run --target upload

# Upload SPIFFS data (sihp1020.dl)
pio run --target uploadfs
```

### 2. Cloud Server (AWS EC2)

```bash
cd server
cp .env.example .env
# Edit .env: set SECRET_KEY, WEB_PASSWORD, DEVICE_API_KEY

# Local dev:
pip install -r requirements.txt
python app.py

# Production (Docker):
docker compose up -d
```

### 3. Print

Open `https://your-server/` in any browser, log in, upload a document, hit Print.

## Project Structure

```
firmware/                 ESP32-S3 firmware (ESP-IDF + PlatformIO)
  components/
    usb_printer/          USB Host Printer Class driver
    zjstream/             JBIG encoder + ZjStream protocol
  src/
    main.c                App entry point
    wifi_manager.c        WiFi STA/AP
    cloud_client.c        WebSocket + HTTPS to AWS
    printer_task.c        FreeRTOS print queue
  data/
    sihp1020.dl           HP 1020 firmware (download separately)

server/                   Flask web print server
  app.py                  Routes, WebSocket, job queue
  renderer.py             PDF/image → PBM conversion
  templates/index.html    Web portal
  static/                 CSS + JS
  Dockerfile
  docker-compose.yml
  nginx.conf
```

## Configuration

### ESP32 (via menuconfig)

| Setting | Description |
|---------|-------------|
| `WIFI_SSID` | Home WiFi network name |
| `WIFI_PASS` | Home WiFi password |
| `CLOUD_URL` | Server WebSocket URL (`wss://print.example.com`) |
| `CLOUD_API_KEY` | Shared secret for ESP32 ↔ server auth |

### Server (.env)

| Setting | Description |
|---------|-------------|
| `SECRET_KEY` | Flask session secret (random string) |
| `WEB_PASSWORD` | Password for the web portal login |
| `DEVICE_API_KEY` | Must match the ESP32's `CLOUD_API_KEY` |
| `JOBS_DIR` | Directory for rendered PBM temp files |

## License

JBIG encoder based on the ITU T.82 standard. ZjStream protocol derived from the foo2zjs project (GPLv2).
