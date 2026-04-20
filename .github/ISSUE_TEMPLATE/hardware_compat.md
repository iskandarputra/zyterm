---
name: Hardware compatibility
about: Report a USB-serial adapter or device that doesn't work as expected
title: "[hw] <vendor> <model>"
labels: hardware
---

**Adapter**
- Vendor / model:
- USB ID:                 <!-- `lsusb` (e.g. `0403:6001`) -->
- Driver:                 <!-- `lsmod | grep -i usbserial` and `dmesg | grep tty` -->

**Target device**
- MCU / SoC:
- Firmware (RTOS / OS):
- Expected line ending:   <!-- CR | LF | CRLF -->

**zyterm command**

```
zyterm --baud … /dev/ttyUSBn
```

**Behaviour**
- [ ] Garbled characters
- [ ] No output at all
- [ ] Output OK but input is not received by device
- [ ] Disconnects intermittently
- [ ] Hangs zyterm
- [ ] Other:

**What does work**
<!-- Does another tool (minicom, screen, tio, picocom) handle this device fine?
     If so, what flags does it need? -->


**Capture**
<!-- Optional: attach a short `--dump 5 -l capture.log` -->
