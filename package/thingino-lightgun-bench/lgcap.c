/*
 * lgcap - native NV12 capture / frame-rate / latency benchmark for the
 * lightgun camera experiments (ISP + FrameSource only, no encoder, no RTSP).
 *
 *   lgcap [options]
 *
 *   -n N        frames to measure after warm-up (default 600)
 *   -W N        warm-up frames discarded before measuring (default 60)
 *   -f FPS      sensor/channel frame rate to request (default /proc/jz/sensor/max_fps)
 *   -o WxH      channel 0 output size; differs from the sensor size -> ISP scaler
 *   -c WxH      also create channel 1 (scaler output) at WxH
 *   -r CH       channel to read in the timing loop (default 0, or 1 when -c given)
 *   -b N        nrVBs per channel (default 2)
 *   -d N        frame depth for GetFrame (default 1)
 *   -w MODE     workload: 0 none, 1 threshold+CCL on full Y, 2 on Y decimated x2 (default 0)
 *   -t LEVEL    workload luma threshold (default 200)
 *   -e LINES    manual exposure in sensor lines (default: auto exposure)
 *   -s PREFIX   after warm-up save one frame as PREFIX.pgm (Y) and PREFIX.nv12
 *   -C FILE     per-frame CSV: seq,ts_us,get_us,lat_us,wait_us,proc_us,blobs,hash
 *   -L GPIO     LED latency test: toggle GPIO (e.g. 50 = IR LEDs) and time the luma step
 *
 * Timing sources:
 *   ts   IMPFrameInfo.timeStamp, stamped by libimp/tx-isp when the ISP finished the frame
 *   now  IMP_System_GetTimeStamp() right after GetFrame returns (same clock base as ts)
 * so "lat" is ISP-done -> application, not photon -> application.  -L measures the
 * full LED-on -> visible-in-frame latency, which includes exposure and readout.
 *
 * Never write frames to /tmp on the camera (RAM); use the SD card or NFS.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <imp/imp_common.h>
#include <imp/imp_framesource.h>
#include <imp/imp_isp.h>
#include <imp/imp_system.h>

#define CK(e, m)                                                       \
	do {                                                           \
		int r_ = (e);                                          \
		if (r_ < 0) {                                          \
			fprintf(stderr, "[E] %s -> %d\n", (m), r_);   \
			exit(1);                                       \
		}                                                      \
	} while (0)

struct opts {
	int n, warmup, fps, ow, oh, c1w, c1h, rd, vbs, depth, work, thr, expo, led;
	const char *save, *csv;
};

struct sample {
	int64_t ts, get, wait, proc;
	uint32_t hash;
	int blobs;
};

/* ---------- small helpers ---------- */

static int read_proc_int(const char *path, int def)
{
	FILE *f = fopen(path, "r");
	int v = def;

	if (f) {
		if (fscanf(f, "%i", &v) != 1)
			v = def;
		fclose(f);
	}
	return v;
}

static void read_proc_str(const char *path, char *buf, size_t len, const char *def)
{
	FILE *f = fopen(path, "r");

	snprintf(buf, len, "%s", def);
	if (f) {
		if (fgets(buf, len, f))
			buf[strcspn(buf, "\r\n ")] = 0;
		fclose(f);
	}
}

static int64_t mono_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static int parse_wh(const char *s, int *w, int *h)
{
	return sscanf(s, "%dx%d", w, h) == 2 && *w > 0 && *h > 0;
}

/* total and idle jiffies from /proc/stat */
static void cpu_jiffies(unsigned long long *total, unsigned long long *idle)
{
	unsigned long long v[10] = {0};
	FILE *f = fopen("/proc/stat", "r");
	int i;

	*total = *idle = 0;
	if (!f)
		return;
	if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4],
		   &v[5], &v[6], &v[7]) >= 4) {
		for (i = 0; i < 8; i++)
			*total += v[i];
		*idle = v[3] + v[4];
	}
	fclose(f);
}

/* utime+stime of this process, in jiffies */
static unsigned long long self_jiffies(void)
{
	char buf[1024], *p;
	unsigned long long ut = 0, st = 0;
	FILE *f = fopen("/proc/self/stat", "r");

	if (!f)
		return 0;
	if (fgets(buf, sizeof(buf), f) && (p = strrchr(buf, ')'))) {
		/* fields after ')' start at #3 (state); utime is #14, stime #15 */
		sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st);
	}
	fclose(f);
	return ut + st;
}

static long status_kb(const char *file, const char *key)
{
	char line[256];
	long v = -1;
	size_t k = strlen(key);
	FILE *f = fopen(file, "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, key, k)) {
			v = atol(line + k);
			break;
		}
	fclose(f);
	return v;
}

static int cmp64(const void *a, const void *b)
{
	int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;

	return x < y ? -1 : x > y;
}

static int64_t pct(int64_t *sorted, int n, int p)
{
	if (n <= 0)
		return 0;
	return sorted[(int)((int64_t)(n - 1) * p / 100)];
}

static void stats(const char *name, const char *unit, int64_t *v, int n)
{
	int64_t sum = 0;
	int i;

	if (n <= 0) {
		printf("%-14s n/a\n", name);
		return;
	}
	for (i = 0; i < n; i++)
		sum += v[i];
	qsort(v, n, sizeof(*v), cmp64);
	printf("%-14s mean %8.1f  min %7lld  p50 %7lld  p95 %7lld  p99 %7lld  max %7lld %s\n", name,
	       (double)sum / n, (long long)v[0], (long long)pct(v, n, 50), (long long)pct(v, n, 95),
	       (long long)pct(v, n, 99), (long long)v[n - 1], unit);
}

/* cheap content hash over a sparse grid of the luma plane */
static uint32_t luma_hash(const uint8_t *y, int w, int h)
{
	uint32_t hsh = 2166136261u;
	int r, c;

	for (r = 3; r < h; r += 17)
		for (c = 5; c < w; c += 29)
			hsh = (hsh ^ y[r * w + c]) * 16777619u;
	return hsh;
}

static int mean_luma_center(const uint8_t *y, int w, int h)
{
	long sum = 0, n = 0;
	int r, c;

	for (r = h / 4; r < 3 * h / 4; r += 4)
		for (c = w / 4; c < 3 * w / 4; c += 4) {
			sum += y[r * w + c];
			n++;
		}
	return n ? (int)(sum / n) : 0;
}

/* ---------- synthetic tracking workload: threshold + run-based CCL ---------- */

#define MAX_RUNS 65536
#define MAX_ROW_RUNS 2048

struct run {
	int16_t row, x0, x1;
	int parent;
};

static struct run runs[MAX_RUNS];

static int uf_find(int i)
{
	while (runs[i].parent != i) {
		runs[i].parent = runs[runs[i].parent].parent;
		i = runs[i].parent;
	}
	return i;
}

static void uf_union(int a, int b)
{
	a = uf_find(a);
	b = uf_find(b);
	if (a != b) {
		if (a < b)
			runs[b].parent = a;
		else
			runs[a].parent = b;
	}
}

struct blob {
	long area, sx, sy;
};

static struct blob blobacc[MAX_RUNS];

/* returns number of components with area >= 4 (in sampled pixels) */
static int ccl_workload(const uint8_t *y, int w, int h, int step, int thr)
{
	int nr = 0, prev_s = 0, prev_e = 0, r, c, i, j, blobs = 0;

	for (r = 0; r < h; r += step) {
		const uint8_t *row = y + (size_t)r * w;
		int cur_s = nr;

		for (c = 0; c < w;) {
			int x0;

			if (row[c] < thr) {
				c += step;
				continue;
			}
			x0 = c;
			while (c < w && row[c] >= thr)
				c += step;
			if (nr >= MAX_RUNS || nr - cur_s >= MAX_ROW_RUNS)
				goto done; /* saturated scene: stop, still counted as processed */
			runs[nr].row = r;
			runs[nr].x0 = x0;
			runs[nr].x1 = c - step;
			runs[nr].parent = nr;
			/* 8-connected overlap with runs on the previous sampled row */
			for (j = prev_s; j < prev_e; j++)
				if (runs[j].x1 + step >= x0 && runs[j].x0 <= c)
					uf_union(j, nr);
			nr++;
		}
		prev_s = cur_s;
		prev_e = nr;
	}
done:
	for (i = 0; i < nr; i++)
		blobacc[i].area = blobacc[i].sx = blobacc[i].sy = 0;
	for (i = 0; i < nr; i++) {
		int root = uf_find(i);
		long len = (runs[i].x1 - runs[i].x0) / step + 1;

		blobacc[root].area += len;
		blobacc[root].sx += len * (runs[i].x0 + runs[i].x1) / 2;
		blobacc[root].sy += len * runs[i].row;
	}
	for (i = 0; i < nr; i++)
		if (runs[i].parent == i && blobacc[i].area >= 4)
			blobs++; /* centroid = sx/area, sy/area; not needed for timing */
	return blobs;
}

/* ---------- LED latency test (sysfs GPIO) ---------- */

static int led_fd = -1;

static void led_open(int gpio)
{
	char path[64];
	int fd;

	snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
	if (access(path, F_OK)) {
		fd = open("/sys/class/gpio/export", O_WRONLY);
		if (fd >= 0) {
			dprintf(fd, "%d", gpio);
			close(fd);
		}
		snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio);
		fd = open(path, O_WRONLY);
		if (fd >= 0) {
			dprintf(fd, "out");
			close(fd);
		}
		snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
	}
	led_fd = open(path, O_WRONLY);
	if (led_fd < 0)
		fprintf(stderr, "[W] cannot open %s: %s\n", path, strerror(errno));
}

static void led_set(int on)
{
	if (led_fd >= 0)
		(void)!pwrite(led_fd, on ? "1" : "0", 1, 0);
}

/* ---------- frame save ---------- */

static void save_frame(const char *prefix, const uint8_t *nv12, int w, int h)
{
	char path[256];
	FILE *f;

	snprintf(path, sizeof(path), "%s.pgm", prefix);
	if ((f = fopen(path, "wb"))) {
		fprintf(f, "P5\n%d %d\n255\n", w, h);
		fwrite(nv12, 1, (size_t)w * h, f);
		fclose(f);
	}
	snprintf(path, sizeof(path), "%s.nv12", prefix);
	if ((f = fopen(path, "wb"))) {
		fwrite(nv12, 1, (size_t)w * h * 3 / 2, f);
		fclose(f);
	}
	fprintf(stderr, "[I] saved %s.{pgm,nv12} (%dx%d)\n", prefix, w, h);
}

/* ---------- main ---------- */

static void usage(const char *p)
{
	fprintf(stderr,
		"usage: %s [-n N] [-W N] [-f FPS] [-o WxH] [-c WxH] [-r CH] [-b VBS] [-d DEPTH]\n"
		"          [-w 0|1|2] [-t THR] [-e LINES] [-s PREFIX] [-C CSV] [-L GPIO]\n",
		p);
	exit(2);
}

int main(int argc, char **argv)
{
	struct opts o = {.n = 600, .warmup = 60, .rd = -1, .vbs = 2, .depth = 1, .thr = 200, .expo = -1, .led = -1};
	char sname[32], i2c_type[32];
	int sw, sh, smax, saddr, sbus, ch, k, opt, rw, rh;
	IMPSensorInfo sn;
	IMPFSChnAttr fa;
	struct sample *s;
	FILE *csv = NULL;
	uint32_t fnum = 0, fden = 0;
	unsigned long long t0, i0, t1, i1, p0, p1;
	int64_t w0, w1, prev_get;
	long hz = sysconf(_SC_CLK_TCK);

	while ((opt = getopt(argc, argv, "n:W:f:o:c:r:b:d:w:t:e:s:C:L:h")) != -1) {
		switch (opt) {
		case 'n': o.n = atoi(optarg); break;
		case 'W': o.warmup = atoi(optarg); break;
		case 'f': o.fps = atoi(optarg); break;
		case 'o': if (!parse_wh(optarg, &o.ow, &o.oh)) usage(argv[0]); break;
		case 'c': if (!parse_wh(optarg, &o.c1w, &o.c1h)) usage(argv[0]); break;
		case 'r': o.rd = atoi(optarg); break;
		case 'b': o.vbs = atoi(optarg); break;
		case 'd': o.depth = atoi(optarg); break;
		case 'w': o.work = atoi(optarg); break;
		case 't': o.thr = atoi(optarg); break;
		case 'e': o.expo = atoi(optarg); break;
		case 's': o.save = optarg; break;
		case 'C': o.csv = optarg; break;
		case 'L': o.led = atoi(optarg); break;
		default: usage(argv[0]);
		}
	}
	if (o.n <= 0)
		usage(argv[0]);

	read_proc_str("/proc/jz/sensor/name", sname, sizeof(sname), "sc2336");
	snprintf(i2c_type, sizeof(i2c_type), "%s", sname);
	sw = read_proc_int("/proc/jz/sensor/width", 1920);
	sh = read_proc_int("/proc/jz/sensor/height", 1080);
	smax = read_proc_int("/proc/jz/sensor/max_fps", 30);
	saddr = read_proc_int("/proc/jz/sensor/i2c_addr", 0x30);
	sbus = read_proc_int("/proc/jz/sensor/i2c_bus", 0);
	if (!o.fps)
		o.fps = smax;
	if (!o.ow) {
		o.ow = sw;
		o.oh = sh;
	}
	if (o.rd < 0)
		o.rd = o.c1w ? 1 : 0;
	rw = o.rd ? o.c1w : o.ow;
	rh = o.rd ? o.c1h : o.oh;
	if (o.rd && !o.c1w)
		usage(argv[0]);

	printf("sensor %s %dx%d max_fps %d i2c %d@0x%x | request %d fps | ch0 %dx%d%s",
	       sname, sw, sh, smax, sbus, saddr, o.fps, o.ow, o.oh,
	       (o.ow != sw || o.oh != sh) ? " (scaled)" : " (native)");
	if (o.c1w)
		printf(" | ch1 %dx%d (scaled)", o.c1w, o.c1h);
	printf(" | read ch%d | vbs %d depth %d | work %d thr %d | expo %d\n", o.rd, o.vbs, o.depth,
	       o.work, o.thr, o.expo);

	memset(&sn, 0, sizeof(sn));
	snprintf(sn.name, sizeof(sn.name), "%s", sname);
	sn.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
	snprintf(sn.i2c.type, sizeof(sn.i2c.type), "%.19s", i2c_type);
	sn.i2c.addr = saddr;
	sn.i2c.i2c_adapter_id = sbus;
	sn.rst_gpio = sn.pwdn_gpio = sn.power_gpio = (unsigned short)-1;

	CK(IMP_ISP_Open(), "ISP_Open");
	CK(IMP_ISP_AddSensor(&sn), "AddSensor");
	CK(IMP_ISP_EnableSensor(), "EnableSensor");
	CK(IMP_System_Init(), "System_Init");
	CK(IMP_ISP_EnableTuning(), "EnableTuning");
	if (IMP_ISP_Tuning_SetSensorFPS(o.fps, 1) < 0)
		fprintf(stderr, "[W] SetSensorFPS(%d) rejected by driver/libimp\n", o.fps);
	if (IMP_ISP_Tuning_GetSensorFPS(&fnum, &fden) == 0 && fden)
		printf("sensor fps reported by ISP: %u/%u = %.2f\n", fnum, fden, (double)fnum / fden);

	if (o.expo > 0) {
		IMPISPExpr e;

		memset(&e, 0, sizeof(e));
		e.s_attr.mode = ISP_CORE_EXPR_MODE_MANUAL;
		e.s_attr.unit = ISP_CORE_EXPR_UNIT_LINE;
		e.s_attr.time = o.expo;
		if (IMP_ISP_Tuning_SetExpr(&e) < 0)
			fprintf(stderr, "[W] SetExpr(manual %d lines) failed\n", o.expo);
	}

	memset(&fa, 0, sizeof(fa));
	fa.pixFmt = PIX_FMT_NV12;
	fa.outFrmRateNum = o.fps;
	fa.outFrmRateDen = 1;
	fa.nrVBs = o.vbs;
	fa.type = FS_PHY_CHANNEL;
	fa.picWidth = o.ow;
	fa.picHeight = o.oh;
	if (o.ow != sw || o.oh != sh) {
		fa.scaler.enable = 1;
		fa.scaler.outwidth = o.ow;
		fa.scaler.outheight = o.oh;
	}
	CK(IMP_FrameSource_CreateChn(0, &fa), "FS_CreateChn0");
	if (o.c1w) {
		IMPFSChnAttr f1 = fa;

		f1.picWidth = o.c1w;
		f1.picHeight = o.c1h;
		f1.scaler.enable = 1;
		f1.scaler.outwidth = o.c1w;
		f1.scaler.outheight = o.c1h;
		CK(IMP_FrameSource_CreateChn(1, &f1), "FS_CreateChn1");
	}
	CK(IMP_FrameSource_EnableChn(0), "FS_Enable0");
	if (o.c1w)
		CK(IMP_FrameSource_EnableChn(1), "FS_Enable1");
	ch = o.rd;
	CK(IMP_FrameSource_SetFrameDepth(ch, o.depth), "SetFrameDepth");

	s = calloc(o.n, sizeof(*s));
	if (!s)
		return 1;
	if (o.csv && !(csv = fopen(o.csv, "w")))
		fprintf(stderr, "[W] cannot open %s\n", o.csv);
	if (csv)
		fprintf(csv, "seq,ts_us,get_us,lat_us,wait_us,proc_us,blobs,hash\n");
	if (o.led >= 0) {
		led_open(o.led);
		led_set(0);
		srand((unsigned)mono_us());
	}

	/* warm-up: AE/AWB settle, pipeline fill */
	for (k = 0; k < o.warmup; k++) {
		IMPFrameInfo *fr = NULL;

		if (IMP_FrameSource_GetFrame(ch, &fr) < 0 || !fr) {
			fprintf(stderr, "[E] GetFrame failed during warm-up\n");
			return 1;
		}
		if (k == 0)
			printf("frame: %ux%u pixfmt 0x%x size %u\n", fr->width, fr->height, fr->pixfmt, fr->size);
		if (k == o.warmup - 1 && o.save)
			save_frame(o.save, (const uint8_t *)(uintptr_t)fr->virAddr, rw, rh);
		IMP_FrameSource_ReleaseFrame(ch, fr);
	}

	/* LED latency state */
	int led_on = 0, led_base = -1, led_hits = 0, led_n = 0;
	int64_t led_t = 0, led_lat[256];

	cpu_jiffies(&t0, &i0);
	p0 = self_jiffies();
	w0 = mono_us();
	prev_get = IMP_System_GetTimeStamp();
	for (k = 0; k < o.n; k++) {
		IMPFrameInfo *fr = NULL;
		const uint8_t *y;
		int64_t a, b;

		if (o.led >= 0 && !led_on && k % 30 == 10 && led_n < 256) {
			/*
			 * Random phase: without it the LED always switches right after a
			 * frame was delivered and every sample measures the same point of
			 * the frame cycle.  A trigger pull lands anywhere in the period.
			 * The frame finishing meanwhile waits in the FIFO (depth >= 1).
			 */
			if (fnum)
				usleep((useconds_t)(rand() % (int)(1000000 * (int64_t)fden / fnum)));
			led_set(1);
			led_t = IMP_System_GetTimeStamp();
			led_on = 1;
		}
		a = IMP_System_GetTimeStamp();
		if (IMP_FrameSource_GetFrame(ch, &fr) < 0 || !fr) {
			fprintf(stderr, "[E] GetFrame failed at %d\n", k);
			break;
		}
		b = IMP_System_GetTimeStamp();
		y = (const uint8_t *)(uintptr_t)fr->virAddr;
		s[k].ts = fr->timeStamp;
		s[k].get = b;
		s[k].wait = b - a;
		s[k].hash = luma_hash(y, rw, rh);
		if (o.work == 1 || o.work == 2)
			s[k].blobs = ccl_workload(y, rw, rh, o.work, o.thr);
		if (o.led >= 0) {
			int m = mean_luma_center(y, rw, rh);

			if (!led_on)
				led_base = m;
			else if (led_base >= 0 && m > led_base + 12 && fr->timeStamp > led_t) {
				/* first frame finished after LED-on whose content shows it */
				led_lat[led_n++] = b - led_t;
				led_hits++;
				led_set(0);
				led_on = 0;
			} else if (b - led_t > 500000) {
				led_set(0); /* no response: count as miss */
				led_on = 0;
			}
		}
		s[k].proc = IMP_System_GetTimeStamp() - b;
		IMP_FrameSource_ReleaseFrame(ch, fr);
		if (csv)
			fprintf(csv, "%d,%lld,%lld,%lld,%lld,%lld,%d,%08x\n", k, (long long)s[k].ts,
				(long long)s[k].get, (long long)(s[k].get - s[k].ts), (long long)s[k].wait,
				(long long)s[k].proc, s[k].blobs, s[k].hash);
		prev_get = b;
	}
	w1 = mono_us();
	p1 = self_jiffies();
	cpu_jiffies(&t1, &i1);
	(void)prev_get;
	if (o.led >= 0)
		led_set(0);

	int n = k;
	int64_t *iv = calloc(n, sizeof(int64_t)), *lat = calloc(n, sizeof(int64_t));
	int64_t *wt = calloc(n, sizeof(int64_t)), *pr = calloc(n, sizeof(int64_t));
	int64_t *gi = calloc(n, sizeof(int64_t));
	int ni = 0, dup_ts = 0, dup_px = 0, drops = 0, i;
	int64_t med, nom;

	for (i = 0; i < n; i++) {
		lat[i] = s[i].get - s[i].ts;
		wt[i] = s[i].wait;
		pr[i] = s[i].proc;
		if (i) {
			int64_t d = s[i].ts - s[i - 1].ts;

			gi[i - 1] = s[i].get - s[i - 1].get;
			if (d <= 0)
				dup_ts++;
			else
				iv[ni++] = d;
			if (s[i].hash == s[i - 1].hash)
				dup_px++;
		}
	}
	/* median interval for drop detection (copy: stats() sorts in place) */
	{
		int64_t *tmp = calloc(ni ? ni : 1, sizeof(int64_t));

		memcpy(tmp, iv, ni * sizeof(int64_t));
		qsort(tmp, ni, sizeof(int64_t), cmp64);
		med = ni ? tmp[ni / 2] : 0;
		free(tmp);
	}
	/*
	 * Drops are counted against the nominal sensor period, not the median:
	 * an app that misses every other frame doubles the median itself.  The
	 * ISP only timestamps frames it hands out, so gaps are the only trace of
	 * frames the sensor produced but the app never saw.
	 */
	nom = (fnum && fden) ? (int64_t)1000000 * fden / fnum : med;
	for (i = 0; nom && i < ni; i++)
		if (iv[i] > nom * 3 / 2)
			drops += (int)((iv[i] + nom / 2) / nom) - 1;

	double wall = (w1 - w0) / 1e6;
	double span = n > 1 ? (s[n - 1].ts - s[0].ts) / 1e6 : 0;
	unsigned long long dt = t1 - t0, di = i1 - i0;

	printf("\n== %d frames in %.3f s wall ==\n", n, wall);
	printf("delivered fps   %.2f  (frames returned by GetFrame / wall time)\n", n / wall);
	printf("isp fps         %.2f  (from frame timestamps: %d intervals over %.3f s)\n",
	       span > 0 ? (n - 1) / span : 0, n - 1, span);
	printf("median interval %lld us -> %.2f fps\n", (long long)med, med ? 1e6 / med : 0);
	printf("dropped (ts gaps > 1.5x nominal %lld us) %d | duplicate ts %d | identical-content %d\n",
	       (long long)nom, drops, dup_ts, dup_px);
	printf("processed %d of ~%d sensor frames (%.1f%%)\n", n, n + drops,
	       n + drops ? 100.0 * n / (n + drops) : 0);
	stats("interval_us", "us", iv, ni);
	stats("get_gap_us", "us", gi, n > 1 ? n - 1 : 0);
	stats("latency_us", "us (ISP frame done -> app)", lat, n);
	stats("wait_us", "us (blocked in GetFrame)", wt, n);
	stats("proc_us", o.work ? "us (workload)" : "us (hash only)", pr, n);
	if (o.led >= 0) {
		printf("LED test: %d responses\n", led_hits);
		stats("led_lat_us", "us (LED on -> frame visible in app)", led_lat, led_n);
	}
	printf("cpu system %.1f%% busy | lgcap %.1f%% of one core\n",
	       dt ? 100.0 * (dt - di) / dt : 0, wall > 0 ? 100.0 * (p1 - p0) / hz / wall : 0);
	printf("mem lgcap VmRSS %ld kB VmHWM %ld kB | MemAvailable %ld kB | MemFree %ld kB\n",
	       status_kb("/proc/self/status", "VmRSS:"), status_kb("/proc/self/status", "VmHWM:"),
	       status_kb("/proc/meminfo", "MemAvailable:"), status_kb("/proc/meminfo", "MemFree:"));
	printf("buffers: %d x %dx%d NV12 = %zu kB on ch%d", o.vbs, o.ow, o.oh,
	       (size_t)o.vbs * o.ow * o.oh * 3 / 2 / 1024, 0);
	if (o.c1w)
		printf(", %d x %dx%d = %zu kB on ch1", o.vbs, o.c1w, o.c1h,
		       (size_t)o.vbs * o.c1w * o.c1h * 3 / 2 / 1024);
	printf("\n");

	if (csv)
		fclose(csv);
	if (o.c1w) {
		IMP_FrameSource_DisableChn(1);
		IMP_FrameSource_DestroyChn(1);
	}
	IMP_FrameSource_DisableChn(0);
	IMP_FrameSource_DestroyChn(0);
	IMP_System_Exit();
	IMP_ISP_DisableTuning();
	IMP_ISP_DisableSensor();
	IMP_ISP_DelSensor(&sn);
	IMP_ISP_Close();
	return 0;
}
