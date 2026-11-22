// Copyright 2026, FreeTouch
/*!
 * @file
 * @brief  The dongle's USB CDC port: found by its USB ID, opened raw, read with a timeout.
 */

#include "freetouch/freetouch.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

#include <windows.h>
#include <setupapi.h>
#include <devguid.h>

struct freetouch_port
{
	HANDLE handle;
	int timeout_ms; // the read timeout last set; -2 none yet
};

bool
freetouch_port_find(char *out_path, size_t out_len)
{
	char want[32];
	snprintf(want, sizeof(want), "VID_%04X&PID_%04X", FREETOUCH_VID, FREETOUCH_PID);

	HDEVINFO set = SetupDiGetClassDevsA(&GUID_DEVCLASS_PORTS, NULL, NULL, DIGCF_PRESENT);
	if (set == INVALID_HANDLE_VALUE) {
		return false;
	}
	bool found = false;
	SP_DEVINFO_DATA dev = {.cbSize = sizeof(dev)};
	for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(set, i, &dev); i++) {
		char hwid[512] = {0};
		if (!SetupDiGetDeviceRegistryPropertyA(set, &dev, SPDRP_HARDWAREID, NULL, (BYTE *)hwid, sizeof(hwid) - 1,
		                                       NULL)) {
			continue;
		}
		// a list of IDs, most specific first: any may match
		bool match = false;
		for (char *id = hwid; id < hwid + sizeof(hwid) && *id != '\0'; id += strlen(id) + 1) {
			for (char *c = id; *c != '\0'; c++) {
				*c = (char)toupper((unsigned char)*c);
			}
			if (strstr(id, want) != NULL) {
				match = true;
				break;
			}
		}
		if (!match) {
			continue;
		}
		HKEY key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
		if (key == INVALID_HANDLE_VALUE) {
			continue;
		}
		char name[64];
		DWORD size = sizeof(name), type;
		if (RegQueryValueExA(key, "PortName", NULL, &type, (BYTE *)name, &size) == ERROR_SUCCESS &&
		    type == REG_SZ) {
			snprintf(out_path, out_len, "%.*s", (int)size, name);
			found = true;
		}
		RegCloseKey(key);
	}
	SetupDiDestroyDeviceInfoList(set);
	return found;
}

freetouch_port *
freetouch_port_open(const char *path)
{
	// COM10 and up open only through the device namespace; it works for the low ones too
	char full[64];
	if (strncmp(path, "\\\\.\\", 4) == 0) {
		snprintf(full, sizeof(full), "%s", path);
	} else {
		snprintf(full, sizeof(full), "\\\\.\\%s", path);
	}
	HANDLE h = CreateFileA(full, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		return NULL;
	}
	DCB dcb = {0};
	dcb.DCBlength = sizeof(dcb);
	GetCommState(h, &dcb);
	dcb.BaudRate = 115200; // USB CDC ignores it, but 1200 would ask for the bootloader
	dcb.ByteSize = 8;
	dcb.StopBits = ONESTOPBIT;
	dcb.Parity = NOPARITY;
	dcb.fBinary = TRUE;
	dcb.fParity = FALSE;
	dcb.fOutxCtsFlow = FALSE;
	dcb.fOutxDsrFlow = FALSE;
	dcb.fDtrControl = DTR_CONTROL_ENABLE;
	dcb.fRtsControl = RTS_CONTROL_ENABLE;
	dcb.fOutX = dcb.fInX = FALSE;
	if (!SetCommState(h, &dcb)) {
		CloseHandle(h);
		return NULL;
	}
	PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

	freetouch_port *port = calloc(1, sizeof(*port));
	if (port == NULL) {
		CloseHandle(h);
		return NULL;
	}
	port->handle = h;
	port->timeout_ms = -2;
	return port;
}

int
freetouch_port_read(freetouch_port *port, uint8_t *data, size_t size, int timeout_ms)
{
	if (timeout_ms != port->timeout_ms) {
		// what is queued comes back at once; with nothing queued, wait for the first byte
		COMMTIMEOUTS to = {0};
		to.ReadIntervalTimeout = MAXDWORD;
		if (timeout_ms > 0) {
			to.ReadTotalTimeoutMultiplier = MAXDWORD;
			to.ReadTotalTimeoutConstant = (DWORD)timeout_ms;
		}
		if (!SetCommTimeouts(port->handle, &to)) {
			return -1;
		}
		port->timeout_ms = timeout_ms;
	}
	DWORD got = 0;
	if (!ReadFile(port->handle, data, (DWORD)size, &got, NULL)) {
		return -1;
	}
	return (int)got;
}

int
freetouch_port_write(freetouch_port *port, const uint8_t *data, size_t size)
{
	DWORD put = 0;
	if (!WriteFile(port->handle, data, (DWORD)size, &put, NULL)) {
		return -1;
	}
	return (int)put;
}

void
freetouch_port_close(freetouch_port *port)
{
	if (port == NULL) {
		return;
	}
	CloseHandle(port->handle);
	free(port);
}

#else // POSIX

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

struct freetouch_port
{
	int fd;
};

static bool
read_hex_file(const char *path, unsigned *out)
{
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		return false;
	}
	bool ok = fscanf(f, "%x", out) == 1;
	fclose(f);
	return ok;
}

bool
freetouch_port_find(char *out_path, size_t out_len)
{
	DIR *dir = opendir("/sys/class/tty");
	if (dir == NULL) {
		return false;
	}
	bool found = false;
	struct dirent *e;
	while (!found && (e = readdir(dir)) != NULL) {
		if (strncmp(e->d_name, "ttyACM", 6) != 0) {
			continue;
		}
		// .../ttyACMn/device is the CDC interface; its parent holds the USB device's IDs
		char path[512];
		unsigned vid, pid;
		snprintf(path, sizeof(path), "/sys/class/tty/%s/device/../idVendor", e->d_name);
		if (!read_hex_file(path, &vid)) {
			continue;
		}
		snprintf(path, sizeof(path), "/sys/class/tty/%s/device/../idProduct", e->d_name);
		if (!read_hex_file(path, &pid)) {
			continue;
		}
		if (vid == FREETOUCH_VID && pid == FREETOUCH_PID) {
			snprintf(out_path, out_len, "/dev/%s", e->d_name);
			found = true;
		}
	}
	closedir(dir);
	return found;
}

freetouch_port *
freetouch_port_open(const char *path)
{
	int fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd < 0) {
		return NULL;
	}
	struct termios t;
	if (tcgetattr(fd, &t) != 0) {
		close(fd);
		return NULL;
	}
	cfmakeraw(&t);
	t.c_cflag |= CLOCAL | CREAD;
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	cfsetspeed(&t, B115200); // 1200 would ask for the bootloader
	tcsetattr(fd, TCSANOW, &t);
	int bits = TIOCM_DTR | TIOCM_RTS;
	ioctl(fd, TIOCMBIS, &bits);
	tcflush(fd, TCIOFLUSH);

	freetouch_port *port = calloc(1, sizeof(*port));
	if (port == NULL) {
		close(fd);
		return NULL;
	}
	port->fd = fd;
	return port;
}

int
freetouch_port_read(freetouch_port *port, uint8_t *data, size_t size, int timeout_ms)
{
	struct pollfd p = {.fd = port->fd, .events = POLLIN};
	int r = poll(&p, 1, timeout_ms < 0 ? 0 : timeout_ms);
	if (r < 0) {
		return errno == EINTR ? 0 : -1;
	}
	if (r == 0) {
		return 0;
	}
	if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
		return -1;
	}
	ssize_t got = read(port->fd, data, size);
	if (got < 0) {
		return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
	}
	return (int)got;
}

int
freetouch_port_write(freetouch_port *port, const uint8_t *data, size_t size)
{
	ssize_t put = write(port->fd, data, size);
	return put < 0 ? -1 : (int)put;
}

void
freetouch_port_close(freetouch_port *port)
{
	if (port == NULL) {
		return;
	}
	close(port->fd);
	free(port);
}

#endif
