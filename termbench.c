/*
 * termbench - in-terminal latency, throughput, and capability benchmark
 *
 * Build: cc -std=c99 -O2 -Wall -Wextra -o termbench termbench.c
 * Usage: ./termbench [--quick | --full] [--frames N] [--mb N] [--json]
 *
 * Measures terminal performance from inside the running terminal emulator
 * using VT query barriers (CPR CSI 6 n) so timings include terminal PTY
 * consumption and parsing, plus draw/X11 backpressure on sustained frames.
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
	while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
		if (read(STDIN_FILENO, tmp, sizeof(tmp)) <= 0)
			break;
	}
}

/* Read until `term_str` appears in reply, or `timeout_ms` expires. */
static int read_reply(char *out, size_t cap, const char *term_str, int timeout_ms) {
	size_t len = 0;
	double deadline = now_ms() + timeout_ms;
	out[0] = '\0';

	while (len + 1 < cap) {
		double rem = deadline - now_ms();
		if (rem <= 0)
			break;
		struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
		int r = poll(&pfd, 1, (int)rem + 1);
		if (r <= 0)
			break;
		ssize_t n = read(STDIN_FILENO, out + len, cap - 1 - len);
		if (n <= 0)
			break;
		len += (size_t)n;
		out[len] = '\0';
		if (term_str && strstr(out, term_str))
			return (int)len;
	}
	return (int)len;
}

/* Read a CPR reply ("\033[<row>;<col>R"), returning 1 on success. */
static int wait_cpr(int *row, int *col, int timeout_ms) {
	char buf[128];
	size_t len = 0;
	double deadline = now_ms() + timeout_ms;
	buf[0] = '\0';

	while (len + 1 < sizeof(buf)) {
		double rem = deadline - now_ms();
		if (rem <= 0)
			return 0;
		struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
		int r = poll(&pfd, 1, (int)rem + 1);
		if (r <= 0)
			return 0;
		ssize_t n = read(STDIN_FILENO, buf + len, sizeof(buf) - 1 - len);
		if (n <= 0)
			return 0;
		len += (size_t)n;
		buf[len] = '\0';
		char *esc = strrchr(buf, '\033');
		if (esc && esc[1] == '[') {
			int rr = 0, cc = 0;
			char fin = 0;
			if (sscanf(esc + 2, "%d;%d%c", &rr, &cc, &fin) == 3 && fin == 'R') {
				if (row) *row = rr;
				if (col) *col = cc;
				return 1;
			}
		}
	}
	return 0;
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

typedef struct {
	const char *name;
	int ok;
	double ms;
	char detail[64];
} ProbeRes;

typedef struct {
	const char *name;
	int frames;
	double min_ms, p50_ms, p95_ms, p99_ms, max_ms, avg_ms, fps;
} LatRes;

typedef struct {
	const char *name;
	size_t bytes;
	long lines;
	double wall_ms;
	double mb_s;
	double klines_s;
} ThrRes;

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

static ProbeRes run_probe(const char *name, const char *seq, const char *term_str, int timeout_ms) {
	ProbeRes r;
	char buf[256];
	memset(&r, 0, sizeof(r));
	r.name = name;
	drain_stdin();
	double t0 = now_ms();
	write_all(STDOUT_FILENO, seq, strlen(seq));
	int n = read_reply(buf, sizeof(buf), term_str, timeout_ms);
	r.ms = now_ms() - t0;
	if (n > 0 && (!term_str || strstr(buf, term_str))) {
		r.ok = 1;
		sanitize(buf, r.detail, sizeof(r.detail));
	} else {
		r.ok = 0;
		snprintf(r.detail, sizeof(r.detail), "no reply (timeout)");
	}
	return r;
}

static void compute_stats(LatRes *res, double *samples, int n, double total_ms) {
	qsort(samples, (size_t)n, sizeof(double), cmp_double);
	res->frames = n;
	res->min_ms = samples[0];
	res->p50_ms = samples[n / 2];
	res->p95_ms = samples[(int)((n - 1) * 0.95)];
	res->p99_ms = samples[(int)((n - 1) * 0.99)];
	res->max_ms = samples[n - 1];
	res->avg_ms = total_ms / n;
	res->fps = (total_ms > 0) ? (n * 1000.0 / total_ms) : 0.0;
}

/* Build a full-screen plain ASCII frame */
static char *build_ascii_frame(int cols, int rows, int frame, size_t *outlen) {
	size_t cap = (size_t)rows * (size_t)(cols + 32) + 64;
	char *buf = malloc(cap);
	size_t o = 0;
	o += (size_t)snprintf(buf + o, cap - o, "\033[H");
	for (int r = 0; r < rows; r++) {
		int written = snprintf(buf + o, cap - o,
		    "2026-11-26 08:00:%02d INFO  frame=%04d row=%02d item=%06d latency=1.24ms status=OK ",
		    (r + frame) % 60, frame, r, (frame * 131 + r * 17) % 1000000);
		if (written > cols) written = cols;
		o += (size_t)written;
		for (int c = written; c < cols; c++)
			buf[o++] = (c < cols - 8) ? (char)('a' + ((c + r + frame) % 26)) : ' ';
		if (r + 1 < rows) { buf[o++] = '\r'; buf[o++] = '\n'; }
	}
	*outlen = o;
	return buf;
}

/* Build a full-screen Unicode box-drawing Markdown table (plain or truecolor SGR) */
static char *build_table_frame(int cols, int rows, int frame, int colored, int sync_wrap, size_t *outlen) {
	int ncols = (cols >= 120) ? 5 : (cols >= 64 ? 4 : 3);
	int cw = (cols - ncols - 1) / ncols;
	if (cw < 8) cw = 8;
	int total_w = ncols * (cw + 1) + 1;
	if (total_w > cols) total_w = cols;

	size_t cap = (size_t)rows * (size_t)(cols * 4 + 256) + 256;
	char *buf = malloc(cap);
	size_t o = 0;
	if (sync_wrap) o += (size_t)snprintf(buf + o, cap - o, "\033[?2026h");
	o += (size_t)snprintf(buf + o, cap - o, "\033[H");

	const char *bdr_on  = colored ? "\033[38;2;100;110;130m" : "";
	const char *bdr_off = colored ? "\033[0m" : "";
	const char *c_cols[5] = {
		"\033[1;36m", "\033[32m", "\033[33m", "\033[38;2;120;200;255m", "\033[37m"
	};

	for (int r = 0; r < rows; r++) {
		int is_top = (r == 0);
		int is_bot = (r == rows - 1);
		int is_sep = (!is_top && !is_bot && (r == 2 || (r % 6 == 0)));
		if (is_top || is_bot || is_sep) {
			const char *l = is_top ? "┌" : (is_bot ? "└" : "├");
			const char *m = is_top ? "┬" : (is_bot ? "┴" : "┼");
			const char *e = is_top ? "┐" : (is_bot ? "┘" : "┤");
			o += (size_t)snprintf(buf + o, cap - o, "%s%s", bdr_on, l);
			for (int k = 0; k < ncols; k++) {
				for (int j = 0; j < cw; j++) {
					memcpy(buf + o, "─", 3); o += 3;
				}
				const char *sep = (k + 1 < ncols) ? m : e;
				memcpy(buf + o, sep, 3); o += 3;
			}
			o += (size_t)snprintf(buf + o, cap - o, "%s", bdr_off);
		} else {
			for (int k = 0; k < ncols; k++) {
				char cell[128];
				int clen;
				if (r == 1)
					clen = snprintf(cell, sizeof(cell), " Col_%d", k + 1);
				else if (k == 0)
					clen = snprintf(cell, sizeof(cell), " metric_%02d_%03d", r, (frame + r) % 1000);
				else if (k == 1)
					clen = snprintf(cell, sizeof(cell), " %6.2f ms", ((frame + r * 7) % 500) * 0.13);
				else if (k == 2)
					clen = snprintf(cell, sizeof(cell), " %6.2f MB/s", 50.0 + ((frame + r) % 200));
				else if (k == 3)
					clen = snprintf(cell, sizeof(cell), " PASS");
				else
					clen = snprintf(cell, sizeof(cell), " row=%d f=%d", r, frame);
				if (clen > cw) clen = cw;
				o += (size_t)snprintf(buf + o, cap - o, "%s│%s%s%.*s",
				                      bdr_on, bdr_off, colored ? c_cols[k % 5] : "", clen, cell);
				for (int j = clen; j < cw; j++) buf[o++] = ' ';
				if (colored) { memcpy(buf + o, "\033[0m", 4); o += 4; }
			}
			o += (size_t)snprintf(buf + o, cap - o, "%s│%s", bdr_on, bdr_off);
		}
		if (r + 1 < rows) { buf[o++] = '\r'; buf[o++] = '\n'; }
	}
	if (sync_wrap) o += (size_t)snprintf(buf + o, cap - o, "\033[?2026l");
	*outlen = o;
	return buf;
}

static LatRes bench_idle_cpr(int n) {
	LatRes res = { .name = "Idle CPR RTT (CSI 6 n)" };
	double *s = malloc((size_t)n * sizeof(double));
	sync_cpr(1000);
	double tot0 = now_ms();
	for (int i = 0; i < n; i++) {
		double t0 = now_ms();
		sync_cpr(1000);
		s[i] = now_ms() - t0;
	}
	compute_stats(&res, s, n, now_ms() - tot0);
	free(s);
	return res;
}

static LatRes bench_single_char(int n, int cols, int rows) {
	LatRes res = { .name = "1-char edit + CPR" };
	double *s = malloc((size_t)n * sizeof(double));
	char seq[64];
	sync_cpr(1000);
	double tot0 = now_ms();
	for (int i = 0; i < n; i++) {
		int len = snprintf(seq, sizeof(seq), "\033[%d;%dH%c\033[6n",
		                   (rows / 2) + 1, 2 + (i % (cols > 4 ? cols - 4 : 1)),
		                   (char)('A' + (i % 26)));
		double t0 = now_ms();
		write_all(STDOUT_FILENO, seq, (size_t)len);
		wait_cpr(NULL, NULL, 1000);
		s[i] = now_ms() - t0;
	}
	compute_stats(&res, s, n, now_ms() - tot0);
	free(s);
	return res;
}

static LatRes bench_two_corners(int n, int cols, int rows) {
	LatRes res = { .name = "2-corner edit (1,1)&(C,R)" };
	double *s = malloc((size_t)n * sizeof(double));
	char seq[96];
	sync_cpr(1000);
	double tot0 = now_ms();
	for (int i = 0; i < n; i++) {
		char ch = (i & 1) ? '*' : '+';
		int len = snprintf(seq, sizeof(seq), "\033[1;1H%c\033[%d;%dH%c\033[6n",
		                   ch, rows, cols, ch);
		double t0 = now_ms();
		write_all(STDOUT_FILENO, seq, (size_t)len);
		wait_cpr(NULL, NULL, 1000);
		s[i] = now_ms() - t0;
	}
	compute_stats(&res, s, n, now_ms() - tot0);
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
	} else {
		int colored = (mode >= 2);
		int sync_w  = (mode == 3);
		f0 = build_table_frame(cols, rows, 0, colored, sync_w, &len0);
		f1 = build_table_frame(cols, rows, 1, colored, sync_w, &len1);
	}
	write_all(STDOUT_FILENO, f0, len0);
	sync_cpr(2000);

	double tot0 = now_ms();
	for (int i = 0; i < n; i++) {
		const char *fb = (i & 1) ? f1 : f0;
		size_t fl = (i & 1) ? len1 : len0;
		double t0 = now_ms();
		write_all(STDOUT_FILENO, fb, fl);
		sync_cpr(2000);
		s[i] = now_ms() - t0;
	}
	compute_stats(&res, s, n, now_ms() - tot0);
	free(f0); free(f1); free(s);
	return res;
}

static LatRes bench_fixed_footer_scroll(int n, int cols, int rows) {
	LatRes res = { .name = "Scroll 1 row w/ 10-row footer" };
	double *s = malloc((size_t)n * sizeof(double));
	int body = (rows > 14) ? (rows - 10) : (rows > 4 ? rows - 2 : rows);
	size_t flen;
	char *init = build_table_frame(cols, rows, 0, 1, 0, &flen);
	write_all(STDOUT_FILENO, init, flen);
	free(init);

	char setup_sr[64];
	int slen = snprintf(setup_sr, sizeof(setup_sr), "\033[1;%dr\033[%d;1H", body, body);
	write_all(STDOUT_FILENO, setup_sr, (size_t)slen);
	sync_cpr(2000);

	char rowbuf[512];
	double tot0 = now_ms();
	for (int i = 0; i < n; i++) {
		int rlen = snprintf(rowbuf, sizeof(rowbuf),
		    "\r\n\033[38;2;100;110;130m│\033[0m \033[1;36mStreamedRow_%05d\033[0m "
		    "\033[38;2;100;110;130m│\033[0m \033[32m  %6.2f ms\033[0m "
		    "\033[38;2;100;110;130m│\033[0m \033[33mPASS\033[0m "
		    "\033[38;2;100;110;130m│\033[0m\033[K\033[6n",
		    i, (i % 100) * 0.17);
		double t0 = now_ms();
		write_all(STDOUT_FILENO, rowbuf, (size_t)rlen);
		wait_cpr(NULL, NULL, 2000);
		s[i] = now_ms() - t0;
	}
	compute_stats(&res, s, n, now_ms() - tot0);
	write_all(STDOUT_FILENO, "\033[r", 3);
	free(s);
	return res;
}

static ThrRes bench_stream(const char *name, const char *pattern, int lines_per_pat, size_t target_bytes) {
	ThrRes r;
	memset(&r, 0, sizeof(r));
	r.name = name;

	size_t plen = strlen(pattern);
	/* Build a 64 KB staging chunk */
	size_t chunk_cap = 65536;
	size_t chunk_len = 0;
	long chunk_lines = 0;
	char *chunk = malloc(chunk_cap + plen);
	while (chunk_len + plen <= chunk_cap) {
		memcpy(chunk + chunk_len, pattern, plen);
		chunk_len += plen;
		chunk_lines += lines_per_pat;
	}

	sync_cpr(2000);
	double t0 = now_ms();
	size_t sent = 0;
	long lines = 0;
	while (sent < target_bytes) {
		size_t n = (target_bytes - sent >= chunk_len) ? chunk_len : ((target_bytes - sent) / plen) * plen;
		if (n == 0) break;
		write_all(STDOUT_FILENO, chunk, n);
		sent += n;
		lines += (long)(n / plen) * lines_per_pat;
	}
	sync_cpr(10000);
	double dt = now_ms() - t0;
	free(chunk);

	r.bytes = sent;
	r.lines = lines;
	r.wall_ms = dt;
	r.mb_s = (dt > 0) ? ((sent / (1024.0 * 1024.0)) / (dt / 1000.0)) : 0.0;
	r.klines_s = (dt > 0) ? ((lines / 1000.0) / (dt / 1000.0)) : 0.0;
	return r;
}

static void usage(const char *prog) {
	fprintf(stderr,
	    "usage: %s [--quick | --full] [--frames N] [--mb N] [--json] [--out FILE]\n"
	    "  --quick     fast run (30 frames, 2 MB streams)\n"
	    "  --full      default run (80 frames, 6 MB streams)\n"
	    "  --frames N  override latency benchmark frame count\n"
	    "  --mb N      override throughput payload size in MB\n"
	    "  --json      emit machine-readable JSON after the tables\n"
	    "  --out FILE  also write report output to FILE\n", prog);
}

int main(int argc, char **argv) {
	int frames = 80;
	int mb = 6;
	int want_json = 0;
	const char *outpath = NULL;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--quick")) { frames = 30; mb = 2; }
		else if (!strcmp(argv[i], "--full")) { frames = 80; mb = 6; }
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc) { frames = atoi(argv[++i]); }
		else if (!strcmp(argv[i], "--mb") && i + 1 < argc) { mb = atoi(argv[++i]); }
		else if (!strcmp(argv[i], "--json")) { want_json = 1; }
		else if (!strcmp(argv[i], "--out") && i + 1 < argc) { outpath = argv[++i]; }
		else { usage(argv[0]); return 2; }
	}
	if (frames < 5) frames = 5;
	if (mb < 1) mb = 1;

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

	/* 1. Capability & probe-latency checks */
	ProbeRes probes[7];
	probes[0] = run_probe("DA1 (CSI c)",           "\033[c",                      "c",   150);
	probes[1] = run_probe("DA2 (CSI > c)",         "\033[>c",                     "c",   150);
	probes[2] = run_probe("DECRQM ?2026 (Sync)",   "\033[?2026$p",                "$y",  150);
	probes[3] = run_probe("XTVERSION (CSI > 0 q)", "\033[>0q",                    "\033\\", 150);
	probes[4] = run_probe("XTGETTCAP (RGB)",       "\033P+q524742\033\\",         "\033\\", 150);
	probes[5] = run_probe("OSC 11 (bg color)",     "\033]11;?\007",               "\007", 150);
	/* Kitty ?u cursor-safety check: move to (5,10), send CSI ? u, check CPR */
	{
		drain_stdin();
		int rr = 0, cc = 0;
		double t0 = now_ms();
		write_all(STDOUT_FILENO, "\033[1;1H\033[s\033[5;10H\033[?u\033[6n", 24);
		int ok = wait_cpr(&rr, &cc, 150);
		probes[6].name = "CSI ? u cursor safety";
		probes[6].ms = now_ms() - t0;
		probes[6].ok = (ok && rr == 5 && cc == 10);
		snprintf(probes[6].detail, sizeof(probes[6].detail),
		         ok ? ((rr == 5 && cc == 10) ? "cursor safe (5,10)" : "CORRUPTED cursor -> (%d,%d)") : "timeout",
		         rr, cc);
	}

	/* 2. Latency benchmarks (on alternate screen) */
	LatRes lats[8];
	lats[0] = bench_idle_cpr(frames);
	lats[1] = bench_single_char(frames, cols, rows);
	lats[2] = bench_two_corners(frames, cols, rows);
	lats[3] = bench_frames("Full-screen ASCII",          frames, cols, rows, 0);
	lats[4] = bench_frames("Full-screen Box table",      frames, cols, rows, 1);
	lats[5] = bench_frames("Full-screen Box+SGR table",  frames, cols, rows, 2);
	lats[6] = bench_fixed_footer_scroll(frames, cols, rows);
	lats[7] = bench_frames("Sync (?2026) Box+SGR table", frames, cols, rows, 3);

	/* Leave alternate screen for streaming throughput (exercises scrollback too) */
	write_all(STDOUT_FILENO, "\033[?1049l", 8);
	in_alt = 0;

	size_t target = (size_t)mb * 1024u * 1024u;
	const char *p_ascii =
	    "2026-11-26 08:00:00 INFO  worker[04] processed item_id=9827341 latency=1.24ms status=OK result=pass\r\n";
	const char *p_md_sgr =
	    "\033[2m|\033[0m \033[1;36mMetricName          \033[0m \033[2m|\033[0m \033[32m   12.45 ms\033[0m \033[2m|\033[0m \033[33m   48.10 ms\033[0m \033[2m|\033[0m \033[38;2;120;200;255mPASS  \033[0m \033[2m|\033[0m\r\n"
	    "\033[2m|----------------------|-------------|-------------|--------|\033[0m\r\n";
	const char *p_box =
	    "┌──────────────────────┬─────────────┬─────────────┬────────┐\r\n"
	    "│ MetricName           │    12.45 ms │    48.10 ms │ PASS   │\r\n"
	    "├──────────────────────┼─────────────┼─────────────┼────────┤\r\n"
	    "│ AnotherMetric        │     8.12 ms │    19.80 ms │ PASS   │\r\n";
	const char *p_box_sgr =
	    "\033[38;2;100;110;130m├──────────────────────┼─────────────┼─────────────┼────────┤\033[0m\r\n"
	    "\033[38;2;100;110;130m│\033[0m \033[1;36mMetricName          \033[0m \033[38;2;100;110;130m│\033[0m \033[32m   12.45 ms\033[0m \033[38;2;100;110;130m│\033[0m \033[33m   48.10 ms\033[0m \033[38;2;100;110;130m│\033[0m \033[38;2;120;200;255mPASS  \033[0m \033[38;2;100;110;130m│\033[0m\r\n";
	const char *p_box_rule =
	    "├──────────────────────────────────────────────────────────────────────────────────────────────────┤\r\n";
	const char *p_box_rep =
	    "├─\033[97b┤\r\n";

	ThrRes thrs[6];
	thrs[0] = bench_stream("Plain ASCII log (100 cols)",   p_ascii,    1, target);
	thrs[1] = bench_stream("ASCII+SGR Markdown table",     p_md_sgr,   2, target);
	thrs[2] = bench_stream("Unicode Box Markdown table",   p_box,      4, target);
	thrs[3] = bench_stream("Unicode Box+SGR table",        p_box_sgr,  2, target);
	thrs[4] = bench_stream("Box horizontal rules (UTF-8)", p_box_rule, 1, target);
	thrs[5] = bench_stream("Box horizontal rules (REP)",   p_box_rep,  1, target / 8);

	/* Clear screen and restore cooked mode before printing final report */
	write_all(STDOUT_FILENO, "\033[2J\033[H", 7);
	restore_term();

	FILE *fps[2] = { stdout, NULL };
	int nfps = 1;
	if (outpath) {
		fps[1] = fopen(outpath, "w");
		if (fps[1]) nfps = 2;
	}

	const char *term_env = getenv("TERM");
	for (int fi = 0; fi < nfps; fi++) {
		FILE *fp = fps[fi];
		fprintf(fp, "termbench: TERM=%s  grid=%dx%d  pixels=%ux%u  frames=%d  stream=%d MB\n\n",
		        term_env ? term_env : "(unset)", cols, rows, ws.ws_xpixel, ws.ws_ypixel, frames, mb);

		fprintf(fp, "┌────────────────────────┬────────┬──────────┬────────────────────────────────────────┐\n");
		fprintf(fp, "│ Capability Probe       │ Status │ Latency  │ Reply / Notes                          │\n");
		fprintf(fp, "├────────────────────────┼────────┼──────────┼────────────────────────────────────────┤\n");
		for (int i = 0; i < 7; i++) {
			fprintf(fp, "│ %-22s │ %-6s │ %6.2fms │ %-38.38s │\n",
			        probes[i].name, probes[i].ok ? "OK" : "MISS", probes[i].ms, probes[i].detail);
		}
		fprintf(fp, "└────────────────────────┴────────┴──────────┴────────────────────────────────────────┘\n\n");

		fprintf(fp, "┌───────────────────────────────┬────────┬────────┬────────┬────────┬────────┬─────────┐\n");
		fprintf(fp, "│ Latency Benchmark (CPR)       │  min   │  p50   │  p95   │  p99   │  max   │   FPS   │\n");
		fprintf(fp, "├───────────────────────────────┼────────┼────────┼────────┼────────┼────────┼─────────┤\n");
		for (int i = 0; i < 8; i++) {
			fprintf(fp, "│ %-29s │ %6.2f │ %6.2f │ %6.2f │ %6.2f │ %6.2f │ %7.1f │\n",
			        lats[i].name, lats[i].min_ms, lats[i].p50_ms, lats[i].p95_ms,
			        lats[i].p99_ms, lats[i].max_ms, lats[i].fps);
		}
		fprintf(fp, "└───────────────────────────────┴────────┴────────┴────────┴────────┴────────┴─────────┘\n");
		fprintf(fp, "  (all latency columns in ms; CPR proves parse+reply and captures draw backpressure)\n\n");

		fprintf(fp, "┌──────────────────────────────┬─────────┬──────────┬──────────┬───────────┐\n");
		fprintf(fp, "│ Streaming Throughput (sync)  │ Payload │  Wall ms │   MB/s   │  klines/s │\n");
		fprintf(fp, "├──────────────────────────────┼─────────┼──────────┼──────────┼───────────┤\n");
		for (int i = 0; i < 6; i++) {
			fprintf(fp, "│ %-28s │ %5.1fMB │ %8.1f │ %8.1f │ %9.1f │\n",
			        thrs[i].name, thrs[i].bytes / (1024.0 * 1024.0),
			        thrs[i].wall_ms, thrs[i].mb_s, thrs[i].klines_s);
		}
		fprintf(fp, "└──────────────────────────────┴─────────┴──────────┴──────────┴───────────┘\n");

		if (want_json) {
			fprintf(fp, "\n{\"term\":\"%s\",\"cols\":%d,\"rows\":%d,\"probes\":[",
			        term_env ? term_env : "", cols, rows);
			for (int i = 0; i < 7; i++)
				fprintf(fp, "%s{\"name\":\"%s\",\"ok\":%d,\"ms\":%.3f}", i ? "," : "",
				        probes[i].name, probes[i].ok, probes[i].ms);
			fprintf(fp, "],\"latency\":[");
			for (int i = 0; i < 8; i++)
				fprintf(fp, "%s{\"name\":\"%s\",\"min\":%.3f,\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f,\"fps\":%.1f}",
				        i ? "," : "", lats[i].name, lats[i].min_ms, lats[i].p50_ms,
				        lats[i].p95_ms, lats[i].p99_ms, lats[i].max_ms, lats[i].fps);
			fprintf(fp, "],\"throughput\":[");
			for (int i = 0; i < 6; i++)
				fprintf(fp, "%s{\"name\":\"%s\",\"mb\":%.2f,\"wall_ms\":%.2f,\"mb_s\":%.2f,\"klines_s\":%.2f}",
				        i ? "," : "", thrs[i].name, thrs[i].bytes / (1024.0 * 1024.0),
				        thrs[i].wall_ms, thrs[i].mb_s, thrs[i].klines_s);
			fprintf(fp, "]}\n");
		}
		fflush(fp);
	}
	if (nfps == 2) fclose(fps[1]);
	return 0;
}
