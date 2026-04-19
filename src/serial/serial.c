/**
 * @file    serial.c
 * @brief   Serial-port setup, baud selection, and flow control.
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#include "zt_ctx.h"
#include "zt_internal.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------ serial ----------------------------------- */

static speed_t baud_to_speed(unsigned b) {
    switch (b) {
    case 50: return B50;
    case 75: return B75;
    case 110: return B110;
    case 134: return B134;
    case 150: return B150;
    case 200: return B200;
    case 300: return B300;
    case 600: return B600;
    case 1200: return B1200;
    case 1800: return B1800;
    case 2400: return B2400;
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B500000
    case 500000: return B500000;
#endif
#ifdef B576000
    case 576000: return B576000;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
#ifdef B1000000
    case 1000000: return B1000000;
#endif
#ifdef B1152000
    case 1152000: return B1152000;
#endif
#ifdef B1500000
    case 1500000: return B1500000;
#endif
#ifdef B2000000
    case 2000000: return B2000000;
#endif
#ifdef B2500000
    case 2500000: return B2500000;
#endif
#ifdef B3000000
    case 3000000: return B3000000;
#endif
#ifdef B3500000
    case 3500000: return B3500000;
#endif
#ifdef B4000000
    case 4000000: return B4000000;
#endif
    default: return (speed_t)-1;
    }
}

int set_custom_baud(int fd, unsigned baud) {
#if defined(__linux__)
    struct termios2 t2;
    if (ioctl(fd, TCGETS2, &t2) < 0) return -1;
    t2.c_cflag &= ~(tcflag_t)CBAUD;
    t2.c_cflag |= BOTHER;
    t2.c_ispeed = baud;
    t2.c_ospeed = baud;
    return ioctl(fd, TCSETS2, &t2);
#elif defined(__APPLE__)
    speed_t sp = (speed_t)baud;
    return ioctl(fd, IOSSIOSPEED, &sp);
#else
    (void)fd;
    (void)baud;
    errno = ENOSYS;
    return -1;
#endif
}

int setup_serial(const char *path, unsigned baud, int data_bits, char parity, int stop_bits,
                 int flow) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) zt_die("zyterm: open(%s): %s", path, strerror(errno));
    if (!isatty(fd)) zt_die("zyterm: %s is not a TTY", path);

    struct termios t;
    if (tcgetattr(fd, &t) < 0) zt_die("zyterm: tcgetattr: %s", strerror(errno));
    t.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON |
                             IXOFF | IXANY);
    t.c_oflag &= ~(tcflag_t)OPOST;
    t.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cflag &= ~(tcflag_t)(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS);
    switch (data_bits) {
    case 5: t.c_cflag |= CS5; break;
    case 6: t.c_cflag |= CS6; break;
    case 7: t.c_cflag |= CS7; break;
    default: t.c_cflag |= CS8; break;
    }
    if (parity == 'e' || parity == 'E')
        t.c_cflag |= PARENB;
    else if (parity == 'o' || parity == 'O')
        t.c_cflag |= PARENB | PARODD;
    if (stop_bits == 2) t.c_cflag |= CSTOPB;
    if (flow == 1) t.c_cflag |= CRTSCTS;
    if (flow == 2) t.c_iflag |= IXON | IXOFF;
    t.c_cflag |= CREAD | CLOCAL;
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;

    speed_t sp    = baud_to_speed(baud);
    if (sp != (speed_t)-1) {
        cfsetispeed(&t, sp);
        cfsetospeed(&t, sp);
        if (tcsetattr(fd, TCSANOW, &t) < 0) zt_die("zyterm: tcsetattr: %s", strerror(errno));
    } else {
        if (tcsetattr(fd, TCSANOW, &t) < 0) zt_die("zyterm: tcsetattr: %s", strerror(errno));
        if (set_custom_baud(fd, baud) < 0)
            zt_die("zyterm: custom baud %u: %s", baud, strerror(errno));
    }
    (void)tcflush(fd, TCIOFLUSH);
    return fd;
}

/* Apply flow control change on an already-open fd (runtime toggle). */
int apply_flow(int fd, int flow) {
    struct termios t;
    if (tcgetattr(fd, &t) < 0) return -1;
    t.c_cflag &= ~(tcflag_t)CRTSCTS;
    t.c_iflag &= ~(tcflag_t)(IXON | IXOFF | IXANY);
    if (flow == 1) t.c_cflag |= CRTSCTS;
    if (flow == 2) t.c_iflag |= IXON | IXOFF;
    return tcsetattr(fd, TCSANOW, &t);
}

/* Non-fatal reopen for reconnect path: returns -1 on failure. */
int try_reopen_serial(const char *path, unsigned baud, int data_bits, char parity,
                      int stop_bits, int flow) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    if (!isatty(fd)) {
        close(fd);
        return -1;
    }
    struct termios t;
    if (tcgetattr(fd, &t) < 0) {
        close(fd);
        return -1;
    }
    t.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON |
                             IXOFF | IXANY);
    t.c_oflag &= ~(tcflag_t)OPOST;
    t.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t.c_cflag &= ~(tcflag_t)(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS);
    switch (data_bits) {
    case 5: t.c_cflag |= CS5; break;
    case 6: t.c_cflag |= CS6; break;
    case 7: t.c_cflag |= CS7; break;
    default: t.c_cflag |= CS8; break;
    }
    if (parity == 'e' || parity == 'E')
        t.c_cflag |= PARENB;
    else if (parity == 'o' || parity == 'O')
        t.c_cflag |= PARENB | PARODD;
    if (stop_bits == 2) t.c_cflag |= CSTOPB;
    if (flow == 1) t.c_cflag |= CRTSCTS;
    if (flow == 2) t.c_iflag |= IXON | IXOFF;
    t.c_cflag |= CREAD | CLOCAL;
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    speed_t sp    = baud_to_speed(baud);
    if (sp != (speed_t)-1) {
        cfsetispeed(&t, sp);
        cfsetospeed(&t, sp);
        if (tcsetattr(fd, TCSANOW, &t) < 0) {
            close(fd);
            return -1;
        }
    } else {
        if (tcsetattr(fd, TCSANOW, &t) < 0) {
            close(fd);
            return -1;
        }
        if (set_custom_baud(fd, baud) < 0) {
            close(fd);
            return -1;
        }
    }
    (void)tcflush(fd, TCIOFLUSH);
    return fd;
}
