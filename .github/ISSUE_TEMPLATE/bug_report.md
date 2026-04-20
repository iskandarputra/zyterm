---
name: Bug report
about: Something isn't working — broken serial, mangled output, crash
title: "[bug] "
labels: bug
---

**zyterm version**
<!-- output of `zyterm --version` -->

**Host environment**
- OS / distro:
- Kernel:                 <!-- `uname -r` -->
- Terminal emulator:      <!-- xterm, alacritty, kitty, gnome-terminal, tmux, ssh, ... -->

**Device**
- Adapter:                <!-- e.g. FTDI FT232R, CH340, CP2102, on-board USB-CDC -->
- Driver in use:          <!-- `dmesg | grep tty` -->
- Baud / framing:         <!-- e.g. `115200 8N1` -->

**zyterm command-line**

```
zyterm --baud … /dev/ttyUSB0
```

**What did you expect to happen?**


**What actually happened?**


**Reproduction steps**
1.
2.
3.

**Logs**
<!-- attach a short capture: `zyterm --dump 5 -l /tmp/zt.log /dev/ttyUSB0` -->
