/*
 * zotac-rgb - control the RGB lighting of Zotac GeForce RTX 50 series cards on Linux.
 *
 * Protocol recovered by disassembling ZOTAC FireStorm 5.0.0.017E (FireStorm.exe),
 * which drives the card through NvAPI_I2CRead/NvAPI_I2CWrite, port 1, 100 kHz.
 * The lighting controller is the I2C device at 7-bit address 0x4B (FireStorm's
 * 8-bit "0x96"). Registers (all single-byte writes of {reg, value}):
 *
 *   0x10        global enable (1 = on, 0 = off)
 *   0x20        zone select          (card has zones 0 and 1)
 *   0x21        LED index select     (card has indices 0..2 per zone)
 *   0x22        effect mode
 *   0x23-0x25   colour 1  R,G,B
 *   0x26-0x28   colour 2  R,G,B
 *   0x29        brightness (0-100)
 *   0x2a        speed
 *   0x2b-0x2f   additional effect parameters (preserved read-modify-write)
 *   0x17 <- 1   apply / commit the selected zone+index
 *
 * FireStorm sleeps ~3 ms between transactions; so do we.
 *
 * Build:  cc -O2 -Wall -o zotac-rgb zotac-rgb.c
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define LED_ADDR     0x4B
#define REG_ENABLE   0x10
#define REG_COMMIT   0x17
#define REG_ZONE     0x20
#define REG_INDEX    0x21
#define REG_MODE     0x22
#define REG_C1       0x23
#define REG_C2       0x26
#define REG_BRIGHT   0x29
#define REG_SPEED    0x2a
#define REG_FIRST    0x22
#define REG_LAST     0x2f
#define NZONES       2
#define NINDEX       3
#define DELAY_US     3000

static int fd = -1;
static int verbose;

static int rd(uint8_t reg, uint8_t *val)
{
	struct i2c_msg msgs[2] = {
		{ .addr = LED_ADDR, .flags = 0,        .len = 1, .buf = &reg },
		{ .addr = LED_ADDR, .flags = I2C_M_RD, .len = 1, .buf = val  },
	};
	struct i2c_rdwr_ioctl_data d = { .msgs = msgs, .nmsgs = 2 };
	int r = ioctl(fd, I2C_RDWR, &d);
	usleep(DELAY_US);
	return r < 0 ? -1 : 0;
}

static int wr(uint8_t reg, uint8_t val)
{
	uint8_t b[2] = { reg, val };
	if (verbose)
		fprintf(stderr, "  write reg 0x%02x = 0x%02x\n", reg, val);
	int r = write(fd, b, 2);
	usleep(DELAY_US);
	return r == 2 ? 0 : -1;
}

/* ---- bus discovery ---------------------------------------------------- */

static int probe_bus(int bus)
{
	char path[64];
	snprintf(path, sizeof path, "/dev/i2c-%d", bus);
	int f = open(path, O_RDWR);
	if (f < 0)
		return -1;
	if (ioctl(f, I2C_SLAVE, LED_ADDR) < 0) {
		close(f);
		return -1;
	}
	return f;
}

static int find_bus(void)
{
	DIR *d = opendir("/sys/bus/i2c/devices");
	if (!d)
		return -1;
	struct dirent *e;
	int found = -1;
	while ((e = readdir(d))) {
		int bus;
		if (sscanf(e->d_name, "i2c-%d", &bus) != 1)
			continue;
		char p[300], name[128] = "";
		snprintf(p, sizeof p, "/sys/bus/i2c/devices/%s/name", e->d_name);
		FILE *f = fopen(p, "r");
		if (!f)
			continue;
		if (!fgets(name, sizeof name, f))
			name[0] = 0;
		fclose(f);
		if (!strstr(name, "NVIDIA i2c adapter"))
			continue;
		int t = probe_bus(bus);
		if (t < 0)
			continue;
		uint8_t v;
		int old = fd;
		fd = t;
		int ok = rd(REG_ENABLE, &v) == 0;
		fd = old;
		close(t);
		if (ok) {
			found = bus;
			break;
		}
	}
	closedir(d);
	return found;
}

/* ---- effect names (numeric ids are authoritative; names are best guess) */

static int parse_color(const char *s, uint8_t rgb[3])
{
	static const struct { const char *n; uint32_t c; } tbl[] = {
		{ "red", 0xff0000 },    { "green", 0x00ff00 }, { "blue", 0x0000ff },
		{ "white", 0xffffff },  { "yellow", 0xffff00 }, { "cyan", 0x00ffff },
		{ "magenta", 0xff00ff }, { "purple", 0x8000ff }, { "orange", 0xff6000 },
		{ "pink", 0xff40a0 },   { "black", 0x000000 },
	};
	for (size_t i = 0; i < sizeof tbl / sizeof *tbl; i++)
		if (!strcasecmp(s, tbl[i].n)) {
			rgb[0] = tbl[i].c >> 16; rgb[1] = tbl[i].c >> 8; rgb[2] = tbl[i].c;
			return 0;
		}
	if (*s == '#')
		s++;
	if (strlen(s) != 6)
		return -1;
	for (int i = 0; i < 6; i++)
		if (!isxdigit((unsigned char)s[i]))
			return -1;
	uint32_t v = strtoul(s, NULL, 16);
	rgb[0] = v >> 16; rgb[1] = v >> 8; rgb[2] = v;
	return 0;
}

/* ---- commands ---------------------------------------------------------- */

static int select_led(int zone, int idx)
{
	if (wr(REG_ZONE, zone) || wr(REG_INDEX, idx))
		return -1;
	return 0;
}

static int read_block(uint8_t blk[REG_LAST + 1])
{
	for (int r = REG_FIRST; r <= REG_LAST; r++)
		if (rd(r, &blk[r]))
			return -1;
	return 0;
}

static void cmd_info(void)
{
	uint8_t en = 0;
	rd(REG_ENABLE, &en);
	printf("controller 0x%02x, enable=%u\n", LED_ADDR, en);
	for (int z = 0; z < NZONES; z++)
		for (int i = 0; i < NINDEX; i++) {
			uint8_t b[REG_LAST + 1] = { 0 };
			if (select_led(z, i) || read_block(b)) {
				fprintf(stderr, "read failed (zone %d idx %d)\n", z, i);
				return;
			}
			printf("zone %d idx %d: mode=%u c1=%02x%02x%02x c2=%02x%02x%02x "
			       "bright=%u speed=%u  raw[2b..2f]=%02x %02x %02x %02x %02x\n",
			       z, i, b[REG_MODE], b[0x23], b[0x24], b[0x25], b[0x26], b[0x27],
			       b[0x28], b[REG_BRIGHT], b[REG_SPEED], b[0x2b], b[0x2c], b[0x2d],
			       b[0x2e], b[0x2f]);
		}
}

struct change {
	int zone;			/* -1 = all */
	int mode, bright, speed;	/* -1 = unchanged */
	int has_c1, has_c2;
	uint8_t c1[3], c2[3];
};

static int apply(const struct change *c)
{
	for (int z = 0; z < NZONES; z++) {
		if (c->zone >= 0 && c->zone != z)
			continue;
		for (int i = 0; i < NINDEX; i++) {
			uint8_t b[REG_LAST + 1] = { 0 };
			if (select_led(z, i) || read_block(b))
				return -1;
			if (c->mode >= 0)   b[REG_MODE] = c->mode;
			if (c->bright >= 0) b[REG_BRIGHT] = c->bright;
			if (c->speed >= 0)  b[REG_SPEED] = c->speed;
			if (c->has_c1) memcpy(&b[REG_C1], c->c1, 3);
			if (c->has_c2) memcpy(&b[REG_C2], c->c2, 3);
			/* FireStorm re-selects before writing, then writes the block. */
			if (select_led(z, i))
				return -1;
			for (int r = REG_FIRST; r <= REG_LAST; r++)
				if (wr(r, b[r]))
					return -1;
			if (wr(REG_COMMIT, 1))
				return -1;
		}
	}
	return 0;
}

static void usage(const char *p)
{
	fprintf(stderr,
"usage: %s [-b BUS] [-v] COMMAND\n"
"\n"
"commands:\n"
"  info                         show current state of every zone/LED\n"
"  on | off                     global lighting enable\n"
"  set [options]                change lighting (unspecified fields are kept)\n"
"      -z ZONE        0 or 1 (default: both)\n"
"      -c COLOR       primary colour: RRGGBB hex or a name (red, blue, ...)\n"
"      -C COLOR       secondary colour (used by two-colour effects)\n"
"      -m MODE        effect mode number (0-11, 23, 24, 31-34 are used by FireStorm)\n"
"      -B PERCENT     brightness 0-100\n"
"      -s SPEED       effect speed\n"
"  raw REG VAL          write a single register (hex ok), no commit\n"
"  commit               write the apply register\n"
"\n"
"Needs access to /dev/i2c-N of the NVIDIA GPU (the bus is auto-detected).\n",
		p);
}

int main(int argc, char **argv)
{
	int bus = -1, opt;
	while ((opt = getopt(argc, argv, "+b:vh")) != -1) {
		switch (opt) {
		case 'b': bus = atoi(optarg); break;
		case 'v': verbose = 1; break;
		default:  usage(argv[0]); return opt == 'h' ? 0 : 2;
		}
	}
	if (optind >= argc) {
		usage(argv[0]);
		return 2;
	}
	const char *cmd = argv[optind++];

	if (bus < 0)
		bus = find_bus();
	if (bus < 0) {
		fprintf(stderr, "no Zotac lighting controller found (is the nvidia driver loaded, "
				"and do you have access to /dev/i2c-*?)\n");
		return 1;
	}
	fd = probe_bus(bus);
	if (fd < 0) {
		fprintf(stderr, "cannot open /dev/i2c-%d: %s\n", bus, strerror(errno));
		return 1;
	}

	int rc = 0;
	if (!strcmp(cmd, "info")) {
		printf("bus i2c-%d\n", bus);
		cmd_info();
	} else if (!strcmp(cmd, "on") || !strcmp(cmd, "off")) {
		rc = wr(REG_ENABLE, !strcmp(cmd, "on"));
	} else if (!strcmp(cmd, "commit")) {
		rc = wr(REG_COMMIT, 1);
	} else if (!strcmp(cmd, "raw")) {
		if (optind + 2 != argc) { usage(argv[0]); return 2; }
		rc = wr(strtoul(argv[optind], NULL, 0), strtoul(argv[optind + 1], NULL, 0));
	} else if (!strcmp(cmd, "set")) {
		struct change c = { .zone = -1, .mode = -1, .bright = -1, .speed = -1 };
		optind--;
		argv[optind] = argv[0];
		argc -= optind;
		argv += optind;
		optind = 1;
		while ((opt = getopt(argc, argv, "z:c:C:m:B:s:")) != -1) {
			switch (opt) {
			case 'z': c.zone = atoi(optarg); break;
			case 'c':
				if (parse_color(optarg, c.c1)) { fprintf(stderr, "bad colour '%s'\n", optarg); return 2; }
				c.has_c1 = 1; break;
			case 'C':
				if (parse_color(optarg, c.c2)) { fprintf(stderr, "bad colour '%s'\n", optarg); return 2; }
				c.has_c2 = 1; break;
			case 'm': c.mode = strtol(optarg, NULL, 0); break;
			case 'B': c.bright = atoi(optarg); break;
			case 's': c.speed = atoi(optarg); break;
			default:  usage(argv[0]); return 2;
			}
		}
		if (c.zone >= NZONES || c.bright > 100 || c.mode > 255 || c.speed > 255) {
			fprintf(stderr, "value out of range\n");
			return 2;
		}
		rc = apply(&c);
	} else {
		usage(argv[0]);
		return 2;
	}
	if (rc)
		fprintf(stderr, "i2c transaction failed: %s\n", strerror(errno));
	close(fd);
	return rc ? 1 : 0;
}
