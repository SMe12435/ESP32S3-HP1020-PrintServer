# Printer Firmware Data

Place the HP LaserJet 1020 firmware file `sihp1020.dl` in this directory
before uploading to SPIFFS.

## How to obtain sihp1020.dl

```bash
git clone https://github.com/OpenPrinting/foo2zjs.git
cd foo2zjs
make
./getweb 1020
# The firmware file is now at firmware/sihp1020.dl
cp firmware/sihp1020.dl /path/to/ESP32S3_PrintServer/firmware/data/
```

Then upload to the ESP32 SPIFFS partition:

```bash
cd /path/to/ESP32S3_PrintServer/firmware
pio run --target uploadfs
```
