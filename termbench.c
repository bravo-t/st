/*
 * termbench - generic in-terminal PTY read speed, latency, throughput, and
 * capability benchmark
 *
 * Build: cc -std=c99 -O2 -Wall -Wextra -o termbench termbench.c -lrt
 * Usage: ./termbench [--quick | --full] [--pty-only] [--frames N]
 *                    [--ms N] [--mb N] [--json] [--out FILE]
 *
 * Measures generic terminal emulator performance from inside the running
 * terminal using VT query barriers (CPR CSI 6 n) and raw PTY output (~OPOST).
 * Reports isolated PTY read speed (write ingest vs CPR-verified drain rate,
 * post-write backlog tail, write-chunk scaling, and write-stall percentiles),
 * interactive frame/burst latency, and streaming throughput across standard
 * VT content encodings.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static struct termios orig_tio;
static int tio_saved = 0;
static int in_alt = 0;
static volatile sig_atomic_t interrupted = 0;

static double now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec * 1e-6;
}

static int write_all(int fd, const void *buf, size_t len) {
	const char *p = (const char *)buf;
	while (len > 0) {
		ssize_t w = write(fd, p, len);
		if (w < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		p += w;
		len -= (size_t)w;
	}
	return 0;
}

static void restore_term(void) {
	const char *seq = "\033[?2026l\033[r\033[0m\033[?25h";
	(void)write_all(STDOUT_FILENO, seq, strlen(seq));
	if (in_alt) {
		const char *alt = "\033[?1049l";
		(void)write_all(STDOUT_FILENO, alt, strlen(alt));
		in_alt = 0;
	}
	if (tio_saved) {
		tcsetattr(STDIN_FILENO, TCSANOW, &orig_tio);
		tio_saved = 0;
	}
}

static void on_signal(int sig) {
	interrupted = sig;
	restore_term();
	_exit(128 + sig);
}

static int enter_raw(void) {
	struct termios raw;
	if (tcgetattr(STDIN_FILENO, &orig_tio) < 0)
		return -1;
	tio_saved = 1;
	raw = orig_tio;
	raw.c_iflag &= ~(unsigned)(ICRNL | IXON | BRKINT | INPCK | ISTRIP);
	/* Disable OPOST so slave writes map 1:1 to master PTY reads without ONLCR overhead. */
	raw.c_oflag &= ~(unsigned)OPOST;
	raw.c_lflag &= ~(unsigned)(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cflag |= (unsigned)CS8;
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0)
		return -1;
	return 0;
}

static void drain_stdin(void) {
	char tmp[512];
	struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
	for (;;) {
		int r = poll(&pfd, 1, 0);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0 || !(pfd.revents & POLLIN)) break;
		ssize_t n = read(STDIN_FILENO, tmp, sizeof(tmp));
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) break;
	}
}

/* Read until `term1` or optional `term2` appears in reply, or `timeout_ms` expires. */
static int read_reply(char *out, size_t cap, const char *term1, const char *term2, int timeout_ms) {
	size_t len = 0;
	double deadline = now_ms() + timeout_ms;
	out[0] = '\0';

	while (len + 1 < cap) {
		double rem = deadline - now_ms();
		if (rem <= 0)
			break;
		int to = (rem > 60000.0) ? 60000 : ((int)rem + 1);
		struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
		int r = poll(&pfd, 1, to);
		if (r < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (r == 0)
			break;
		ssize_t n = read(STDIN_FILENO, out + len, cap - 1 - len);
		if (n < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (n == 0)
			break;
		len += (size_t)n;
		out[len] = '\0';
		if ((term1 && strstr(out, term1)) || (term2 && strstr(out, term2)))
			return (int)len;
	}
	return (int)len;
}

/* Scan buffer for a CPR reply ("\033[<row>;<col>R"). */
static int parse_cpr(const char *buf, int *row, int *col) {
	const char *p = buf;
	while ((p = strchr(p, '\033')) != NULL) {
		if (p[1] == '[') {
			int rr = 0, cc = 0;
			char fin = 0;
			if (sscanf(p + 2, "%d;%d%c", &rr, &cc, &fin) == 3 && fin == 'R') {
				if (row) *row = rr;
				if (col) *col = cc;
				return 1;
			}
		}
		p++;
	}
	return 0;
}

/* Read a CPR reply ("\033[<row>;<col>R"), returning 1 on success. */
static int wait_cpr(int *row, int *col, int timeout_ms) {
	char buf[256];
	size_t len = 0;
	double deadline = now_ms() + timeout_ms;
	buf[0] = '\0';

	for (;;) {
		double rem = deadline - now_ms();
		if (rem <= 0)
			return 0;
		int to = (rem > 60000.0) ? 60000 : ((int)rem + 1);
		struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
		int r = poll(&pfd, 1, to);
		if (r < 0) {
			if (errno == EINTR) continue;
			return 0;
		}
		if (r == 0)
			return 0;
		if (len + 1 >= sizeof(buf)) {
			/* Retain tail bytes so a split CPR sequence across reads still matches. */
			size_t keep = 32;
			memmove(buf, buf + len - keep, keep);
			len = keep;
		}
		ssize_t n = read(STDIN_FILENO, buf + len, sizeof(buf) - 1 - len);
		if (n < 0) {
			if (errno == EINTR) continue;
			return 0;
		}
		if (n == 0)
			return 0;
		len += (size_t)n;
		buf[len] = '\0';
		if (parse_cpr(buf, row, col))
			return 1;
	}
}

/* Sync barrier: send CSI 6 n and wait for reply. */
static int sync_cpr(int timeout_ms) {
	if (write_all(STDOUT_FILENO, "\033[6n", 4) < 0)
		return 0;
	return wait_cpr(NULL, NULL, timeout_ms);
}

static int cmp_double(const void *a, const void *b) {
	double da = *(const double *)a, db = *(const double *)b;
	return (da > db) - (da < db);
}

static int cmp_float(const void *a, const void *b) {
	float fa = *(const float *)a, fb = *(const float *)b;
	return (fa > fb) - (fa < fb);
}

typedef struct {
	const char *name;
	int ok;
	double ms;
	char detail[64];
} ProbeRes;

typedef struct {
	const char *name;
	int ok;
	int frames;
	double min_ms, p50_ms, p95_ms, p99_ms, max_ms, avg_ms, ops_s;
} LatRes;

typedef struct {
	const char *name;
	int ok;
	size_t chunk_bytes;
	size_t bytes;
	double lines;
	double write_ms;
	double drain_ms;
	double tail_ms;
	double write_mb_s;
	double drain_mb_s;
	double klines_s;
	double wr_p50_ms;
	double wr_p99_ms;
	double wr_max_ms;
} PtyRes;

static void fmt_ms6(char *out, size_t cap, double ms) {
	if (ms < 10.0)
		snprintf(out, cap, "%6.3f", ms);
	else if (ms < 1000.0)
		snprintf(out, cap, "%6.2f", ms);
	else if (ms < 10000.0)
		snprintf(out, cap, "%6.1f", ms);
	else
		snprintf(out, cap, "%6.0f", ms);
}

static void fmt_ms7(char *out, size_t cap, double ms) {
	if (ms < 1000.0)
		snprintf(out, cap, "%7.2f", ms);
	else if (ms < 100000.0)
		snprintf(out, cap, "%7.1f", ms);
	else
		snprintf(out, cap, "%7.0f", ms);
}

static int parse_pos_int(const char *s, int *out) {
	if (!s || !*s) return 0;
	char *end = NULL;
	errno = 0;
	long v = strtol(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0' || v < 1 || v > 1000000)
		return 0;
	*out = (int)v;
	return 1;
}

static void fmt_mb7(char *out, size_t cap, size_t bytes) {
	double mb = bytes / (1024.0 * 1024.0);
	if (mb < 1000.0)
		snprintf(out, cap, "%5.1fMB", mb);
	else
		snprintf(out, cap, "%5.0fMB", mb);
}

static void json_str(FILE *fp, const char *s) {
	fputc('"', fp);
	for (; s && *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') {
			fputc('\\', fp);
			fputc((int)c, fp);
		} else if (c >= 0x20 && c < 0x7f) {
			fputc((int)c, fp);
		}
	}
	fputc('"', fp);
}

static void sanitize(const char *in, char *out, size_t cap) {
	size_t o = 0;
	for (size_t i = 0; in[i] && o + 4 < cap; i++) {
		unsigned char c = (unsigned char)in[i];
		if (c == 0x1b) {
			out[o++] = 'E'; out[o++] = 'S'; out[o++] = 'C';
		} else if (c == 0x07) {
			out[o++] = 'B'; out[o++] = 'E'; out[o++] = 'L';
		} else if (c >= 0x20 && c < 0x7f) {
			out[o++] = (char)c;
		}
	}
	out[o] = '\0';
}

static ProbeRes run_probe(const char *name, const char *seq, const char *term1, const char *term2, int timeout_ms) {
	ProbeRes r;
	char buf[256];
	memset(&r, 0, sizeof(r));
	r.name = name;
	drain_stdin();
	double t0 = now_ms();
	write_all(STDOUT_FILENO, seq, strlen(seq));
	int n = read_reply(buf, sizeof(buf), term1, term2, timeout_ms);
	r.ms = now_ms() - t0;
	int matched = (!term1 && !term2) ||
	              (term1 && strstr(buf, term1)) ||
	              (term2 && strstr(buf, term2));
	if (n > 0 && matched) {
		r.ok = 1;
		sanitize(buf, r.detail, sizeof(r.detail));
	} else {
		r.ok = 0;
		snprintf(r.detail, sizeof(r.detail), "no reply (timeout)");
	}
	return r;
}

static void compute_stats(LatRes *res, double *samples, int n, double total_ms, int ok) {
	res->ok = ok;
	res->frames = n;
	if (n <= 0) return;
	qsort(samples, (size_t)n, sizeof(double), cmp_double);
	res->min_ms = samples[0];
	res->p50_ms = samples[n / 2];
	res->p95_ms = samples[(int)((n - 1) * 0.95)];
	res->p99_ms = samples[(int)((n - 1) * 0.99)];
	res->max_ms = samples[n - 1];
	res->avg_ms = total_ms / n;
	res->ops_s = (total_ms > 0) ? (n * 1000.0 / total_ms) : 0.0;
}

static size_t append_str(char *buf, size_t cap, size_t o, const char *s, size_t slen) {
	if (o >= cap) return o;
	if (slen > cap - o) slen = cap - o;
	memcpy(buf + o, s, slen);
	return o + slen;
}

/* Build a full-screen plain ASCII frame */
static char *build_ascii_frame(int cols, int rows, int frame, size_t *outlen) {
	size_t cap = (size_t)rows * (size_t)(cols + 32) + 64;
	char *buf = malloc(cap);
	size_t o = 0;
	o = append_str(buf, cap, o, "\033[H", 3);
	for (int r = 0; r < rows; r++) {
		char hdr[96];
		int written = snprintf(hdr, sizeof(hdr),
		    "frame=%04d row=%03d seq=%06d ",
		    frame, r, (frame * 131 + r * 17) % 1000000);
		if (written < 0) written = 0;
		if (written > cols) written = cols;
		o = append_str(buf, cap, o, hdr, (size_t)written);
		for (int c = written; c < cols; c++)
			if (o < cap) buf[o++] = (char)('!' + ((c + r + frame) % 90));
		if (r + 1 < rows) o = append_str(buf, cap, o, "\r\n", 2);
	}
	*outlen = o;
	return buf;
}

/* Build a full-screen generic multibyte UTF-8 frame (2- and 3-byte codepoints) */
static char *build_utf8_frame(int cols, int rows, int frame, size_t *outlen) {
	static const char *const glyphs[] = {
		"\xc3\xa9", "\xc3\xbc", "\xc3\xb1", "\xc3\xb6", /* é ü ñ ö (2-byte Latin) */
		"\xce\xb1", "\xce\xb2", "\xce\xb3", "\xcf\x80", /* α β γ π (2-byte Greek) */
		"\xe2\x86\x90", "\xe2\x86\x91", "\xe2\x86\x92", "\xe2\x86\x93", /* ← ↑ → ↓ (3-byte arrows) */
		"\xe2\x89\xa4", "\xe2\x89\xa5", "\xe2\x89\xa0", "\xe2\x88\x9e", /* ≤ ≥ ≠ ∞ (3-byte math) */
		"\xe2\x94\x80", "\xe2\x94\x82", "\xe2\x96\xa0", "\xe2\x97\x8f"  /* ─ │ ■ ● (3-byte geom/box) */
	};
	const int ng = (int)(sizeof(glyphs) / sizeof(glyphs[0]));
	size_t cap = (size_t)rows * (size_t)(cols * 4 + 16) + 64;
	char *buf = malloc(cap);
	size_t o = 0;
	o = append_str(buf, cap, o, "\033[H", 3);
	for (int r = 0; r < rows; r++) {
		for (int c = 0; c < cols; c++) {
			if ((c & 3) == 0) {
				const char *g = glyphs[(r + c + frame) % ng];
				o = append_str(buf, cap, o, g, strlen(g));
			} else {
				if (o < cap) buf[o++] = (char)('a' + ((c + r + frame) % 26));
			}
		}
		if (r + 1 < rows) o = append_str(buf, cap, o, "\r\n", 2);
	}
	*outlen = o;
	return buf;
}

/* Build a full-screen generic 24-bit SGR frame (with optional ?2026 sync wrap) */
static char *build_sgr_frame(int cols, int rows, int frame, int sync_wrap, size_t *outlen) {
	/* Change truecolor SGR every 8 cells across the grid */
	size_t cap = (size_t)rows * (size_t)(cols * 8 + 128) + 256;
	char *buf = malloc(cap);
	size_t o = 0;
	if (sync_wrap) o = append_str(buf, cap, o, "\033[?2026h", 8);
	o = append_str(buf, cap, o, "\033[H", 3);
	for (int r = 0; r < rows; r++) {
		for (int c = 0; c < cols; c++) {
			if ((c & 7) == 0) {
				char sgr[48];
				int cr = 80 + ((r * 13 + c * 3 + frame * 29) & 127);
				int cg = 80 + ((r * 7 + c * 11 + frame * 17) & 127);
				int cb = 80 + ((r * 19 + c * 5 + frame * 11) & 127);
				int sl = snprintf(sgr, sizeof(sgr), "\033[%d;38;2;%d;%d;%dm",
				                  ((r + c + frame) & 1) ? 1 : 0, cr, cg, cb);
				if (sl > 0) o = append_str(buf, cap, o, sgr, (size_t)sl);
			}
			if (o < cap) buf[o++] = (char)('A' + ((c + r + frame) % 26));
		}
		o = append_str(buf, cap, o, "\033[0m", 4);
		if (r + 1 < rows) o = append_str(buf, cap, o, "\r\n", 2);
	}
	if (sync_wrap) o = append_str(buf, cap, o, "\033[?2026l", 8);
	*outlen = o;
	return buf;
}

/* Build 2 alternating in-place ASCII lines ("\r" + line_w chars each, no scroll). */
static char *build_inplace_pattern(int cols, size_t *outlen) {
	int lw = (cols > 80) ? 79 : (cols > 2 ? cols - 1 : 1);
	size_t line_sz = (size_t)(lw + 1);
	size_t plen = 2 * line_sz;
	char *pat = malloc(plen);
	pat[0] = '\r';
	for (int c = 0; c < lw; c++) pat[1 + c] = (char)('a' + (c % 26));
	pat[line_sz] = '\r';
	for (int c = 0; c < lw; c++) pat[line_sz + 1 + c] = (char)('A' + (c % 26));
	*outlen = plen;
	return pat;
}

static LatRes bench_idle_cpr(int n) {
	LatRes res = { .name = "Idle CPR RTT (CSI 6 n)" };
	double *s = malloc((size_t)n * sizeof(double));
	int ok = 1;
	drain_stdin();
	sync_cpr(1000);
	double tot0 = now_ms();
	for (int i = 0; i < n; i++) {
		double t0 = now_ms();
		if (!sync_cpr(1000)) ok = 0;
		s[i] = now_ms() - t0;
	}
	compute_stats(&res, s, n, now_ms() - tot0, ok);
	free(s);
	return res;
}

static LatRes bench_single_char(int n, int cols, int rows) {
	LatRes res = { .name = "1-char edit + CPR" };
	double *s = malloc((size_t)n * sizeof(double));
	char seq[64];
	int ok = 1;
	sync_cpr(1000);
	double tot0 = now_ms();
	for (int i = 0; i < n; i++) {
		int len = snprintf(seq, sizeof(seq), "\033[%d;%dH%c\033[6n",
		                   (rows / 2) + 1, 2 + (i % (cols > 4 ? cols - 4 : 1)),
		                   (char)('A' + (i % 26)));
		double t0 = now_ms();
		write_all(STDOUT_FILENO, seq, (size_t)len);
		if (!wait_cpr(NULL, NULL, 1000)) ok = 0;
		s[i] = now_ms() - t0;
	}
	compute_stats(&res, s, n, now_ms() - tot0, ok);
	free(s);
	return res;
}

static LatRes bench_two_corners(int n, int cols, int rows) {
	LatRes res = { .name = "2-corner edit (1,1)&(C,R)" };
	double *s = malloc((size_t)n * sizeof(double));
	char seq[96];
	int ok = 1;
	sync_cpr(1000);
	double tot0 = now_ms();
	for (int i = 0; i < n; i++) {
		char ch = (i & 1) ? '*' : '+';
		int len = snprintf(seq, sizeof(seq), "\033[1;1H%c\033[%d;%dH%c\033[6n",
		                   ch, rows, cols, ch);
		double t0 = now_ms();
		write_all(STDOUT_FILENO, seq, (size_t)len);
		if (!wait_cpr(NULL, NULL, 1000)) ok = 0;
		s[i] = now_ms() - t0;
	}
	compute_stats(&res, s, n, now_ms() - tot0, ok);
	free(s);
	return res;
}

/* 64 KB in-place ASCII burst (> kernel PTY buffer) + CPR drain latency */
static LatRes bench_pty_burst(int n, int cols) {
	LatRes res = { .name = "64 KB PTY burst (in-place)" };
	double *s = malloc((size_t)n * sizeof(double));
	size_t plen = 0;
	char *pat = build_inplace_pattern(cols, &plen);
	size_t burst_sz = (65536 / plen) * plen;
	if (burst_sz == 0) burst_sz = plen;
	char *buf = malloc(burst_sz + 4);
	for (size_t off = 0; off < burst_sz; off += plen)
		memcpy(buf + off, pat, plen);
	memcpy(buf + burst_sz, "\033[6n", 4);
	free(pat);

	int ok = 1;
	write_all(STDOUT_FILENO, "\033[H", 3);
	sync_cpr(2000);
	double tot0 = now_ms();
	for (int i = 0; i < n; i++) {
		double t0 = now_ms();
		write_all(STDOUT_FILENO, buf, burst_sz + 4);
		if (!wait_cpr(NULL, NULL, 5000)) ok = 0;
		s[i] = now_ms() - t0;
	}
	compute_stats(&res, s, n, now_ms() - tot0, ok);
	free(buf);
	free(s);
	return res;
}

static LatRes bench_frames(const char *name, int n, int cols, int rows, int mode) {
	LatRes res = { .name = name };
	double *s = malloc((size_t)n * sizeof(double));
	/* Pre-generate 2 alternating frames to keep generator CPU out of the timed loop. */
	size_t len0, len1;
	char *f0, *f1;
	if (mode == 0) {
		f0 = build_ascii_frame(cols, rows, 0, &len0);
		f1 = build_ascii_frame(cols, rows, 1, &len1);
	} else if (mode == 1) {
		f0 = build_utf8_frame(cols, rows, 0, &len0);
		f1 = build_utf8_frame(cols, rows, 1, &len1);
	} else {
		int sync_w = (mode == 3);
		f0 = build_sgr_frame(cols, rows, 0, sync_w, &len0);
		f1 = build_sgr_frame(cols, rows, 1, sync_w, &len1);
	}
	int ok = 1;
	write_all(STDOUT_FILENO, f0, len0);
	sync_cpr(2000);

	double tot0 = now_ms();
	for (int i = 0; i < n; i++) {
		const char *fb = (i & 1) ? f1 : f0;
		size_t fl = (i & 1) ? len1 : len0;
		double t0 = now_ms();
		write_all(STDOUT_FILENO, fb, fl);
		if (!sync_cpr(2000)) ok = 0;
		s[i] = now_ms() - t0;
	}
	compute_stats(&res, s, n, now_ms() - tot0, ok);
	free(f0); free(f1); free(s);
	return res;
}

/* Generic DECSTBM scroll region (middle half of screen, 1-row scroll + CPR) */
static LatRes bench_scroll_region(int n, int cols, int rows) {
	LatRes res = { .name = "Scroll region (DECSTBM)" };
	double *s = malloc((size_t)n * sizeof(double));
	int top = (rows >= 8) ? (rows / 4) + 1 : 1;
	int bot = (rows >= 8) ? rows - (rows / 4) : rows;
	if (bot <= top) { top = 1; bot = rows; }

	size_t flen;
	char *init = build_ascii_frame(cols, rows, 0, &flen);
	write_all(STDOUT_FILENO, init, flen);
	free(init);

	char setup_sr[64];
	int slen = snprintf(setup_sr, sizeof(setup_sr), "\033[%d;%dr\033[%d;1H", top, bot, bot);
	write_all(STDOUT_FILENO, setup_sr, (size_t)slen);
	sync_cpr(2000);

	char rowbuf[512];
	int fill = (cols > 48) ? (cols - 36) : 10;
	if (fill > 200) fill = 200;
	int ok = 1;
	double tot0 = now_ms();
	for (int i = 0; i < n; i++) {
		int rlen = snprintf(rowbuf, sizeof(rowbuf),
		    "\r\n\033[36mline=%05d\033[0m \033[32mval=%04d\033[0m %.*s\033[K\033[6n",
		    i, (i * 37) % 10000, fill,
		    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
		    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
		    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
		    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
		double t0 = now_ms();
		write_all(STDOUT_FILENO, rowbuf, (size_t)rlen);
		if (!wait_cpr(NULL, NULL, 2000)) ok = 0;
		s[i] = now_ms() - t0;
	}
	compute_stats(&res, s, n, now_ms() - tot0, ok);
	write_all(STDOUT_FILENO, "\033[r\033[0m", 7);
	free(s);
	return res;
}

/*
 * Measure sustained PTY read throughput, post-write drain tail, and per-write
 * backpressure stall latency for an arbitrary byte pattern (may include NUL).
 *
 * If `fixed_bytes > 0`, writes exactly `fixed_bytes` (rounded up to a pattern
 * boundary). Otherwise writes until both `min_bytes` and `target_ms` are met
 * (up to `max_bytes`), so measurements stay fast on slow terminals and stable
 * on multi-hundred-MB/s terminals.
 */
static PtyRes bench_pty_stream(const char *name, const char *pattern, size_t plen,
                               int lines_per_pat, size_t wsize,
                               double target_ms, size_t min_bytes, size_t fixed_bytes) {
	PtyRes r;
	memset(&r, 0, sizeof(r));
	r.name = name;
	r.chunk_bytes = wsize;
	if (plen == 0 || wsize == 0) return r;

	const size_t max_bytes = (size_t)1024u * 1024u * 1024u;
	size_t base = (wsize > 65536) ? wsize : 65536;
	size_t reps = (base + plen - 1) / plen;
	size_t period = reps * plen;
	char *ring = malloc(period + wsize);
	for (size_t i = 0; i < reps; i++)
		memcpy(ring + i * plen, pattern, plen);
	/* Duplicate leading wsize bytes so every offset < period has wsize contiguous bytes. */
	for (size_t done = 0; done < wsize; ) {
		size_t cp = (wsize - done < period) ? (wsize - done) : period;
		memcpy(ring + period + done, ring, cp);
		done += cp;
	}

	/* Pre-size and pre-fault stalls[] before t0 so small-write loops avoid realloc/fault jitter. */
	size_t stall_cap = (wsize <= 1024) ? 262144 : ((wsize <= 4096) ? 65536 : 8192);
	size_t stall_n = 0;
	const size_t stall_max_cap = 4u * 1024u * 1024u;
	float *stalls = malloc(stall_cap * sizeof(float));
	if (stalls) memset(stalls, 0, stall_cap * sizeof(float));

	drain_stdin();
	sync_cpr(3000);
	double t0 = now_ms();
	double t_prev = t0;
	size_t sent = 0;
	size_t off = 0;
	int write_err = 0;

	for (;;) {
		size_t n = wsize;
		if (fixed_bytes > 0) {
			if (sent >= fixed_bytes) break;
			if (fixed_bytes - sent < n) n = fixed_bytes - sent;
		} else {
			if (sent >= min_bytes && (t_prev - t0) >= target_ms) break;
			if (sent >= max_bytes) break;
		}
		if (write_all(STDOUT_FILENO, ring + off, n) < 0) {
			write_err = 1;
			break;
		}
		double t_cur = now_ms();
		float wdt = (float)(t_cur - t_prev);
		t_prev = t_cur;
		if (stalls && stall_n == stall_cap && stall_cap < stall_max_cap) {
			size_t ncap = stall_cap * 2;
			if (ncap > stall_max_cap) ncap = stall_max_cap;
			float *ns = realloc(stalls, ncap * sizeof(float));
			if (ns) { stalls = ns; stall_cap = ncap; }
			/* Exclude realloc time from the next write()'s stall sample. */
			t_prev = now_ms();
		}
		if (stalls && stall_n < stall_cap)
			stalls[stall_n++] = wdt;
		sent += n;
		off += n;
		if (off >= period) off %= period;
	}

	/* Complete any partial pattern (e.g. split UTF-8 or CSI) before sending CPR barrier. */
	if (!write_err && (off % plen) != 0) {
		size_t rem = plen - (off % plen);
		if (write_all(STDOUT_FILENO, ring + off, rem) == 0) {
			sent += rem;
		} else {
			write_err = 1;
		}
	}

	double t_wr = now_ms();
	int ok = (!write_err) && sync_cpr(15000);
	double t_dr = now_ms();

	/* Reset attributes cleanly in case the pattern set SGR state. */
	write_all(STDOUT_FILENO, "\033[0m", 4);

	r.ok = ok;
	r.bytes = sent;
	r.lines = (lines_per_pat > 0) ? (((double)sent / (double)plen) * lines_per_pat) : 0.0;
	r.write_ms = t_wr - t0;
	r.drain_ms = t_dr - t0;
	r.tail_ms = t_dr - t_wr;
	double mb = sent / (1024.0 * 1024.0);
	r.write_mb_s = (r.write_ms > 0.0) ? (mb / (r.write_ms / 1000.0)) : 0.0;
	r.drain_mb_s = (ok && r.drain_ms > 0.0) ? (mb / (r.drain_ms / 1000.0)) : 0.0;
	r.klines_s = (ok && r.drain_ms > 0.0 && r.lines > 0.0) ? ((r.lines / 1000.0) / (r.drain_ms / 1000.0)) : 0.0;

	if (stall_n > 0) {
		qsort(stalls, stall_n, sizeof(float), cmp_float);
		r.wr_p50_ms = stalls[stall_n / 2];
		r.wr_p99_ms = stalls[(size_t)((stall_n - 1) * 0.99)];
		r.wr_max_ms = stalls[stall_n - 1];
	}

	free(stalls);
	free(ring);
	return r;
}

static void usage(const char *prog) {
	fprintf(stderr,
	    "usage: %s [--quick | --full] [--pty-only] [--frames N] [--ms N] [--mb N] [--json] [--out FILE]\n"
	    "  --quick     fast run (30 frames, 200 ms per PTY/stream test)\n"
	    "  --full      default run (80 frames, 400 ms per PTY/stream test)\n"
	    "  --pty-only  run only the PTY read speed benchmarks\n"
	    "  --frames N  override latency benchmark iteration count\n"
	    "  --ms N      override target duration per PTY/stream test in ms\n"
	    "  --mb N      use exact fixed N MB payload per PTY/stream test\n"
	    "  --json      emit machine-readable JSON after the tables\n"
	    "  --out FILE  also write report output to FILE\n", prog);
}

int main(int argc, char **argv) {
	int frames = 80;
	int target_ms = 400;
	int fixed_mb = 0;
	int pty_only = 0;
	int want_json = 0;
	const char *outpath = NULL;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--quick")) { frames = 30; target_ms = 200; }
		else if (!strcmp(argv[i], "--full")) { frames = 80; target_ms = 400; }
		else if (!strcmp(argv[i], "--pty-only")) { pty_only = 1; }
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
			if (!parse_pos_int(argv[++i], &frames)) { usage(argv[0]); return 2; }
		}
		else if (!strcmp(argv[i], "--ms") && i + 1 < argc) {
			if (!parse_pos_int(argv[++i], &target_ms)) { usage(argv[0]); return 2; }
		}
		else if (!strcmp(argv[i], "--mb") && i + 1 < argc) {
			if (!parse_pos_int(argv[++i], &fixed_mb)) { usage(argv[0]); return 2; }
		}
		else if (!strcmp(argv[i], "--json")) { want_json = 1; }
		else if (!strcmp(argv[i], "--out") && i + 1 < argc) { outpath = argv[++i]; }
		else { usage(argv[0]); return 2; }
	}
	if (frames < 5) frames = 5;
	if (target_ms < 50) target_ms = 50;
	size_t fixed_bytes = (size_t)fixed_mb * 1024u * 1024u;
	size_t min_bulk = 1024u * 1024u;
	size_t min_small = 256u * 1024u;

	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
		fprintf(stderr, "termbench: stdin and stdout must be a terminal\n");
		return 1;
	}

	struct winsize ws;
	memset(&ws, 0, sizeof(ws));
	ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
	int cols = ws.ws_col ? ws.ws_col : 80;
	int rows = ws.ws_row ? ws.ws_row : 24;

	atexit(restore_term);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	if (enter_raw() < 0) {
		perror("tcsetattr");
		return 1;
	}

	/* Enter alternate screen for probes + latency benchmarks */
	write_all(STDOUT_FILENO, "\033[?1049h\033[?25l\033[2J\033[H", 22);
	in_alt = 1;

	/* 1. Generic VT/xterm capability probes */
	ProbeRes probes[7];
	int nprobes = 0;
	if (!pty_only) {
		probes[0] = run_probe("DA1 (CSI c)",           "\033[c",              "c",     NULL,     150);
		probes[1] = run_probe("DA2 (CSI > c)",         "\033[>c",             "c",     NULL,     150);
		probes[2] = run_probe("CPR (CSI 6 n)",         "\033[6n",             "R",     NULL,     150);
		probes[3] = run_probe("DECRQM ?2026 (Sync)",   "\033[?2026$p",        "$y",    NULL,     150);
		probes[4] = run_probe("XTVERSION (CSI > 0 q)", "\033[>0q",            "\033\\", NULL,    150);
		probes[5] = run_probe("XTGETTCAP (RGB)",       "\033P+q524742\033\\", "\033\\", NULL,    150);
		probes[6] = run_probe("OSC 11 (bg color)",     "\033]11;?\007",       "\007",  "\033\\", 150);
		nprobes = 7;
	}

	/* 2. Generic interactive & burst latency benchmarks (on alternate screen) */
	LatRes lats[9];
	int nlats = 0;
	if (!pty_only) {
		lats[0] = bench_idle_cpr(frames);
		lats[1] = bench_single_char(frames, cols, rows);
		lats[2] = bench_two_corners(frames, cols, rows);
		lats[3] = bench_pty_burst(frames, cols);
		lats[4] = bench_frames("Full-screen ASCII",            frames, cols, rows, 0);
		lats[5] = bench_frames("Full-screen UTF-8",            frames, cols, rows, 1);
		lats[6] = bench_frames("Full-screen 24-bit SGR",       frames, cols, rows, 2);
		lats[7] = bench_scroll_region(frames, cols, rows);
		lats[8] = bench_frames("Sync (?2026) full-screen SGR", frames, cols, rows, 3);
		nlats = 9;
	}

	/* 3. PTY read speed benchmarks (alt screen: no scrollback) */
	size_t ip_len = 0;
	char *p_inplace = build_inplace_pattern(cols, &ip_len);
	const char p_nul[1] = { '\0' };
	const char *p_csi_nop = "\033[0m";
	const char *p_ascii80 =
	    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdef\r\n";

	PtyRes ptys[7];
	write_all(STDOUT_FILENO, "\033[2J\033[H", 7);
	ptys[0] = bench_pty_stream("NUL drain (no-op, 64KB)",  p_nul,     1,                 0, 65536, (double)target_ms, min_bulk,  fixed_bytes);
	ptys[1] = bench_pty_stream("CSI no-op (SGR 0, 64KB)",  p_csi_nop, strlen(p_csi_nop), 0, 65536, (double)target_ms, min_bulk,  fixed_bytes);
	ptys[2] = bench_pty_stream("ASCII in-place (64KB wr)", p_inplace, ip_len,            0, 65536, (double)target_ms, min_bulk,  fixed_bytes);
	ptys[3] = bench_pty_stream("ASCII in-place (4KB wr)",  p_inplace, ip_len,            0,  4096, (double)target_ms, min_bulk,  fixed_bytes);
	ptys[4] = bench_pty_stream("ASCII in-place (512B wr)", p_inplace, ip_len,            0,   512, (double)target_ms, min_small, fixed_bytes);
	ptys[5] = bench_pty_stream("ASCII scroll (alt, 64KB)", p_ascii80, strlen(p_ascii80), 1, 65536, (double)target_ms, min_bulk,  fixed_bytes);
	free(p_inplace);

	/* Leave alternate screen for primary-screen scrolling & scrollback benchmarks */
	write_all(STDOUT_FILENO, "\033[?1049l\033[0m", 12);
	in_alt = 0;

	ptys[6] = bench_pty_stream("ASCII scroll (pri, 64KB)", p_ascii80, strlen(p_ascii80), 1, 65536, (double)target_ms, min_bulk, fixed_bytes);

	/* 4. Generic streaming throughput across standard VT encodings (primary screen) */
	PtyRes thrs[5];
	int nthrs = 0;
	if (!pty_only) {
		const char *p_wrap240 =
		    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdef"
		    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdef"
		    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdef"
		    "012345\r\n";
		const char *p_sgr16 =
		    "\033[1;36mfield0\033[0m=\033[32m12345\033[0m "
		    "\033[1;33mfield1\033[0m=\033[35m67890\033[0m "
		    "\033[34mfield2\033[0m=\033[31mabcde\033[0m "
		    "\033[37mfield3\033[0m=\033[36mfghij\033[0m "
		    "\033[2mtrail_text_0123456789\033[0m\r\n";
		const char *p_sgr24 =
		    "\033[38;2;120;200;255msegA\033[0m "
		    "\033[38;2;255;180;90msegB_012345\033[0m "
		    "\033[38;2;110;230;140msegC_67890\033[0m "
		    "\033[38;2;240;110;160msegD_abcdef\033[0m "
		    "\033[38;2;190;190;210msegE_ghijkl\033[0m\r\n";
		const char *p_utf8 =
		    "caf\xc3\xa9 na\xc3\xafve \xc3\xbc""ber \xce\xb1+\xce\xb2=\xcf\x80 "
		    "\xe2\x86\x90\xe2\x86\x91\xe2\x86\x92\xe2\x86\x93 "
		    "x\xe2\x89\xa4y\xe2\x89\xa5z\xe2\x89\xa0\xe2\x88\x9e "
		    "\xe2\x94\x8c\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x90 "
		    "\xe2\x96\xa0\xe2\x97\x8f plain_ascii_tail_0123456789\r\n";
		const char *p_csi_dense =
		    "\033[1;32mA\033[0m\033[33mB\033[0m\033[34mC\033[0m\033[35mD\033[0m"
		    "\033[36mE\033[0m\033[1;31mF\033[0m\033[4mG\033[0m\033[7mH\033[0m"
		    "\033[K\r\n";

		thrs[0] = bench_pty_stream("Long wrapping ASCII (240 col)", p_wrap240,   strlen(p_wrap240),   1, 65536, (double)target_ms, min_bulk, fixed_bytes);
		thrs[1] = bench_pty_stream("ASCII + 16-color SGR",          p_sgr16,     strlen(p_sgr16),     1, 65536, (double)target_ms, min_bulk, fixed_bytes);
		thrs[2] = bench_pty_stream("ASCII + 24-bit SGR (RGB)",      p_sgr24,     strlen(p_sgr24),     1, 65536, (double)target_ms, min_bulk, fixed_bytes);
		thrs[3] = bench_pty_stream("Multibyte UTF-8 (2/3-byte)",    p_utf8,      strlen(p_utf8),      1, 65536, (double)target_ms, min_bulk, fixed_bytes);
		thrs[4] = bench_pty_stream("Dense CSI escapes (SGR/EL)",    p_csi_dense, strlen(p_csi_dense), 1, 65536, (double)target_ms, min_bulk, fixed_bytes);
		nthrs = 5;
	}

	/* Clear screen and restore cooked mode before printing final report */
	write_all(STDOUT_FILENO, "\033[2J\033[H", 7);
	restore_term();

	FILE *fps[2] = { stdout, NULL };
	int nfps = 1;
	if (outpath) {
		fps[1] = fopen(outpath, "w");
		if (!fps[1]) {
			perror(outpath);
		} else {
			nfps = 2;
		}
	}

	const char *term_env = getenv("TERM");
	for (int fi = 0; fi < nfps; fi++) {
		FILE *fp = fps[fi];
		if (fixed_mb > 0) {
			fprintf(fp, "termbench: TERM=%s  grid=%dx%d  pixels=%ux%u  frames=%d  payload=%d MB (raw ~OPOST)\n\n",
			        term_env ? term_env : "(unset)", cols, rows, ws.ws_xpixel, ws.ws_ypixel, frames, fixed_mb);
		} else {
			fprintf(fp, "termbench: TERM=%s  grid=%dx%d  pixels=%ux%u  frames=%d  target=%d ms (raw ~OPOST)\n\n",
			        term_env ? term_env : "(unset)", cols, rows, ws.ws_xpixel, ws.ws_ypixel, frames, target_ms);
		}

		if (nprobes > 0) {
			fprintf(fp, "┌────────────────────────┬────────┬──────────┬────────────────────────────────────────┐\n");
			fprintf(fp, "│ Capability Probe       │ Status │ Latency  │ Reply / Notes                          │\n");
			fprintf(fp, "├────────────────────────┼────────┼──────────┼────────────────────────────────────────┤\n");
			for (int i = 0; i < nprobes; i++) {
				fprintf(fp, "│ %-22s │ %-6s │ %6.2fms │ %-38.38s │\n",
				        probes[i].name, probes[i].ok ? "OK" : "MISS", probes[i].ms, probes[i].detail);
			}
			fprintf(fp, "└────────────────────────┴────────┴──────────┴────────────────────────────────────────┘\n\n");
		}

		fprintf(fp, "┌───────────────────────────┬─────────┬────────────┬────────────┬─────────┬────────┬────────┬────────┐\n");
		fprintf(fp, "│ PTY Read Speed            │ Payload │ Write MB/s │ Drain MB/s │ Tail ms │ Wr p50 │ Wr p99 │ Wr max │\n");
		fprintf(fp, "├───────────────────────────┼─────────┼────────────┼────────────┼─────────┼────────┼────────┼────────┤\n");
		for (int i = 0; i < 7; i++) {
			char mb_s[16], tail_s[16], p50_s[16], p99_s[16], max_s[16];
			fmt_mb7(mb_s, sizeof(mb_s), ptys[i].bytes);
			fmt_ms7(tail_s, sizeof(tail_s), ptys[i].tail_ms);
			fmt_ms6(p50_s, sizeof(p50_s), ptys[i].wr_p50_ms);
			fmt_ms6(p99_s, sizeof(p99_s), ptys[i].wr_p99_ms);
			fmt_ms6(max_s, sizeof(max_s), ptys[i].wr_max_ms);
			if (ptys[i].ok) {
				fprintf(fp, "│ %-25s │ %7s │ %10.1f │ %10.1f │ %7s │ %6s │ %6s │ %6s │\n",
				        ptys[i].name, mb_s, ptys[i].write_mb_s, ptys[i].drain_mb_s,
				        tail_s, p50_s, p99_s, max_s);
			} else {
				fprintf(fp, "│ %-25s │ %7s │ %10.1f │    TIMEOUT │   --    │ %6s │ %6s │ %6s │\n",
				        ptys[i].name, mb_s, ptys[i].write_mb_s, p50_s, p99_s, max_s);
			}
		}
		fprintf(fp, "└───────────────────────────┴─────────┴────────────┴────────────┴─────────┴────────┴────────┴────────┘\n");
		fprintf(fp, "  (Write=slave PTY ingest; Drain=CPR-verified read+parse; Tail=post-write drain ms; Wr p50/p99/max=per-write stall ms)\n\n");

		if (nlats > 0) {
			fprintf(fp, "┌───────────────────────────────┬────────┬────────┬────────┬────────┬────────┬─────────┐\n");
			fprintf(fp, "│ Latency Benchmark (CPR)       │  min   │  p50   │  p95   │  p99   │  max   │  ops/s  │\n");
			fprintf(fp, "├───────────────────────────────┼────────┼────────┼────────┼────────┼────────┼─────────┤\n");
			for (int i = 0; i < nlats; i++) {
				if (lats[i].ok) {
					fprintf(fp, "│ %-29s │ %6.2f │ %6.2f │ %6.2f │ %6.2f │ %6.2f │ %7.1f │\n",
					        lats[i].name, lats[i].min_ms, lats[i].p50_ms, lats[i].p95_ms,
					        lats[i].p99_ms, lats[i].max_ms, lats[i].ops_s);
				} else {
					fprintf(fp, "│ %-29s │ %6.2f │ %6.2f │ %6.2f │ %6.2f │ %6.2f │ TIMEOUT │\n",
					        lats[i].name, lats[i].min_ms, lats[i].p50_ms, lats[i].p95_ms,
					        lats[i].p99_ms, lats[i].max_ms);
				}
			}
			fprintf(fp, "└───────────────────────────────┴────────┴────────┴────────┴────────┴────────┴─────────┘\n");
			fprintf(fp, "  (all latency columns in ms; CPR proves PTY drain+parse+reply and captures draw backpressure)\n\n");
		}

		if (nthrs > 0) {
			fprintf(fp, "┌────────────────────────────────┬─────────┬──────────┬────────────┬───────────┬────────┐\n");
			fprintf(fp, "│ Streaming Throughput (primary) │ Payload │  Wall ms │ Drain MB/s │  klines/s │ Wr p99 │\n");
			fprintf(fp, "├────────────────────────────────┼─────────┼──────────┼────────────┼───────────┼────────┤\n");
			for (int i = 0; i < nthrs; i++) {
				char mb_s[16], p99_s[16];
				fmt_mb7(mb_s, sizeof(mb_s), thrs[i].bytes);
				fmt_ms6(p99_s, sizeof(p99_s), thrs[i].wr_p99_ms);
				if (thrs[i].ok) {
					fprintf(fp, "│ %-30s │ %7s │ %8.1f │ %10.1f │ %9.1f │ %6s │\n",
					        thrs[i].name, mb_s, thrs[i].drain_ms, thrs[i].drain_mb_s,
					        thrs[i].klines_s, p99_s);
				} else {
					fprintf(fp, "│ %-30s │ %7s │ %8.1f │    TIMEOUT │    --     │ %6s │\n",
					        thrs[i].name, mb_s, thrs[i].drain_ms, p99_s);
				}
			}
			fprintf(fp, "└────────────────────────────────┴─────────┴──────────┴────────────┴───────────┴────────┘\n");
		}

		if (want_json) {
			fprintf(fp, "\n{\"term\":");
			json_str(fp, term_env ? term_env : "");
			fprintf(fp, ",\"cols\":%d,\"rows\":%d,\"probes\":[", cols, rows);
			for (int i = 0; i < nprobes; i++) {
				fprintf(fp, "%s{\"name\":", i ? "," : "");
				json_str(fp, probes[i].name);
				fprintf(fp, ",\"ok\":%d,\"ms\":%.3f}", probes[i].ok, probes[i].ms);
			}
			fprintf(fp, "],\"pty_read\":[");
			for (int i = 0; i < 7; i++) {
				fprintf(fp, "%s{\"name\":", i ? "," : "");
				json_str(fp, ptys[i].name);
				fprintf(fp, ",\"ok\":%d,\"chunk_bytes\":%zu,\"mb\":%.2f,"
				        "\"write_ms\":%.2f,\"drain_ms\":%.2f,\"tail_ms\":%.2f,"
				        "\"write_mb_s\":%.2f,\"drain_mb_s\":%.2f,"
				        "\"wr_p50_ms\":%.3f,\"wr_p99_ms\":%.3f,\"wr_max_ms\":%.3f}",
				        ptys[i].ok, ptys[i].chunk_bytes, ptys[i].bytes / (1024.0 * 1024.0),
				        ptys[i].write_ms, ptys[i].drain_ms, ptys[i].tail_ms,
				        ptys[i].write_mb_s, ptys[i].drain_mb_s,
				        ptys[i].wr_p50_ms, ptys[i].wr_p99_ms, ptys[i].wr_max_ms);
			}
			fprintf(fp, "],\"latency\":[");
			for (int i = 0; i < nlats; i++) {
				fprintf(fp, "%s{\"name\":", i ? "," : "");
				json_str(fp, lats[i].name);
				fprintf(fp, ",\"ok\":%d,\"min\":%.3f,\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f,\"ops_s\":%.1f}",
				        lats[i].ok, lats[i].min_ms, lats[i].p50_ms,
				        lats[i].p95_ms, lats[i].p99_ms, lats[i].max_ms, lats[i].ops_s);
			}
			fprintf(fp, "],\"throughput\":[");
			for (int i = 0; i < nthrs; i++) {
				fprintf(fp, "%s{\"name\":", i ? "," : "");
				json_str(fp, thrs[i].name);
				fprintf(fp, ",\"ok\":%d,\"mb\":%.2f,\"wall_ms\":%.2f,\"drain_mb_s\":%.2f,"
				        "\"klines_s\":%.2f,\"wr_p99_ms\":%.3f}",
				        thrs[i].ok, thrs[i].bytes / (1024.0 * 1024.0),
				        thrs[i].drain_ms, thrs[i].drain_mb_s,
				        thrs[i].klines_s, thrs[i].wr_p99_ms);
			}
			fprintf(fp, "]}\n");
		}
		fflush(fp);
	}
	if (nfps == 2) fclose(fps[1]);
	return 0;
}
