/* See LICENSE for license details. */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

#include "autocomplete.h"
#include "st.h"
#include "win.h"

#if   defined(__linux)
 #include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
 #include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
 #include <libutil.h>
#endif

/* Arbitrary sizes */
#define UTF_INVALID   0xFFFD
#define UTF_SIZ       4
#define ESC_BUF_SIZ   (128*UTF_SIZ)
#define ESC_ARG_SIZ   16
#define STR_BUF_SIZ   ESC_BUF_SIZ
#define STR_ARG_SIZ   ESC_ARG_SIZ
#ifndef HISTSIZE
#define HISTSIZE      1000000 // scrollback limit
#endif

/* macros */
#define IS_SET(flag)		((term.mode & (flag)) != 0)
#define ISCONTROLC0(c)		(BETWEEN(c, 0, 0x1f) || (c) == 0x7f)
#define ISCONTROLC1(c)		(BETWEEN(c, 0x80, 0x9f))
#define ISCONTROL(c)		(ISCONTROLC0(c) || ISCONTROLC1(c))
#define ISDELIM(u)		(u && wcschr(worddelimiters, u))
#define TLINE(y)		((y) < term.scr ? thistline(((y) + term.histi - \
               term.scr + HISTSIZE + 1) % HISTSIZE) : \
               term.line[(y) - term.scr])

enum term_mode {
	MODE_WRAP        = 1 << 0,
	MODE_INSERT      = 1 << 1,
	MODE_ALTSCREEN   = 1 << 2,
	MODE_CRLF        = 1 << 3,
	MODE_ECHO        = 1 << 4,
	MODE_PRINT       = 1 << 5,
	MODE_UTF8        = 1 << 6,
};

enum cursor_movement {
	CURSOR_SAVE,
	CURSOR_LOAD
};

enum cursor_state {
	CURSOR_DEFAULT  = 0,
	CURSOR_WRAPNEXT = 1,
	CURSOR_ORIGIN   = 2
};

enum charset {
	CS_GRAPHIC0,
	CS_GRAPHIC1,
	CS_UK,
	CS_USA,
	CS_MULTI,
	CS_GER,
	CS_FIN
};

enum escape_state {
	ESC_START      = 1,
	ESC_CSI        = 2,
	ESC_STR        = 4,  /* DCS, OSC, PM, APC */
	ESC_ALTCHARSET = 8,
	ESC_STR_END    = 16, /* a final string was encountered */
	ESC_TEST       = 32, /* Enter in test mode */
	ESC_UTF8       = 64,
};

typedef struct {
	Glyph attr; /* current char attributes */
	int x;
	int y;
	char state;
} TCursor;

typedef struct {
	int mode;
	int type;
	int snap;
	/*
	 * Selection variables:
	 * nb – normalized coordinates of the beginning of the selection
	 * ne – normalized coordinates of the end of the selection
	 * ob – original coordinates of the beginning of the selection
	 * oe – original coordinates of the end of the selection
	 */
	struct {
		int x, y;
	} nb, ne, ob, oe;

	int alt;
} Selection;

/* Internal representation of the screen */
#define HIST_SLAB_BYTES 262144

enum hist_fmt {
	HIST_EMPTY      = 0,
	HIST_ASCII      = 1, /* raw ASCII bytes (1 B/cell), default fg/bg/mode=0 */
	HIST_ASCII_WRAP = 2, /* same as HIST_ASCII, with ATTR_WRAP on cell histcol[i]-1 */
	HIST_SGR_ASCII  = 3, /* packed HistRun headers + raw ASCII bytes */
	HIST_GLYPH      = 4, /* aligned Glyph[] array */
};

typedef struct {
	uint16_t count; /* number of ASCII cells in this run (>= 1) */
	uint16_t mode;  /* exact Glyph.mode */
	uint32_t fg;    /* exact Glyph.fg */
	uint32_t bg;    /* exact Glyph.bg */
} HistRun;

typedef struct HistSlab {
	size_t used;
	int refs;
	struct HistSlab *prev;
	struct HistSlab *next;
	union {
		Glyph align_glyph;
		uchar data[HIST_SLAB_BYTES];
	} u;
} HistSlab;

typedef struct {
	int row;      /* nb row */
	int col;      /* nb col */
	Line *line;   /* screen */
	Line *alt;    /* alternate screen */
	int *rowmax;      /* rightmost written column + 1 per row (conservative) */
	int *alt_rowmax;
	unsigned char *rowwide;     /* 1 if row may contain ATTR_WIDE/ATTR_WDUMMY */
	unsigned char *alt_rowwide;
    void *hist[HISTSIZE]; /* history payload (format in histfmt[]) */
    int histcol[HISTSIZE]; /* stored (trimmed) width of each history line */
    HistSlab *histslab[HISTSIZE]; /* slab owning hist[i], or NULL if heap */
    uchar histfmt[HISTSIZE]; /* enum hist_fmt */
    int histi;           /* history index */
    int scr;             /* history scroll back */
    int histwcount;      /* how many lines have been written */
	int *dirty;   /* dirtyness of lines */
	TCursor c;    /* cursor */
	int ocx;      /* old cursor col */
	int ocy;      /* old cursor row */
	int top;      /* top    scroll limit */
	int bot;      /* bottom scroll limit */
	int mode;     /* terminal mode flags */
	int esc;      /* escape state flags */
	char trantbl[4]; /* charset table translation */
	int charset;  /* current charset */
	int icharset; /* selected charset for sequence */
	int *tabs;
	Rune lastc;   /* last printed char outside of sequence, 0 if control */
	int has_blink; /* conservative flag: 0 means no cell has ATTR_BLINK */
} Term;

/* CSI Escape sequence structs */
/* ESC '[' [[ [<priv>] <arg> [;]] <mode> [<mode>]] */
typedef struct {
	char buf[ESC_BUF_SIZ]; /* raw string */
	size_t len;            /* raw string length */
	char priv;
	char sep;
	int arg[ESC_ARG_SIZ];
	int narg;              /* nb of args */
	char mode[2];
} CSIEscape;

/* STR Escape sequence structs */
/* ESC type [[ [<priv>] <arg> [;]] <mode>] ESC '\' */
typedef struct {
	char type;             /* ESC type ... */
	char *buf;             /* allocated raw string */
	size_t siz;            /* allocation size */
	size_t len;            /* raw string length */
	char *args[STR_ARG_SIZ];
	int narg;              /* nb of args */
} STREscape;

static void execsh(char *, char **);
static char *getcwd_by_pid(pid_t pid);
static void stty(char **);
static void sigchld(int);
static void ttywriteraw(const char *, size_t);

static void csidump(void);
static void csihandle(void);
static void csiparse(void);
static void csireset(void);
static void osc_color_response(int, int, int);
static int eschandle(uchar);
static void strdump(void);
static void strhandle(void);
static void strparse(void);
static void strreset(void);

static void tprinter(char *, size_t);
static void tdumpsel(void);
static void tdumpline(int);
static void tdump(void);
static void tclearregion(int, int, int, int);
static void tcursor(int);
static void tdeletechar(int);
static void tdeleteline(int);
static void tinsertblank(int);
static void tinsertblankline(int);
static int tlinelen(int);
static void tmoveto(int, int);
static void tmoveato(int, int);
static void tnewline(int);
static void tputtab(int);
static void tputc(Rune);
static void treset(void);
static void tscrollup(int, int, int);
static void tscrolldown(int, int, int);
static void tsetattr(const int *, int);
static void tsetchar(Rune, const Glyph *, int, int);
static void tsetdirt(int, int);
static void tsetscroll(int, int);
static void tswapscreen(void);
static void tsetmode(int, int, const int *, int);
static int twrite(const char *, int, int);
static void tfulldirt(void);
static void tcontrolcode(uchar );
static void tdectest(char );
static void tdefutf8(char);
static int32_t tdefcolor(const int *, int *, int);
static void tdeftran(char);
static void tstrsequence(uchar);
static Line thistline(int);
static Line tvisline(int, Glyph *);
static void thistclear(void);
static void thistunpack(int, Glyph *, int);

static void selnormalize(void);
static void selscroll(int, int);
static void selsnap(int *, int *, int);

static size_t utf8decode(const char *, Rune *, size_t);
static Rune utf8decodebyte(char, size_t *);
static char utf8encodebyte(Rune, size_t);
static size_t utf8validate(Rune *, size_t);

static char *base64dec(const char *);
static char base64dec_getc(const char **);

static ssize_t xwrite(int, const char *, size_t);

/* Globals */
static Term term;
static Selection sel;
static CSIEscape csiescseq;
static STREscape strescseq;
static int iofd = 1;
static int cmdfd;
static pid_t pid;
static char *ttywbuf = NULL;
static size_t ttywhead = 0;
static size_t ttywtail = 0;
static size_t ttywcap = 0;
static HistSlab *histslab_head = NULL;
static HistSlab *histslab_cur = NULL;
static HistSlab *histslab_spare = NULL;
static int hist_heap_count = 0;
static uchar hist_empty_line = 0;
static unsigned int xmode_shadow = 0;

static inline void
txsetmode(int set, unsigned int flags)
{
	MODBIT(xmode_shadow, set, flags);
	xsetmode(set, flags);
}

static const uchar utfbyte[UTF_SIZ + 1] = {0x80,    0, 0xC0, 0xE0, 0xF0};
static const uchar utfmask[UTF_SIZ + 1] = {0xC0, 0x80, 0xE0, 0xF0, 0xF8};
static const Rune utfmin[UTF_SIZ + 1] = {       0,    0,  0x80,  0x800,  0x10000};
static const Rune utfmax[UTF_SIZ + 1] = {0x10FFFF, 0x7F, 0x7FF, 0xFFFF, 0x10FFFF};

#include <time.h>
static int su = 0;
struct timespec sutv;

static void
tsync_begin()
{
	clock_gettime(CLOCK_MONOTONIC, &sutv);
	su = 1;
}

static void
tsync_end()
{
	su = 0;
}

int
tinsync(uint timeout)
{
	struct timespec now;
	if (su && !clock_gettime(CLOCK_MONOTONIC, &now)
	       && TIMEDIFF(now, sutv) >= timeout)
		su = 0;
	return su;
}

ssize_t
xwrite(int fd, const char *s, size_t len)
{
	size_t aux = len;
	ssize_t r;

	while (len > 0) {
		r = write(fd, s, len);
		if (r < 0)
			return r;
		len -= r;
		s += r;
	}

	return aux;
}

void *
xmalloc(size_t len)
{
	void *p;

	if (!(p = malloc(len)))
		die("malloc: %s\n", strerror(errno));

	return p;
}

void *
xrealloc(void *p, size_t len)
{
	if ((p = realloc(p, len)) == NULL)
		die("realloc: %s\n", strerror(errno));

	return p;
}

char *
xstrdup(const char *s)
{
	char *p;

	if ((p = strdup(s)) == NULL)
		die("strdup: %s\n", strerror(errno));

	return p;
}

size_t
utf8decode(const char *c, Rune *u, size_t clen)
{
	size_t i, j, len, type;
	Rune udecoded;

	*u = UTF_INVALID;
	if (!clen)
		return 0;
	/*
	 * Fast path: plain ASCII is the overwhelming majority of terminal
	 * output. It needs no utfmask table walk and is always valid, so
	 * skip straight out instead of running the general decoder.
	 */
	if (!((uchar)c[0] & 0x80)) {
		*u = (uchar)c[0];
		return 1;
	}
	udecoded = utf8decodebyte(c[0], &len);
	if (!BETWEEN(len, 1, UTF_SIZ))
		return 1;
	for (i = 1, j = 1; i < clen && j < len; ++i, ++j) {
		udecoded = (udecoded << 6) | utf8decodebyte(c[i], &type);
		if (type != 0)
			return j;
	}
	if (j < len)
		return 0;
	*u = udecoded;
	utf8validate(u, len);

	return len;
}

Rune
utf8decodebyte(char c, size_t *i)
{
	for (*i = 0; *i < LEN(utfmask); ++(*i))
		if (((uchar)c & utfmask[*i]) == utfbyte[*i])
			return (uchar)c & ~utfmask[*i];

	return 0;
}

size_t
utf8encode(Rune u, char *c)
{
	size_t len, i;

	len = utf8validate(&u, 0);
	if (len > UTF_SIZ)
		return 0;

	for (i = len - 1; i != 0; --i) {
		c[i] = utf8encodebyte(u, 0);
		u >>= 6;
	}
	c[0] = utf8encodebyte(u, len);

	return len;
}

char
utf8encodebyte(Rune u, size_t i)
{
	return utfbyte[i] | (u & ~utfmask[i]);
}

size_t
utf8validate(Rune *u, size_t i)
{
	if (!BETWEEN(*u, utfmin[i], utfmax[i]) || BETWEEN(*u, 0xD800, 0xDFFF))
		*u = UTF_INVALID;
	for (i = 1; *u > utfmax[i]; ++i)
		;

	return i;
}

char
base64dec_getc(const char **src)
{
	while (**src && !isprint((unsigned char)**src))
		(*src)++;
	return **src ? *((*src)++) : '=';  /* emulate padding if string ends */
}

char *
base64dec(const char *src)
{
	size_t in_len = strlen(src);
	char *result, *dst;
	static const char base64_digits[256] = {
		[43] = 62, 0, 0, 0, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61,
		0, 0, 0, -1, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
		13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 0, 0, 0, 0,
		0, 0, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
		40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51
	};

	if (in_len % 4)
		in_len += 4 - (in_len % 4);
	result = dst = xmalloc(in_len / 4 * 3 + 1);
	while (*src) {
		int a = base64_digits[(unsigned char) base64dec_getc(&src)];
		int b = base64_digits[(unsigned char) base64dec_getc(&src)];
		int c = base64_digits[(unsigned char) base64dec_getc(&src)];
		int d = base64_digits[(unsigned char) base64dec_getc(&src)];

		/* invalid input. 'a' can be -1, e.g. if src is "\n" (c-str) */
		if (a == -1 || b == -1)
			break;

		*dst++ = (a << 2) | ((b & 0x30) >> 4);
		if (c == -1)
			break;
		*dst++ = ((b & 0x0f) << 4) | ((c & 0x3c) >> 2);
		if (d == -1)
			break;
		*dst++ = ((c & 0x03) << 6) | d;
	}
	*dst = '\0';
	return result;
}

void
selinit(void)
{
	sel.mode = SEL_IDLE;
	sel.snap = 0;
	sel.ob.x = -1;
}

int
tlinelen(int y)
{
	int i = term.col;

	if (TLINE(y)[i - 1].mode & ATTR_WRAP)
		return i;

	while (i > 0 && TLINE(y)[i - 1].u == ' ')
		--i;

	return i;
}

void
selstart(int col, int row, int snap)
{
	selclear();
	sel.mode = SEL_EMPTY;
	sel.type = SEL_REGULAR;
	sel.alt = IS_SET(MODE_ALTSCREEN);
	sel.snap = snap;
	sel.oe.x = sel.ob.x = col;
	sel.oe.y = sel.ob.y = row;
	selnormalize();

	if (sel.snap != 0)
		sel.mode = SEL_READY;
	tsetdirt(sel.nb.y, sel.ne.y);
}

void
selextend(int col, int row, int type, int done)
{
	int oldey, oldex, oldsby, oldsey, oldtype;

	if (sel.mode == SEL_IDLE)
		return;
	if (done && sel.mode == SEL_EMPTY) {
		selclear();
		return;
	}

	oldey = sel.oe.y;
	oldex = sel.oe.x;
	oldsby = sel.nb.y;
	oldsey = sel.ne.y;
	oldtype = sel.type;

	sel.oe.x = col;
	sel.oe.y = row;
	selnormalize();
	sel.type = type;

	if (oldey != sel.oe.y || oldex != sel.oe.x || oldtype != sel.type || sel.mode == SEL_EMPTY)
		tsetdirt(MIN(sel.nb.y, oldsby), MAX(sel.ne.y, oldsey));

	sel.mode = done ? SEL_IDLE : SEL_READY;
}

void
selnormalize(void)
{
	int i;

	if (sel.type == SEL_REGULAR && sel.ob.y != sel.oe.y) {
		sel.nb.x = sel.ob.y < sel.oe.y ? sel.ob.x : sel.oe.x;
		sel.ne.x = sel.ob.y < sel.oe.y ? sel.oe.x : sel.ob.x;
	} else {
		sel.nb.x = MIN(sel.ob.x, sel.oe.x);
		sel.ne.x = MAX(sel.ob.x, sel.oe.x);
	}
	sel.nb.y = MIN(sel.ob.y, sel.oe.y);
	sel.ne.y = MAX(sel.ob.y, sel.oe.y);

	selsnap(&sel.nb.x, &sel.nb.y, -1);
	selsnap(&sel.ne.x, &sel.ne.y, +1);

	/* expand selection over line breaks */
	if (sel.type == SEL_RECTANGULAR)
		return;
	i = tlinelen(sel.nb.y);
	if (i < sel.nb.x)
		sel.nb.x = i;
	if (tlinelen(sel.ne.y) <= sel.ne.x)
		sel.ne.x = term.col - 1;
}

int
selected(int x, int y)
{
	if (sel.mode == SEL_EMPTY || sel.ob.x == -1 ||
			sel.alt != IS_SET(MODE_ALTSCREEN))
		return 0;

	if (sel.type == SEL_RECTANGULAR)
		return BETWEEN(y, sel.nb.y, sel.ne.y)
		    && BETWEEN(x, sel.nb.x, sel.ne.x);

	return BETWEEN(y, sel.nb.y, sel.ne.y)
	    && (y != sel.nb.y || x >= sel.nb.x)
	    && (y != sel.ne.y || x <= sel.ne.x);
}

void
selsnap(int *x, int *y, int direction)
{
	int newx, newy, xt, yt;
	int delim, prevdelim;
	const Glyph *gp, *prevgp;

	switch (sel.snap) {
	case SNAP_WORD:
		/*
		 * Snap around if the word wraps around at the end or
		 * beginning of a line.
		 */
		prevgp = &TLINE(*y)[*x];
		prevdelim = ISDELIM(prevgp->u);
		for (;;) {
			newx = *x + direction;
			newy = *y;
			if (!BETWEEN(newx, 0, term.col - 1)) {
				newy += direction;
				newx = (newx + term.col) % term.col;
				if (!BETWEEN(newy, 0, term.row - 1))
					break;

				if (direction > 0)
					yt = *y, xt = *x;
				else
					yt = newy, xt = newx;
				if (!(TLINE(yt)[xt].mode & ATTR_WRAP))
					break;
			}

			if (newx >= tlinelen(newy))
				break;

			gp = &TLINE(newy)[newx];
			delim = ISDELIM(gp->u);
			if (!(gp->mode & ATTR_WDUMMY) && (delim != prevdelim
					|| (delim && gp->u != prevgp->u)))
				break;

			*x = newx;
			*y = newy;
			prevgp = gp;
			prevdelim = delim;
		}
		break;
	case SNAP_LINE:
		/*
		 * Snap around if the the previous line or the current one
		 * has set ATTR_WRAP at its end. Then the whole next or
		 * previous line will be selected.
		 */
		*x = (direction < 0) ? 0 : term.col - 1;
		if (direction < 0) {
			for (; *y > 0; *y += direction) {
				if (!(TLINE(*y-1)[term.col-1].mode
						& ATTR_WRAP)) {
					break;
				}
			}
		} else if (direction > 0) {
			for (; *y < term.row-1; *y += direction) {
				if (!(TLINE(*y)[term.col-1].mode
						& ATTR_WRAP)) {
					break;
				}
			}
		}
		break;
	}
}

char *
getsel(void)
{
	char *str, *ptr;
	int y, bufsize, lastx, linelen;
	const Glyph *gp, *last;

	if (sel.ob.x == -1)
		return NULL;

	bufsize = (term.col+1) * (sel.ne.y-sel.nb.y+1) * UTF_SIZ;
	ptr = str = xmalloc(bufsize);

	/* append every set & selected glyph to the selection */
	for (y = sel.nb.y; y <= sel.ne.y; y++) {
		if ((linelen = tlinelen(y)) == 0) {
			*ptr++ = '\n';
			continue;
		}

		if (sel.type == SEL_RECTANGULAR) {
			gp = &TLINE(y)[sel.nb.x];
			lastx = sel.ne.x;
		} else {
			gp = &TLINE(y)[sel.nb.y == y ? sel.nb.x : 0];
			lastx = (sel.ne.y == y) ? sel.ne.x : term.col-1;
		}
		last = &TLINE(y)[MIN(lastx, linelen-1)];
		while (last >= gp && last->u == ' ')
			--last;

		for ( ; gp <= last; ++gp) {
			if (gp->mode & ATTR_WDUMMY)
				continue;

			ptr += utf8encode(gp->u, ptr);
		}

		/*
		 * Copy and pasting of line endings is inconsistent
		 * in the inconsistent terminal and GUI world.
		 * The best solution seems like to produce '\n' when
		 * something is copied from st and convert '\n' to
		 * '\r', when something to be pasted is received by
		 * st.
		 * FIXME: Fix the computer world.
		 */
		if ((y < sel.ne.y || lastx >= linelen) &&
		    (!(last->mode & ATTR_WRAP) || sel.type == SEL_RECTANGULAR))
			*ptr++ = '\n';
	}
	*ptr = 0;
	return str;
}

void
selclear(void)
{
	if (sel.ob.x == -1)
		return;
	sel.mode = SEL_IDLE;
	sel.ob.x = -1;
	tsetdirt(sel.nb.y, sel.ne.y);
}

void
die(const char *errstr, ...)
{
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(1);
}

void
execsh(char *cmd, char **args)
{
	char *sh, *prog, *arg;
	const struct passwd *pw;

	errno = 0;
	if ((pw = getpwuid(getuid())) == NULL) {
		if (errno)
			die("getpwuid: %s\n", strerror(errno));
		else
			die("who are you?\n");
	}

	if ((sh = getenv("SHELL")) == NULL)
		sh = (pw->pw_shell[0]) ? pw->pw_shell : cmd;

	if (args) {
		prog = args[0];
		arg = NULL;
	} else if (scroll) {
		prog = scroll;
		arg = utmp ? utmp : sh;
	} else if (utmp) {
		prog = utmp;
		arg = NULL;
	} else {
		prog = sh;
		arg = NULL;
	}
	DEFAULT(args, ((char *[]) {prog, arg, NULL}));

	unsetenv("COLUMNS");
	unsetenv("LINES");
	unsetenv("TERMCAP");
	setenv("LOGNAME", pw->pw_name, 1);
	setenv("USER", pw->pw_name, 1);
	setenv("SHELL", sh, 1);
	setenv("HOME", pw->pw_dir, 1);
	setenv("TERM", termname, 1);

	signal(SIGCHLD, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGALRM, SIG_DFL);

	execvp(prog, args);
	_exit(1);
}

void
sigchld(int a)
{
	int saved_errno = errno;
	int stat;
	pid_t p;

	/* Reap all exited children without ever blocking or calling stdio. */
	while ((p = waitpid(-1, &stat, WNOHANG)) > 0) {
		if (p == pid) {
			if (WIFEXITED(stat))
				_exit(WEXITSTATUS(stat));
			_exit(1);
		}
	}
	errno = saved_errno;
}

void
stty(char **args)
{
	char cmd[_POSIX_ARG_MAX], **p, *q, *s;
	size_t n, siz;

	if ((n = strlen(stty_args)) > sizeof(cmd)-1)
		die("incorrect stty parameters\n");
	memcpy(cmd, stty_args, n);
	q = cmd + n;
	siz = sizeof(cmd) - n;
	for (p = args; p && (s = *p); ++p) {
		if ((n = strlen(s)) > siz-1)
			die("stty parameter length too long\n");
		*q++ = ' ';
		memcpy(q, s, n);
		q += n;
		siz -= n + 1;
	}
	*q = '\0';
	if (system(cmd) != 0)
		perror("Couldn't call stty");
}

int
ttynew(const char *line, char *cmd, const char *out, char **args)
{
	int m, s;

	if (out) {
		term.mode |= MODE_PRINT;
		iofd = (!strcmp(out, "-")) ?
			  1 : open(out, O_WRONLY | O_CREAT, 0666);
		if (iofd < 0) {
			fprintf(stderr, "Error opening %s:%s\n",
				out, strerror(errno));
		}
	}

	if (line) {
		if ((cmdfd = open(line, O_RDWR)) < 0)
			die("open line '%s' failed: %s\n",
			    line, strerror(errno));
		dup2(cmdfd, 0);
		stty(args);
		int fl = fcntl(cmdfd, F_GETFL, 0);
		if (fl >= 0)
			fcntl(cmdfd, F_SETFL, fl | O_NONBLOCK);
		return cmdfd;
	}

	/* seems to work fine on linux, openbsd and freebsd */
	if (openpty(&m, &s, NULL, NULL, NULL) < 0)
		die("openpty failed: %s\n", strerror(errno));

	switch (pid = fork()) {
	case -1:
		die("fork failed: %s\n", strerror(errno));
		break;
	case 0:
		close(iofd);
		close(m);
		setsid(); /* create a new process group */
		dup2(s, 0);
		dup2(s, 1);
		dup2(s, 2);
		if (ioctl(s, TIOCSCTTY, NULL) < 0)
			die("ioctl TIOCSCTTY failed: %s\n", strerror(errno));
		if (s > 2)
			close(s);
#ifdef __OpenBSD__
		if (pledge("stdio getpw proc exec", NULL) == -1)
			die("pledge\n");
#endif
		execsh(cmd, args);
		break;
	default:
#ifdef __OpenBSD__
		if (pledge("stdio rpath tty proc", NULL) == -1)
			die("pledge\n");
#endif
		close(s);
		cmdfd = m;
		{
			int fl = fcntl(cmdfd, F_GETFL, 0);
			if (fl >= 0)
				fcntl(cmdfd, F_SETFL, fl | O_NONBLOCK);
		}
		{
			struct sigaction sa;
			memset(&sa, 0, sizeof(sa));
			sa.sa_handler = sigchld;
			sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
			sigemptyset(&sa.sa_mask);
			sigaction(SIGCHLD, &sa, NULL);
		}
		break;
	}
	return cmdfd;
}

static int twrite_aborted = 0;
int ttyread_pending() { return twrite_aborted; }

size_t
ttyread(void)
{
	static char buf[65536];
	static int buflen = 0;
	int ret, written;
	size_t total = 0;

	if (twrite_aborted) {
		total = 1;
	} else {
		/* append read bytes to unprocessed bytes */
		ret = read(cmdfd, buf + buflen, LEN(buf) - buflen);
		switch (ret) {
		case 0:
			exit(0);
		case -1:
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				break;
			die("couldn't read from shell: %s\n", strerror(errno));
		default:
			buflen += ret;
			total += ret;
			break;
		}

		/*
		 * Drain additional immediately-ready PTY chunks into buf
		 * without blocking, avoiding a full pselect + XPending loop
		 * every 4095 bytes during bulk output.
		 */
		while (LEN(buf) - buflen >= 4096) {
			struct pollfd pfd = { .fd = cmdfd, .events = POLLIN };
			if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN))
				break;
			ret = read(cmdfd, buf + buflen, LEN(buf) - buflen);
			if (ret <= 0)
				break;
			buflen += ret;
			total += ret;
		}
	}

	written = twrite(buf, buflen, 0);
	buflen -= written;
	/* keep any incomplete UTF-8 byte sequence for the next call */
	if (buflen > 0)
		memmove(buf, buf + written, buflen);
	return total;
}

void
ttywrite(const char *s, size_t n, int may_echo)
{
	const char *next;

	if (may_echo && IS_SET(MODE_ECHO))
		twrite(s, n, 1);

	if (!IS_SET(MODE_CRLF)) {
		ttywriteraw(s, n);
		return;
	}

	/* This is similar to how the kernel handles ONLCR for ttys */
	while (n > 0) {
		if (*s == '\r') {
			next = s + 1;
			ttywriteraw("\r\n", 2);
		} else {
			next = memchr(s, '\r', n);
			DEFAULT(next, s + n);
			ttywriteraw(s, next - s);
		}
		n -= next - s;
		s = next;
	}
}

int
ttywrite_pending(void)
{
	return ttywtail > ttywhead;
}

/*
 * Drain queued PTY writes without blocking the X11 event loop. Stops if the
 * PTY input buffer is full (or after 64 KB per turn so X11/reads stay live).
 */
void
ttywrite_flush(void)
{
	size_t budget = 65536;

	while (ttywtail > ttywhead && budget > 0) {
		struct pollfd pfd = { .fd = cmdfd, .events = POLLOUT };
		ssize_t r;
		size_t avail = ttywtail - ttywhead;
		size_t chunk = MIN(avail, MIN(budget, (size_t)4096));

		if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLOUT))
			break;
		r = write(cmdfd, ttywbuf + ttywhead, chunk);
		if (r < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				break;
			die("write error on tty: %s\n", strerror(errno));
		}
		if (r == 0)
			break;
		ttywhead += (size_t)r;
		budget -= (size_t)r;
	}

	if (ttywhead == ttywtail) {
		ttywhead = ttywtail = 0;
		if (ttywcap > 65536) {
			free(ttywbuf);
			ttywbuf = NULL;
			ttywcap = 0;
		}
	} else if (ttywhead >= 4096 && ttywhead * 2 >= ttywcap) {
		size_t rem = ttywtail - ttywhead;
		memmove(ttywbuf, ttywbuf + ttywhead, rem);
		ttywhead = 0;
		ttywtail = rem;
	}
}

void
ttywriteraw(const char *s, size_t n)
{
	if (n == 0)
		return;

	/* Fast path: when nothing is queued and PTY is writable, write directly. */
	if (ttywtail == ttywhead) {
		size_t budget = 65536;
		while (n > 0 && budget > 0) {
			struct pollfd pfd = { .fd = cmdfd, .events = POLLOUT };
			ssize_t r;
			size_t chunk = MIN(n, MIN(budget, (size_t)4096));
			if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLOUT))
				break;
			r = write(cmdfd, s, chunk);
			if (r < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
					break;
				die("write error on tty: %s\n", strerror(errno));
			}
			if (r == 0)
				break;
			s += r;
			n -= (size_t)r;
			budget -= (size_t)r;
		}
		if (n == 0)
			return;
	}

	/* Queue remaining bytes to be drained by run()'s pselect() loop. */
	if (ttywhead > 0 && ttywtail + n > ttywcap) {
		size_t rem = ttywtail - ttywhead;
		memmove(ttywbuf, ttywbuf + ttywhead, rem);
		ttywhead = 0;
		ttywtail = rem;
	}
	if (ttywtail + n > ttywcap) {
		size_t newcap = ttywcap ? ttywcap * 2 : 4096;
		while (newcap < ttywtail + n)
			newcap *= 2;
		ttywbuf = xrealloc(ttywbuf, newcap);
		ttywcap = newcap;
	}
	memcpy(ttywbuf + ttywtail, s, n);
	ttywtail += n;
	ttywrite_flush();
}

void
ttyresize(int tw, int th)
{
	struct winsize w;

	w.ws_row = term.row;
	w.ws_col = term.col;
	w.ws_xpixel = tw;
	w.ws_ypixel = th;
	if (ioctl(cmdfd, TIOCSWINSZ, &w) < 0)
		fprintf(stderr, "Couldn't set window size: %s\n", strerror(errno));
}

void
ttyhangup(void)
{
	/* Send SIGHUP to shell */
	kill(pid, SIGHUP);
}

int
tattrset(int attr)
{
	int i, j;

	if (attr == ATTR_BLINK && !term.has_blink)
		return 0;

	for (i = 0; i < term.row; i++) {
		for (j = 0; j < term.col; j++) {
			if (term.line[i][j].mode & attr)
				return 1;
		}
	}

	if (attr == ATTR_BLINK)
		term.has_blink = 0;
	return 0;
}

void
tsetdirt(int top, int bot)
{
	int i;

	LIMIT(top, 0, term.row-1);
	LIMIT(bot, 0, term.row-1);

	for (i = top; i <= bot; i++)
		term.dirty[i] = 1;
}

void
tsetdirtattr(int attr)
{
	int i, j;

	for (i = 0; i < term.row; i++) {
		for (j = 0; j < term.col; j++) {
			if (term.line[i][j].mode & attr) {
				tsetdirt(i, i);
				break;
			}
		}
	}
}

void
tfulldirt(void)
{
	tsync_end();
	tsetdirt(0, term.row-1);
}

void
tcursor(int mode)
{
	static TCursor c[2];
	int alt = IS_SET(MODE_ALTSCREEN);

	if (mode == CURSOR_SAVE) {
		c[alt] = term.c;
	} else if (mode == CURSOR_LOAD) {
		term.c = c[alt];
		tmoveto(c[alt].x, c[alt].y);
	}
}

void
treset(void)
{
	uint i;

	term.c = (TCursor){{
		.mode = ATTR_NULL,
		.fg = seloptsch->defaultfg,
		.bg = seloptsch->defaultbg
	}, .x = 0, .y = 0, .state = CURSOR_DEFAULT};

	memset(term.tabs, 0, term.col * sizeof(*term.tabs));
	for (i = tabspaces; i < term.col; i += tabspaces)
		term.tabs[i] = 1;
	term.top = 0;
	term.bot = term.row - 1;
	term.mode = MODE_WRAP|MODE_UTF8;
	memset(term.trantbl, CS_USA, sizeof(term.trantbl));
	term.charset = 0;
	term.has_blink = 0;

	for (i = 0; i < 2; i++) {
		tmoveto(0, 0);
		tcursor(CURSOR_SAVE);
		for (int y = 0; y < term.row; y++) {
			term.rowmax[y] = term.col;
			term.rowwide[y] = 1;
		}
		tclearregion(0, 0, term.col-1, term.row-1);
		tswapscreen();
	}
}

void
tnew(int col, int row)
{
	thistclear();
	xmode_shadow = 0;
	term = (Term){ .c = { .attr = { .fg = seloptsch->defaultfg, .bg = seloptsch->defaultbg } } };
	tresize(col, row);
	treset();
}

int tisaltscr(void)
{
    return IS_SET(MODE_ALTSCREEN);
}

void
tswapscreen(void)
{
	Line *tmp = term.line;
	int *tmpm = term.rowmax;
	unsigned char *tmpw = term.rowwide;

	term.line = term.alt;
	term.alt = tmp;
	term.rowmax = term.alt_rowmax;
	term.alt_rowmax = tmpm;
	term.rowwide = term.alt_rowwide;
	term.alt_rowwide = tmpw;
	term.mode ^= MODE_ALTSCREEN;
	tfulldirt();
}

void
newterm(const Arg* a)
{
    switch (fork()) {
    case -1:
        die("fork failed: %s\n", strerror(errno));
        break;
    case 0:
        chdir(getcwd_by_pid(pid));
        execlp("st", "./st", NULL);
        break;
    }
}

static char *getcwd_by_pid(pid_t pid) {
    char buf[32];
    char path[PATH_MAX];
    ssize_t n;
    snprintf(buf, sizeof buf, "/proc/%d/cwd", pid);
    n = readlink(buf, path, sizeof(path) - 1);
    if (n <= 0)
        return NULL;
    path[n] = '\0';
    return strdup(path);
}

void
kscrolldown(const Arg* a)
{
	int n = a->i;

	if (n < 0)
		n = term.row + n;

	if (n > term.scr)
		n = term.scr;

	if (term.scr > 0) {
		term.scr -= n;
		selscroll(0, -n);
		tfulldirt();
	}
}

void
kscrollup(const Arg* a)
{
	int n = a->i;

	if (n < 0)
		n = term.row + n;

	/*
	 * History lines are now allocated lazily, so we must never scroll past
	 * the number of lines actually written - those slots would be NULL.
	 * Clamp n to the available history instead of the full HISTSIZE.
	 */
	if (n > term.histwcount - term.scr)
		n = term.histwcount - term.scr;

	if (n > 0) {
		term.scr += n;
		selscroll(0, n);
		tfulldirt();
	}
}

/*
 * Scrollback storage
 *
 * A history line is stored trimmed: only the cells up to its last cell
 * that differs from a default blank, with that width in term.histcol[].
 * Plain default-attribute ASCII lines are stored compactly as 1 byte/cell
 * (HIST_ASCII / HIST_ASCII_WRAP), styled ASCII lines as packed HistRun
 * attribute runs + raw bytes (HIST_SGR_ASCII, always smaller than Glyph[]),
 * and non-ASCII/wide/boxdraw lines as aligned Glyph[] (HIST_GLYPH), all
 * sharing 256 KB refcounted byte slabs.
 *
 * Missing trailing blanks are recreated on demand with tdefblank(), and
 * compact lines are inflated lazily into scratch by tvisline()/externalpipe()
 * (or materialized by thistline() on selection). Two consequences:
 *  - memory and page faults scale with compact text (~1 B/char for ASCII
 *    instead of 16 B/Glyph), cutting bulk flood RSS and page faults ~15x;
 *  - tresize() never touches history, so it stays O(rows) regardless of
 *    HISTSIZE.
 */
static void
tdefblank(Glyph *g)
{
	/* zeroed first: padding bytes must not differ between equal cells */
	memset(g, 0, sizeof(*g));
	g->u = ' ';
	g->fg = seloptsch->defaultfg;
	g->bg = seloptsch->defaultbg;
}

/*
 * Number of leading cells of a screen line that history must keep.
 * Compares whole cells against a zero-padded default blank: cleared cells
 * match byte for byte, and a cell that differs only in padding is merely
 * kept (less trimming), never dropped, so the result is always lossless.
 */
static int
thistlen(const Glyph *line, int maxcol)
{
	Glyph blank;
	int x = MIN(maxcol, term.col);

	tdefblank(&blank);
	while (x > 0 && !memcmp(&line[x-1], &blank, sizeof(blank)))
		x--;
	return x;
}

static void
histslab_release(HistSlab *s)
{
	if (--s->refs > 0 || s == histslab_cur)
		return;
	if (s->prev) s->prev->next = s->next;
	else histslab_head = s->next;
	if (s->next) s->next->prev = s->prev;
	s->prev = s->next = NULL;
	if (!histslab_spare) {
		s->used = 0;
		histslab_spare = s;
	} else {
		free(s);
	}
}

static void *
histslab_alloc(size_t nbytes, size_t align, HistSlab **out_slab)
{
	size_t off;

	if (nbytes == 0) {
		*out_slab = NULL;
		return &hist_empty_line;
	}
	if (nbytes <= HIST_SLAB_BYTES) {
		off = histslab_cur ? ((histslab_cur->used + align - 1) & ~(align - 1)) : 0;
		if (!histslab_cur || off + nbytes > HIST_SLAB_BYTES) {
			if (histslab_cur && histslab_cur->refs == 0) {
				histslab_cur->used = 0;
			} else {
				HistSlab *s = histslab_spare;
				if (s) {
					histslab_spare = NULL;
				} else {
					s = xmalloc(sizeof(*s));
				}
				s->used = 0;
				s->refs = 0;
				s->prev = NULL;
				s->next = histslab_head;
				if (histslab_head)
					histslab_head->prev = s;
				histslab_head = s;
				histslab_cur = s;
			}
			off = 0;
		}
		histslab_cur->used = off + nbytes;
		histslab_cur->refs++;
		*out_slab = histslab_cur;
		return &histslab_cur->u.data[off];
	}
	*out_slab = NULL;
	hist_heap_count++;
	return xmalloc(nbytes);
}

static void
thistfree(int idx)
{
	if (term.histslab[idx]) {
		HistSlab *old = term.histslab[idx];
		term.histslab[idx] = NULL;
		term.hist[idx] = NULL;
		histslab_release(old);
	} else if (term.hist[idx] && term.hist[idx] != &hist_empty_line) {
		free(term.hist[idx]);
		term.hist[idx] = NULL;
		hist_heap_count--;
	} else {
		term.hist[idx] = NULL;
	}
	term.histcol[idx] = 0;
	term.histfmt[idx] = HIST_EMPTY;
}

static void
thistclear(void)
{
	HistSlab *s, *next;
	int i;

	if (hist_heap_count > 0) {
		for (i = 0; i < HISTSIZE; i++) {
			if (term.hist[i] && !term.histslab[i] &&
			    term.hist[i] != &hist_empty_line) {
				free(term.hist[i]);
				term.hist[i] = NULL;
			}
		}
		hist_heap_count = 0;
	}
	for (s = histslab_head; s; s = next) {
		next = s->next;
		free(s);
	}
	free(histslab_spare);
	histslab_head = histslab_cur = histslab_spare = NULL;
	if (term.histwcount > 0) {
		memset(term.hist, 0, sizeof(term.hist));
		memset(term.histslab, 0, sizeof(term.histslab));
		memset(term.histcol, 0, sizeof(term.histcol));
		memset(term.histfmt, 0, sizeof(term.histfmt));
		term.histwcount = 0;
		term.histi = 0;
		if (term.scr > 0) {
			term.scr = 0;
			tfulldirt();
		}
	}
}

/* Push a raw plain-ASCII line directly into compact history (1 byte/cell). */
static void
thistpush_ascii(int idx, const char *s, int len, int wrapped)
{
	uchar *dst;

	if (!wrapped) {
		while (len > 0 && s[len - 1] == ' ')
			len--;
	}
	thistfree(idx);
	if (len == 0) {
		term.hist[idx] = &hist_empty_line;
		term.histcol[idx] = 0;
		term.histfmt[idx] = HIST_ASCII;
		return;
	}
	dst = histslab_alloc((size_t)len, 1, &term.histslab[idx]);
	memcpy(dst, s, (size_t)len);
	term.hist[idx] = dst;
	term.histcol[idx] = len;
	term.histfmt[idx] = wrapped ? HIST_ASCII_WRAP : HIST_ASCII;
}

/* Push a pre-encoded HIST_SGR_ASCII payload into history slot idx. */
static void
thistpush_sgr_packed(int idx, const uchar *packed, size_t nbytes, int cols)
{
	uchar *dst;

	thistfree(idx);
	if (cols == 0) {
		term.hist[idx] = &hist_empty_line;
		term.histcol[idx] = 0;
		term.histfmt[idx] = HIST_ASCII;
		return;
	}
	dst = histslab_alloc(nbytes, 1, &term.histslab[idx]);
	memcpy(dst, packed, nbytes);
	term.hist[idx] = dst;
	term.histcol[idx] = cols;
	term.histfmt[idx] = HIST_SGR_ASCII;
}

/* Copy a screen line into history slot idx in the most compact lossless form. */
static void
thistpush(int idx, const Glyph *line, int maxcol)
{
	int len = thistlen(line, maxcol);
	uint32_t dfg = seloptsch->defaultfg;
	uint32_t dbg = seloptsch->defaultbg;
	int x, plain = 1, ascii_sgr = 1, nruns = 0;

	thistfree(idx);
	if (len == 0) {
		term.hist[idx] = &hist_empty_line;
		term.histcol[idx] = 0;
		term.histfmt[idx] = HIST_ASCII;
		return;
	}

	for (x = 0; x < len; x++) {
		const Glyph *g = &line[x];
		if (g->u < 0x20 || g->u > 0x7e ||
		    (g->mode & (ATTR_WIDE | ATTR_WDUMMY | ATTR_BOXDRAW))) {
			plain = 0;
			ascii_sgr = 0;
			break;
		}
		if (plain) {
			if (g->fg != dfg || g->bg != dbg ||
			    (g->mode != 0 && !(x == len - 1 && g->mode == ATTR_WRAP)))
				plain = 0;
		}
		if (x == 0 || ATTRCMP(*g, line[x - 1]))
			nruns++;
	}

	if (plain) {
		uchar *dst = histslab_alloc((size_t)len, 1, &term.histslab[idx]);
		for (x = 0; x < len; x++)
			dst[x] = (uchar)line[x].u;
		term.hist[idx] = dst;
		term.histcol[idx] = len;
		term.histfmt[idx] = (line[len - 1].mode == ATTR_WRAP) ?
		                    HIST_ASCII_WRAP : HIST_ASCII;
		return;
	}

	if (ascii_sgr) {
		size_t need = (size_t)nruns * sizeof(HistRun) + (size_t)len;
		if (need < (size_t)len * sizeof(Glyph)) {
			uchar *dst = histslab_alloc(need, 1, &term.histslab[idx]);
			uchar *p = dst;
			for (x = 0; x < len; ) {
				int k = 1;
				HistRun hdr;
				while (x + k < len && k < 65535 && !ATTRCMP(line[x + k], line[x]))
					k++;
				hdr.count = (uint16_t)k;
				hdr.mode = line[x].mode;
				hdr.fg = line[x].fg;
				hdr.bg = line[x].bg;
				memcpy(p, &hdr, sizeof(hdr));
				p += sizeof(hdr);
				for (int j = 0; j < k; j++)
					p[j] = (uchar)line[x + j].u;
				p += k;
				x += k;
			}
			term.hist[idx] = dst;
			term.histcol[idx] = len;
			term.histfmt[idx] = HIST_SGR_ASCII;
			return;
		}
	}

	Line dst = histslab_alloc((size_t)len * sizeof(Glyph), sizeof(Glyph),
	                          &term.histslab[idx]);
	memcpy(dst, line, (size_t)len * sizeof(Glyph));
	term.hist[idx] = dst;
	term.histcol[idx] = len;
	term.histfmt[idx] = HIST_GLYPH;
}

/* Unpack up to `limit` stored cells of history slot `idx` into `dst`. */
static void
thistunpack(int idx, Glyph *dst, int limit)
{
	int stored = term.histcol[idx];
	int w = MIN(stored, limit);
	uchar fmt = term.histfmt[idx];
	int x;

	if (w <= 0 || fmt == HIST_EMPTY || !term.hist[idx])
		return;

	if (fmt == HIST_ASCII || fmt == HIST_ASCII_WRAP) {
		const uchar *p = (const uchar *)term.hist[idx];
		Glyph g;
		tdefblank(&g);
		for (x = 0; x < w; x++) {
			dst[x] = g;
			dst[x].u = p[x];
		}
		if (fmt == HIST_ASCII_WRAP && stored <= limit)
			dst[w - 1].mode |= ATTR_WRAP;
		return;
	}

	if (fmt == HIST_SGR_ASCII) {
		const uchar *p = (const uchar *)term.hist[idx];
		x = 0;
		while (x < w) {
			HistRun hdr;
			Glyph g;
			int n, k;
			memcpy(&hdr, p, sizeof(hdr));
			p += sizeof(hdr);
			memset(&g, 0, sizeof(g));
			g.mode = hdr.mode;
			g.fg = hdr.fg;
			g.bg = hdr.bg;
			n = MIN((int)hdr.count, w - x);
			for (k = 0; k < n; k++) {
				dst[x + k] = g;
				dst[x + k].u = p[k];
			}
			p += hdr.count;
			x += n;
		}
		return;
	}

	memcpy(dst, term.hist[idx], (size_t)w * sizeof(Glyph));
}

/*
 * Return history line idx with at least term.col valid cells, widening it
 * in place. Used by selection/word-snapping, which index whole rows;
 * draw() uses tvisline() instead so that merely viewing scrollback does
 * not re-inflate it.
 */
static Line
thistline(int idx)
{
	int x, stored, newcol;
	Line nl;

	if (term.histfmt[idx] == HIST_EMPTY || !term.hist[idx])
		return NULL;
	if (term.histfmt[idx] == HIST_GLYPH && term.histcol[idx] >= term.col)
		return (Line)term.hist[idx];

	stored = term.histcol[idx];
	newcol = MAX(stored, term.col);
	nl = xmalloc((size_t)newcol * sizeof(Glyph));
	thistunpack(idx, nl, stored);
	for (x = stored; x < newcol; x++)
		tdefblank(&nl[x]);
	thistfree(idx);
	term.hist[idx] = nl;
	term.histslab[idx] = NULL;
	term.histcol[idx] = newcol;
	term.histfmt[idx] = HIST_GLYPH;
	hist_heap_count++;
	return nl;
}

/* like TLINE(y), but unpacks/pads a history line into scratch instead */
static Line
tvisline(int y, Glyph *scratch)
{
	int idx, w, x;

	if (y >= term.scr)
		return term.line[y - term.scr];
	idx = (y + term.histi - term.scr + HISTSIZE + 1) % HISTSIZE;
	if (term.histfmt[idx] == HIST_GLYPH && term.histcol[idx] >= term.col)
		return (Line)term.hist[idx];
	w = MIN(term.histcol[idx], term.col);
	thistunpack(idx, scratch, w);
	for (x = w; x < term.col; x++)
		tdefblank(&scratch[x]);
	return scratch;
}

void
tscrolldown(int orig, int n, int copyhist)
{
	int i;
	Line temp;

	LIMIT(n, 0, term.bot-orig+1);
	if (IS_SET(MODE_ALTSCREEN))
		copyhist = 0;

	/*
	 * Same history effect as the scrollback patch's pointer swap: slot
	 * histi-1 takes the bottom line and its old content is dropped. The
	 * screen keeps its own row buffer, which is cleared below.
	 */
	if (copyhist) {
		term.histi = (term.histi - 1 + HISTSIZE) % HISTSIZE;
		thistpush(term.histi, term.line[term.bot], term.rowmax[term.bot]);
		/*
		 * The visible history window [histi-histwcount+1, histi] just
		 * moved back one slot. Unless the ring is full, its new oldest
		 * slot was never written (NULL), and scrolling back to it or
		 * selecting it crashed. Shrink the window instead.
		 */
		if (term.histwcount > 0 && term.histwcount < HISTSIZE) {
			term.histwcount--;
			if (term.scr > term.histwcount)
				term.scr = term.histwcount;
		}
	}

	tsetdirt(orig, term.bot-n);
	tclearregion(0, term.bot-n+1, term.col-1, term.bot);

	for (i = term.bot; i >= orig+n; i--) {
		int tmpm;
		unsigned char tmpw;
		temp = term.line[i];
		term.line[i] = term.line[i-n];
		term.line[i-n] = temp;
		tmpm = term.rowmax[i];
		term.rowmax[i] = term.rowmax[i-n];
		term.rowmax[i-n] = tmpm;
		tmpw = term.rowwide[i];
		term.rowwide[i] = term.rowwide[i-n];
		term.rowwide[i-n] = tmpw;
	}

	selscroll(orig, n);
}

void
tscrollup(int orig, int n, int copyhist)
{
	int i;
	Line temp;

	LIMIT(n, 0, term.bot-orig+1);
	if (IS_SET(MODE_ALTSCREEN))
		copyhist = 0;

	/* the screen row keeps its buffer; history gets a trimmed copy */
	if (copyhist) {
		term.histi = (term.histi + 1) % HISTSIZE;
		thistpush(term.histi, term.line[orig], term.rowmax[orig]);
		if (term.histwcount < HISTSIZE)
			term.histwcount++;
	}

	if (copyhist && term.scr > 0 && term.scr < HISTSIZE)
		term.scr = MIN(term.scr + n, MIN(term.histwcount, HISTSIZE-1));

	tclearregion(0, orig, term.col-1, orig+n-1);
	tsetdirt(orig+n, term.bot);

	for (i = orig; i <= term.bot-n; i++) {
		int tmpm;
		unsigned char tmpw;
		temp = term.line[i];
		term.line[i] = term.line[i+n];
		term.line[i+n] = temp;
		tmpm = term.rowmax[i];
		term.rowmax[i] = term.rowmax[i+n];
		term.rowmax[i+n] = tmpm;
		tmpw = term.rowwide[i];
		term.rowwide[i] = term.rowwide[i+n];
		term.rowwide[i+n] = tmpw;
	}

	selscroll(orig, -n);
}

void
selscroll(int orig, int n)
{
	if (sel.ob.x == -1 || sel.alt != IS_SET(MODE_ALTSCREEN))
		return;

	if (BETWEEN(sel.nb.y, orig, term.bot) != BETWEEN(sel.ne.y, orig, term.bot)) {
		selclear();
	} else if (BETWEEN(sel.nb.y, orig, term.bot)) {
		sel.ob.y += n;
		sel.oe.y += n;
		if (sel.ob.y < term.top || sel.ob.y > term.bot ||
		    sel.oe.y < term.top || sel.oe.y > term.bot) {
			selclear();
		} else {
			selnormalize();
		}
	}
}

void
tnewline(int first_col)
{
	int y = term.c.y;

	if (y == term.bot) {
		tscrollup(term.top, 1, 1);
	} else {
		y++;
	}
	tmoveto(first_col ? 0 : term.c.x, y);
}

void
csiparse(void)
{
	char *p = csiescseq.buf;
	char *end = csiescseq.buf + csiescseq.len;
	int sep = ';'; /* colon or semi-colon, but not both */

	csiescseq.narg = 0;
	csiescseq.sep = ';';
	if (*p == '?' || *p == '>' || *p == '<' || *p == '=') {
		csiescseq.priv = *p;
		p++;
	}

	csiescseq.buf[csiescseq.len] = '\0';
	while (p < end) {
		long int v = 0;
		int neg = 0;
		if (*p == '-') {
			neg = 1;
			p++;
		}
		if (*p >= '0' && *p <= '9') {
			while (*p >= '0' && *p <= '9') {
				if (v <= (LONG_MAX - 9) / 10)
					v = v * 10 + (*p - '0');
				else
					v = LONG_MAX;
				p++;
			}
			if (v == LONG_MAX)
				v = -1;
			else if (neg)
				v = -v;
		}
		csiescseq.arg[csiescseq.narg++] = (int)v;
		if (sep == ';' && *p == ':') {
			sep = ':'; /* allow override to colon once */
			csiescseq.sep = ':';
		}
		if (*p != sep || csiescseq.narg == ESC_ARG_SIZ)
			break;
		p++;
	}
	csiescseq.mode[0] = *p++;
	csiescseq.mode[1] = (p < end) ? *p : '\0';
}

/* for absolute user moves, when decom is set */
void
tmoveato(int x, int y)
{
	tmoveto(x, y + ((term.c.state & CURSOR_ORIGIN) ? term.top: 0));
}

void
tmoveto(int x, int y)
{
	int miny, maxy;

	if (term.c.state & CURSOR_ORIGIN) {
		miny = term.top;
		maxy = term.bot;
	} else {
		miny = 0;
		maxy = term.row - 1;
	}
	term.c.state &= ~CURSOR_WRAPNEXT;
	term.c.x = LIMIT(x, 0, term.col-1);
	term.c.y = LIMIT(y, miny, maxy);
}

void
tsetchar(Rune u, const Glyph *attr, int x, int y)
{
	static const char *vt100_0[62] = { /* 0x41 - 0x7e */
		"↑", "↓", "→", "←", "█", "▚", "☃", /* A - G */
		0, 0, 0, 0, 0, 0, 0, 0, /* H - O */
		0, 0, 0, 0, 0, 0, 0, 0, /* P - W */
		0, 0, 0, 0, 0, 0, 0, " ", /* X - _ */
		"◆", "▒", "␉", "␌", "␍", "␊", "°", "±", /* ` - g */
		"␤", "␋", "┘", "┐", "┌", "└", "┼", "⎺", /* h - o */
		"⎻", "─", "⎼", "⎽", "├", "┤", "┴", "┬", /* p - w */
		"│", "≤", "≥", "π", "≠", "£", "·", /* x - ~ */
	};

	/*
	 * The table is proudly stolen from rxvt.
	 */
	if (term.trantbl[term.charset] == CS_GRAPHIC0 &&
	   BETWEEN(u, 0x41, 0x7e) && vt100_0[u - 0x41])
		utf8decode(vt100_0[u - 0x41], &u, UTF_SIZ);

	if (term.line[y][x].mode & ATTR_WIDE) {
		if (x+1 < term.col) {
			term.line[y][x+1].u = ' ';
			term.line[y][x+1].mode &= ~ATTR_WDUMMY;
		}
	} else if (term.line[y][x].mode & ATTR_WDUMMY) {
		term.line[y][x-1].u = ' ';
		term.line[y][x-1].mode &= ~ATTR_WIDE;
	}

	term.dirty[y] = 1;
	term.line[y][x] = *attr;
	term.line[y][x].u = u;
	if (x + 1 > term.rowmax[y])
		term.rowmax[y] = x + 1;

    /*
     * Inline boxdraw lookup using boxdraw_lut (no cross-TU call).
     */
    if (u >= 0x2500 && isboxdraw_fast(u))
        term.line[y][x].mode |= ATTR_BOXDRAW;
}

void
tclearregion(int x1, int y1, int x2, int y2)
{
	int x, y, temp;
	Glyph blank;
	Line line;

	if (x1 > x2)
		temp = x1, x1 = x2, x2 = temp;
	if (y1 > y2)
		temp = y1, y1 = y2, y2 = temp;

	LIMIT(x1, 0, term.col-1);
	LIMIT(x2, 0, term.col-1);
	LIMIT(y1, 0, term.row-1);
	LIMIT(y2, 0, term.row-1);

	/*
	 * Test selection overlap once up front so the inner cell-clearing
	 * loop is branchless and auto-vectorizes cleanly.
	 */
	if (sel.mode != SEL_EMPTY && sel.ob.x != -1 &&
	    sel.alt == IS_SET(MODE_ALTSCREEN) &&
	    y1 <= sel.ne.y && y2 >= sel.nb.y) {
		if (sel.type == SEL_RECTANGULAR) {
			if (x1 <= sel.ne.x && x2 >= sel.nb.x)
				selclear();
		} else {
			int cy1 = MAX(y1, sel.nb.y);
			int cy2 = MIN(y2, sel.ne.y);
			for (y = cy1; y <= cy2; y++) {
				int sx1 = (y == sel.nb.y) ? sel.nb.x : 0;
				int sx2 = (y == sel.ne.y) ? sel.ne.x : term.col - 1;
				if (x1 <= sx2 && x2 >= sx1) {
					selclear();
					break;
				}
			}
		}
	}

	/*
	 * Zero the struct first so its padding bytes are deterministic: the
	 * renderer diffs rows with memcmp, and garbage padding would make
	 * identical blank cells look changed and force needless redraws.
	 */
	memset(&blank, 0, sizeof(blank));
	blank.u = ' ';
	blank.fg = term.c.attr.fg;
	blank.bg = term.c.attr.bg;
	int is_defblank = (blank.fg == seloptsch->defaultfg &&
	                   blank.bg == seloptsch->defaultbg);

	for (y = y1; y <= y2; y++) {
		int endx = x2;
		term.dirty[y] = 1;
		line = term.line[y];
		if (is_defblank && x2 == term.col - 1) {
			int rm = MIN(term.rowmax[y], term.col);
			endx = rm - 1;
			if (x1 == 0) {
				term.rowmax[y] = 0;
				term.rowwide[y] = 0;
			} else if (x1 < term.rowmax[y]) {
				term.rowmax[y] = x1;
			}
		} else {
			if (x2 + 1 > term.rowmax[y])
				term.rowmax[y] = x2 + 1;
			if (x1 == 0 && x2 == term.col - 1)
				term.rowwide[y] = 0;
		}
		for (x = x1; x <= endx; x++)
			line[x] = blank;
	}
}

void
tdeletechar(int n)
{
	int dst, src, size;
	Glyph *line;

	LIMIT(n, 0, term.col - term.c.x);

	dst = term.c.x;
	src = term.c.x + n;
	size = term.col - src;
	line = term.line[term.c.y];

	memmove(&line[dst], &line[src], size * sizeof(Glyph));
	term.rowmax[term.c.y] = term.col;
	tclearregion(term.col-n, term.c.y, term.col-1, term.c.y);
}

void
tinsertblank(int n)
{
	int dst, src, size;
	Glyph *line;

	LIMIT(n, 0, term.col - term.c.x);

	dst = term.c.x + n;
	src = term.c.x;
	size = term.col - dst;
	line = term.line[term.c.y];

	memmove(&line[dst], &line[src], size * sizeof(Glyph));
	term.rowmax[term.c.y] = term.col;
	tclearregion(src, term.c.y, dst - 1, term.c.y);
}

void
tinsertblankline(int n)
{
	if (BETWEEN(term.c.y, term.top, term.bot))
		tscrolldown(term.c.y, n, 0);
}

void
tdeleteline(int n)
{
	if (BETWEEN(term.c.y, term.top, term.bot))
		tscrollup(term.c.y, n, 0);
}

int32_t
tdefcolor(const int *attr, int *npar, int l)
{
	int32_t idx = -1;
	uint r, g, b;

	switch (attr[*npar + 1]) {
	case 2: /* direct color in RGB space */
		if (*npar + 4 >= l) {
			fprintf(stderr,
				"erresc(38): Incorrect number of parameters (%d)\n",
				*npar);
			break;
		}
		r = attr[*npar + 2];
		g = attr[*npar + 3];
		b = attr[*npar + 4];
		*npar += 4;
		if (!BETWEEN(r, 0, 255) || !BETWEEN(g, 0, 255) || !BETWEEN(b, 0, 255))
			fprintf(stderr, "erresc: bad rgb color (%u,%u,%u)\n",
				r, g, b);
		else
			idx = TRUECOLOR(r, g, b);
		break;
	case 5: /* indexed color */
		if (*npar + 2 >= l) {
			fprintf(stderr,
				"erresc(38): Incorrect number of parameters (%d)\n",
				*npar);
			break;
		}
		*npar += 2;
		if (!BETWEEN(attr[*npar], 0, 255))
			fprintf(stderr, "erresc: bad fgcolor %d\n", attr[*npar]);
		else
			idx = attr[*npar];
		break;
	case 0: /* implemented defined (only foreground) */
	case 1: /* transparent */
	case 3: /* direct color in CMY space */
	case 4: /* direct color in CMYK space */
	default:
		fprintf(stderr,
		        "erresc(38): gfx attr %d unknown\n", attr[*npar]);
		break;
	}

	return idx;
}

void
tsetattr(const int *attr, int l)
{
	int i;
	int32_t idx;

	for (i = 0; i < l; i++) {
		switch (attr[i]) {
		case 0:
			term.c.attr.mode &= ~(
				ATTR_BOLD       |
				ATTR_FAINT      |
				ATTR_ITALIC     |
				ATTR_UNDERLINE  |
				ATTR_BLINK      |
				ATTR_REVERSE    |
				ATTR_INVISIBLE  |
				ATTR_STRUCK     );
			term.c.attr.fg = seloptsch->defaultfg;
			term.c.attr.bg = seloptsch->defaultbg;
			break;
		case 1:
			term.c.attr.mode |= ATTR_BOLD;
			break;
		case 2:
			term.c.attr.mode |= ATTR_FAINT;
			break;
		case 3:
			term.c.attr.mode |= ATTR_ITALIC;
			break;
		case 4:
			term.c.attr.mode |= ATTR_UNDERLINE;
			/* Handle colon underline styles (SGR 4:0..4:5) without
			 * misinterpreting the subparameter as italic/etc. */
			if (csiescseq.sep == ':' && i == 0 && l == 2) {
				if (attr[1] == 0)
					term.c.attr.mode &= ~ATTR_UNDERLINE;
				i++;
			}
			break;
		case 5: /* slow blink */
			/* FALLTHROUGH */
		case 6: /* rapid blink */
			term.c.attr.mode |= ATTR_BLINK;
			term.has_blink = 1;
			break;
		case 7:
			term.c.attr.mode |= ATTR_REVERSE;
			break;
		case 8:
			term.c.attr.mode |= ATTR_INVISIBLE;
			break;
		case 9:
			term.c.attr.mode |= ATTR_STRUCK;
			break;
		case 22:
			term.c.attr.mode &= ~(ATTR_BOLD | ATTR_FAINT);
			break;
		case 23:
			term.c.attr.mode &= ~ATTR_ITALIC;
			break;
		case 24:
			term.c.attr.mode &= ~ATTR_UNDERLINE;
			break;
		case 25:
			term.c.attr.mode &= ~ATTR_BLINK;
			break;
		case 27:
			term.c.attr.mode &= ~ATTR_REVERSE;
			break;
		case 28:
			term.c.attr.mode &= ~ATTR_INVISIBLE;
			break;
		case 29:
			term.c.attr.mode &= ~ATTR_STRUCK;
			break;
		case 38:
			if ((idx = tdefcolor(attr, &i, l)) >= 0)
				term.c.attr.fg = idx;
			break;
		case 39:
			term.c.attr.fg = seloptsch->defaultfg;
			break;
		case 48:
			if ((idx = tdefcolor(attr, &i, l)) >= 0)
				term.c.attr.bg = idx;
			break;
		case 49:
			term.c.attr.bg = seloptsch->defaultbg;
			break;
		case 58: /* underline color (consume cleanly without stderr spam) */
			tdefcolor(attr, &i, l);
			break;
		case 59: /* default underline color */
			break;
		default:
			if (BETWEEN(attr[i], 30, 37)) {
				term.c.attr.fg = attr[i] - 30;
			} else if (BETWEEN(attr[i], 40, 47)) {
				term.c.attr.bg = attr[i] - 40;
			} else if (BETWEEN(attr[i], 90, 97)) {
				term.c.attr.fg = attr[i] - 90 + 8;
			} else if (BETWEEN(attr[i], 100, 107)) {
				term.c.attr.bg = attr[i] - 100 + 8;
			} else {
				fprintf(stderr,
					"erresc(default): gfx attr %d unknown\n",
					attr[i]);
				csidump();
			}
			break;
		}
	}
}

void
tsetscroll(int t, int b)
{
	int temp;

	LIMIT(t, 0, term.row-1);
	LIMIT(b, 0, term.row-1);
	if (t > b) {
		temp = t;
		t = b;
		b = temp;
	}
	term.top = t;
	term.bot = b;
}

void
tsetmode(int priv, int set, const int *args, int narg)
{
	int alt; const int *lim;

	for (lim = args + narg; args < lim; ++args) {
		if (priv == '?') {
			switch (*args) {
			case 1: /* DECCKM -- Cursor key */
				txsetmode(set, MODE_APPCURSOR);
				break;
			case 5: /* DECSCNM -- Reverse video */
				txsetmode(set, MODE_REVERSE);
				break;
			case 6: /* DECOM -- Origin */
				MODBIT(term.c.state, set, CURSOR_ORIGIN);
				tmoveato(0, 0);
				break;
			case 7: /* DECAWM -- Auto wrap */
				MODBIT(term.mode, set, MODE_WRAP);
				break;
			case 0:  /* Error (IGNORED) */
			case 2:  /* DECANM -- ANSI/VT52 (IGNORED) */
			case 3:  /* DECCOLM -- Column  (IGNORED) */
			case 4:  /* DECSCLM -- Scroll (IGNORED) */
			case 8:  /* DECARM -- Auto repeat (IGNORED) */
			case 18: /* DECPFF -- Printer feed (IGNORED) */
			case 19: /* DECPEX -- Printer extent (IGNORED) */
			case 42: /* DECNRCM -- National characters (IGNORED) */
			case 12: /* att610 -- Start blinking cursor (IGNORED) */
				break;
			case 25: /* DECTCEM -- Text Cursor Enable Mode */
				txsetmode(!set, MODE_HIDE);
				break;
			case 9:    /* X10 mouse compatibility mode */
				xsetpointermotion(0);
				txsetmode(0, MODE_MOUSE);
				txsetmode(set, MODE_MOUSEX10);
				break;
			case 1000: /* 1000: report button press */
				xsetpointermotion(0);
				txsetmode(0, MODE_MOUSE);
				txsetmode(set, MODE_MOUSEBTN);
				break;
			case 1002: /* 1002: report motion on button press */
				xsetpointermotion(0);
				txsetmode(0, MODE_MOUSE);
				txsetmode(set, MODE_MOUSEMOTION);
				break;
			case 1003: /* 1003: enable all mouse motions */
				xsetpointermotion(set);
				txsetmode(0, MODE_MOUSE);
				txsetmode(set, MODE_MOUSEMANY);
				break;
			case 1004: /* 1004: send focus events to tty */
				txsetmode(set, MODE_FOCUS);
				break;
			case 1006: /* 1006: extended reporting mode */
				txsetmode(set, MODE_MOUSESGR);
				break;
			case 1034:
				txsetmode(set, MODE_8BIT);
				break;
			case 1049: /* swap screen & set/restore cursor as xterm */
				if (!allowaltscreen)
					break;
				tcursor((set) ? CURSOR_SAVE : CURSOR_LOAD);
				/* FALLTHROUGH */
			case 47: /* swap screen */
			case 1047:
				if (!allowaltscreen)
					break;
				alt = IS_SET(MODE_ALTSCREEN);
				if (alt) {
					tclearregion(0, 0, term.col-1,
							term.row-1);
				}
				if (set ^ alt) /* set is always 1 or 0 */
					tswapscreen();
				if (*args != 1049)
					break;
				/* FALLTHROUGH */
			case 1048:
				tcursor((set) ? CURSOR_SAVE : CURSOR_LOAD);
				break;
			case 2004: /* 2004: bracketed paste mode */
				txsetmode(set, MODE_BRCKTPASTE);
				break;
			/* Not implemented mouse modes. See comments there. */
			case 1001: /* mouse highlight mode; can hang the
				      terminal by design when implemented. */
			case 1005: /* UTF-8 mouse mode; will confuse
				      applications not supporting UTF-8
				      and luit. */
			case 1015: /* urxvt mangled mouse mode; incompatible
				      and can be mistaken for other control
				      codes. */
				break;
      case 2026:
 	      if (set) {
 		      tsync_begin();
 	      } else {
 		      tsync_end();
 	      }
 	      break;
			default:
				fprintf(stderr,
					"erresc: unknown private set/reset mode %d\n",
					*args);
				break;
			}
		} else if (!priv) {
			switch (*args) {
			case 0:  /* Error (IGNORED) */
				break;
			case 2:
				txsetmode(set, MODE_KBDLOCK);
				break;
			case 4:  /* IRM -- Insertion-replacement */
				MODBIT(term.mode, set, MODE_INSERT);
				break;
			case 12: /* SRM -- Send/Receive */
				MODBIT(term.mode, !set, MODE_ECHO);
				break;
			case 20: /* LNM -- Linefeed/new line */
				MODBIT(term.mode, set, MODE_CRLF);
				break;
			default:
				fprintf(stderr,
					"erresc: unknown set/reset mode %d\n",
					*args);
				break;
			}
		}
	}
}

static int
tquerymode(int priv, int mode)
{
	if (priv == '?') {
		switch (mode) {
		case 1:    return (xmode_shadow & MODE_APPCURSOR)   ? 1 : 2;
		case 5:    return (xmode_shadow & MODE_REVERSE)     ? 1 : 2;
		case 6:    return (term.c.state & CURSOR_ORIGIN)    ? 1 : 2;
		case 7:    return (term.mode & MODE_WRAP)           ? 1 : 2;
		case 9:    return (xmode_shadow & MODE_MOUSEX10)    ? 1 : 2;
		case 25:   return !(xmode_shadow & MODE_HIDE)       ? 1 : 2;
		case 47:
		case 1047:
		case 1049: return allowaltscreen ? ((term.mode & MODE_ALTSCREEN) ? 1 : 2) : 0;
		case 1000: return (xmode_shadow & MODE_MOUSEBTN)    ? 1 : 2;
		case 1002: return (xmode_shadow & MODE_MOUSEMOTION) ? 1 : 2;
		case 1003: return (xmode_shadow & MODE_MOUSEMANY)   ? 1 : 2;
		case 1004: return (xmode_shadow & MODE_FOCUS)       ? 1 : 2;
		case 1006: return (xmode_shadow & MODE_MOUSESGR)    ? 1 : 2;
		case 1034: return (xmode_shadow & MODE_8BIT)        ? 1 : 2;
		case 2004: return (xmode_shadow & MODE_BRCKTPASTE)  ? 1 : 2;
		case 2026: return su ? 1 : 2; /* Synchronized Output */
		default:   return 0;
		}
	} else if (!priv) {
		switch (mode) {
		case 2:  return (xmode_shadow & MODE_KBDLOCK) ? 1 : 2;
		case 4:  return (term.mode & MODE_INSERT)     ? 1 : 2;
		case 12: return !(term.mode & MODE_ECHO)      ? 1 : 2;
		case 20: return (term.mode & MODE_CRLF)       ? 1 : 2;
		default: return 0;
		}
	}
	return 0;
}

void
csihandle(void)
{
	char buf[40];
	int len;

	switch (csiescseq.mode[0]) {
	default:
	unknown:
		fprintf(stderr, "erresc: unknown csi ");
		csidump();
		/* die(""); */
		break;
	case '@': /* ICH -- Insert <n> blank char */
		DEFAULT(csiescseq.arg[0], 1);
		tinsertblank(csiescseq.arg[0]);
		break;
	case 'A': /* CUU -- Cursor <n> Up */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term.c.x, term.c.y-csiescseq.arg[0]);
		break;
	case 'B': /* CUD -- Cursor <n> Down */
	case 'e': /* VPR --Cursor <n> Down */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term.c.x, term.c.y+csiescseq.arg[0]);
		break;
	case 'i': /* MC -- Media Copy */
		switch (csiescseq.arg[0]) {
		case 0:
			tdump();
			break;
		case 1:
			tdumpline(term.c.y);
			break;
		case 2:
			tdumpsel();
			break;
		case 4:
			term.mode &= ~MODE_PRINT;
			break;
		case 5:
			term.mode |= MODE_PRINT;
			break;
		}
		break;
	case 'c': /* DA -- Device Attributes */
		if (csiescseq.arg[0] == 0) {
			if (!csiescseq.priv)
				ttywrite(vtiden, strlen(vtiden), 0);
			else if (csiescseq.priv == '>')
				ttywrite("\033[>0;92;0c", 10, 0); /* DA2 */
		}
		break;
	case 'b': /* REP -- if last char is printable print it <n> more times */
		LIMIT(csiescseq.arg[0], 1, 65535);
		if (term.lastc) {
			Rune u = term.lastc;
			int rep = csiescseq.arg[0];
			if (((u >= 0x20 && u < 0x7f) || BETWEEN(u, 0x2500, 0x259F)) &&
			    !(term.mode & (MODE_PRINT|MODE_INSERT)) &&
			    term.trantbl[term.charset] != CS_GRAPHIC0 &&
			    sel.ob.x == -1) {
				Glyph fill = term.c.attr;
				fill.u = u;
				if (u >= 0x2500 && isboxdraw_fast(u))
					fill.mode |= ATTR_BOXDRAW;
				while (rep > 0) {
					if ((term.mode & MODE_WRAP) && (term.c.state & CURSOR_WRAPNEXT)) {
						term.line[term.c.y][term.c.x].mode |= ATTR_WRAP;
						tnewline(1);
					}
					int y = term.c.y, x = term.c.x, col = term.col;
					int n = MIN(rep, col - x);
					Line line = term.line[y];
					if (!term.rowwide[y]) {
						for (int k = 0; k < n; k++)
							line[x + k] = fill;
					} else {
						for (int k = 0; k < n; k++) {
							int cx = x + k;
							if (line[cx].mode & ATTR_WIDE) {
								if (cx + 1 < col) {
									line[cx + 1].u = ' ';
									line[cx + 1].mode &= ~ATTR_WDUMMY;
								}
							} else if (line[cx].mode & ATTR_WDUMMY) {
								line[cx - 1].u = ' ';
								line[cx - 1].mode &= ~ATTR_WIDE;
							}
							line[cx] = fill;
						}
					}
					term.dirty[y] = 1;
					if (x + n > term.rowmax[y])
						term.rowmax[y] = x + n;
					if (x + n < col) {
						term.c.x = x + n;
						term.c.state &= ~CURSOR_WRAPNEXT;
					} else {
						term.c.x = col - 1;
						term.c.state |= CURSOR_WRAPNEXT;
					}
					rep -= n;
				}
				break;
			}
			while (csiescseq.arg[0]-- > 0)
				tputc(term.lastc);
		}
		break;
	case 'C': /* CUF -- Cursor <n> Forward */
	case 'a': /* HPR -- Cursor <n> Forward */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term.c.x+csiescseq.arg[0], term.c.y);
		break;
	case 'D': /* CUB -- Cursor <n> Backward */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(term.c.x-csiescseq.arg[0], term.c.y);
		break;
	case 'E': /* CNL -- Cursor <n> Down and first col */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(0, term.c.y+csiescseq.arg[0]);
		break;
	case 'F': /* CPL -- Cursor <n> Up and first col */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(0, term.c.y-csiescseq.arg[0]);
		break;
	case 'g': /* TBC -- Tabulation clear */
		switch (csiescseq.arg[0]) {
		case 0: /* clear current tab stop */
			term.tabs[term.c.x] = 0;
			break;
		case 3: /* clear all the tabs */
			memset(term.tabs, 0, term.col * sizeof(*term.tabs));
			break;
		default:
			goto unknown;
		}
		break;
	case 'G': /* CHA -- Move to <col> */
	case '`': /* HPA */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveto(csiescseq.arg[0]-1, term.c.y);
		break;
	case 'H': /* CUP -- Move to <row> <col> */
	case 'f': /* HVP */
		DEFAULT(csiescseq.arg[0], 1);
		DEFAULT(csiescseq.arg[1], 1);
		tmoveato(csiescseq.arg[1]-1, csiescseq.arg[0]-1);
		break;
	case 'I': /* CHT -- Cursor Forward Tabulation <n> tab stops */
		DEFAULT(csiescseq.arg[0], 1);
		tputtab(csiescseq.arg[0]);
		break;
	case 'J': /* ED -- Clear screen */
		switch (csiescseq.arg[0]) {
		case 0: /* below */
			tclearregion(term.c.x, term.c.y, term.col-1, term.c.y);
			if (term.c.y < term.row-1) {
				tclearregion(0, term.c.y+1, term.col-1,
						term.row-1);
			}
			break;
		case 1: /* above */
			if (term.c.y > 1)
				tclearregion(0, 0, term.col-1, term.c.y-1);
			tclearregion(0, term.c.y, term.c.x, term.c.y);
			break;
		case 2: /* all */
			tclearregion(0, 0, term.col-1, term.row-1);
			break;
		case 3: /* ED 3 -- clear scrollback history */
			thistclear();
			break;
		default:
			goto unknown;
		}
		break;
	case 'K': /* EL -- Clear line */
		switch (csiescseq.arg[0]) {
		case 0: /* right */
			tclearregion(term.c.x, term.c.y, term.col-1,
					term.c.y);
			break;
		case 1: /* left */
			tclearregion(0, term.c.y, term.c.x, term.c.y);
			break;
		case 2: /* all */
			tclearregion(0, term.c.y, term.col-1, term.c.y);
			break;
		}
		break;
	case 'S': /* SU -- Scroll <n> line up */
		if (csiescseq.priv) break;
		DEFAULT(csiescseq.arg[0], 1);
		tscrollup(term.top, csiescseq.arg[0], 0);
		break;
	case 'T': /* SD -- Scroll <n> line down */
		DEFAULT(csiescseq.arg[0], 1);
		tscrolldown(term.top, csiescseq.arg[0], 0);
		break;
	case 'L': /* IL -- Insert <n> blank lines */
		DEFAULT(csiescseq.arg[0], 1);
		tinsertblankline(csiescseq.arg[0]);
		break;
	case 'l': /* RM -- Reset Mode */
		tsetmode(csiescseq.priv, 0, csiescseq.arg, csiescseq.narg);
		break;
	case 'M': /* DL -- Delete <n> lines */
		DEFAULT(csiescseq.arg[0], 1);
		tdeleteline(csiescseq.arg[0]);
		break;
	case 'X': /* ECH -- Erase <n> char */
		DEFAULT(csiescseq.arg[0], 1);
		tclearregion(term.c.x, term.c.y,
				term.c.x + csiescseq.arg[0] - 1, term.c.y);
		break;
	case 'P': /* DCH -- Delete <n> char */
		DEFAULT(csiescseq.arg[0], 1);
		tdeletechar(csiescseq.arg[0]);
		break;
	case 'Z': /* CBT -- Cursor Backward Tabulation <n> tab stops */
		DEFAULT(csiescseq.arg[0], 1);
		tputtab(-csiescseq.arg[0]);
		break;
	case 'd': /* VPA -- Move to <row> */
		DEFAULT(csiescseq.arg[0], 1);
		tmoveato(term.c.x, csiescseq.arg[0]-1);
		break;
	case 'h': /* SM -- Set terminal mode */
		tsetmode(csiescseq.priv, 1, csiescseq.arg, csiescseq.narg);
		break;
	case 'm': /* SGR -- Terminal attribute (color) */
		if (!csiescseq.priv)
			tsetattr(csiescseq.arg, csiescseq.narg);
		break;
	case 'q':
		if (csiescseq.priv == '>' && csiescseq.arg[0] == 0) {
			/* XTVERSION */
			len = snprintf(buf, sizeof(buf), "\033P>|st(%s)\033\\", VERSION);
			ttywrite(buf, len, 0);
		} else {
			goto unknown;
		}
		break;
	case '$':
		if (csiescseq.mode[1] == 'p') {
			/* DECRQM -- Request Mode (supports ?2026 sync updates etc.) */
			int pm = tquerymode(csiescseq.priv, csiescseq.arg[0]);
			len = snprintf(buf, sizeof(buf), "\033[%s%d;%d$y",
			               csiescseq.priv == '?' ? "?" : "",
			               csiescseq.arg[0], pm);
			ttywrite(buf, len, 0);
		} else {
			goto unknown;
		}
		break;
	case 'n': /* DSR -- Device Status Report */
		switch (csiescseq.arg[0]) {
		case 5: /* Status Report "OK" `0n` */
			ttywrite("\033[0n", sizeof("\033[0n") - 1, 0);
			break;
		case 6: /* Report Cursor Position (CPR) "<row>;<column>R" */
			len = snprintf(buf, sizeof(buf), "\033[%i;%iR",
			               term.c.y+1, term.c.x+1);
			ttywrite(buf, len, 0);
			break;
		default:
			goto unknown;
		}
		break;
	case 'r': /* DECSTBM -- Set Scrolling Region */
		if (csiescseq.priv) {
			goto unknown;
		} else {
			DEFAULT(csiescseq.arg[0], 1);
			DEFAULT(csiescseq.arg[1], term.row);
			tsetscroll(csiescseq.arg[0]-1, csiescseq.arg[1]-1);
			tmoveato(0, 0);
		}
		break;
	case 's': /* DECSC -- Save cursor position (ANSI.SYS) */
		if (!csiescseq.priv)
			tcursor(CURSOR_SAVE);
		break;
	case 'u': /* DECRC -- Restore cursor position (ANSI.SYS) */
		/* Guard against Kitty keyboard queries (CSI ? u / > u / < u). */
		if (!csiescseq.priv)
			tcursor(CURSOR_LOAD);
		break;
	case ' ':
		switch (csiescseq.mode[1]) {
		case 'q': /* DECSCUSR -- Set Cursor Style */
			if (xsetcursor(csiescseq.arg[0]))
				goto unknown;
			break;
		default:
			goto unknown;
		}
		break;
	case 't': /* title stack operations */
		switch (csiescseq.arg[0]) {
		case 22: /* pust current title on stack */
			switch (csiescseq.arg[1]) {
			case 0:
			case 1:
			case 2:
				xpushtitle();
				break;
			default:
				goto unknown;
			}
			break;
		case 23: /* pop last title from stack */
			switch (csiescseq.arg[1]) {
			case 0:
			case 1:
			case 2:
				xsettitle(NULL, 1);
				break;
			default:
				goto unknown;
			}
			break;
		default:
			goto unknown;
		}
	}
}

void
csidump(void)
{
	size_t i;
	uint c;

	fprintf(stderr, "ESC[");
	for (i = 0; i < csiescseq.len; i++) {
		c = csiescseq.buf[i] & 0xff;
		if (isprint(c)) {
			putc(c, stderr);
		} else if (c == '\n') {
			fprintf(stderr, "(\\n)");
		} else if (c == '\r') {
			fprintf(stderr, "(\\r)");
		} else if (c == 0x1b) {
			fprintf(stderr, "(\\e)");
		} else {
			fprintf(stderr, "(%02x)", c);
		}
	}
	putc('\n', stderr);
}

void
csireset(void)
{
	csiescseq.len = 0;
	csiescseq.priv = 0;
	csiescseq.sep = ';';
	csiescseq.narg = 0;
	csiescseq.mode[0] = 0;
	csiescseq.mode[1] = 0;
	memset(csiescseq.arg, 0, sizeof(csiescseq.arg));
}

void
osc_color_response(int num, int index, int is_osc4)
{
	int n;
	char buf[32];
	unsigned char r, g, b;

	if (xgetcolor(is_osc4 ? num : index, &r, &g, &b)) {
		fprintf(stderr, "erresc: failed to fetch %s color %d\n",
		        is_osc4 ? "osc4" : "osc",
		        is_osc4 ? num : index);
		return;
	}

	n = snprintf(buf, sizeof buf, "\033]%s%d;rgb:%02x%02x/%02x%02x/%02x%02x\007",
	             is_osc4 ? "4;" : "", num, r, r, g, g, b, b);
	if (n < 0 || n >= sizeof(buf)) {
		fprintf(stderr, "error: %s while printing %s response\n",
		        n < 0 ? "snprintf failed" : "truncation occurred",
		        is_osc4 ? "osc4" : "osc");
	} else {
		ttywrite(buf, n, 1);
	}
}

static int
hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static void
xtgettcap_handle(char **args, int narg)
{
	static const char hex[] = "0123456789abcdef";
	char resp[256];
	int ai;

	for (ai = 0; ai < narg; ai++) {
		const char *start = (ai == 0) ? (args[0] + 2) : args[ai];
		char name[64];
		size_t nlen = 0, hexlen = strlen(start);
		int ok = 1;

		if (hexlen == 0 || (hexlen & 1) || hexlen / 2 >= sizeof(name)) {
			ok = 0;
		} else {
			for (size_t i = 0; i < hexlen; i += 2) {
				int hi = hexval(start[i]), lo = hexval(start[i + 1]);
				if (hi < 0 || lo < 0) { ok = 0; break; }
				name[nlen++] = (char)((hi << 4) | lo);
			}
			name[nlen] = '\0';
		}
		if (ok && (!strcmp(name, "RGB") || !strcmp(name, "Tc"))) {
			int len = snprintf(resp, sizeof(resp), "\033P1+r%.*s\033\\", (int)hexlen, start);
			ttywrite(resp, len, 0);
		} else if (ok && !strcmp(name, "Sync")) {
			/* \033P=%p1%ds\033\\ */
			const char *r = "\033P1+r53796e63=1b503d2570312564731b5c\033\\";
			ttywrite(r, strlen(r), 0);
		} else if (ok && !strcmp(name, "TN") && termname) {
			int pos = snprintf(resp, sizeof(resp), "\033P1+r%.*s=", (int)hexlen, start);
			for (const char *t = termname; *t && pos + 5 < (int)sizeof(resp); t++) {
				resp[pos++] = hex[((uchar)*t) >> 4];
				resp[pos++] = hex[((uchar)*t) & 0xf];
			}
			resp[pos++] = '\033'; resp[pos++] = '\\';
			ttywrite(resp, pos, 0);
		} else if (hexlen > 0 && hexlen < 128) {
			int len = snprintf(resp, sizeof(resp), "\033P0+r%.*s\033\\", (int)hexlen, start);
			ttywrite(resp, len, 0);
		}
	}
}

void
strhandle(void)
{
	char *p = NULL, *dec;
	int j, narg, par;
	const struct { int idx; char *str; } osc_table[] = {
		{ seloptsch->defaultfg, "foreground" },
		{ seloptsch->defaultbg, "background" },
		{ seloptsch->defaultcs, "cursor" }
	};

	term.esc &= ~(ESC_STR_END|ESC_STR);
	strparse();
	par = (narg = strescseq.narg) ? atoi(strescseq.args[0]) : 0;

	switch (strescseq.type) {
	case ']': /* OSC -- Operating System Command */
		switch (par) {
		case 0:
			if (narg > 1) {
				xsettitle(strescseq.args[1], 0);
				xseticontitle(strescseq.args[1]);
			}
			return;
		case 1:
			if (narg > 1)
				xseticontitle(strescseq.args[1]);
			return;
		case 2:
			if (narg > 1)
				xsettitle(strescseq.args[1], 0);
			return;
		case 8: /* OSC 8 hyperlinks: ignore cleanly without stderr spam */
			return;
		case 52:
			if (narg > 2 && allowwindowops) {
				dec = base64dec(strescseq.args[2]);
				if (dec) {
					xsetsel(dec);
					xclipcopy();
				} else {
					fprintf(stderr, "erresc: invalid base64\n");
				}
			}
			return;
		case 10:
		case 11:
		case 12:
			if (narg < 2)
				break;
			p = strescseq.args[1];
			if ((j = par - 10) < 0 || j >= LEN(osc_table))
				break; /* shouldn't be possible */

			if (!strcmp(p, "?")) {
				osc_color_response(par, osc_table[j].idx, 0);
			} else if (xsetcolorname(osc_table[j].idx, p)) {
				fprintf(stderr, "erresc: invalid %s color: %s\n",
				        osc_table[j].str, p);
			} else {
				tfulldirt();
			}
			return;
		case 4: /* color set */
			if (narg < 3)
				break;
			p = strescseq.args[2];
			/* FALLTHROUGH */
		case 104: /* color reset */
			j = (narg > 1) ? atoi(strescseq.args[1]) : -1;

			if (p && !strcmp(p, "?")) {
				osc_color_response(j, 0, 1);
			} else if (xsetcolorname(j, p)) {
				if (par == 104 && narg <= 1) {
					xloadcols();
					return; /* color reset without parameter */
				}
				fprintf(stderr, "erresc: invalid color j=%d, p=%s\n",
				        j, p ? p : "(null)");
			} else {
				/*
				 * TODO if defaultbg color is changed, borders
				 * are dirty
				 */
				tfulldirt();
			}
			return;
		}
		break;
	case 'k': /* old title set compatibility */
		xsettitle(strescseq.args[0], 0);
		return;
	case 'P': /* DCS -- Device Control String */
		/* https://gitlab.com/gnachman/iterm2/-/wikis/synchronized-updates-spec */
		if (strstr(strescseq.buf, "=1s") == strescseq.buf)
			tsync_begin();  /* BSU */
		else if (strstr(strescseq.buf, "=2s") == strescseq.buf)
			tsync_end();  /* ESU */
		else if (strescseq.buf[0] == '+' && strescseq.buf[1] == 'q')
			xtgettcap_handle(strescseq.args, strescseq.narg); /* XTGETTCAP */
		return;
	case '_': /* APC -- Application Program Command */
	case '^': /* PM -- Privacy Message */
		return;
	}

	fprintf(stderr, "erresc: unknown str ");
	strdump();
}

void
strparse(void)
{
	int c;
	char *p = strescseq.buf;

	strescseq.narg = 0;
	strescseq.buf[strescseq.len] = '\0';

	if (*p == '\0')
		return;

	while (strescseq.narg < STR_ARG_SIZ) {
		strescseq.args[strescseq.narg++] = p;
		while ((c = *p) != ';' && c != '\0')
			++p;
		if (c == '\0')
			return;
		*p++ = '\0';
	}
}

void
externalpipe(const Arg *arg)
{
   int to[2];
   void (*oldsigpipe)(int);
   Glyph *bp;
   int lastpos, n, newline;

   if (pipe(to) == -1)
       return;

   switch (fork()) {
   case -1:
       close(to[0]);
       close(to[1]);
       return;
   case 0:
       dup2(to[0], STDIN_FILENO);
       close(to[0]);
       close(to[1]);
       execvp(((char **)arg->v)[0], (char **)arg->v);
       fprintf(stderr, "st: execvp %s\n", ((char **)arg->v)[0]);
       perror("failed");
       exit(0);
   }

   close(to[0]);
   /* ignore sigpipe for now, in case child exists early */
   oldsigpipe = signal(SIGPIPE, SIG_IGN);
   newline = 0;
   char *linebuf = xmalloc((size_t)term.col * UTF_SIZ + 2);
   Glyph *hscratch = xmalloc((size_t)term.col * sizeof(Glyph));
   for (n = 0; n <= HISTSIZE + 2; n++) {
       size_t outlen = 0;
       int w, len, x;

       /*
        * Same row order and output as before: history slots by index,
        * then the screen. History lines are read in trimmed form (cells
        * from w on are default blanks) rather than widened, which would
        * re-inflate up to HISTSIZE lines in one go.
        */
       if (n <= HISTSIZE - term.row + 2) {
           if (term.histfmt[n] == HIST_EMPTY || !term.hist[n]) {
               bp = NULL;
               w = 0;
           } else if (term.histfmt[n] == HIST_GLYPH) {
               bp = (Line)term.hist[n];
               w = MIN(term.histcol[n], term.col);
           } else {
               w = MIN(term.histcol[n], term.col);
               thistunpack(n, hscratch, w);
               bp = hscratch;
           }
       } else {
           bp = term.line[n - HISTSIZE + term.row - 3];
           w = term.col;
       }
       /* skip history lines that were never written (lazily allocated) */
       if (!bp)
           continue;
       if (w == term.col && (bp[w - 1].mode & ATTR_WRAP)) {
           len = w;
       } else {
           for (len = w; len > 0 && bp[len - 1].u == ' '; len--)
               ;
       }
       lastpos = MIN(len + 1, term.col) - 1;
       if (lastpos < 0)
           break;
       if (lastpos == 0)
           continue;
       for (x = 0; x <= lastpos; x++)
           outlen += utf8encode(x < w ? bp[x].u : ' ', linebuf + outlen);
       if ((newline = lastpos < w && (bp[lastpos].mode & ATTR_WRAP))) {
           if (outlen > 0 && xwrite(to[1], linebuf, outlen) < 0)
               break;
           continue;
       }
       linebuf[outlen++] = '\n';
       if (xwrite(to[1], linebuf, outlen) < 0)
           break;
       newline = 0;
   }
   free(hscratch);
   free(linebuf);
   if (newline)
       (void)xwrite(to[1], "\n", 1);
   close(to[1]);
   /* restore */
   signal(SIGPIPE, oldsigpipe);
}

void
strdump(void)
{
	size_t i;
	uint c;

	fprintf(stderr, "ESC%c", strescseq.type);
	for (i = 0; i < strescseq.len; i++) {
		c = strescseq.buf[i] & 0xff;
		if (c == '\0') {
			putc('\n', stderr);
			return;
		} else if (isprint(c)) {
			putc(c, stderr);
		} else if (c == '\n') {
			fprintf(stderr, "(\\n)");
		} else if (c == '\r') {
			fprintf(stderr, "(\\r)");
		} else if (c == 0x1b) {
			fprintf(stderr, "(\\e)");
		} else {
			fprintf(stderr, "(%02x)", c);
		}
	}
	fprintf(stderr, "ESC\\\n");
}

void
strreset(void)
{
	strescseq = (STREscape){
		.buf = xrealloc(strescseq.buf, STR_BUF_SIZ),
		.siz = STR_BUF_SIZ,
	};
}

void
sendbreak(const Arg *arg)
{
	if (tcsendbreak(cmdfd, 0))
		perror("Error sending break");
}

void
tprinter(char *s, size_t len)
{
	if (iofd != -1 && xwrite(iofd, s, len) < 0) {
		perror("Error writing to output file");
		close(iofd);
		iofd = -1;
	}
}

void
toggleprinter(const Arg *arg)
{
	term.mode ^= MODE_PRINT;
}

void
printscreen(const Arg *arg)
{
	tdump();
}

void
printsel(const Arg *arg)
{
	tdumpsel();
}

void
tdumpsel(void)
{
	char *ptr;

	if ((ptr = getsel())) {
		tprinter(ptr, strlen(ptr));
		free(ptr);
	}
}

void
tdumpline(int n)
{
	char buf[UTF_SIZ];
	const Glyph *bp, *end;

	bp = &term.line[n][0];
	end = &bp[MIN(tlinelen(n), term.col) - 1];
	if (bp != end || bp->u != ' ') {
		for ( ; bp <= end; ++bp)
			tprinter(buf, utf8encode(bp->u, buf));
	}
	tprinter("\n", 1);
}

void
tdump(void)
{
	int i;

	for (i = 0; i < term.row; ++i)
		tdumpline(i);
}

void
tputtab(int n)
{
	uint x = term.c.x;

	if (n > 0) {
		while (x < term.col && n--)
			for (++x; x < term.col && !term.tabs[x]; ++x)
				/* nothing */ ;
	} else if (n < 0) {
		while (x > 0 && n++)
			for (--x; x > 0 && !term.tabs[x]; --x)
				/* nothing */ ;
	}
	term.c.x = LIMIT(x, 0, term.col-1);
}

void
tdefutf8(char ascii)
{
	if (ascii == 'G')
		term.mode |= MODE_UTF8;
	else if (ascii == '@')
		term.mode &= ~MODE_UTF8;
}

void
tdeftran(char ascii)
{
	static char cs[] = "0B";
	static int vcs[] = {CS_GRAPHIC0, CS_USA};
	char *p;

	if ((p = strchr(cs, ascii)) == NULL) {
		fprintf(stderr, "esc unhandled charset: ESC ( %c\n", ascii);
	} else {
		term.trantbl[term.icharset] = vcs[p - cs];
	}
}

void
tdectest(char c)
{
	int x, y;

	if (c == '8') { /* DEC screen alignment test. */
		for (x = 0; x < term.col; ++x) {
			for (y = 0; y < term.row; ++y)
				tsetchar('E', &term.c.attr, x, y);
		}
	}
}

void
tstrsequence(uchar c)
{
	switch (c) {
	case 0x90:   /* DCS -- Device Control String */
		c = 'P';
		break;
	case 0x9f:   /* APC -- Application Program Command */
		c = '_';
		break;
	case 0x9e:   /* PM -- Privacy Message */
		c = '^';
		break;
	case 0x9d:   /* OSC -- Operating System Command */
		c = ']';
		break;
	}
	strreset();
	strescseq.type = c;
	term.esc |= ESC_STR;
}

void
tcontrolcode(uchar ascii)
{
	switch (ascii) {
	case '\t':   /* HT */
		tputtab(1);
		return;
	case '\b':   /* BS */
		tmoveto(term.c.x-1, term.c.y);
		return;
	case '\r':   /* CR */
		tmoveto(0, term.c.y);
		return;
	case '\f':   /* LF */
	case '\v':   /* VT */
	case '\n':   /* LF */
		/* go to first col if the mode is set */
		tnewline(IS_SET(MODE_CRLF));
		return;
	case '\a':   /* BEL */
		if (term.esc & ESC_STR_END) {
			/* backwards compatibility to xterm */
			strhandle();
		} else {
			xbell();
		}
		break;
	case '\033': /* ESC */
		csireset();
		term.esc &= ~(ESC_CSI|ESC_ALTCHARSET|ESC_TEST);
		term.esc |= ESC_START;
		return;
	case '\016': /* SO (LS1 -- Locking shift 1) */
	case '\017': /* SI (LS0 -- Locking shift 0) */
		term.charset = 1 - (ascii - '\016');
		return;
	case '\032': /* SUB */
		tsetchar('?', &term.c.attr, term.c.x, term.c.y);
		/* FALLTHROUGH */
	case '\030': /* CAN */
		csireset();
		break;
	case '\005': /* ENQ (IGNORED) */
	case '\000': /* NUL (IGNORED) */
	case '\021': /* XON (IGNORED) */
	case '\023': /* XOFF (IGNORED) */
	case 0177:   /* DEL (IGNORED) */
		return;
	case 0x80:   /* TODO: PAD */
	case 0x81:   /* TODO: HOP */
	case 0x82:   /* TODO: BPH */
	case 0x83:   /* TODO: NBH */
	case 0x84:   /* TODO: IND */
		break;
	case 0x85:   /* NEL -- Next line */
		tnewline(1); /* always go to first col */
		break;
	case 0x86:   /* TODO: SSA */
	case 0x87:   /* TODO: ESA */
		break;
	case 0x88:   /* HTS -- Horizontal tab stop */
		term.tabs[term.c.x] = 1;
		break;
	case 0x89:   /* TODO: HTJ */
	case 0x8a:   /* TODO: VTS */
	case 0x8b:   /* TODO: PLD */
	case 0x8c:   /* TODO: PLU */
	case 0x8d:   /* TODO: RI */
	case 0x8e:   /* TODO: SS2 */
	case 0x8f:   /* TODO: SS3 */
	case 0x91:   /* TODO: PU1 */
	case 0x92:   /* TODO: PU2 */
	case 0x93:   /* TODO: STS */
	case 0x94:   /* TODO: CCH */
	case 0x95:   /* TODO: MW */
	case 0x96:   /* TODO: SPA */
	case 0x97:   /* TODO: EPA */
	case 0x98:   /* TODO: SOS */
	case 0x99:   /* TODO: SGCI */
		break;
	case 0x9a:   /* DECID -- Identify Terminal */
		ttywrite(vtiden, strlen(vtiden), 0);
		break;
	case 0x9b:   /* TODO: CSI */
	case 0x9c:   /* TODO: ST */
		break;
	case 0x90:   /* DCS -- Device Control String */
	case 0x9d:   /* OSC -- Operating System Command */
	case 0x9e:   /* PM -- Privacy Message */
	case 0x9f:   /* APC -- Application Program Command */
		tstrsequence(ascii);
		return;
	}
	/* only CAN, SUB, \a and C1 chars interrupt a sequence */
	term.esc &= ~(ESC_STR_END|ESC_STR);
}

/*
 * returns 1 when the sequence is finished and it hasn't to read
 * more characters for this sequence, otherwise 0
 */
int
eschandle(uchar ascii)
{
	switch (ascii) {
	case '[':
		term.esc |= ESC_CSI;
		return 0;
	case '#':
		term.esc |= ESC_TEST;
		return 0;
	case '%':
		term.esc |= ESC_UTF8;
		return 0;
	case 'P': /* DCS -- Device Control String */
	case '_': /* APC -- Application Program Command */
	case '^': /* PM -- Privacy Message */
	case ']': /* OSC -- Operating System Command */
	case 'k': /* old title set compatibility */
		tstrsequence(ascii);
		return 0;
	case 'n': /* LS2 -- Locking shift 2 */
	case 'o': /* LS3 -- Locking shift 3 */
		term.charset = 2 + (ascii - 'n');
		break;
	case '(': /* GZD4 -- set primary charset G0 */
	case ')': /* G1D4 -- set secondary charset G1 */
	case '*': /* G2D4 -- set tertiary charset G2 */
	case '+': /* G3D4 -- set quaternary charset G3 */
		term.icharset = ascii - '(';
		term.esc |= ESC_ALTCHARSET;
		return 0;
	case 'D': /* IND -- Linefeed */
		if (term.c.y == term.bot) {
			tscrollup(term.top, 1, 1);
		} else {
			tmoveto(term.c.x, term.c.y+1);
		}
		break;
	case 'E': /* NEL -- Next line */
		tnewline(1); /* always go to first col */
		break;
	case 'H': /* HTS -- Horizontal tab stop */
		term.tabs[term.c.x] = 1;
		break;
	case 'M': /* RI -- Reverse index */
		if (term.c.y == term.top) {
			tscrolldown(term.top, 1, 1);
		} else {
			tmoveto(term.c.x, term.c.y-1);
		}
		break;
	case 'Z': /* DECID -- Identify Terminal */
		ttywrite(vtiden, strlen(vtiden), 0);
		break;
	case 'c': /* RIS -- Reset to initial state */
		treset();
    xfreetitlestack();
		resettitle();
		xloadcols();
		txsetmode(0, MODE_HIDE);
		break;
	case '=': /* DECPAM -- Application keypad */
		txsetmode(1, MODE_APPKEYPAD);
		break;
	case '>': /* DECPNM -- Normal keypad */
		txsetmode(0, MODE_APPKEYPAD);
		break;
	case '7': /* DECSC -- Save Cursor */
		tcursor(CURSOR_SAVE);
		break;
	case '8': /* DECRC -- Restore Cursor */
		tcursor(CURSOR_LOAD);
		break;
	case '\\': /* ST -- String Terminator */
		if (term.esc & ESC_STR_END)
			strhandle();
		break;
	default:
		fprintf(stderr, "erresc: unknown sequence ESC 0x%02X '%c'\n",
			(uchar) ascii, isprint(ascii)? ascii:'.');
		break;
	}
	return 1;
}

void
tputc(Rune u)
{
	char c[UTF_SIZ];
	int control;
	int width, len = 0;
	Glyph *gp;

	control = ISCONTROL(u);
	if (u < 127 || !IS_SET(MODE_UTF8)) {
		c[0] = u;
		width = len = 1;
	} else {
		/* Encode back to UTF-8 lazily only when printer or STR needs it. */
		if (IS_SET(MODE_PRINT) || (term.esc & ESC_STR))
			len = utf8encode(u, c);
		if (control) {
			width = 0;
		} else if (BETWEEN(u, 0x2500, 0x259F)) {
			width = 1;
		} else if ((width = wcwidth(u)) == -1) {
			width = 1;
		}
	}

	if (IS_SET(MODE_PRINT))
		tprinter(c, len);

	/*
	 * STR sequence must be checked before anything else
	 * because it uses all following characters until it
	 * receives a ESC, a SUB, a ST or any other C1 control
	 * character.
	 */
	if (term.esc & ESC_STR) {
		if (u == '\a' || u == 030 || u == 032 || u == 033 ||
		   ISCONTROLC1(u)) {
			term.esc &= ~(ESC_START|ESC_STR);
			term.esc |= ESC_STR_END;
			goto check_control_code;
		}

		if (strescseq.len+len >= strescseq.siz) {
			/*
			 * Here is a bug in terminals. If the user never sends
			 * some code to stop the str or esc command, then st
			 * will stop responding. But this is better than
			 * silently failing with unknown characters. At least
			 * then users will report back.
			 *
			 * In the case users ever get fixed, here is the code:
			 */
			/*
			 * term.esc = 0;
			 * strhandle();
			 */
			if (strescseq.siz > (SIZE_MAX - UTF_SIZ) / 2)
				return;
			strescseq.siz *= 2;
			strescseq.buf = xrealloc(strescseq.buf, strescseq.siz);
		}

		memmove(&strescseq.buf[strescseq.len], c, len);
		strescseq.len += len;
		return;
	}

check_control_code:
	/*
	 * Actions of control codes must be performed as soon they arrive
	 * because they can be embedded inside a control sequence, and
	 * they must not cause conflicts with sequences.
	 */
	if (control) {
		/* in UTF-8 mode ignore handling C1 control characters */
		if (IS_SET(MODE_UTF8) && ISCONTROLC1(u))
			return;
		tcontrolcode(u);
		/*
		 * control codes are not shown ever
		 */
		if (!term.esc)
			term.lastc = 0;
		return;
	} else if (term.esc & ESC_START) {
		if (term.esc & ESC_CSI) {
			csiescseq.buf[csiescseq.len++] = u;
			if (BETWEEN(u, 0x40, 0x7E)
					|| csiescseq.len >= \
					sizeof(csiescseq.buf)-1) {
				term.esc = 0;
				csiparse();
				csihandle();
			}
			return;
		} else if (term.esc & ESC_UTF8) {
			tdefutf8(u);
		} else if (term.esc & ESC_ALTCHARSET) {
			tdeftran(u);
		} else if (term.esc & ESC_TEST) {
			tdectest(u);
		} else {
			if (!eschandle(u))
				return;
			/* sequence already finished */
		}
		term.esc = 0;
		/*
		 * All characters which form part of a sequence are not
		 * printed
		 */
		return;
	}
	/* cheap inline test first: this runs for every printed character */
	if (sel.ob.x != -1 && selected(term.c.x, term.c.y))
		selclear();

	gp = &term.line[term.c.y][term.c.x];
	if (IS_SET(MODE_WRAP) && (term.c.state & CURSOR_WRAPNEXT)) {
		gp->mode |= ATTR_WRAP;
		tnewline(1);
		gp = &term.line[term.c.y][term.c.x];
	}

	if (IS_SET(MODE_INSERT) && term.c.x+width < term.col) {
		memmove(gp+width, gp, (term.col - term.c.x - width) * sizeof(Glyph));
		gp->mode &= ~ATTR_WIDE;
	}

	if (term.c.x+width > term.col) {
		if (IS_SET(MODE_WRAP))
			tnewline(1);
		else
			tmoveto(term.col - width, term.c.y);
		gp = &term.line[term.c.y][term.c.x];
	}

	tsetchar(u, &term.c.attr, term.c.x, term.c.y);
	term.lastc = u;

	if (width == 2) {
		gp->mode |= ATTR_WIDE;
		term.rowwide[term.c.y] = 1;
		if (term.c.x+1 < term.col) {
			if (gp[1].mode == ATTR_WIDE && term.c.x+2 < term.col) {
				gp[2].u = ' ';
				gp[2].mode &= ~ATTR_WDUMMY;
			}
			gp[1].u = '\0';
			gp[1].mode = ATTR_WDUMMY;
			if (term.c.x + 2 > term.rowmax[term.c.y])
				term.rowmax[term.c.y] = term.c.x + 2;
		}
	}
	if (term.c.x+width < term.col) {
		tmoveto(term.c.x+width, term.c.y);
	} else {
		term.c.state |= CURSOR_WRAPNEXT;
	}
}

/*
 * Fast path for a run of printable ASCII in plain-text state: the bulk of
 * all terminal output, and tputc() was the top CPU cost while printing.
 * It must leave exactly the state tputc() would (tests/test_st.c fuzzes
 * the two against each other), so it only runs when none of the cases
 * tputc() handles specially can apply: an escape or string sequence in
 * progress, printer or insert mode, the DEC line-drawing charset (which
 * translates 0x41-0x7e), or an active selection (which output can clear).
 * Returns the number of bytes consumed; 0 means "use tputc()".
 */
static int asciifastpath = 1;
static int bulk_min_bytes = 4096; /* flood threshold: only use twrite_bulk() on >=4KB bursts */
static long bulk_hits = 0;        /* count of flooded batches handled by twrite_bulk() */

/*
 * Apply a clean semicolon-separated SGR sequence ("\033[<digits/;>m") to
 * `attr` without mutating global `term` or printing stderr diagnostics.
 * Returns 1 on success, or 0 if the sequence uses exotic/unknown syntax
 * (caller bails out to the general CSI parser).
 */
static int
tsgr_apply_fast(const char *s, int len, Glyph *attr, int *saw_blink)
{
	int args[ESC_ARG_SIZ];
	int narg = 0, i = 0;

	if (len == 0) {
		args[0] = 0;
		narg = 1;
	} else {
		while (i < len) {
			int v = 0;
			if (narg >= ESC_ARG_SIZ)
				return 0;
			while (i < len && s[i] >= '0' && s[i] <= '9') {
				v = v * 10 + (s[i] - '0');
				if (v > 255)
					return 0;
				i++;
			}
			args[narg++] = v;
			if (i < len) {
				if (s[i] != ';')
					return 0;
				i++;
				if (i == len) {
					if (narg >= ESC_ARG_SIZ)
						return 0;
					args[narg++] = 0;
				}
			}
		}
	}

	for (i = 0; i < narg; i++) {
		int a = args[i];
		switch (a) {
		case 0:
			attr->mode &= ~(ATTR_BOLD | ATTR_FAINT | ATTR_ITALIC |
			                ATTR_UNDERLINE | ATTR_BLINK | ATTR_REVERSE |
			                ATTR_INVISIBLE | ATTR_STRUCK);
			attr->fg = seloptsch->defaultfg;
			attr->bg = seloptsch->defaultbg;
			break;
		case 1: attr->mode |= ATTR_BOLD; break;
		case 2: attr->mode |= ATTR_FAINT; break;
		case 3: attr->mode |= ATTR_ITALIC; break;
		case 4: attr->mode |= ATTR_UNDERLINE; break;
		case 5:
		case 6:
			attr->mode |= ATTR_BLINK;
			if (saw_blink) *saw_blink = 1;
			break;
		case 7: attr->mode |= ATTR_REVERSE; break;
		case 8: attr->mode |= ATTR_INVISIBLE; break;
		case 9: attr->mode |= ATTR_STRUCK; break;
		case 22: attr->mode &= ~(ATTR_BOLD | ATTR_FAINT); break;
		case 23: attr->mode &= ~ATTR_ITALIC; break;
		case 24: attr->mode &= ~ATTR_UNDERLINE; break;
		case 25: attr->mode &= ~ATTR_BLINK; break;
		case 27: attr->mode &= ~ATTR_REVERSE; break;
		case 28: attr->mode &= ~ATTR_INVISIBLE; break;
		case 29: attr->mode &= ~ATTR_STRUCK; break;
		case 38:
		case 48:
		case 58:
			if (i + 1 >= narg)
				return 0;
			if (args[i + 1] == 5) {
				if (i + 2 >= narg)
					return 0;
				uint32_t c = (uint32_t)args[i + 2];
				if (a == 38) attr->fg = c;
				else if (a == 48) attr->bg = c;
				i += 2;
			} else if (args[i + 1] == 2) {
				if (i + 4 >= narg)
					return 0;
				uint32_t c = TRUECOLOR(args[i + 2], args[i + 3], args[i + 4]);
				if (a == 38) attr->fg = c;
				else if (a == 48) attr->bg = c;
				i += 4;
			} else {
				return 0;
			}
			break;
		case 39: attr->fg = seloptsch->defaultfg; break;
		case 49: attr->bg = seloptsch->defaultbg; break;
		case 59: break;
		default:
			if (BETWEEN(a, 30, 37))
				attr->fg = (uint32_t)(a - 30);
			else if (BETWEEN(a, 40, 47))
				attr->bg = (uint32_t)(a - 40);
			else if (BETWEEN(a, 90, 97))
				attr->fg = (uint32_t)(a - 90 + 8);
			else if (BETWEEN(a, 100, 107))
				attr->bg = (uint32_t)(a - 100 + 8);
			else
				return 0;
			break;
		}
	}
	return 1;
}

/* Encode a validated SGR+ASCII line directly from PTY bytes into compact history. */
static void
thistpush_sgr_raw(int idx, const char *s, int raw_len, int cols, int wrapped,
                  Glyph attr)
{
	static uchar *pbuf = NULL;
	static size_t pcap = 0;
	size_t max_need = (size_t)cols * (sizeof(HistRun) + 1) + sizeof(HistRun) + 4;
	uint32_t dfg = seloptsch->defaultfg;
	uint32_t dbg = seloptsch->defaultbg;
	uchar *p;
	int pos = 0, out_cols = 0;
	size_t last_hdr_off = (size_t)-1;
	HistRun last_hdr;

	if (cols == 0) {
		thistpush_sgr_packed(idx, NULL, 0, 0);
		return;
	}
	if (max_need > pcap) {
		pcap = MAX(max_need, 8192);
		pbuf = xrealloc(pbuf, pcap);
	}
	p = pbuf;
	memset(&last_hdr, 0, sizeof(last_hdr));

	while (pos < raw_len) {
		uchar b = (uchar)s[pos];
		if (b == '\033') {
			int j = pos + 2;
			if (j < raw_len && s[j] == 'K') {
				pos = j + 1;
				continue;
			}
			if (j + 1 < raw_len && s[j] == '0' && s[j + 1] == 'K') {
				pos = j + 2;
				continue;
			}
			while (j < raw_len && s[j] != 'm')
				j++;
			tsgr_apply_fast(s + pos + 2, j - (pos + 2), &attr, NULL);
			pos = j + 1;
			continue;
		}
		int rstart = pos;
		while (pos < raw_len && (uchar)s[pos] >= 0x20 && (uchar)s[pos] <= 0x7e)
			pos++;
		int rlen = pos - rstart;
		if (rlen <= 0)
			continue;

		/* Merge with previous run if attributes match (e.g. redundant SGR). */
		if (last_hdr_off != (size_t)-1 &&
		    last_hdr.mode == attr.mode && last_hdr.fg == attr.fg &&
		    last_hdr.bg == attr.bg && (int)last_hdr.count + rlen <= 65535) {
			last_hdr.count = (uint16_t)(last_hdr.count + rlen);
			memcpy(pbuf + last_hdr_off, &last_hdr, sizeof(last_hdr));
			memcpy(p, s + rstart, (size_t)rlen);
			p += rlen;
			out_cols += rlen;
		} else {
			int off = 0;
			while (off < rlen) {
				int chunk = MIN(rlen - off, 65535);
				last_hdr_off = (size_t)(p - pbuf);
				last_hdr.count = (uint16_t)chunk;
				last_hdr.mode = attr.mode;
				last_hdr.fg = attr.fg;
				last_hdr.bg = attr.bg;
				memcpy(p, &last_hdr, sizeof(last_hdr));
				p += sizeof(last_hdr);
				memcpy(p, s + rstart + off, (size_t)chunk);
				p += chunk;
				out_cols += chunk;
				off += chunk;
			}
		}
	}

	/*
	 * Trim trailing default-blank spaces (matching thistlen()) when not wrapped.
	 * Because adjacent identical runs were merged above, default-blank runs at
	 * the tail can only span the final run.
	 */
	if (!wrapped && last_hdr_off != (size_t)-1 &&
	    last_hdr.mode == 0 && last_hdr.fg == dfg && last_hdr.bg == dbg) {
		int trim = 0;
		uchar *run_bytes = pbuf + last_hdr_off + sizeof(HistRun);
		while (trim < (int)last_hdr.count &&
		       run_bytes[last_hdr.count - 1 - trim] == ' ')
			trim++;
		if (trim > 0) {
			out_cols -= trim;
			p -= trim;
			if (trim == (int)last_hdr.count) {
				p = pbuf + last_hdr_off;
				last_hdr_off = (size_t)-1;
			} else {
				last_hdr.count = (uint16_t)(last_hdr.count - trim);
				memcpy(pbuf + last_hdr_off, &last_hdr, sizeof(last_hdr));
			}
		}
	}

	if (out_cols == 0) {
		thistpush_sgr_packed(idx, NULL, 0, 0);
		return;
	}

	/* If wrapped, only the final cell carries ATTR_WRAP. */
	if (wrapped && last_hdr_off != (size_t)-1) {
		if (last_hdr.count == 1) {
			last_hdr.mode |= ATTR_WRAP;
			memcpy(pbuf + last_hdr_off, &last_hdr, sizeof(last_hdr));
		} else {
			uchar last_ch = p[-1];
			HistRun whdr = last_hdr;
			last_hdr.count--;
			memcpy(pbuf + last_hdr_off, &last_hdr, sizeof(last_hdr));
			p--;
			whdr.count = 1;
			whdr.mode |= ATTR_WRAP;
			memcpy(p, &whdr, sizeof(whdr));
			p += sizeof(whdr);
			*p++ = last_ch;
		}
	}

	/* If only one default-attr run remains, store as HIST_ASCII (1 B/cell). */
	if (last_hdr_off == 0 && last_hdr.fg == dfg && last_hdr.bg == dbg &&
	    (last_hdr.mode == 0 || (last_hdr.count == 1 && last_hdr.mode == ATTR_WRAP))) {
		thistpush_ascii(idx, (const char *)(pbuf + sizeof(HistRun)), out_cols,
		                (last_hdr.mode & ATTR_WRAP) != 0);
		return;
	}

	thistpush_sgr_packed(idx, pbuf, (size_t)(p - pbuf), out_cols);
}

/* Render a validated BulkLine directly onto visible screen row `y`. */
static void
twrite_bulk_render_row(int y, const char *s, int raw_len, int cols,
                       int is_sgr, int wrapped, Glyph attr)
{
	Line line = term.line[y];
	int prev_rm = MIN(term.rowmax[y], term.col);
	Glyph blank;
	int x;

	tdefblank(&blank);
	if (!is_sgr) {
		for (x = 0; x < cols; x++) {
			line[x] = blank;
			line[x].u = (uchar)s[x];
		}
	} else {
		int pos = 0;
		x = 0;
		while (pos < raw_len && x < term.col) {
			uchar b = (uchar)s[pos];
			if (b == '\033') {
				int j = pos + 2;
				if (j < raw_len && s[j] == 'K') {
					pos = j + 1;
					continue;
				}
				if (j + 1 < raw_len && s[j] == '0' && s[j + 1] == 'K') {
					pos = j + 2;
					continue;
				}
				while (j < raw_len && s[j] != 'm')
					j++;
				tsgr_apply_fast(s + pos + 2, j - (pos + 2), &attr, NULL);
				pos = j + 1;
				continue;
			}
			Glyph g;
			memset(&g, 0, sizeof(g));
			g.mode = attr.mode;
			g.fg = attr.fg;
			g.bg = attr.bg;
			while (pos < raw_len && x < term.col &&
			       (uchar)s[pos] >= 0x20 && (uchar)s[pos] <= 0x7e) {
				line[x] = g;
				line[x].u = (uchar)s[pos++];
				x++;
			}
		}
	}
	if (wrapped && cols > 0)
		line[cols - 1].mode |= ATTR_WRAP;
	for (x = cols; x < prev_rm; x++)
		line[x] = blank;
	term.rowmax[y] = cols;
	term.rowwide[y] = 0;
}

#define BULK_MAX_LINES 2048
typedef struct {
	int off;
	int raw_len;
	int cols;
	uchar is_sgr;
	uchar wrapped;
	uchar last_ch;
	Glyph start_attr;
} BulkLine;

/*
 * Direct-to-history bulk fast path for scrolling floods (plain ASCII and
 * semicolon-separated SGR ASCII, including soft-wrapped lines).
 *
 * Enabled only under flooded conditions: incoming buffer >= bulk_min_bytes
 * AND at least `term.row` complete lines arrive while the cursor is at
 * (0, term.bot) on a clean full-screen scroll region. Overflow lines go
 * straight into compact history (or are skipped on alt screen) without
 * rotating term.line[], calling tclearregion(), or expanding to 16-byte
 * Glyphs; only the final `term.row - 1` visible rows are rendered onto screen.
 *
 * Returns bytes consumed (>= 0); 0 means "use normal twrite() path".
 */
static int
twrite_bulk(const char *s, int len, int *fail_off)
{
	static BulkLine blines[BULK_MAX_LINES];

	uint32_t dfg = seloptsch->defaultfg;
	uint32_t dbg = seloptsch->defaultbg;
	int col = term.col;
	int rows = term.row;
	int crlf = IS_SET(MODE_CRLF);
	int wrap = IS_SET(MODE_WRAP);
	Glyph cur_attr = term.c.attr;
	int saw_blink = 0;
	int pos = 0, k = 0;
	int max_pos = 0;

	/* Quick check: a flood of lines must contain a newline early in the buffer. */
	if (!memchr(s, '\n', (size_t)MIN(len, 4096))) {
		if (fail_off) *fail_off = MIN(len, 4096);
		return 0;
	}

	while (pos < len && k < BULK_MAX_LINES) {
		int lstart = pos;
		int lcols = 0;
		Glyph line_start_attr = cur_attr;
		int line_sgr = (cur_attr.mode != 0 || cur_attr.fg != dfg || cur_attr.bg != dbg);
		int line_blink = 0;
		uchar last_ch = 0;
		int ok_line = 0;
		int wrapped = 0;
		int raw_end = pos;

		for (;;) {
			if (pos >= len)
				break;
			uchar b = (uchar)s[pos];
			if (b >= 0x20 && b <= 0x7e) {
				if (lcols == col) {
					/*
					 * Soft-wrap on next printable char: valid only when
					 * MODE_WRAP is on and cur_attr has default fg/bg so
					 * tscrollup()'s tclearregion() produces a default-blank row.
					 */
					if (wrap && cur_attr.fg == dfg && cur_attr.bg == dbg) {
						raw_end = pos;
						wrapped = 1;
						ok_line = 1;
					}
					break;
				}
				int lim = MIN(len - pos, col - lcols);
				int run = 0;
				while (run < lim) {
					uchar c = (uchar)s[pos + run];
					if (c < 0x20 || c > 0x7e)
						break;
					run++;
				}
				pos += run;
				lcols += run;
				last_ch = (uchar)s[pos - 1];
				continue;
			}
			if (b == '\r' && pos + 1 < len && s[pos + 1] == '\n') {
				if (cur_attr.fg == dfg && cur_attr.bg == dbg) {
					raw_end = pos;
					pos += 2;
					ok_line = 1;
				}
				break;
			}
			if (b == '\n' && (crlf || lcols == 0)) {
				if (cur_attr.fg == dfg && cur_attr.bg == dbg) {
					raw_end = pos;
					pos += 1;
					ok_line = 1;
				}
				break;
			}
			if (b == '\033' && pos + 2 < len && s[pos + 1] == '[' && lcols < col) {
				int j = pos + 2;
				/* EL 0 (\033[K or \033[0K) on a clean row with default fg/bg is a no-op. */
				if (cur_attr.fg == dfg && cur_attr.bg == dbg) {
					if (s[j] == 'K') {
						line_sgr = 1;
						pos = j + 1;
						continue;
					}
					if (s[j] == '0' && j + 1 < len && s[j + 1] == 'K') {
						line_sgr = 1;
						pos = j + 2;
						continue;
					}
				}
				int cap = MIN(len, pos + 2 + ESC_BUF_SIZ - 1);
				while (j < cap && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';'))
					j++;
				if (j < cap && s[j] == 'm') {
					Glyph next_attr = cur_attr;
					if (tsgr_apply_fast(s + pos + 2, j - (pos + 2),
					                    &next_attr, &line_blink)) {
						cur_attr = next_attr;
						line_sgr = 1;
						pos = j + 1;
						continue;
					}
				}
			}
			break;
		}

		if (!ok_line) {
			/* Roll back partial line state so normal twrite() handles it exactly. */
			if (pos > max_pos) max_pos = pos;
			cur_attr = line_start_attr;
			pos = lstart;
			break;
		}
		if (pos > max_pos) max_pos = pos;

		if (line_blink)
			saw_blink = 1;
		blines[k].off = lstart;
		blines[k].raw_len = raw_end - lstart;
		blines[k].cols = lcols;
		blines[k].is_sgr = (uchar)line_sgr;
		blines[k].wrapped = (uchar)wrapped;
		blines[k].last_ch = last_ch;
		blines[k].start_attr = line_start_attr;
		k++;
	}

	/* Stay disabled under normal output; only engage when lines overflow the window. */
	if (k < rows || k < 2) {
		if (fail_off) *fail_off = max_pos;
		return 0;
	}
	bulk_hits++;

	int vis = rows - 1;
	int alt = IS_SET(MODE_ALTSCREEN);

	/* Step 1: Push scrolled-off lines directly to compact history (skip on alt screen). */
	if (!alt) {
		int skip = (k > HISTSIZE) ? (k - HISTSIZE) : 0;
		if (skip > 0) {
			term.histi = (term.histi + skip) % HISTSIZE;
			term.histwcount = HISTSIZE;
		}
		for (int i = skip; i < k; i++) {
			term.histi = (term.histi + 1) % HISTSIZE;
			if (i < vis) {
				thistpush(term.histi, term.line[i], term.rowmax[i]);
			} else {
				const BulkLine *bl = &blines[i - vis];
				if (!bl->is_sgr) {
					thistpush_ascii(term.histi, s + bl->off, bl->raw_len, bl->wrapped);
				} else {
					thistpush_sgr_raw(term.histi, s + bl->off, bl->raw_len,
					                  bl->cols, bl->wrapped, bl->start_attr);
				}
			}
			if (term.histwcount < HISTSIZE)
				term.histwcount++;
		}
		if (term.scr > 0 && term.scr < HISTSIZE)
			term.scr = MIN(term.scr + k, MIN(term.histwcount, HISTSIZE - 1));
	}

	/* Step 2: Populate ONLY the final visible screen rows (k >= rows > vis). */
	if (vis > 0) {
		int first = k - vis;
		for (int y = 0; y < vis; y++) {
			const BulkLine *bl = &blines[first + y];
			twrite_bulk_render_row(y, s + bl->off, bl->raw_len, bl->cols,
			                       bl->is_sgr, bl->wrapped, bl->start_attr);
		}
	}

	tsetdirt(0, rows - 1);
	term.c.attr = cur_attr;
	if (saw_blink)
		term.has_blink = 1;
	term.c.x = 0;
	term.c.y = term.bot;
	term.c.state &= ~CURSOR_WRAPNEXT;
	term.lastc = blines[k - 1].wrapped ? blines[k - 1].last_ch : 0;
	return pos;
}

static int
tputascii(const char *s, int len)
{
	int i = 0;

	if (!asciifastpath || term.esc || (term.mode & (MODE_PRINT|MODE_INSERT)) ||
	    term.trantbl[term.charset] == CS_GRAPHIC0 || sel.ob.x != -1)
		return 0;

	while (i < len) {
		uchar c = (uchar)s[i];
		if (c < 0x20 || c > 0x7e)
			break;
		if ((term.mode & MODE_WRAP) && (term.c.state & CURSOR_WRAPNEXT)) {
			term.line[term.c.y][term.c.x].mode |= ATTR_WRAP;
			tnewline(1);
		}
		int x = term.c.x;
		int y = term.c.y;
		int col = term.col;
		Line line = term.line[y];
		Glyph attr = term.c.attr;
		int lim = MIN(len - i, col - x);
		int k = 0;

		if (!term.rowwide[y]) {
			while (k < lim) {
				uchar ch = (uchar)s[i + k];
				if (ch < 0x20 || ch > 0x7e)
					break;
				line[x + k] = attr;
				line[x + k].u = ch;
				k++;
			}
		} else {
			while (k < lim) {
				uchar ch = (uchar)s[i + k];
				Glyph *gp;
				int cx;
				if (ch < 0x20 || ch > 0x7e)
					break;
				cx = x + k;
				gp = &line[cx];
				/* overwriting half of a wide character blanks the other half */
				if (gp->mode & ATTR_WIDE) {
					if (cx + 1 < col) {
						line[cx + 1].u = ' ';
						line[cx + 1].mode &= ~ATTR_WDUMMY;
					}
				} else if (gp->mode & ATTR_WDUMMY) {
					line[cx - 1].u = ' ';
					line[cx - 1].mode &= ~ATTR_WIDE;
				}
				*gp = attr;
				gp->u = ch;
				k++;
			}
		}
		term.dirty[y] = 1;
		if (x + k > term.rowmax[y])
			term.rowmax[y] = x + k;
		if (x + k < col) {
			term.c.x = x + k;
			term.c.state &= ~CURSOR_WRAPNEXT;
		} else {
			term.c.x = col - 1;
			term.c.state |= CURSOR_WRAPNEXT;
		}
		i += k;
	}
	if (i)
		term.lastc = (uchar)s[i - 1];
	return i;
}

/*
 * Fast path for runs of UTF-8 box-drawing & block characters (U+2500..U+259F:
 * 0xE2 0x94/0x95 0x80..0xBF and 0xE2 0x96 0x80..0x9F), all width 1.
 */
static int
tputbox(const char *s, int len)
{
	int i = 0;
	Rune lastu = 0;

	if (!asciifastpath || !IS_SET(MODE_UTF8) || term.esc ||
	    (term.mode & (MODE_PRINT|MODE_INSERT)) || sel.ob.x != -1)
		return 0;

	while (i + 3 <= len) {
		uchar b0 = (uchar)s[i], b1 = (uchar)s[i + 1], b2 = (uchar)s[i + 2];
		if (b0 != 0xE2 || b1 < 0x94 || b1 > 0x96 ||
		    (b2 & 0xC0) != 0x80 || (b1 == 0x96 && b2 > 0x9F))
			break;
		if ((term.mode & MODE_WRAP) && (term.c.state & CURSOR_WRAPNEXT)) {
			term.line[term.c.y][term.c.x].mode |= ATTR_WRAP;
			tnewline(1);
		}
		int x = term.c.x;
		int y = term.c.y;
		int col = term.col;
		Line line = term.line[y];
		Glyph attr = term.c.attr;
		int avail = col - x;
		int k = 0;
		while (k < avail && i + 3 <= len) {
			b0 = (uchar)s[i]; b1 = (uchar)s[i + 1]; b2 = (uchar)s[i + 2];
			if (b0 != 0xE2 || b1 < 0x94 || b1 > 0x96 ||
			    (b2 & 0xC0) != 0x80 || (b1 == 0x96 && b2 > 0x9F))
				break;
			Rune u = 0x2500u + (((uint)(b1 - 0x94) << 6) | (uint)(b2 & 0x3Fu));
			int cx = x + k;
			Glyph *gp = &line[cx];
			if (term.rowwide[y]) {
				if (gp->mode & ATTR_WIDE) {
					if (cx + 1 < col) {
						line[cx + 1].u = ' ';
						line[cx + 1].mode &= ~ATTR_WDUMMY;
					}
				} else if (gp->mode & ATTR_WDUMMY) {
					line[cx - 1].u = ' ';
					line[cx - 1].mode &= ~ATTR_WIDE;
				}
			}
			*gp = attr;
			gp->u = u;
			if (isboxdraw_fast(u))
				gp->mode |= ATTR_BOXDRAW;
			lastu = u;
			i += 3;
			k++;
		}
		term.dirty[y] = 1;
		if (x + k > term.rowmax[y])
			term.rowmax[y] = x + k;
		if (x + k < col) {
			term.c.x = x + k;
			term.c.state &= ~CURSOR_WRAPNEXT;
		} else {
			term.c.x = col - 1;
			term.c.state |= CURSOR_WRAPNEXT;
		}
	}
	if (i)
		term.lastc = lastu;
	return i;
}

int
twrite(const char *buf, int buflen, int show_ctrl)
{
	int charsize, k;
	Rune u;
	int n;
	int bulk_retry_at = (buflen >= bulk_min_bytes) ? 0 : buflen;
	int bulk_fails = 0;

	int su0 = su;
	twrite_aborted = 0;

	for (n = 0; n < buflen; n += charsize) {
		if (su0 && !su &&
		    (!IS_SET(MODE_UTF8) || !((uchar)buf[n] & 0x80) ||
		     utf8decode(buf + n, &u, buflen - n) > 0)) {
			twrite_aborted = 1;
			break;  // ESU - allow rendering before a new BSU
		}
		/*
		 * Fast paths before per-char decode when in plain-text state:
		 * printable ASCII runs, UTF-8 box-drawing runs (tables), and
		 * inline CSI sequences (SGR colors/cursor moves).
		 */
		if (asciifastpath && !term.esc && !show_ctrl && !IS_SET(MODE_PRINT)) {
			uchar b = (uchar)buf[n];
			if (n >= bulk_retry_at && term.c.y == term.bot && term.c.x == 0 &&
			    buflen - n >= bulk_min_bytes &&
			    !(term.mode & MODE_INSERT) &&
			    term.trantbl[term.charset] != CS_GRAPHIC0 &&
			    sel.ob.x == -1 && term.top == 0 && term.bot == term.row - 1 &&
			    !(term.c.state & CURSOR_WRAPNEXT) && term.rowmax[term.bot] == 0 &&
			    term.c.attr.fg == seloptsch->defaultfg &&
			    term.c.attr.bg == seloptsch->defaultbg &&
			    ((b >= 0x20 && b <= 0x7e) || b == '\033' || b == '\r' || b == '\n')) {
				int fail_off = 0;
				if ((k = twrite_bulk(buf + n, buflen - n, &fail_off))) {
					bulk_fails = 0;
					charsize = k;
					continue;
				}
				if (++bulk_fails >= 2)
					bulk_retry_at = buflen;
				else
					bulk_retry_at = n + fail_off + 1;
			}
			if (b >= 0x20 && b < 0x7f) {
				if ((k = tputascii(buf + n, buflen - n))) {
					charsize = k;
					continue;
				}
			} else if (b == 0xE2 && buflen - n >= 3 && IS_SET(MODE_UTF8)) {
				if ((k = tputbox(buf + n, buflen - n))) {
					charsize = k;
					continue;
				}
			} else if (b == '\033' && buflen - n >= 3 && buf[n + 1] == '[') {
				int j = n + 2;
				int cap = MIN(buflen, n + 2 + (int)sizeof(csiescseq.buf) - 1);
				while (j < cap && (uchar)buf[j] >= 0x20 && (uchar)buf[j] <= 0x3F)
					j++;
				if (j < cap && (uchar)buf[j] >= 0x40 && (uchar)buf[j] <= 0x7E) {
					size_t clen = (size_t)(j - (n + 2) + 1);
					csireset();
					memcpy(csiescseq.buf, buf + n + 2, clen);
					csiescseq.len = clen;
					csiparse();
					csihandle();
					charsize = (int)clen + 2;
					continue;
				}
			}
		}

		if (IS_SET(MODE_UTF8)) {
			/* process a complete utf8 char */
			charsize = utf8decode(buf + n, &u, buflen - n);
			if (charsize == 0)
				break;
		} else {
			u = buf[n] & 0xFF;
			charsize = 1;
		}
		/*
		 * Printable ASCII cannot end a synchronized update or be
		 * affected by show_ctrl, so a whole run can go at once.
		 */
		if (u >= 0x20 && u < 0x7f && (k = tputascii(buf + n, buflen - n))) {
			charsize = k;
			continue;
		}
		if (show_ctrl && ISCONTROL(u)) {
			if (u & 0x80) {
				u &= 0x7f;
				tputc('^');
				tputc('[');
			} else if (u != '\n' && u != '\r' && u != '\t') {
				u ^= 0x40;
				tputc('^');
			}
		}
		tputc(u);
	}
	return n;
}

void
tresize(int col, int row)
{
	int i;
	int minrow = MIN(row, term.row);
	int mincol = MIN(col, term.col);
	int *bp;
	TCursor c;

	if (col < 1 || row < 1) {
		fprintf(stderr,
		        "tresize: error resizing to %dx%d\n", col, row);
		return;
	}

  autocomplete ((const Arg []) { ACMPL_DEACTIVATE });

	/*
	 * slide screen to keep cursor where we expect it -
	 * tscrollup would work here, but we can optimize to
	 * memmove because we're freeing the earlier lines
	 */
	for (i = 0; i <= term.c.y - row; i++) {
		free(term.line[i]);
		free(term.alt[i]);
	}
	/* ensure that both src and dst are not NULL */
	if (i > 0) {
		memmove(term.line, term.line + i, row * sizeof(Line));
		memmove(term.alt, term.alt + i, row * sizeof(Line));
		memmove(term.rowmax, term.rowmax + i, row * sizeof(*term.rowmax));
		memmove(term.alt_rowmax, term.alt_rowmax + i, row * sizeof(*term.alt_rowmax));
		memmove(term.rowwide, term.rowwide + i, row * sizeof(*term.rowwide));
		memmove(term.alt_rowwide, term.alt_rowwide + i, row * sizeof(*term.alt_rowwide));
	}
	for (i += row; i < term.row; i++) {
		free(term.line[i]);
		free(term.alt[i]);
	}

	/* resize to new height */
	term.line = xrealloc(term.line, row * sizeof(Line));
	term.alt  = xrealloc(term.alt,  row * sizeof(Line));
	term.rowmax = xrealloc(term.rowmax, row * sizeof(*term.rowmax));
	term.alt_rowmax = xrealloc(term.alt_rowmax, row * sizeof(*term.alt_rowmax));
	term.rowwide = xrealloc(term.rowwide, row * sizeof(*term.rowwide));
	term.alt_rowwide = xrealloc(term.alt_rowwide, row * sizeof(*term.alt_rowwide));
	term.dirty = xrealloc(term.dirty, row * sizeof(*term.dirty));
	term.tabs = xrealloc(term.tabs, col * sizeof(*term.tabs));

	/*
	 * History is not resized here: each line records its own width in
	 * term.histcol[] and is padded on access (see thistpush()), which
	 * keeps tresize() O(rows) rather than O(HISTSIZE).
	 */

	/* resize each row to new width, zero-pad if needed */
	for (i = 0; i < minrow; i++) {
		term.line[i] = xrealloc(term.line[i], col * sizeof(Glyph));
		term.alt[i]  = xrealloc(term.alt[i],  col * sizeof(Glyph));
		if (col > term.col) {
			term.rowmax[i] = col;
			term.alt_rowmax[i] = col;
		} else {
			if (term.rowmax[i] > col) term.rowmax[i] = col;
			if (term.alt_rowmax[i] > col) term.alt_rowmax[i] = col;
		}
	}

	/* allocate any new rows */
	for (/* i = minrow */; i < row; i++) {
		term.line[i] = xmalloc(col * sizeof(Glyph));
		term.alt[i] = xmalloc(col * sizeof(Glyph));
		term.rowmax[i] = col;
		term.alt_rowmax[i] = col;
		term.rowwide[i] = 1;
		term.alt_rowwide[i] = 1;
	}
	if (col > term.col) {
		bp = term.tabs + term.col;

		memset(bp, 0, sizeof(*term.tabs) * (col - term.col));
		while (--bp > term.tabs && !*bp)
			/* nothing */ ;
		for (bp += tabspaces; bp < term.tabs + col; bp += tabspaces)
			*bp = 1;
	}
	/* update terminal size */
	term.col = col;
	term.row = row;
	/* reset scrolling region */
	tsetscroll(0, row-1);
	/* make use of the LIMIT in tmoveto */
	tmoveto(term.c.x, term.c.y);
	/* Clearing both screens (it makes dirty all lines) */
	c = term.c;
	for (i = 0; i < 2; i++) {
		if (mincol < col && 0 < minrow) {
			tclearregion(mincol, 0, col - 1, minrow - 1);
		}
		if (0 < col && minrow < row) {
			tclearregion(0, minrow, col - 1, row - 1);
		}
		tswapscreen();
		tcursor(CURSOR_LOAD);
	}
	term.c = c;
}

void
resettitle(void)
{
	xsettitle(NULL, 0);
}

void
draw(void)
{
	static Line *vislines = NULL;
	static Glyph *scratch = NULL; /* padded copies of narrow history rows */
	static size_t viscells = 0;
	static int visrows = 0;
	int y, hassel;
	int cx = term.c.x, ocx = term.ocx, ocy = term.ocy;

	if (!xstartdraw())
		return;

	/* adjust cursor position */
	LIMIT(term.ocx, 0, term.col-1);
	LIMIT(term.ocy, 0, term.row-1);
	if (term.line[term.ocy][term.ocx].mode & ATTR_WDUMMY)
		term.ocx--;
	if (term.line[term.c.y][cx].mode & ATTR_WDUMMY)
		cx--;

	if (term.row > visrows) {
		visrows = term.row;
		vislines = xrealloc(vislines, (size_t)visrows * sizeof(*vislines));
	}
	if ((size_t)term.row * term.col > viscells) {
		viscells = (size_t)term.row * term.col;
		scratch = xrealloc(scratch, viscells * sizeof(*scratch));
	}
	/*
	 * The renderer diffs whole rows against what it last drew, so the
	 * per-line dirty flags are not needed to find changes.
	 */
	for (y = 0; y < term.row; y++) {
		term.dirty[y] = 0;
		vislines[y] = tvisline(y, &scratch[(size_t)y * term.col]);
	}
	hassel = !(sel.mode == SEL_EMPTY || sel.ob.x == -1 ||
	           sel.alt != IS_SET(MODE_ALTSCREEN));

	xdrawscreen(vislines, term.col, term.row, term.scr == 0, cx, term.c.y, hassel);
	term.ocx = cx, term.ocy = term.c.y;
	xfinishdraw();
	if (ocx != term.ocx || ocy != term.ocy)
		xximspot(term.ocx, term.ocy);
}

void
redraw(void)
{
	tfulldirt();
	draw();
}

void autocomplete (const Arg *arg) {
    static _Bool active = 0;
    int acmpl_cmdindex = arg->i;
    static int acmpl_cmdindex_prev;

    if (active == 0)
        acmpl_cmdindex_prev = acmpl_cmdindex;

    static const char * const acmpl_cmd[] = {
        [ACMPL_DEACTIVATE]   = "__DEACTIVATE__",
        [ACMPL_WORD]         = "word-complete",
        [ACMPL_WWORD]        = "WORD-complete",
        [ACMPL_FUZZY_WORD]   = "fuzzy-word-complete",
        [ACMPL_FUZZY_WWORD]  = "fuzzy-WORD-complete",
        [ACMPL_FUZZY]        = "fuzzy-complete",
        [ACMPL_SUFFIX]       = "suffix-complete",
        [ACMPL_SURROUND]     = "surround-complete",
        [ACMPL_UNDO]         = "__UNDO__",
    };

    static FILE *acmpl_exec = NULL;
    static int acmpl_status;
    static char *stbuffile;
    static char *target = NULL;
    static size_t targetlen;
    static char *completion = NULL;
    static size_t complen_prev = 0;
    static int cx, cy;

    if (acmpl_cmdindex == ACMPL_DEACTIVATE) {
        if (active) {
            active = 0;
            pclose(acmpl_exec);
            unlink(stbuffile);
            free(stbuffile);
            stbuffile = NULL;

            if (complen_prev) {
                selclear();
                complen_prev = 0;
            }
        }
        return;
    }

    if (acmpl_cmdindex == ACMPL_UNDO) {
        if (active) {
            active = 0;
            pclose(acmpl_exec);
            unlink(stbuffile);
            free(stbuffile);
            stbuffile = NULL;

            if (complen_prev) {
                selclear();
                for (size_t i = 0; i < complen_prev; i++)
                    ttywrite((char[]) {'\b'}, 1, 1);
                complen_prev = 0;
                ttywrite(target, targetlen, 0);
            }
        }
        return;
    }

    if (acmpl_cmdindex != acmpl_cmdindex_prev) {
        if (active) {
            acmpl_cmdindex_prev = acmpl_cmdindex;
            goto acmpl_begin;
        }
    }

    if (active == 0) {
        acmpl_cmdindex_prev = acmpl_cmdindex;
        cx = term.c.x;
        cy = term.c.y;

        char filename[] = "/tmp/st-autocomplete-XXXXXX";
        int fd = mkstemp(filename);

        if (fd == -1) {
            perror("mkstemp");
            return;
        }

        stbuffile = strdup(filename);

        FILE *stbuf = fdopen(fd, "w");
        if (!stbuf) {
            perror("fdopen");
            close(fd);
            unlink(stbuffile);
            free(stbuffile);
            stbuffile = NULL;
            return;
        }

        char *stbufline = malloc(term.col + 2);
        if (!stbufline) {
            perror("malloc");
            fclose(stbuf);
            unlink(stbuffile);
            free(stbuffile);
            stbuffile = NULL;
            return;
        }

        int cxp = 0;
        for (size_t y = 0; y < term.row; y++) {
            if (y == term.c.y) cx += cxp * term.col;

            size_t x = 0;
            for (; x < term.col; x++)
                utf8encode(term.line[y][x].u, stbufline + x);
            if (term.line[y][x - 1].mode & ATTR_WRAP) {
                x--;
                if (y <= term.c.y) cy--;
                cxp++;
            } else {
                stbufline[x] = '\n';
                cxp = 0;
            }
            stbufline[x + 1] = 0;
            fputs(stbufline, stbuf);
        }

        free(stbufline);
        fclose(stbuf);

acmpl_begin:
        target = malloc(term.col + 1);
        completion = malloc(term.col + 1);
        if (!target || !completion) {
            perror("malloc");
            free(target);
            free(completion);
            unlink(stbuffile);
            free(stbuffile);
            stbuffile = NULL;
            return;
        }

        char acmpl[1500];
        snprintf(acmpl, sizeof(acmpl),
                 "cat %s | st-autocomplete %s %d %d",
                 stbuffile, acmpl_cmd[acmpl_cmdindex], cy, cx);

        acmpl_exec = popen(acmpl, "r");
        if (!acmpl_exec) {
            perror("popen");
            free(target);
            free(completion);
            unlink(stbuffile);
            free(stbuffile);
            stbuffile = NULL;
            return;
        }

        if (fscanf(acmpl_exec, "%s\n", target) != 1) {
            perror("fscanf");
            pclose(acmpl_exec);
            free(target);
            free(completion);
            unlink(stbuffile);
            free(stbuffile);
            stbuffile = NULL;
            return;
        }
        targetlen = strlen(target);
    }

    unsigned line, beg, end;

    acmpl_status = fscanf(acmpl_exec, "%[^\n]\n%u\n%u\n%u\n", completion, &line, &beg, &end);
    if (acmpl_status == EOF) {
        if (active == 0) {
            pclose(acmpl_exec);
            free(target);
            free(completion);
            unlink(stbuffile);
            free(stbuffile);
            stbuffile = NULL;
            return;
        }
        active = 0;
        pclose(acmpl_exec);
        ttywrite(target, targetlen, 0);
        goto acmpl_begin;
    }

    active = 1;

    if (complen_prev == 0) {
        for (size_t i = 0; i < targetlen; i++)
            ttywrite((char[]) {'\b'}, 1, 1);
    } else {
        selclear();
        for (size_t i = 0; i < complen_prev; i++)
            ttywrite((char[]) {'\b'}, 1, 1);
        complen_prev = 0;
    }

    complen_prev = strlen(completion);
    ttywrite(completion, complen_prev, 0);

    if (line == cy && beg > cx) {
        beg += complen_prev - targetlen;
        end += complen_prev - targetlen;
    }

    end--;

    int wl = 0;
    int tl = line;
    for (int l = 0; l < tl; l++)
        if (term.line[l][term.col - 1].mode & ATTR_WRAP) {
            wl++;
            tl++;
        }

    selstart(beg % term.col, line + wl + beg / term.col, 0);
    selextend(end % term.col, line + wl + end / term.col, 1, 0);
    xsetsel(getsel());
}
