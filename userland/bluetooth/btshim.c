/*
 * btshim: Apple's bluetoothd on the sapphire's Qualcomm WCN3988 (research/bluetooth.md).
 *
 * How bluetoothd (iOS 16.7, 20H392) reaches its controller, from its disassembly:
 *  - BT_HCI_TRANSPORT="<transport>:<driver>:<options>" in the environment overrides the device
 *    tree. Transports: USB H4DS H4BC H4 H5 APPLEBT VIRT PCIE PCIESW. Drivers: USB POSIX APPLEBT
 *    VIRT PCIE PCIESW. On a real iPhone X the device tree gives transport-encoding 3 = H4BC over
 *    the POSIX driver, which is NOT a tty: it is the AppleSerialShim Skywalk nexus (kernel
 *    control "com.apple.uart.sk.bluetooth" + os_channel). /dev/uart.bluetooth is only access()ed.
 *  - The VIRT driver ("VirtualCTRL") opens <options>-in O_WRONLY (host -> controller bytes) and
 *    <options>-out O_RDONLY|O_NONBLOCK (controller -> host bytes), and a thread select()s and
 *    read()s the second one. With the H4 transport the bytes are plain H4 (type byte + packet).
 *  So bluetoothd runs with BT_HCI_TRANSPORT=VIRT:VIRT:/var/run/btshim/hci, and this program owns
 *  the two FIFOs /var/run/btshim/hci-in and hci-out.
 *  - bluetoothd starts only if the local address is non-zero (BT_DEVICE_ADDRESS=aa:bb:.. in the
 *    environment, else the device tree's local-mac-address) and if /dev/cu.bluetooth or
 *    /dev/uart.bluetooth exists or an IOService is named "marconi-bt" or "bluetooth"
 *    ("No bluetooth on this device." otherwise).
 *  - Before HCI, bluetoothd asks com.apple.BlueTool (XPC, mach service) to run its "boot" /
 *    "init" / "deepsleep" scripts: {kMsgId: "RunBuiltinScript", kMsgArgs: {script, btaddress}}
 *    -> reply {result: int64 (0 = ok), manufacturer: int64, firmware: string}, and
 *    {kMsgId: "RunCommand", kMsgArgs: {command}} -> {result, output}. Apple's BlueTool only knows
 *    Broadcom/ACI chips, so "btshim bluetool" replaces it (same mach service name) and forwards
 *    the script names to the daemon over /var/run/btshim/ctl.
 *  - Then the standard HCI init chain (every step fatal on error except Reset and Read BD_ADDR):
 *    Reset, Read Local Version, Read Local Supported Commands, Read BD_ADDR, Read Local Features,
 *    Read Local Extended Features, LE Read Local Features, Set Event Mask (+ Page 2), LE Set Event
 *    Mask, Write Simple Pairing Mode, Write LE Host Support, LE Read Buffer Size (v2 or v1), LE
 *    Read Supported States, LE Read Filter Accept List Size, Read Buffer Size, LE Read Periodic
 *    Advertiser List Size, LE Set Host Feature, LE Set Default Subrate, Host Buffer Size, Set
 *    Controller To Host Flow Control, Write Default Erroneous Data Reporting; then the Broadcom
 *    vendor commands of bcm_vsc.c (the chipset family comes from the platform, not from HCI, so
 *    an iPhone10,3 always gets the Broadcom set).
 *
 * Modes:
 *   btshim daemon [opts]   the bridge (launchd job, root). Real controller on /dev/qcabt.
 *   btshim daemon -F       fake controller, no hardware: answers like a BCM4357B1 and logs every
 *                          packet, to record what bluetoothd sends.
 *   btshim bluetool        the com.apple.BlueTool replacement (launchd job, MachServices).
 *   btshim bdstart         bluetoothd's launchd program: execs it with DYLD_INSERT_LIBRARIES=btbridge.dylib.
 * The Qualcomm bring-up itself is qca.c (shared with the standalone qcabtfw).
 * Options: -d fw-dir (/var/root/btfw)  -r rampatch.tlv  -n nvm.bin  -a aa:bb:cc:dd:ee:ff
 *          -s speed (default 115200; faster needs QCABT_SETBAUD)  -p fifo-prefix  -v  -I
 *          -w seconds: the bring-up waits until the boot is that old (120)
 * Log: /var/tmp/btshim.log (and stderr).
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include <dispatch/dispatch.h>

/* libxpc (libSystem): the SDK in use ships no xpc headers, so the few calls are declared here */
typedef void *xpc_object_t;
typedef void *xpc_connection_t;
typedef const struct _xpc_type_s *xpc_type_t;
typedef void (^xpc_handler_t)(xpc_object_t);
extern const struct _xpc_type_s _xpc_type_connection, _xpc_type_dictionary;
#define XPC_TYPE_CONNECTION (&_xpc_type_connection)
#define XPC_TYPE_DICTIONARY (&_xpc_type_dictionary)
#define XPC_CONNECTION_MACH_SERVICE_LISTENER (1 << 0)
extern xpc_connection_t xpc_connection_create_mach_service(const char *, dispatch_queue_t, uint64_t);
extern void xpc_connection_set_event_handler(xpc_connection_t, xpc_handler_t);
extern void xpc_connection_resume(xpc_connection_t);
extern void xpc_connection_send_message(xpc_connection_t, xpc_object_t);
extern xpc_type_t xpc_get_type(xpc_object_t);
extern const char *xpc_dictionary_get_string(xpc_object_t, const char *);
extern xpc_object_t xpc_dictionary_get_value(xpc_object_t, const char *);
extern xpc_object_t xpc_dictionary_create_reply(xpc_object_t);
extern void xpc_dictionary_set_int64(xpc_object_t, const char *, int64_t);
extern void xpc_dictionary_set_string(xpc_object_t, const char *, const char *);
extern void xpc_release(xpc_object_t);
#include "qcabt.h"

#define RUN_DIR         "/var/run/btshim"
#define CTL_SOCK        RUN_DIR "/ctl"
#define DEF_PREFIX      RUN_DIR "/hci"
#define LOG_PATH        "/var/tmp/btshim.log"
#define FW_DIR          "/var/root/btfw"

#include "qca.c"

/* ---------------------------------------------------------------- options, log */

static const char *opt_dir = FW_DIR, *opt_ramp, *opt_nvm;   /* NULL: picked from the version */
static const char *opt_prefix = DEF_PREFIX;
static uint32_t opt_speed = 115200;
static uint8_t opt_bdaddr[6] = { 0x02, 0x5a, 0x50, 0x48, 0x52, 0x45 };  /* MSB first as printed */
static int opt_have_addr, opt_fake, opt_verbose;
static long opt_settle = 120;   /* -w: seconds of boot age before the bring-up */
static int log_fd = -1;

void
say(const char *fmt, ...)
{
	char buf[1024];
	struct timeval tv;
	gettimeofday(&tv, NULL);
	int n = snprintf(buf, sizeof(buf), "%ld.%03d [%d] ", (long)tv.tv_sec, (int)(tv.tv_usec / 1000), getpid());
	va_list ap;
	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof(buf) - (size_t)n - 2, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(buf) - 2)
		n = (int)sizeof(buf) - 2;
	buf[n++] = '\n';
	if (log_fd < 0)
		log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (log_fd >= 0)
		(void)write(log_fd, buf, (size_t)n);
	(void)write(2, buf, (size_t)n);
}

static void
hexdump(const char *tag, const uint8_t *p, size_t n)
{
	char line[3 * 64 + 8];
	size_t i = 0;
	while (i < n) {
		size_t k = 0, j;
		for (j = 0; j < 48 && i < n; j++, i++)
			k += (size_t)snprintf(line + k, sizeof(line) - k, "%02x ", p[i]);
		say("%s %s", tag, line);
		tag = "  ";
	}
}

static void
msleep(unsigned ms)
{
	usleep(ms * 1000);
}

static int
parse_addr(const char *s, uint8_t a[6])
{
	unsigned v[6];
	if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
		return -1;
	for (int i = 0; i < 6; i++)
		a[i] = (uint8_t)v[i];
	return 0;
}

/* ---------------------------------------------------------------- full write & H4 stream framing */

/*
 * Write an entire frame across EINTR, partial write(), and transient EAGAIN/EWOULDBLOCK.
 * - While 0 bytes have been written, wait up to init_timeout_ms via poll(POLLOUT); if the
 *   FIFO/socket remains full (e.g. bluetoothd is stopped), abort cleanly with 0 bytes sent
 *   so neither the H4 stream is torn nor the single-threaded daemon is wedged.
 * - Once >= 1 byte of a frame has been written, extend the retry budget to partial_timeout_ms
 *   so outbound H4 packets are never truncated mid-frame.
 */
static int
write_all(int fd, const void *buf, size_t len, unsigned init_timeout_ms,
    unsigned partial_timeout_ms, const char *what)
{
	if (fd < 0) {
		errno = EBADF;
		return -1;
	}
	const uint8_t *p = (const uint8_t *)buf;
	size_t sent = 0;
	uint64_t stall_start = 0;

	while (sent < len) {
		ssize_t w = write(fd, p + sent, len - sent);
		if (w > 0) {
			sent += (size_t)w;
			stall_start = 0;
			continue;
		}
		if (w < 0 && errno == EINTR)
			continue;
		if (w == 0 || (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
			uint64_t now = q_now();
			if (stall_start == 0)
				stall_start = now;
			unsigned budget = sent > 0 ? partial_timeout_ms : init_timeout_ms;
			uint64_t elapsed = now - stall_start;
			if (elapsed >= budget) {
				if (what)
					say("%s write timeout (%zu/%zu bytes after %llu ms)",
					    what, sent, len, (unsigned long long)elapsed);
				errno = EAGAIN;
				return -1;
			}
			unsigned rem = (unsigned)(budget - elapsed);
			int step = rem > 20 ? 20 : (int)rem;
			struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
			int pr = poll(&pfd, 1, step);
			if (pr < 0 && errno == EINTR)
				continue;
			if (pr > 0 && (pfd.revents & POLLNVAL)) {
				/* XNU cdev without kqfilter: fall back to 1 ms retry */
				msleep(1);
			} else if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP)) &&
			    !(pfd.revents & POLLOUT)) {
				msleep(1);
			} else if (w == 0 && pr > 0) {
				msleep(1);
			}
			continue;
		}
		if (what)
			say("%s write (%zu/%zu bytes): %s", what, sent, len, strerror(errno));
		return -1;
	}
	return 0;
}

#define H4_RX_BUF_SIZE          8192
#define H4_MAX_ACL_LEN          4096
#define H4_MAX_ISO_LEN          4096

#define H4_ALLOW_CMD            (1u << 1)       /* 0x01: Host -> Controller */
#define H4_ALLOW_ACL            (1u << 2)       /* 0x02: Bidirectional */
#define H4_ALLOW_SCO            (1u << 3)       /* 0x03: Bidirectional */
#define H4_ALLOW_EVT            (1u << 4)       /* 0x04: Controller -> Host */
#define H4_ALLOW_ISO            (1u << 5)       /* 0x05: Bidirectional */

#define H4_HOST_TO_CTL_MASK     (H4_ALLOW_CMD | H4_ALLOW_ACL | H4_ALLOW_SCO | H4_ALLOW_ISO)
#define H4_CTL_TO_HOST_MASK     (H4_ALLOW_ACL | H4_ALLOW_SCO | H4_ALLOW_EVT | H4_ALLOW_ISO)

struct h4 {
	const char *name;
	uint32_t allow_mask;
	uint8_t rx_buf[H4_RX_BUF_SIZE];
	size_t rx_len;
	uint64_t desync_cnt;
};

/*
 * Extract and dispatch all complete HCI H4 packets accumulated in h->rx_buf[0 .. h->rx_len),
 * handling coalesced multi-packet reads, partial headers/payloads across wakeups, and
 * 1-byte sliding resynchronization on invalid packet indicators or oversized ACL/ISO headers.
 */
static void
h4_drain_frames(struct h4 *h, void (*fn)(const uint8_t *, size_t))
{
	const char *ep = h->name ? h->name : "h4";
	uint32_t mask = h->allow_mask ? h->allow_mask : 0x3eu;
	size_t off = 0;

	while (off < h->rx_len) {
		const uint8_t *p = h->rx_buf + off;
		size_t avail = h->rx_len - off;
		uint8_t t = p[0];

		if (t < 1 || t > 5 || !(mask & (1u << t))) {
			h->desync_cnt++;
			if (h->desync_cnt <= 16 || (h->desync_cnt % 256) == 0) {
				if (t == 0xfc || t == 0xfd || t == 0xfe)
					say("h4(%s): ignoring IBS pulse byte %#02x (%llu total)",
					    ep, t, (unsigned long long)h->desync_cnt);
				else
					say("h4(%s): out-of-sync packet type %#02x dropped (%llu total)",
					    ep, t, (unsigned long long)h->desync_cnt);
			}
			off++;
			continue;
		}

		size_t hdr_len, pl;
		switch (t) {
		case 0x01:      /* HCI Command: 1B type + 2B opcode + 1B plen -> 4 + plen (max 259) */
		case 0x03:      /* HCI SCO Data: 1B type + 2B handle + 1B dlen -> 4 + dlen (max 259) */
			hdr_len = 4;
			if (avail < hdr_len)
				goto out;
			pl = p[3];
			break;
		case 0x04:      /* HCI Event: 1B type + 1B evt + 1B plen -> 3 + plen (max 258) */
			hdr_len = 3;
			if (avail < hdr_len)
				goto out;
			pl = p[2];
			break;
		case 0x02:      /* HCI ACL Data: 1B type + 2B handle + 2B dlen LE -> 5 + dlen (dlen <= 4096) */
			hdr_len = 5;
			if (avail < hdr_len)
				goto out;
			pl = (size_t)p[3] | ((size_t)p[4] << 8);
			if (pl > H4_MAX_ACL_LEN) {
				h->desync_cnt++;
				say("h4(%s): invalid ACL dlen %zu (> %u), resyncing",
				    ep, pl, (unsigned)H4_MAX_ACL_LEN);
				off++;
				continue;
			}
			break;
		case 0x05:      /* HCI ISO Data: 1B type + 2B handle + 2B dlen LE -> 5 + (dlen & 0x3FFF) */
			hdr_len = 5;
			if (avail < hdr_len)
				goto out;
			pl = ((size_t)p[3] | ((size_t)p[4] << 8)) & 0x3fffu;
			if (pl > H4_MAX_ISO_LEN) {
				h->desync_cnt++;
				say("h4(%s): invalid ISO dlen %zu (> %u), resyncing",
				    ep, pl, (unsigned)H4_MAX_ISO_LEN);
				off++;
				continue;
			}
			break;
		default:
			off++;
			continue;
		}

		size_t frame_len = hdr_len + pl;
		if (avail < frame_len)
			goto out;

		fn(p, frame_len);
		off += frame_len;
	}

out:
	if (off > 0) {
		size_t rem = h->rx_len - off;
		if (rem > 0)
			memmove(h->rx_buf, h->rx_buf + off, rem);
		h->rx_len = rem;
	}
}

/* Feed arbitrary-length byte buffer into h->rx_buf in bounded chunks and dispatch complete frames */
static void
h4_feed(struct h4 *h, const uint8_t *p, size_t n, void (*fn)(const uint8_t *, size_t))
{
	while (n > 0) {
		size_t space = sizeof(h->rx_buf) - h->rx_len;
		if (space == 0) {
			/* Max valid H4 frame is 4101 < 8192; guard against wedge regardless */
			h->desync_cnt++;
			say("h4(%s): rx_buf full (%zu bytes), dropping %#02x to resync",
			    h->name ? h->name : "h4", h->rx_len, h->rx_buf[0]);
			memmove(h->rx_buf, h->rx_buf + 1, --h->rx_len);
			h4_drain_frames(h, fn);
			space = sizeof(h->rx_buf) - h->rx_len;
		}
		size_t chunk = n < space ? n : space;
		memcpy(h->rx_buf + h->rx_len, p, chunk);
		h->rx_len += chunk;
		p += chunk;
		n -= chunk;
		h4_drain_frames(h, fn);
	}
}

/* ---------------------------------------------------------------- the host side (bluetoothd) */

static int host_rd = -1, host_wr = -1;  /* we read <prefix>-in, write <prefix>-out */

static void
to_host(const uint8_t *p, size_t n)
{
	if (opt_verbose)
		hexdump("C>H", p, n);
	if (write_all(host_wr, p, n, 250, 2000, NULL) < 0) {
		static unsigned drop_cnt;
		drop_cnt++;
		if (drop_cnt <= 8 || (drop_cnt % 100) == 0)
			say("host write failed/timed out (%zu bytes, %u total): %s",
			    n, drop_cnt, strerror(errno));
	}
}

static void
host_cc(uint16_t op, const uint8_t *ret, size_t rlen)
{
	uint8_t e[3 + 3 + 255];
	if (rlen > 252)
		rlen = 252;
	e[0] = 4, e[1] = 0x0e, e[2] = (uint8_t)(3 + rlen);
	e[3] = 1, e[4] = op & 0xff, e[5] = op >> 8;
	memcpy(e + 6, ret, rlen);
	to_host(e, 6 + rlen);
}

static void
host_cs(uint16_t op, uint8_t status)
{
	uint8_t e[7] = { 4, 0x0f, 4, status, 1, (uint8_t)(op & 0xff), (uint8_t)(op >> 8) };
	to_host(e, sizeof(e));
}

/* ---------------------------------------------------------------- the controller (qca.c) */

static struct h4 c_h4 = {
	.name = "ctl->host",
	.allow_mask = H4_CTL_TO_HOST_MASK,
};
static int controller_up, ctl_notify[2] = { -1, -1 };

static int
c_write(const uint8_t *p, size_t n)
{
	if (opt_verbose)
		hexdump("H>C", p, n);
	return write_all(qca_fd(), p, n, 1000, 5000, "controller");
}

static int
controller_bringup(void)
{
	if (controller_up)
		return 0;
	if (qca_open(1) < 0)
		return -1;
	if (qca_bringup(opt_dir, opt_ramp, opt_nvm, opt_speed, opt_bdaddr) < 0) {
		qca_close();    /* the next try power-cycles the BT side again */
		return -1;
	}
	qca_print_identity();
	/* the SoC's own debug log arrives as ACL on handle 0x2EDC; turn it off as Linux does for WCN399x
	 * (QCA_DISABLE_LOGGING 0xFC17 [0x14 0x00]); from_controller drops what still comes */
	{
		uint8_t off[2] = { 0x14, 0x00 };
		qca_cmd(0xfc17, off, 2);
		int st = qca_wait_cc(0xfc17, 500, NULL, NULL);
		say("SoC logging off (0xFC17) -> %d", st);
	}
	if (ctl_notify[0] < 0 && pipe(ctl_notify) == 0) {
		fcntl(ctl_notify[0], F_SETFL, O_NONBLOCK);
		fcntl(ctl_notify[1], F_SETFL, O_NONBLOCK);
	}
	qca_rx_notify_fd = ctl_notify[1];
	controller_up = 1;
	return 0;
}

/* ---------------------------------------------------------------- what bluetoothd is told */

/* identity reported to bluetoothd (Broadcom, like the iPhone X's BCM4357B1); adjust after a
 * capture of a real D22's Read Local Version if the stack turns out to check it */
#define ID_HCI_VER      0x09            /* 5.0 */
#define ID_HCI_REV      0x0000
#define ID_LMP_VER      0x09
#define ID_MANUF        0x000f          /* Broadcom */
#define ID_LMP_SUBVER   0x2209

static const uint8_t fake_features[8] = { 0xff, 0xfe, 0x8f, 0xfe, 0xd8, 0x3f, 0x5b, 0x87 };
static const uint8_t fake_le_features[8] = { 0xff, 0x01, 0, 0, 0, 0, 0, 0 };

/* a reply to a command bluetoothd sent while no controller answers it (fake mode), or the
 * Broadcom vendor commands that must never reach the Qualcomm chip (both modes) */
static void
answer_locally(uint16_t op, const uint8_t *par, size_t plen)
{
	uint8_t r[256];
	size_t n = 1;
	memset(r, 0, sizeof(r));
	uint8_t ogf = (uint8_t)(op >> 10);
	switch (op) {
	case 0x1001:    /* Read Local Version */
		r[1] = ID_HCI_VER, r[2] = ID_HCI_REV & 0xff, r[3] = ID_HCI_REV >> 8, r[4] = ID_LMP_VER;
		r[5] = ID_MANUF & 0xff, r[6] = ID_MANUF >> 8, r[7] = ID_LMP_SUBVER & 0xff, r[8] = ID_LMP_SUBVER >> 8;
		n = 9;
		break;
	case 0x1002:    /* Read Local Supported Commands: everything but LE Read Buffer Size v2 (41.5) */
		memset(r + 1, 0xff, 64);
		r[1 + 41] &= (uint8_t)~0x20;
		n = 65;
		break;
	case 0x1003:
		memcpy(r + 1, fake_features, 8);
		n = 9;
		break;
	case 0x1004:    /* Read Local Extended Features (page) */
		r[1] = plen ? par[0] : 0, r[2] = 2;
		if (r[1] == 0)
			memcpy(r + 3, fake_features, 8);
		else if (r[1] == 1)
			r[3] = 0x0f;
		n = 11;
		break;
	case 0x1005:    /* Read Buffer Size: ACL 1021 x 8, SCO 64 x 12 */
		r[1] = 0xfd, r[2] = 0x03, r[3] = 64, r[4] = 8, r[5] = 0, r[6] = 12, r[7] = 0;
		n = 8;
		break;
	case 0x1009:    /* Read BD_ADDR */
		for (int i = 0; i < 6; i++)
			r[1 + i] = opt_bdaddr[5 - i];
		n = 7;
		break;
	case 0x2002:    /* LE Read Buffer Size v1 */
		r[1] = 0xfb, r[2] = 0, r[3] = 8;
		n = 4;
		break;
	case 0x2060:    /* LE Read Buffer Size v2 */
		r[1] = 0xfb, r[2] = 0, r[3] = 8;
		n = 7;
		break;
	case 0x2003:
		memcpy(r + 1, fake_le_features, 8);
		n = 9;
		break;
	case 0x0c5a:    /* Read Default Erroneous Data Reporting: [status][mode 0] */
		n = 2;
		break;
	case 0x200f:    /* LE Read Filter Accept List Size */
		r[1] = 16;
		n = 2;
		break;
	case 0x201c:    /* LE Read Supported States */
		memset(r + 1, 0xff, 8);
		n = 9;
		break;
	case 0x204a:    /* LE Read Periodic Advertiser List Size */
		r[1] = 8;
		n = 2;
		break;
	case 0x200b: case 0x2036: case 0x2041:  /* LE scan / adv params: status only */
		break;
	case 0x0c14:    /* Read Local Name */
		strcpy((char *)r + 1, "BCM4357B1 37.4MHz Hercules-E USI");
		n = 249;
		break;
	case 0xfc79:    /* BCM Read Verbose Config Version Info: chip id, target, build base, build num */
		r[1] = 0x57, r[2] = 0x07, r[3] = 0x13, r[4] = 0x00, r[5] = 0x10, r[6] = 0x01;
		n = 7;
		break;
	case 0xfce2:    /* Apple "Olympic" multiplexed VSC: the reply must echo the sub-opcode (else "timedout waiting
			 * for controller", e.g. BD_VSC_OLYMPIC_ANTENNA_PREF_B, sub-op 0x15, and a restart) */
		/* fall through */
	case 0xfce9:    /* also multiplexed; byte-stream callbacks (_BCM_ByteStreamEventParser) want more than
			 * one byte: [status][sub-op][8 zero bytes] */
		r[1] = plen ? par[0] : 0;
		n = 10;
		break;
	case 0xfd48:    /* BCM LE create multi-adv instance: [status][instance] (bcm_vsc.c asserts on the byte) */
		{
			static uint8_t inst;
			r[1] = (uint8_t)(++inst ? inst : ++inst);
			n = 2;
		}
		break;
	default:
		/* other Broadcom VSCs: status only until a parser is seen to need more (record.sh) */
		/* byte-stream VSCs (_BCM_ByteStreamEventParser, e.g. BD_VSC_FACTORY_CAL_READ_TABLE 0xFD1B) need
		 * more than one byte or the caller times out and bluetoothd restarts: [status][first param][zeros] */
		if (ogf == 0x3f) {
			r[1] = plen ? par[0] : 0;
			n = 32;
		}
		break;
	}
	/* link control commands and the LE connection-type ones answer with Command Status */
	if (ogf == 0x01 && op != 0x0402 && op != 0x040b && op != 0x040c && op != 0x040d && op != 0x040e &&
	    op != 0x041a && op != 0x0434) {
		host_cs(op, 0);
		say("fake: Command Status for %#06x (len %zu)", op, plen);
		return;
	}
	if (op == 0x200d || op == 0x2016 || op == 0x2019 || op == 0x2025 || op == 0x2026 || op == 0x2043 ||
	    op == 0x2032 || op == 0x2013) {
		host_cs(op, 0);
		say("fake: Command Status for %#06x (len %zu)", op, plen);
		return;
	}
	host_cc(op, r, n);
	if (opt_verbose || ogf == 0x3f)
		say("%s: Command Complete for %#06x (ogf %#x ocf %#x, %zu param bytes in, %zu out)",
		    opt_fake ? "fake" : "vsc", op, ogf, op & 0x3ff, plen, n);
}

/* real mode: Broadcom vendor commands with a Qualcomm meaning */
#include "le_ext.c"

/* one line per classic-BT milestone (inquiry, scan enable, connections, pairing), also without -v */
static unsigned inq_results;

static void
trace_cmd(uint16_t op, const uint8_t *par, size_t plen)
{
	switch (op) {
	case 0x0401: say("BR: inquiry started"); inq_results = 0; break;
	case 0x0402: say("BR: inquiry cancelled"); break;
	case 0x0405: say("BR: create connection"); break;
	case 0x0409: say("BR: accept connection request (role %u)", plen >= 7 ? par[6] : 0); break;
	case 0x040a: say("BR: reject connection request"); break;
	case 0x0411: say("BR: authentication requested"); break;
	case 0x042b: say("BR: IO capability reply (cap %u)", plen >= 7 ? par[6] : 0); break;
	case 0x042c: say("BR: user confirmation reply (yes)"); break;
	case 0x042d: say("BR: user confirmation negative reply"); break;
	case 0x0c1a: say("BR: write scan enable %#x (1 inquiry scan = discoverable, 2 page scan = connectable)",
	    plen ? par[0] : 0); break;
	case 0x0c13: say("BR: local name set"); break;
	case 0x0419: say("BR: remote name request"); break;
	case 0x0c45: say("BR: write inquiry mode %u", plen ? par[0] : 0); break;
	default: break;
	}
}

/*
 * one inquiry result: [num=1][addr 6][psrm][reserved 1 or 2][class 3][clock 2]([rssi])...; *cod, *rssi filled.
 * 0x02 has 2 reserved bytes and no RSSI, 0x22 / 0x2F one reserved byte and an RSSI. Only num == 1 is decoded.
 */
static int
inq_decode(const uint8_t *p, size_t n, const uint8_t **addr, uint32_t *cod, int *rssi)
{
	if (n < 4 || p[3] < 1)
		return -1;
	size_t cl = p[1] == 0x02 ? 4 + 6 + 1 + 2 : 4 + 6 + 1 + 1;   /* offset of the class */
	if (p[3] != 1 || n < cl + 3 + 2 + (p[1] == 0x02 ? 0 : 1))
		return -1;
	*addr = p + 4;
	*cod = (uint32_t)(p[cl] | (p[cl + 1] << 8) | (p[cl + 2] << 16));
	*rssi = p[1] == 0x02 ? 0 : (int8_t)p[cl + 5];
	return 0;
}

/* the Major Device Class, as iOS Settings uses it (1 computer, 2 phone, 4 audio/video, 5 peripheral, ...) */
static const char *
cod_major(uint32_t cod)
{
	static const char *m[] = { "misc", "computer", "phone", "network", "audio/video", "peripheral", "imaging",
		"wearable", "toy", "health" };
	unsigned k = (cod >> 8) & 0x1f;
	return k < sizeof(m) / sizeof(m[0]) ? m[k] : "uncategorized";
}

static void ctl_broadcast(const char *event);

#define MAX_ACL_TRACK 16
static struct {
	int used;
	uint16_t handle;
	uint8_t addr[6];
} acl_track[MAX_ACL_TRACK];

static void
acl_remember(uint16_t handle, const uint8_t *addr)
{
	int free_slot = -1;
	for (int i = 0; i < MAX_ACL_TRACK; i++) {
		if (acl_track[i].used && acl_track[i].handle == handle) {
			memcpy(acl_track[i].addr, addr, 6);
			return;
		}
		if (!acl_track[i].used && free_slot < 0)
			free_slot = i;
	}
	if (free_slot >= 0) {
		acl_track[free_slot].used = 1;
		acl_track[free_slot].handle = handle;
		memcpy(acl_track[free_slot].addr, addr, 6);
	}
}

static void
acl_forget(uint16_t handle, uint8_t out_addr[6])
{
	memset(out_addr, 0, 6);
	for (int i = 0; i < MAX_ACL_TRACK; i++) {
		if (acl_track[i].used && acl_track[i].handle == handle) {
			memcpy(out_addr, acl_track[i].addr, 6);
			acl_track[i].used = 0;
			return;
		}
	}
}

static void
broadcast_acl_connected(const uint8_t *addr)
{
	char event[64];
	snprintf(event, sizeof(event), "ACL_CONNECTED %02x:%02x:%02x:%02x:%02x:%02x\n",
		 addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
	ctl_broadcast(event);
}

static void
broadcast_acl_disconnected(const uint8_t *addr)
{
	char event[64];
	snprintf(event, sizeof(event), "ACL_DISCONNECTED %02x:%02x:%02x:%02x:%02x:%02x\n",
		 addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
	ctl_broadcast(event);
}

static void
broadcast_a2dp_connected(void)
{
	ctl_broadcast("A2DP_CONNECTED\n");
}

static void
broadcast_a2dp_disconnected(void)
{
	ctl_broadcast("A2DP_DISCONNECTED\n");
}

static void
trace_event(const uint8_t *p, size_t n)
{
	if (p[0] != 4 || n < 3)
		return;
	switch (p[1]) {
	case 0x01: say("BR: inquiry complete status %#x, %u results", n >= 4 ? p[3] : 0, inq_results); break;
	case 0x02: case 0x22: case 0x2f:
		if (inq_results++ < 20) {
			const uint8_t *a;
			uint32_t cod;
			int rssi;
			if (inq_decode(p, n, &a, &cod, &rssi) == 0)
				say("BR: inquiry result (event %#x, %u so far): ..:%02x:%02x class %06x (%s) rssi %d", p[1],
				    inq_results, a[1], a[0], cod, cod_major(cod), rssi);
			else
				say("BR: inquiry result (event %#x, %u so far)", p[1], inq_results);
		}
		break;
	case 0x07: say("BR: remote name complete status %#x", n >= 4 ? p[3] : 0xff); break;
	case 0x0f:
		if (n >= 7 && (p[5] | (p[6] << 8)) == 0x0401)
			say("BR: inquiry Command Status %#x", p[3]);
		break;
	case 0x03: { /* Connection Complete */
		uint16_t handle = n >= 6 ? (uint16_t)((p[4] | (p[5] << 8)) & 0x0fff) : 0;
		say("BR: connection complete status %#x handle %#x", n >= 4 ? p[3] : 0xff, handle);
		if (n >= 12 && p[3] == 0) { /* status=0, BD_ADDR at p[6..11] */
			acl_remember(handle, p + 6);
			broadcast_acl_connected(p + 6);
		}
		break;
	}
	case 0x04: say("BR: connection request (class %02x%02x%02x)", n >= 12 ? p[11] : 0, n >= 11 ? p[10] : 0,
	    n >= 10 ? p[9] : 0); break;
	case 0x05: { /* Disconnection Complete */
		say("BR: disconnection complete reason %#x", n >= 7 ? p[6] : 0);
		if (n >= 7 && p[3] == 0) { /* status=0, handle at p[4..5], reason at p[6] */
			uint16_t handle = (uint16_t)((p[4] | (p[5] << 8)) & 0x0fff);
			uint8_t addr[6];
			acl_forget(handle, addr);
			broadcast_acl_disconnected(addr);
		}
		break;
	}
	case 0x06: say("BR: authentication complete status %#x", n >= 4 ? p[3] : 0xff); break;
	case 0x31: say("BR: IO capability request"); break;
	case 0x32: say("BR: IO capability response (remote cap %u)", n >= 10 ? p[9] : 0); break;
	case 0x33: say("BR: user confirmation request"); break;
	case 0x36: say("BR: simple pairing complete status %#x", n >= 4 ? p[3] : 0xff); break;
	case 0x18: say("BR: link key notification"); break;
	case 0x17: say("BR: link key request"); break;
	case 0x16: say("BR: PIN code request"); break;
	/* LE Meta Event (0x3e) - LE Connection Complete (0x01) or LE Enhanced Connection Complete (0x0a) */
	case 0x3e: {
		if (n >= 15 && (p[3] == 0x01 || p[3] == 0x0a) && p[4] == 0) {
			/* subevent 0x01/0x0a: status(p[4]) handle(p[5..6]) role(p[7]) peer_addr_type(p[8]) peer_addr(p[9..14]) */
			uint16_t handle = (uint16_t)((p[5] | (p[6] << 8)) & 0x0fff);
			acl_remember(handle, p + 9);
			broadcast_acl_connected(p + 9);
		}
		break;
	}
	default: break;
	}
}

static int
translate_vsc(uint16_t op, const uint8_t *par, size_t plen)
{
	if (le_vsc(op, par, plen))
		return 0;
	switch (op) {
	case 0xfc01:    /* BCM Write_BD_ADDR (LE order) -> QCA 0xFC14 */
		if (plen >= 6) {
			for (int i = 0; i < 6; i++)
				opt_bdaddr[5 - i] = par[i];
			if (qca_cmd(0xfc14, opt_bdaddr, 6) < 0)   /* MSB first; its answer is dropped in from_controller */
				return -1;
			say("vsc: Write_BD_ADDR -> QCA 0xFC14");
		}
		answer_locally(op, par, plen);
		return 0;
	default:
		answer_locally(op, par, plen);
		return 0;
	}
}

/* the opcode of a Command Complete / Status we must not pass on (answers to our own commands) */
static int drop_vendor_events = 1;

/* "scan N" on the control socket: btshim's own passive LE scan for N seconds, straight to the WCN (a radio
 * test independent of bluetoothd): its Command Completes and the advertising reports are kept from
 * bluetoothd and counted */
static int scan_test;
static time_t scan_end;
static unsigned scan_reports, scan_devs;
static uint8_t scan_seen[64][6];

static int scan_ext;     /* legacy LE scan commands answered 0x0C (Command Disallowed): use the extended ones */

static void
scan_cmd(int on)
{
	if (scan_ext) {
		if (on) {
			/* LE Set Extended Scan Parameters: public, accept all, 1M PHY, passive, 60 ms / 60 ms */
			uint8_t sp[8] = { 0x00, 0x00, 0x01, 0x00, 0x60, 0x00, 0x60, 0x00 };
			qca_cmd(0x2041, sp, 8);
			usleep(50000);
		}
		uint8_t se[6] = { (uint8_t)on, 0x00, 0, 0, 0, 0 };
		qca_cmd(0x2042, se, 6);
		return;
	}
	if (on) {
		/* LE Set Scan Parameters: passive, interval = window = 0x60 (60 ms), public, accept all */
		uint8_t sp[7] = { 0x00, 0x60, 0x00, 0x60, 0x00, 0x00, 0x00 };
		qca_cmd(0x200b, sp, 7);
		usleep(50000);
	}
	uint8_t se[2] = { (uint8_t)on, 0x00 };
	qca_cmd(0x200c, se, 2);
}

static void
scan_seen_add(const uint8_t *addr, unsigned type, int rssi)
{
	scan_reports++;
	for (unsigned k = 0; k < scan_devs; k++)
		if (!memcmp(scan_seen[k], addr, 6))
			return;
	if (scan_devs < 64) {
		memcpy(scan_seen[scan_devs++], addr, 6);
		say("scan test: device %u, address type %u, rssi %d dBm", scan_devs, type, rssi);
	}
}

/* "inquiry N" on the control socket: btshim's own classic inquiry (GIAC) of about N s, straight to the WCN, results
 * kept from bluetoothd and logged: tells "the radio hears nothing" from "bluetoothd/Settings drop what it hears".
 * Run it with Settings > Bluetooth closed (bluetoothd must not be inquiring at the same time). */
static int inq_test;
static time_t inq_end;
static unsigned inq_test_results;

static void
from_controller(const uint8_t *p, size_t n)
{
	if (inq_test && p[0] == 4 && n >= 3) {
		if (p[1] == 0x0f && n >= 7 && (p[5] | (p[6] << 8)) == 0x0401) {
			say("inquiry test: Command Status %#x%s", p[3], p[3] ? " (refused)" : "");
			if (p[3])
				inq_test = 0;
			return;
		}
		if (p[1] == 0x02 || p[1] == 0x22 || p[1] == 0x2f) {
			const uint8_t *a;
			uint32_t cod;
			int rssi;
			inq_test_results++;
			if (inq_decode(p, n, &a, &cod, &rssi) == 0)
				say("inquiry test: answer %u (event %#x): ..:%02x:%02x class %06x (%s) rssi %d", inq_test_results,
				    p[1], a[1], a[0], cod, cod_major(cod), rssi);
			return;
		}
		if (p[1] == 0x01) {
			say("inquiry test: complete, status %#x, %u answers%s", n >= 4 ? p[3] : 0, inq_test_results,
			    inq_test_results ? "" : " (nothing discoverable in range, or the radio does not hear)");
			inq_test = 0;
			return;
		}
	}
	if (scan_test) {
		if (p[0] == 4 && p[1] == 0x0e && n >= 7) {
			uint16_t cop = (uint16_t)(p[4] | (p[5] << 8));
			if (cop == 0x200b || cop == 0x200c || cop == 0x2041 || cop == 0x2042) {
				say("scan test: %#06x -> status %#x", cop, p[6]);
				if (p[6] == 0x0c && (cop == 0x200b || cop == 0x200c) && scan_test == 1 && !scan_ext) {
					say("scan test: legacy scan disallowed, switching to extended scanning");
					scan_ext = 1;
					scan_cmd(1);
				}
				return;
			}
		}
		if (p[0] == 4 && p[1] == 0x3e && n >= 5 && p[3] == 0x0d) {      /* LE Extended Advertising Report */
			unsigned nr = p[4];
			const uint8_t *r = p + 5;
			for (unsigned i = 0; i < nr && r + 24 <= p + n; i++) {
				unsigned dl = r[23];
				if (r + 24 + dl > p + n)
					break;
				scan_seen_add(r + 3, r[2], (int8_t)r[13]);
				r += 24 + dl;
			}
			return;
		}
		if (p[0] == 4 && p[1] == 0x3e && n >= 5 && p[3] == 0x02) {      /* LE Advertising Report */
			unsigned nr = p[4];
			const uint8_t *r = p + 5;
			for (unsigned i = 0; i < nr && r + 9 <= p + n; i++) {
				unsigned dl = r[8];
				if (r + 10 + dl > p + n)
					break;
				int8_t rssi = (int8_t)r[9 + dl];
				scan_seen_add(r + 2, r[1], rssi);
				r += 10 + dl;
			}
			return;
		}
	}

	if (p[0] == 4 && p[1] == 0xff && drop_vendor_events) {
		if (opt_verbose)
			hexdump("drop VSE", p, n);
		return;
	}
	if (own_event(p, n))
		return;
	if (le_event(p, n)) {
		trace_event(p, n);
		return;
	}
	trace_event(p, n);
	if (p[0] == 2 && n >= 5) {
		unsigned raw_h = (unsigned)(p[1] | (p[2] << 8));
		if (raw_h == 0x2edc || raw_h == 0x2edd) {       /* QCA_DEBUG_HANDLE / memdump: not for bluetoothd */
			static unsigned dropped;
			if (dropped++ % 100 == 0)
				say("dropped SoC debug ACL (handle %#x), %u so far", raw_h, dropped);
			return;
		}
	}
	if (p[0] == 4 && p[1] == 0x0f && n >= 7 && p[3] != 0) {
		/* Command Status with an error: an OGF 3 (controller & baseband) or OGF 8 (LE) command the WCN
		 * does not know (those normally end with Command Complete): report success as a Command Complete */
		uint16_t op = (uint16_t)(p[5] | (p[6] << 8));
		say("WCN: Command Status %#x for %#06x", p[3], op);
		if ((op >> 10) == 0x03 || (op >> 10) == 0x08) {
			host_cc(op, (const uint8_t *)"", 1);
			return;
		}
	}
	if (p[0] == 4 && p[1] == 0x0e && n == 7 && p[6] != 0 && (p[4] | (p[5] << 8)) < 0xfc00) {
		/* a write-type command (status is its only return parameter) the WCN refuses, e.g. Write Default
		 * Erroneous Data Reporting 0x0C5B (unknown command): bluetoothd treats an error in its init chain as
		 * fatal and restarts, so report success */
		uint8_t q[7];
		memcpy(q, p, 7);
		say("WCN refused %#06x with status %#x: reported as success", q[4] | (q[5] << 8), q[6]);
		q[6] = 0;
		to_host(q, 7);
		return;
	}
	if (p[0] == 4 && p[1] == 0x0e && n >= 6) {
		uint16_t op = (uint16_t)(p[4] | (p[5] << 8));
		if (op >= 0xfc00) {             /* our 0xFC14 etc. */
			if (opt_verbose)
				hexdump("drop CC", p, n);
			return;
		}
		if (op == 0x1002 && n >= 7 + 64 && p[6] == 0) {
			/* Read Local Supported Commands: bluetoothd skips (and then fails its init chain on) commands
			 * whose bit is clear; claim Read/Write Default Erroneous Data Reporting (octet 18 bits 2, 3),
			 * which btshim answers itself */
			uint8_t q[7 + 64];
			memcpy(q, p, sizeof(q));
			q[2] = (uint8_t)(sizeof(q) - 3);
			q[7 + 18] |= 0x0c;
			to_host(q, sizeof(q));
			return;
		}
		if (op == 0x1001 && n >= 15) {  /* Read Local Version: report the Broadcom identity */
			uint8_t q[16];
			memcpy(q, p, 15);
			q[2] = 12;
			/* 04 0e 0c 01 01 10 | status hci_ver rev(2) lmp_ver manufacturer(2) subversion(2) */
			say("Read Local Version from the WCN: manufacturer %u subversion %#x -> reported as Broadcom",
			    q[11] | (q[12] << 8), q[13] | (q[14] << 8));
			q[11] = ID_MANUF & 0xff, q[12] = ID_MANUF >> 8;
			q[13] = ID_LMP_SUBVER & 0xff, q[14] = ID_LMP_SUBVER >> 8;
			to_host(q, 15);
			return;
		}
	}
	to_host(p, n);
}

static void
from_host(const uint8_t *p, size_t n)
{
	if (opt_verbose || opt_fake)
		hexdump("H>  ", p, n);
	if (p[0] == 1 && n >= 4) {
		uint16_t op = (uint16_t)(p[1] | (p[2] << 8));
		const uint8_t *par = p + 4;
		size_t plen = n - 4;
		if (opt_fake) {
			answer_locally(op, par, plen);
			return;
		}
		if ((op >> 10) == 0x3f) {
			translate_vsc(op, par, plen);
			return;
		}
		trace_cmd(op, par, plen);
		if (op == 0x0c03) {     /* HCI_Reset: bluetoothd (re)starts; its advertising instances are gone */
			memset(adv, 0, sizeof(adv));
			le_scanning = 0;
			le_random_set = 0;      /* the reset clears the controller's random address too */
		}
		if ((op >> 10) == 0x08 && le_legacy(op, par, plen))
			return;
		/* commands the WCN3988 does not implement but bluetoothd's init chain requires to succeed */
		if (op == 0x0c5b || op == 0x0c5a) {     /* Write / Read Default Erroneous Data Reporting */
			say("answering %#06x locally (not in the WCN)", op);
			answer_locally(op, par, plen);
			return;
		}
	}
	if (opt_fake || !controller_up)
		return;         /* ACL/SCO/ISO to nowhere */
	c_write(p, n);
}

/* ---------------------------------------------------------------- daemon */

static struct h4 h_h4 = {
	.name = "host->ctl",
	.allow_mask = H4_HOST_TO_CTL_MASK,
};

static void
drain_controller_rx(void)
{
	if (ctl_notify[0] >= 0) {
		uint8_t drain[256];
		for (;;) {
			ssize_t r = read(ctl_notify[0], drain, sizeof(drain));
			if (r > 0 || (r < 0 && errno == EINTR))
				continue;
			break;
		}
	}
	for (;;) {
		size_t space = sizeof(c_h4.rx_buf) - c_h4.rx_len;
		if (space == 0) {
			c_h4.desync_cnt++;
			say("h4(%s): rx_buf full (%zu bytes), dropping %#02x to resync",
			    c_h4.name, c_h4.rx_len, c_h4.rx_buf[0]);
			memmove(c_h4.rx_buf, c_h4.rx_buf + 1, --c_h4.rx_len);
			h4_drain_frames(&c_h4, from_controller);
			space = sizeof(c_h4.rx_buf) - c_h4.rx_len;
		}
		size_t n = qca_rx_take(c_h4.rx_buf + c_h4.rx_len, space);
		if (n == 0)
			break;
		c_h4.rx_len += n;
		h4_drain_frames(&c_h4, from_controller);
	}
}

static int
make_fifo(const char *path)
{
	if (mkfifo(path, 0666) < 0 && errno != EEXIST) {
		say("mkfifo %s: %s", path, strerror(errno));
		return -1;
	}
	chmod(path, 0666);
	/* O_RDWR: a FIFO opened this way never blocks and always has a reader and a writer, so
	 * bluetoothd's O_WRONLY open does not hang and its reader never sees EOF */
	int fd = open(path, O_RDWR | O_NONBLOCK);
	if (fd < 0)
		say("open %s: %s", path, strerror(errno));
	return fd;
}

static int
ctl_listen(void)
{
	unlink(CTL_SOCK);
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	strlcpy(a.sun_path, CTL_SOCK, sizeof(a.sun_path));
	if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 4) < 0) {
		say("control socket: %s", strerror(errno));
		return -1;
	}
	chmod(CTL_SOCK, 0666);
	return s;
}

/* real mode: "boot"/"init" requests that came before the controller was up wait here */
static int pending[64], npending, bringup_failed;

/* control socket clients for event broadcasting (btmonitor, etc.) */
#define MAX_CTL_CLIENTS 8
static int ctl_clients[MAX_CTL_CLIENTS];
static int nctl_clients = 0;

static void
ctl_broadcast(const char *event)
{
	size_t elen = strlen(event);
	for (int i = 0; i < nctl_clients; i++) {
		int fd = ctl_clients[i];
		if (fd >= 0) {
			if (write_all(fd, event, elen, 20, 200, NULL) < 0 &&
			    (errno == EPIPE || errno == ECONNRESET || errno == EBADF)) {
				/* client disconnected, remove from list */
				close(fd);
				ctl_clients[i] = ctl_clients[--nctl_clients];
				i--;
			}
		}
	}
}

static void
ctl_add_client(int fd)
{
	if (nctl_clients < MAX_CTL_CLIENTS) {
		ctl_clients[nctl_clients++] = fd;
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
		say("ctl: client connected (%d total)", nctl_clients);
	} else {
		close(fd);
	}
}

static void
ctl_remove_client(int fd)
{
	for (int i = 0; i < nctl_clients; i++) {
		if (ctl_clients[i] == fd) {
			close(fd);
			ctl_clients[i] = ctl_clients[--nctl_clients];
			say("ctl: client disconnected (%d total)", nctl_clients);
			return;
		}
	}
}

static void
ctl_reply(int c, int result, const char *line)
{
	say("bluetool request \"%s\" -> %d", line, result);
	char out[32];
	int k = snprintf(out, sizeof(out), "%d\n", result);
	if (k > 0)
		(void)write_all(c, out, (size_t)k, 200, 500, "ctl reply");
	close(c);
}

/* one request line from "btshim bluetool": boot | init | deepsleep | reset | cmd <text>
 * For event streaming (btmonitor), the client stays connected and we broadcast events.
 * Commands are handled as before, but the connection remains open. */
static void
ctl_serve(int s)
{
	int c = accept(s, NULL, NULL);
	if (c < 0)
		return;

	struct timeval rcv_tv = { .tv_sec = 0, .tv_usec = 250000 };
	setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));

	/* Accumulate newline-terminated request line safely across split SOCK_STREAM writes */
	char line[256];
	size_t len = 0;
	while (len < sizeof(line) - 1) {
		ssize_t n = read(c, line + len, sizeof(line) - 1 - len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0)
			break;
		len += (size_t)n;
		if (memchr(line, '\n', len) != NULL)
			break;
	}
	if (len == 0) {
		close(c);
		return;
	}
	struct timeval zero_tv = { 0, 0 };
	setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &zero_tv, sizeof(zero_tv));
	line[len] = 0;
	line[strcspn(line, "\r\n")] = 0;

	/* Streaming clients send "stream" as first command and stay connected */
	if (strcmp(line, "stream") == 0) {
		/* Send initial confirmation before switching fd to non-blocking */
		if (write_all(c, "0\n", 2, 200, 500, "ctl stream ack") < 0) {
			close(c);
			return;
		}
		ctl_add_client(c);
		return;
	}

	/* Regular command clients (bluetool) - handle command and close */
	if (!opt_fake && !controller_up && (!strcmp(line, "boot") || !strcmp(line, "init"))) {
		if (bringup_failed) {
			ctl_reply(c, 1, line);
		} else if (npending < (int)(sizeof(pending) / sizeof(pending[0]))) {
			say("bluetool request \"%s\": waits for the bring-up", line);
			pending[npending++] = c;
		} else {
			ctl_reply(c, 1, line);
		}
		return;
	}
	if (!strncmp(line, "scan", 4)) {
		int secs = atoi(line + 4);
		if (!controller_up || opt_fake) {
			ctl_reply(c, 1, line);
			return;
		}
		scan_test = 1;
		scan_reports = scan_devs = 0;
		scan_end = time(NULL) + (secs > 0 ? secs : 10);
		scan_cmd(1);
		say("scan test: started for %d s", secs > 0 ? secs : 10);
		ctl_reply(c, 0, line);
		return;
	}
	if (!strncmp(line, "inquiry", 7)) {
		int secs = atoi(line + 7);
		if (!controller_up || opt_fake || inq_test) {
			ctl_reply(c, 1, line);
			return;
		}
		if (secs <= 0)
			secs = 10;
		if (secs > 60)
			secs = 60;
		/* HCI_Inquiry: GIAC 0x9E8B33, length in 1.28 s units, unlimited responses */
		uint8_t q[5] = { 0x33, 0x8b, 0x9e, (uint8_t)((secs * 100 + 127) / 128), 0x00 };
		inq_test = 1;
		inq_test_results = 0;
		inq_end = time(NULL) + secs + 5;
		qca_cmd(0x0401, q, 5);
		say("inquiry test: started for %d x 1.28 s", q[3]);
		ctl_reply(c, 0, line);
		return;
	}
	/* deepsleep, reset and RunCommand text: nothing to do yet (the WCN stays up) */
	ctl_reply(c, 0, line);
}

static long
boot_age(void)
{
	struct timeval bt;
	size_t sz = sizeof(bt);
	int mib[2] = { CTL_KERN, KERN_BOOTTIME };
	if (sysctl(mib, 2, &bt, &sz, NULL, 0) < 0)
		return 1 << 20;
	return (long)(time(NULL) - bt.tv_sec);
}

/* bluetoothd's start gate: /dev/uart.bluetooth (or cu.bluetooth) must exist; it is only access()ed.
 * The kernel should create it; if it did not, try a node of our own (devfs: gone at reboot). */
static void
uart_node(void)
{
	if (access("/dev/uart.bluetooth", F_OK) == 0 || access("/dev/cu.bluetooth", F_OK) == 0)
		return;
	if (mknod("/dev/uart.bluetooth", S_IFCHR | 0666, makedev(3, 2)) == 0)
		say("made /dev/uart.bluetooth (char 3,2) for bluetoothd's start check");
	else if (symlink("/dev/null", "/dev/uart.bluetooth") == 0)
		say("made /dev/uart.bluetooth -> /dev/null for bluetoothd's start check");
	else
		say("cannot make /dev/uart.bluetooth (%s): bluetoothd will say \"No bluetooth on this device\"",
		    strerror(errno));
}

static int
daemon_main(void)
{
	char in[256], out[256];
	mkdir(RUN_DIR, 0755);
	chmod(RUN_DIR, 0755);
	snprintf(in, sizeof(in), "%s-in", opt_prefix);
	snprintf(out, sizeof(out), "%s-out", opt_prefix);
	host_rd = make_fifo(in);
	host_wr = make_fifo(out);
	int ls = ctl_listen();
	if (host_rd < 0 || host_wr < 0 || ls < 0)
		return 1;
	uart_node();
	if (log_fd >= 0)
		fchmod(log_fd, 0666);   /* "btshim bluetool" runs as mobile and logs here too */
	say("btshim daemon: %s mode, host FIFOs %s / %s, speed %u", opt_fake ? "FAKE" : "real", in, out, opt_speed);
	signal(SIGPIPE, SIG_IGN);
	/* real mode: the bring-up waits until the boot is opt_settle s old (like the other daemons);
	 * what bluetoothd writes before that is held, not answered */
	static uint8_t held[65536];
	size_t nheld = 0;
	int held_overflow = 0;
	int tries = 0;
	long age = boot_age();
	time_t next_try = time(NULL) + (age >= 0 && age < opt_settle ? opt_settle - age : 0);
	if (!opt_fake)
		say("bring-up in %ld s", (long)(next_try - time(NULL)));
	for (;;) {
		if (!opt_fake && !controller_up && !bringup_failed && time(NULL) >= next_try) {
			if (controller_bringup() == 0) {
				drain_controller_rx();
				say("controller up: %d bluetool request(s) answered, %zu held bytes passed on", npending, nheld);
				for (int i = 0; i < npending; i++)
					ctl_reply(pending[i], 0, "boot/init");
				npending = 0;
				h4_feed(&h_h4, held, nheld, from_host);
				nheld = 0;
				held_overflow = 0;
			} else {
				/* never answer 1: bluetoothd asks BlueTool again at once and spins (seen: a request every
				 * 12 ms); pending requests keep waiting and the bring-up is retried */
				++tries;
				int wait = tries < 5 ? 30 : 120;
				say("bring-up failed (try %d): again in %d s", tries, wait);
				next_try = time(NULL) + wait;
			}
		}
		fd_set r;
		FD_ZERO(&r);
		FD_SET(host_rd, &r);
		FD_SET(ls, &r);
		int mx = host_rd > ls ? host_rd : ls;
		if (controller_up && ctl_notify[0] >= 0) {
			FD_SET(ctl_notify[0], &r);
			if (ctl_notify[0] > mx)
				mx = ctl_notify[0];
		}
		/* Add control socket clients to select set */
		for (int i = 0; i < nctl_clients; i++) {
			if (ctl_clients[i] >= 0) {
				FD_SET(ctl_clients[i], &r);
				if (ctl_clients[i] > mx)
					mx = ctl_clients[i];
			}
		}
		if (scan_test == 1 && time(NULL) >= scan_end) {
			scan_cmd(0);
			scan_test = 2;          /* still swallow the disable's Command Complete */
			scan_end = time(NULL) + 2;
			say("scan test: done, %u advertising reports from %u devices", scan_reports, scan_devs);
		} else if (scan_test == 2 && time(NULL) >= scan_end) {
			scan_test = 0;
		}
		if (inq_test && time(NULL) >= inq_end) {
			say("inquiry test: no Inquiry Complete in time (%u answers)", inq_test_results);
			inq_test = 0;
		}
		struct timeval tv = { 1, 0 };
		int k = select(mx + 1, &r, NULL, NULL, opt_fake ? NULL : &tv);
		if (controller_up && !opt_fake)
			own_pump();     /* time out a lost answer to one of our own commands */
		if (k < 0) {
			if (errno == EINTR)
				continue;
			say("select: %s", strerror(errno));
			return 1;
		}
		if (FD_ISSET(ls, &r))
			ctl_serve(ls);
		if (FD_ISSET(host_rd, &r)) {
			if (!opt_fake && !controller_up) {
				uint8_t tmp[4096];
				for (;;) {
					ssize_t n = read(host_rd, tmp, sizeof(tmp));
					if (n < 0) {
						if (errno == EINTR)
							continue;
						if (errno != EAGAIN && errno != EWOULDBLOCK)
							say("host read: %s", strerror(errno));
						break;
					}
					if (n == 0)
						break;
					if (!held_overflow && nheld + (size_t)n <= sizeof(held)) {
						memcpy(held + nheld, tmp, (size_t)n);
						nheld += (size_t)n;
					} else if (!held_overflow) {
						held_overflow = 1;
						say("host pre-bringup buffer full (%zu bytes), dropping excess until bring-up",
						    nheld);
					}
				}
			} else {
				for (;;) {
					size_t space = sizeof(h_h4.rx_buf) - h_h4.rx_len;
					if (space == 0) {
						h_h4.desync_cnt++;
						say("h4(%s): rx_buf full (%zu bytes), dropping %#02x to resync",
						    h_h4.name, h_h4.rx_len, h_h4.rx_buf[0]);
						memmove(h_h4.rx_buf, h_h4.rx_buf + 1, --h_h4.rx_len);
						h4_drain_frames(&h_h4, from_host);
						space = sizeof(h_h4.rx_buf) - h_h4.rx_len;
					}
					ssize_t n = read(host_rd, h_h4.rx_buf + h_h4.rx_len, space);
					if (n < 0) {
						if (errno == EINTR)
							continue;
						if (errno != EAGAIN && errno != EWOULDBLOCK)
							say("host read: %s", strerror(errno));
						break;
					}
					if (n == 0)
						break;
					h_h4.rx_len += (size_t)n;
					h4_drain_frames(&h_h4, from_host);
				}
			}
		}
		if (controller_up && !opt_fake &&
		    (k == 0 || (ctl_notify[0] >= 0 && FD_ISSET(ctl_notify[0], &r)))) {
			drain_controller_rx();
		}
		/* Check control socket clients for disconnections */
		for (int i = 0; i < nctl_clients; i++) {
			int fd = ctl_clients[i];
			if (fd >= 0 && FD_ISSET(fd, &r)) {
				char tmp[64];
				ssize_t n = read(fd, tmp, sizeof(tmp));
				if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
					/* Client disconnected */
					ctl_remove_client(fd);
					i--; /* re-check this index */
				}
			}
		}
	}
}

/* ---------------------------------------------------------------- com.apple.BlueTool */

static int
ask_daemon(const char *req)
{
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a;
	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	strlcpy(a.sun_path, CTL_SOCK, sizeof(a.sun_path));
	if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
		say("bluetool: daemon not reachable: %s", strerror(errno));
		if (s >= 0)
			close(s);
		return 1;
	}
	char msg[256];
	int mlen = snprintf(msg, sizeof(msg), "%s\n", req);
	if (mlen <= 0 || (size_t)mlen >= sizeof(msg) ||
	    write_all(s, msg, (size_t)mlen, 1000, 2000, "bluetool req") < 0) {
		close(s);
		return 1;
	}
	char b[32] = { 0 };
	struct timeval tv = { 300, 0 };  /* the daemon waits for the boot to settle, then ~25 s of download */
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	ssize_t n;
	do {
		n = read(s, b, sizeof(b) - 1);
	} while (n < 0 && errno == EINTR);
	close(s);
	return n > 0 ? atoi(b) : 1;
}

static void
bluetool_message(xpc_connection_t peer, xpc_object_t msg)
{
	const char *id = xpc_dictionary_get_string(msg, "kMsgId");
	xpc_object_t args = xpc_dictionary_get_value(msg, "kMsgArgs");
	xpc_object_t reply = xpc_dictionary_create_reply(msg);
	if (!reply)
		return;
	if (id && !strcmp(id, "RunBuiltinScript")) {
		const char *script = args ? xpc_dictionary_get_string(args, "script") : NULL;
		const char *addr = args ? xpc_dictionary_get_string(args, "btaddress") : NULL;
		say("bluetool: RunBuiltinScript \"%s\" btaddress %s", script ? script : "?", addr ? addr : "-");
		int r = script ? ask_daemon(script) : 1;
		xpc_dictionary_set_int64(reply, "result", r);
		xpc_dictionary_set_int64(reply, "manufacturer", 0);
		xpc_dictionary_set_string(reply, "firmware", "QCA WCN3988 apbtfw (btshim)");
	} else if (id && !strcmp(id, "RunCommand")) {
		const char *cmd = args ? xpc_dictionary_get_string(args, "command") : NULL;
		say("bluetool: RunCommand \"%s\"", cmd ? cmd : "?");
		xpc_dictionary_set_int64(reply, "result", 0);
		xpc_dictionary_set_string(reply, "output", "");
	} else {
		say("bluetool: message %s ignored", id ? id : "(no kMsgId)");
		xpc_dictionary_set_int64(reply, "result", 0);
	}
	xpc_connection_send_message(peer, reply);
	xpc_release(reply);
}

static int
bluetool_main(void)
{
	xpc_connection_t l = xpc_connection_create_mach_service("com.apple.BlueTool", NULL,
	    XPC_CONNECTION_MACH_SERVICE_LISTENER);
	xpc_connection_set_event_handler(l, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != XPC_TYPE_CONNECTION)
			return;
		xpc_connection_set_event_handler(peer, ^(xpc_object_t m) {
			if (xpc_get_type(m) == XPC_TYPE_DICTIONARY)
				bluetool_message(peer, m);
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(l);
	say("bluetool: listening as com.apple.BlueTool");
	dispatch_main();
}

/* ---------------------------------------------------------------- main */

static void
usage(void)
{
	fprintf(stderr, "usage: btshim daemon [-F] [-d dir] [-r tlv] [-n nvm] [-a addr] [-s speed] "
	    "[-p fifo-prefix] [-w settle-s] [-v] [-I] | bluetool\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	if (argc < 2)
		usage();
	const char *mode = argv[1];
	const char *env = getenv("BT_DEVICE_ADDRESS");
	if (env && parse_addr(env, opt_bdaddr) == 0)
		opt_have_addr = 1;
	int c;
	optind = 2;
	while ((c = getopt(argc, argv, "Fd:r:n:a:s:p:vIw:")) != -1) {
		switch (c) {
		case 'F': opt_fake = 1; break;
		case 'd': opt_dir = optarg; break;
		case 'w': opt_settle = strtol(optarg, NULL, 0); break;
		case 'r': opt_ramp = optarg; break;
		case 'n': opt_nvm = optarg; break;
		case 'a':
			if (parse_addr(optarg, opt_bdaddr) < 0)
				usage();
			opt_have_addr = 1;
			break;
		case 's': opt_speed = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'p': opt_prefix = optarg; break;
		case 'v': opt_verbose = qca_verbose = 1; break;
		case 'I': qca_keep_ibs = 1; break;
		default: usage();
		}
	}
	if (!strcmp(mode, "bdstart")) {
		/* bluetoothd's launchd program: launchd drops DYLD_* for system daemons, so set it here and exec
		 * the real bluetoothd (same pid: the job's Mach services stay). btbridge.dylib answers bluetoothd's
		 * local-mac-address lookup (its BT_DEVICE_ADDRESS is zeroed in release builds). */
		setenv("DYLD_INSERT_LIBRARIES", "/redwood/lib/btbridge.dylib", 1);
		char *bargv[] = { "/usr/sbin/bluetoothd", NULL };
		execv(bargv[0], bargv);
		perror("btshim bdstart: exec bluetoothd");
		return 1;
	}
	if (!strcmp(mode, "daemon"))
		return daemon_main();
	if (!strcmp(mode, "scan")) {     /* btshim scan [seconds]: the daemon's LE scan test, result in its log */
		char req[32];
		snprintf(req, sizeof(req), "scan %s", argc > 2 ? argv[2] : "10");
		int r = ask_daemon(req);
		printf("scan test %s (see /var/tmp/btshim.log)\n", r == 0 ? "started" : "refused");
		return r;
	}
	if (!strcmp(mode, "inquiry")) {  /* btshim inquiry [seconds]: the daemon's classic inquiry test */
		char req[32];
		snprintf(req, sizeof(req), "inquiry %s", argc > 2 ? argv[2] : "10");
		int r = ask_daemon(req);
		printf("inquiry test %s (see /var/tmp/btshim.log)\n", r == 0 ? "started" : "refused");
		return r;
	}
	if (!strcmp(mode, "bluetool"))
		return bluetool_main();
	usage();
	return 2;
}
