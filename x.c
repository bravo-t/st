/* See LICENSE for license details. */
#include <errno.h>
#include <math.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <X11/XKBlib.h>

char *argv0;
#include "arg.h"
#include "st.h"
#include "win.h"

/* types used in config.h */
typedef struct {
	uint mod;
	KeySym keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Shortcut;

typedef struct {
	uint mod;
	uint button;
	void (*func)(const Arg *);
	const Arg arg;
	uint  release;
    int  altscrn;  /* 0: don't care,  -1: not alt screen,  1: alt screen */
} MouseShortcut;

typedef struct {
	KeySym k;
	uint mask;
	char *s;
	/* three-valued logic variables: 0 indifferent, 1 on, -1 off */
	signed char appkey;    /* application keypad */
	signed char appcursor; /* application cursor */
} Key;

/* X modifiers */
#define XK_ANY_MOD    UINT_MAX
#define XK_NO_MOD     0
#define XK_SWITCH_MOD (1<<13|1<<14)

/* function definitions used in config.h */
static void clipcopy(const Arg *);
static void clippaste(const Arg *);
static void numlock(const Arg *);
static void selpaste(const Arg *);
static void zoom(const Arg *);
static void zoomabs(const Arg *);
static void zoomreset(const Arg *);
static void ttysend(const Arg *);
static void invert(const Arg *);

/* config.h for applying patches and the configuration. */
#include "config.h"

/* size of title stack */
#define TITLESTACKSIZE 8

/* XEMBED messages */
#define XEMBED_FOCUS_IN  4
#define XEMBED_FOCUS_OUT 5

/* macros */
#define IS_SET(flag)		((win.mode & (flag)) != 0)
#define TRUERED(x)		(((x) & 0xff0000) >> 8)
#define TRUEGREEN(x)		(((x) & 0xff00))
#define TRUEBLUE(x)		(((x) & 0xff) << 8)

typedef XftDraw *Draw;
typedef XftColor Color;
typedef XftGlyphFontSpec GlyphFontSpec;

/* Purely graphic info */
typedef struct {
	int tw, th; /* tty width and height */
	int w, h; /* window width and height */
    int hborderpx, vborderpx;
	int ch; /* char height */
	int cw; /* char width  */
    int cyo; /* char y offset */
	int mode; /* window state/mode flags */
	int cursor; /* cursor style */
} TermWindow;

typedef struct {
	Display *dpy;
	Colormap cmap;
	Window win;
	Drawable buf;
	GlyphFontSpec *specbuf; /* font spec buffer used for rendering */
	Atom xembed, wmdeletewin, netwmname, netwmiconname, netwmpid;
  Atom netwmstate, netwmfullscreen;
	Atom clipboard, incr, targets;
	struct {
		XIM xim;
		XIC xic;
		XPoint spot;
		XVaNestedList spotlist;
	} ime;
	Draw draw;
	Visual *vis;
	XSetWindowAttributes attrs;
	int scr;
	int isfixed; /* is fixed geometry? */
    int depth; /* bit depth */
	int l, t; /* left and top offset */
	int gm; /* geometry mask */
} XWindow;

typedef struct {
	Atom xtarget;
	char *primary, *clipboard;
	struct timespec tclick1;
	struct timespec tclick2;
} XSelection;

/* Font structure */
#define Font Font_
typedef struct {
	int height;
	int width;
	int ascent;
	int descent;
	int badslant;
	int badweight;
	short lbearing;
	short rbearing;
	XftFont *match;
	FcFontSet *set;
	FcCharSet *setcs; /* union coverage of set, from FcFontSort() */
	FcPattern *pattern;
	FT_UInt ascii[128]; /* precomputed glyph indices for ASCII 0x20..0x7e */
	FT_UInt space_glyph; /* 0-pixel space glyph for bridging short XRender gaps */
	int space_xoff;      /* advance width of space_glyph */
} Font;

/* Drawing Context */
typedef struct {
	Color *col;
	size_t collen;
	Font font, bfont, ifont, ibfont;
	GC gc;
} DC;

const OptionScheme *seloptsch = &optionscheme_default;
static const char *seloptschname = NULL;

static inline ushort sixd_to_16bit(int);
static int xmakeglyphfontspecs(XftGlyphFontSpec *, const Glyph *, int, int, int);
static void xdrawglyphfontspecs(const XftGlyphFontSpec *, Glyph, int, int, int, int);
static void xdrawglyph(Glyph, int, int);
static void xclear(int, int, int, int);
static int xgeommasktogravity(int);
static int ximopen(Display *);
static void ximinstantiate(Display *, XPointer, XPointer);
static void ximdestroy(XIM, XPointer, XPointer);
static int xicdestroy(XIC, XPointer, XPointer);
static void xinit(int, int);
static int countoptsch();
static void cresize(int, int);
static void xresize(int, int);
static void xhints(void);
static int xloadcolor(int, const char *, Color *);
static int xloadfont(Font *, FcPattern *);
static void xloadfonts(const char *, double);
static int xloadsparefont(FcPattern *, int);
static void xloadsparefonts(void);
static void xunloadfont(Font *);
static void xunloadfonts(void);
static void xsetenv(void);
static void xseturgency(int);
static int evcol(XEvent *);
static int evrow(XEvent *);

static void expose(XEvent *);
static void visibility(XEvent *);
static void unmap(XEvent *);
static void kpress(XEvent *);
static void cmessage(XEvent *);
static void resize(XEvent *);
static void focus(XEvent *);
static uint buttonmask(uint);
static int mouseaction(XEvent *, uint, int);
static void brelease(XEvent *);
static void bpress(XEvent *);
static void bmotion(XEvent *);
static void propnotify(XEvent *);
static void selnotify(XEvent *);
static void selclear_(XEvent *);
static void selrequest(XEvent *);
static void setsel(char *, Time);
static void mousesel(XEvent *, int);
static void mousereport(XEvent *);
static char *kmap(KeySym, uint);
static int match(uint, uint);

static void run(void);
static void usage(void);

static void (*handler[LASTEvent])(XEvent *) = {
	[KeyPress] = kpress,
	[ClientMessage] = cmessage,
	[ConfigureNotify] = resize,
	[VisibilityNotify] = visibility,
	[UnmapNotify] = unmap,
	[Expose] = expose,
	[FocusIn] = focus,
	[FocusOut] = focus,
	[MotionNotify] = bmotion,
	[ButtonPress] = bpress,
	[ButtonRelease] = brelease,
/*
 * Uncomment if you want the selection to disappear when you select something
 * different in another window.
 */
/*	[SelectionClear] = selclear_, */
	[SelectionNotify] = selnotify,
/*
 * PropertyNotify is only turned on when there is some INCR transfer happening
 * for the selection retrieval.
 */
	[PropertyNotify] = propnotify,
	[SelectionRequest] = selrequest,
};

/* Globals */
static DC dc;
static XWindow xw;
static XSelection xsel;
static TermWindow win;
static int tstki; /* title stack index */
static char *titlestack[TITLESTACKSIZE]; /* title stack */

/* Font Ring Cache */
enum {
	FRC_NORMAL,
	FRC_ITALIC,
	FRC_BOLD,
	FRC_ITALICBOLD
};

typedef struct {
	XftFont *font;
	int flags;
	Rune unicodep;
	int iscolor;
} Fontcache;

/*
 * Direct-mapped cache of fallback-font lookups (hits and misses), so a rune
 * missing from the primary font costs one probe instead of a scan of frc[]
 * (and, for runes no font has, a 5-26 ms fontconfig query) on every redraw.
 *
 * Spec convention shared with xdrawglyphfontspecs(): a spec whose font is
 * NULL is a placeholder that keeps specs[] 1:1 with the row's cells but is
 * never sent to XRender (spaces, colour-font glyphs).
 */
#define FGCACHE_SIZE  4096
#define FG_NOTFOUND   -1 /* no font has it: draw primary font's .notdef */
#define FG_SKIP       -2 /* only a colour font has it: leave cell blank */
typedef struct {
	Rune rune;
	int flags;
	int frcidx; /* index into frc[], FG_NOTFOUND or FG_SKIP */
	FT_UInt glyphidx;
	unsigned char valid;
} FallbackGlyphCache;

/* Fontcache is an array now. A new font will be appended to the array. */
static Fontcache *frc = NULL;
static int frclen = 0;
static int frccap = 0;
static FallbackGlyphCache fgcache[FGCACHE_SIZE];
static char *usedfont = NULL;
static double usedfontsize = 0;
static double defaultfontsize = 0;

/* Shadow screen state for scroll-blitting and partial-rect XCopyArea */
static Glyph *drawn_buf = NULL;
static unsigned char *drawn_row_valid = NULL;
static uint64_t *drawn_hash = NULL;
static uint64_t *cur_hash = NULL;
static unsigned char *cur_row_same = NULL;
static Glyph *drawn_eff_row = NULL;
static int drawn_col = 0;
static int drawn_row = 0;
static int drawn_valid = 0;
static int prev_cursor_drawn = 0;
static int prev_cx = 0;
static int prev_cy = 0;
static int cursor_dirty = 0;
static int xneeddraw = 0;
static int xresetdrawthrottle = 0;
static int win_urgent = 0;
static int is_etx = 0;
static int xdirect = 0; /* 1 on ETX with opaque (<32-bit) visual: draw directly to xw.win */
static int dirty_x1 = 0, dirty_y1 = 0, dirty_x2 = 0, dirty_y2 = 0;
#define MAX_DIRTY_RECTS 64
static XRectangle dirty_rects[MAX_DIRTY_RECTS];
static int ndirty_rects = 0;
static int dirty_overflow = 0;

/*
 * Frame-wide XRender batching buffers: accumulate background rectangles,
 * foreground text glyphs, box-drawing cells, and underline/strikethrough runs
 * across all changed spans on all rows of a frame before flushing, so dense
 * scattered updates (like vim hlsearch) emit O(unique_colors) XRender requests
 * per frame without repainting unchanged cells between matches.
 */
enum {
	MAX_BG_BUCKETS = 8,
	BG_RECT_BATCH  = 1024,
	MAX_FG_BUCKETS = 12,
	FG_SPEC_BATCH  = 2048,
	MAX_BOX_BUCKETS = 8,
	BOX_SPEC_BATCH = 1024,
	MAX_DECO_RUNS  = 512,
};
static int xbatch_active = 0;
static Picture xbg_pic = None;
static Color xbg_col[MAX_BG_BUCKETS];
static XRectangle xbg_rects[MAX_BG_BUCKETS][BG_RECT_BATCH];
static int xbg_cnt[MAX_BG_BUCKETS];
static int xnbg_buckets = 0;

static Color xfg_col[MAX_FG_BUCKETS];
static XftGlyphFontSpec xfg_specs[MAX_FG_BUCKETS][FG_SPEC_BATCH];
static int xfg_cnt[MAX_FG_BUCKETS];
static size_t xfg_wire[MAX_FG_BUCKETS];
static int xnfg_buckets = 0;

static Color xbox_col[MAX_BOX_BUCKETS];
static XftGlyphFontSpec xbox_specs[MAX_BOX_BUCKETS][BOX_SPEC_BATCH];
static short xbox_winy[MAX_BOX_BUCKETS][BOX_SPEC_BATCH];
static int xbox_cnt[MAX_BOX_BUCKETS];
static int xnbox_buckets = 0;

static struct { int x, y, w; Color col; } xdeco_runs[MAX_DECO_RUNS];
static int xndeco = 0;
static size_t xdirect_buf_est = 0; /* conservative bytes queued in libX11's 16 KB buffer */

static void xbatch_flush_bg_bucket(int bi);
static void xbatch_flush_bg(void);
static void xbatch_flush_fg(void);
static void xbatch_flush_all(void);
static size_t xbatch_est_bytes(void);
static void xdirect_flush_strip(void);

static void
xinvalidatedrawn(void)
{
	drawn_valid = 0;
	prev_cursor_drawn = 0;
	cursor_dirty = 0;
}

static inline void
xdirtyrect(int x1, int y1, int x2, int y2)
{
	if (x1 >= x2 || y1 >= y2)
		return;
	if (x1 < dirty_x1) dirty_x1 = x1;
	if (y1 < dirty_y1) dirty_y1 = y1;
	if (x2 > dirty_x2) dirty_x2 = x2;
	if (y2 > dirty_y2) dirty_y2 = y2;

	if (dirty_overflow)
		return;
	if (ndirty_rects > 0) {
		XRectangle *r = &dirty_rects[ndirty_rects - 1];
		int rx1 = r->x, ry1 = r->y;
		int rx2 = rx1 + r->width, ry2 = ry1 + r->height;
		if (x1 >= rx1 && x2 <= rx2 && y1 >= ry1 && y2 <= ry2)
			return;
		if (rx1 == x1 && rx2 == x2 && y1 <= ry2 && y2 >= ry1) {
			int ny1 = MIN(ry1, y1), ny2 = MAX(ry2, y2);
			r->y = (short)ny1;
			r->height = (unsigned short)(ny2 - ny1);
			return;
		}
		if (ry1 == y1 && ry2 == y2 && x1 <= rx2 + 32 && x2 >= rx1 - 32) {
			int nx1 = MIN(rx1, x1), nx2 = MAX(rx2, x2);
			r->x = (short)nx1;
			r->width = (unsigned short)(nx2 - nx1);
			return;
		}
	}
	if (ndirty_rects < MAX_DIRTY_RECTS) {
		dirty_rects[ndirty_rects].x = (short)x1;
		dirty_rects[ndirty_rects].y = (short)y1;
		dirty_rects[ndirty_rects].width = (unsigned short)(x2 - x1);
		dirty_rects[ndirty_rects].height = (unsigned short)(y2 - y1);
		ndirty_rects++;
	} else {
		dirty_overflow = 1;
	}
}

static inline void
xdirtycells(int x1, int y1, int x2, int y2, int col, int row)
{
	int px1 = (x1 <= 0) ? 0 : (win.hborderpx + x1 * win.cw);
	int py1 = (y1 <= 0) ? 0 : (win.vborderpx + y1 * win.ch);
	int px2 = (x2 >= col) ? win.w : (win.hborderpx + x2 * win.cw);
	int py2 = (y2 >= row) ? win.h : (win.vborderpx + y2 * win.ch);
	xdirtyrect(px1, py1, px2, py2);
}

static char *opt_alpha = NULL;
static char *opt_class = NULL;
static char **opt_cmd  = NULL;
static char *opt_embed = NULL;
static char *opt_font  = NULL;
static char *opt_io    = NULL;
static char *opt_line  = NULL;
static char *opt_name  = NULL;
static char *opt_title = NULL;
static bool focused = true;

static int invertcolors = 0;
static uint buttons; /* bit field of pressed buttons */

void
invert(const Arg *dummy)
{
    invertcolors = !invertcolors;
    xinvalidatedrawn();
    redraw();
}

Color
invertedcolor(Color *clr) {
    XRenderColor rc;
    Color inverted;
    rc.red = ~clr->color.red;
    rc.green = ~clr->color.green;
    rc.blue = ~clr->color.blue;
    rc.alpha = clr->color.alpha;
    XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &rc, &inverted);
    return inverted;
}

void
clipcopy(const Arg *dummy)
{
	Atom clipboard;

	free(xsel.clipboard);
	xsel.clipboard = NULL;

	if (xsel.primary != NULL) {
		xsel.clipboard = xstrdup(xsel.primary);
		clipboard = xw.clipboard ? xw.clipboard : XInternAtom(xw.dpy, "CLIPBOARD", 0);
		XSetSelectionOwner(xw.dpy, clipboard, xw.win, CurrentTime);
	}
}

void
clippaste(const Arg *dummy)
{
	Atom clipboard;

	clipboard = xw.clipboard ? xw.clipboard : XInternAtom(xw.dpy, "CLIPBOARD", 0);
	XConvertSelection(xw.dpy, clipboard, xsel.xtarget, clipboard,
			xw.win, CurrentTime);
}

void
selpaste(const Arg *dummy)
{
	XConvertSelection(xw.dpy, XA_PRIMARY, xsel.xtarget, XA_PRIMARY,
			xw.win, CurrentTime);
}

void
numlock(const Arg *dummy)
{
	win.mode ^= MODE_NUMLOCK;
}

void
zoom(const Arg *arg)
{
	Arg larg;

	larg.f = usedfontsize + arg->f;
	zoomabs(&larg);
}

void
zoomabs(const Arg *arg)
{
	xunloadfonts();
	xloadfonts(usedfont, arg->f);
    xloadsparefonts();
	cresize(0, 0);
	redraw();
	xhints();
}

void
zoomreset(const Arg *arg)
{
	Arg larg;

	if (defaultfontsize > 0) {
		larg.f = defaultfontsize;
		zoomabs(&larg);
	}
}

void
ttysend(const Arg *arg)
{
	ttywrite(arg->s, strlen(arg->s), 1);
}

int
evcol(XEvent *e)
{
	int x = e->xbutton.x - win.hborderpx;
	LIMIT(x, 0, win.tw - 1);
	return x / win.cw;
}

int
evrow(XEvent *e)
{
	int y = e->xbutton.y - win.vborderpx;
	LIMIT(y, 0, win.th - 1);
	return y / win.ch;
}

void
mousesel(XEvent *e, int done)
{
	int type, seltype = SEL_REGULAR;
	uint state = e->xbutton.state & ~(Button1Mask | forcemousemod);

	for (type = 1; type < LEN(selmasks); ++type) {
		if (match(selmasks[type], state)) {
			seltype = type;
			break;
		}
	}
	selextend(evcol(e), evrow(e), seltype, done);
	if (done)
		setsel(getsel(), e->xbutton.time);
}

void
mousereport(XEvent *e)
{
	int len, btn, code;
	int x = evcol(e), y = evrow(e);
	int state = e->xbutton.state;
	char buf[40];
	static int ox, oy;

	if (e->type == MotionNotify) {
		if (x == ox && y == oy)
			return;
		if (!IS_SET(MODE_MOUSEMOTION) && !IS_SET(MODE_MOUSEMANY))
			return;
		/* MODE_MOUSEMOTION: no reporting if no button is pressed */
		if (IS_SET(MODE_MOUSEMOTION) && buttons == 0)
			return;
		/* Set btn to lowest-numbered pressed button, or 12 if no
		 * buttons are pressed. */
		for (btn = 1; btn <= 11 && !(buttons & (1<<(btn-1))); btn++)
			;
		code = 32;
	} else {
		btn = e->xbutton.button;
		/* Only buttons 1 through 11 can be encoded */
		if (btn < 1 || btn > 11)
			return;
		if (e->type == ButtonRelease) {
			/* MODE_MOUSEX10: no button release reporting */
			if (IS_SET(MODE_MOUSEX10))
				return;
			/* Don't send release events for the scroll wheel */
			if (btn == 4 || btn == 5)
				return;
		}
		code = 0;
	}

	ox = x;
	oy = y;

	/* Encode btn into code. If no button is pressed for a motion event in
	 * MODE_MOUSEMANY, then encode it as a release. */
	if ((!IS_SET(MODE_MOUSESGR) && e->type == ButtonRelease) || btn == 12)
		code += 3;
	else if (btn >= 8)
		code += 128 + btn - 8;
	else if (btn >= 4)
		code += 64 + btn - 4;
	else
		code += btn - 1;

	if (!IS_SET(MODE_MOUSEX10)) {
		code += ((state & ShiftMask  ) ?  4 : 0)
		      + ((state & Mod1Mask   ) ?  8 : 0) /* meta key: alt */
		      + ((state & ControlMask) ? 16 : 0);
	}

	if (IS_SET(MODE_MOUSESGR)) {
		len = snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c",
				code, x+1, y+1,
				e->type == ButtonRelease ? 'm' : 'M');
	} else if (x < 223 && y < 223) {
		len = snprintf(buf, sizeof(buf), "\033[M%c%c%c",
				32+code, 32+x+1, 32+y+1);
	} else {
		return;
	}

	ttywrite(buf, len, 0);
}

uint
buttonmask(uint button)
{
	return button == Button1 ? Button1Mask
	     : button == Button2 ? Button2Mask
	     : button == Button3 ? Button3Mask
	     : button == Button4 ? Button4Mask
	     : button == Button5 ? Button5Mask
	     : 0;
}

/*
 * Run the first mshortcuts[] entry matching e. With explicitonly set,
 * XK_ANY_MOD entries are ignored: used when the application has asked for
 * mouse reports, so that only deliberate shortcuts (e.g. Alt+wheel) are
 * taken away from it.
 */
int
mouseaction(XEvent *e, uint release, int explicitonly)
{
	MouseShortcut *ms;

	/* ignore Button<N>mask for Button<N> - it's set on release */
	uint state = e->xbutton.state & ~buttonmask(e->xbutton.button);

	for (ms = mshortcuts; ms < mshortcuts + LEN(mshortcuts); ms++) {
		if (explicitonly && ms->mod == XK_ANY_MOD)
			continue;
		if (ms->release == release &&
		    ms->button == e->xbutton.button &&
            (!ms->altscrn || (ms->altscrn == (tisaltscr() ? 1 : -1))) &&
		    (match(ms->mod, state) ||  /* exact or forced */
		     match(ms->mod, state & ~forcemousemod))) {
			ms->func(&(ms->arg));
			return 1;
		}
	}

	return 0;
}

void
bpress(XEvent *e)
{
	int btn = e->xbutton.button;
	struct timespec now;
	int snap;

	if (1 <= btn && btn <= 11)
		buttons |= 1 << (btn-1);

	if (IS_SET(MODE_MOUSE) && !(e->xbutton.state & forcemousemod)) {
		/*
		 * The app wants mouse reports, which used to swallow Alt+wheel
		 * entirely. On the primary screen st's own scrollback is what
		 * the user sees, so let explicit-modifier wheel shortcuts
		 * (Alt/Ctrl + Button4/5) scroll it; everything else, including
		 * plain wheel, is still reported to the application.
		 */
		if (!tisaltscr() && (btn == Button4 || btn == Button5) &&
		    mouseaction(e, 0, 1)) {
			xneeddraw = 1;
			return;
		}
		mousereport(e);
		return;
	}

	if (mouseaction(e, 0, 0)) {
		xneeddraw = 1;
		return;
	}

	if (btn == Button1) {
		/*
		 * If the user clicks below predefined timeouts specific
		 * snapping behaviour is exposed.
		 */
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (TIMEDIFF(now, xsel.tclick2) <= tripleclicktimeout) {
			snap = SNAP_LINE;
		} else if (TIMEDIFF(now, xsel.tclick1) <= doubleclicktimeout) {
			snap = SNAP_WORD;
		} else {
			snap = 0;
		}
		xsel.tclick2 = xsel.tclick1;
		xsel.tclick1 = now;

		selstart(evcol(e), evrow(e), snap);
		xneeddraw = 1;
	}
}

void
propnotify(XEvent *e)
{
	XPropertyEvent *xpev;
	Atom clipboard = xw.clipboard ? xw.clipboard : XInternAtom(xw.dpy, "CLIPBOARD", 0);

	xpev = &e->xproperty;
	if (xpev->state == PropertyNewValue &&
			(xpev->atom == XA_PRIMARY ||
			 xpev->atom == clipboard)) {
		selnotify(e);
	}
}

void
selnotify(XEvent *e)
{
	ulong nitems, ofs, rem;
	int format;
	uchar *data, *last, *repl;
	Atom type, incratom, property = None;

	incratom = xw.incr ? xw.incr : XInternAtom(xw.dpy, "INCR", 0);

	ofs = 0;
	if (e->type == SelectionNotify)
		property = e->xselection.property;
	else if (e->type == PropertyNotify)
		property = e->xproperty.atom;

	if (property == None)
		return;

	do {
		if (XGetWindowProperty(xw.dpy, xw.win, property, ofs,
					65536, False, AnyPropertyType,
					&type, &format, &nitems, &rem,
					&data)) {
			fprintf(stderr, "Clipboard allocation failed\n");
			return;
		}

		if (e->type == PropertyNotify && nitems == 0 && rem == 0) {
			/*
			 * If there is some PropertyNotify with no data, then
			 * this is the signal of the selection owner that all
			 * data has been transferred. We won't need to receive
			 * PropertyNotify events anymore.
			 */
			MODBIT(xw.attrs.event_mask, 0, PropertyChangeMask);
			XChangeWindowAttributes(xw.dpy, xw.win, CWEventMask,
					&xw.attrs);
		}

		if (type == incratom) {
			/*
			 * Activate the PropertyNotify events so we receive
			 * when the selection owner does send us the next
			 * chunk of data.
			 */
			MODBIT(xw.attrs.event_mask, 1, PropertyChangeMask);
			XChangeWindowAttributes(xw.dpy, xw.win, CWEventMask,
					&xw.attrs);

			/*
			 * Deleting the property is the transfer start signal.
			 */
			XDeleteProperty(xw.dpy, xw.win, (int)property);
			continue;
		}

		/*
		 * As seen in getsel:
		 * Line endings are inconsistent in the terminal and GUI world
		 * copy and pasting. When receiving some selection data,
		 * replace all '\n' with '\r'.
		 * FIXME: Fix the computer world.
		 */
		repl = data;
		last = data + nitems * format / 8;
		while ((repl = memchr(repl, '\n', last - repl))) {
			*repl++ = '\r';
		}

		if (IS_SET(MODE_BRCKTPASTE) && ofs == 0)
			ttywrite("\033[200~", 6, 0);
		ttywrite((char *)data, nitems * format / 8, 1);
		if (IS_SET(MODE_BRCKTPASTE) && rem == 0)
			ttywrite("\033[201~", 6, 0);
		XFree(data);
		/* number of 32-bit chunks returned */
		ofs += nitems * format / 32;
	} while (rem > 0);

	/*
	 * Deleting the property again tells the selection owner to send the
	 * next data chunk in the property.
	 */
	XDeleteProperty(xw.dpy, xw.win, (int)property);
}

void
xclipcopy(void)
{
	clipcopy(NULL);
}

void
selclear_(XEvent *e)
{
	selclear();
}

void
selrequest(XEvent *e)
{
	XSelectionRequestEvent *xsre;
	XSelectionEvent xev;
	Atom xa_targets, string, clipboard;
	char *seltext;

	xsre = (XSelectionRequestEvent *) e;
	xev.type = SelectionNotify;
	xev.requestor = xsre->requestor;
	xev.selection = xsre->selection;
	xev.target = xsre->target;
	xev.time = xsre->time;
	if (xsre->property == None)
		xsre->property = xsre->target;

	/* reject */
	xev.property = None;

	xa_targets = xw.targets ? xw.targets : XInternAtom(xw.dpy, "TARGETS", 0);
	if (xsre->target == xa_targets) {
		/* respond with the supported type */
		string = xsel.xtarget;
		XChangeProperty(xsre->display, xsre->requestor, xsre->property,
				XA_ATOM, 32, PropModeReplace,
				(uchar *) &string, 1);
		xev.property = xsre->property;
	} else if (xsre->target == xsel.xtarget || xsre->target == XA_STRING) {
		/*
		 * xith XA_STRING non ascii characters may be incorrect in the
		 * requestor. It is not our problem, use utf8.
		 */
		clipboard = xw.clipboard ? xw.clipboard : XInternAtom(xw.dpy, "CLIPBOARD", 0);
		if (xsre->selection == XA_PRIMARY) {
			seltext = xsel.primary;
		} else if (xsre->selection == clipboard) {
			seltext = xsel.clipboard;
		} else {
			fprintf(stderr,
				"Unhandled clipboard selection 0x%lx\n",
				xsre->selection);
			return;
		}
		if (seltext != NULL) {
			XChangeProperty(xsre->display, xsre->requestor,
					xsre->property, xsre->target,
					8, PropModeReplace,
					(uchar *)seltext, strlen(seltext));
			xev.property = xsre->property;
		}
	}

	/* all done, send a notification to the listener */
	if (!XSendEvent(xsre->display, xsre->requestor, 1, 0, (XEvent *) &xev))
		fprintf(stderr, "Error sending SelectionNotify event\n");
}

void
setsel(char *str, Time t)
{
	if (!str)
		return;

	free(xsel.primary);
	xsel.primary = str;

	XSetSelectionOwner(xw.dpy, XA_PRIMARY, xw.win, t);
	if (XGetSelectionOwner(xw.dpy, XA_PRIMARY) != xw.win)
		selclear();

    clipcopy(NULL);
}

void
xsetsel(char *str)
{
	setsel(str, CurrentTime);
}

void
brelease(XEvent *e)
{
	int btn = e->xbutton.button;

	if (1 <= btn && btn <= 11)
		buttons &= ~(1 << (btn-1));

	if (IS_SET(MODE_MOUSE) && !(e->xbutton.state & forcemousemod)) {
		mousereport(e);
		return;
	}

	if (mouseaction(e, 1, 0)) {
		xneeddraw = 1;
		return;
	}
	if (btn == Button1) {
		mousesel(e, 1);
		xneeddraw = 1;
	}
    else if (e->xbutton.button == Button3)
        clippaste(NULL);
}

void
bmotion(XEvent *e)
{
	if (IS_SET(MODE_MOUSE) && !(e->xbutton.state & forcemousemod)) {
		mousereport(e);
		return;
	}

	mousesel(e, 0);
	xneeddraw = 1;
}

void
cresize(int width, int height)
{
	int col, row;

	if (width != 0)
		win.w = width;
	if (height != 0)
		win.h = height;

	col = (win.w - 2 * borderpx) / win.cw;
	row = (win.h - 2 * borderpx) / win.ch;
	col = MAX(1, col);
	row = MAX(1, row);

    win.hborderpx = (win.w - col * win.cw) / 2;
    win.vborderpx = (win.h - row * win.ch) / 2;

	tresize(col, row);
	xresize(col, row);
	ttyresize(win.tw, win.th);
}

void
xresize(int col, int row)
{
	win.tw = col * win.cw;
	win.th = row * win.ch;

	if (xdirect) {
		xw.buf = xw.win;
	} else {
		XFreePixmap(xw.dpy, xw.buf);
		xw.buf = XCreatePixmap(xw.dpy, xw.win, win.w, win.h,
				xw.depth);
		XftDrawChange(xw.draw, xw.buf);
	}
	xclear(0, 0, win.w, win.h);

	/* resize to new width */
	xw.specbuf = xrealloc(xw.specbuf, col * sizeof(GlyphFontSpec));
	xinvalidatedrawn();
}

void
fullscreen(const Arg *arg)
{
	XEvent ev;

	memset(&ev, 0, sizeof(ev));

	ev.xclient.type = ClientMessage;
	ev.xclient.message_type = xw.netwmstate;
	ev.xclient.display = xw.dpy;
	ev.xclient.window = xw.win;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = 2; /* _NET_WM_STATE_TOGGLE */
	ev.xclient.data.l[1] = xw.netwmfullscreen;

	XSendEvent(xw.dpy, DefaultRootWindow(xw.dpy), False, SubstructureNotifyMask|SubstructureRedirectMask, &ev);
}


ushort
sixd_to_16bit(int x)
{
	return x == 0 ? 0 : 0x3737 + 0x2828 * x;
}

int
xloadcolor(int i, const char *name, Color *ncolor)
{
	XRenderColor color = { .alpha = 0xffff };

	if (!name) {
		if (BETWEEN(i, 16, 255)) { /* 256 color */
			if (i < 6*6*6+16) { /* same colors as xterm */
				color.red   = sixd_to_16bit( ((i-16)/36)%6 );
				color.green = sixd_to_16bit( ((i-16)/6) %6 );
				color.blue  = sixd_to_16bit( ((i-16)/1) %6 );
			} else { /* greyscale */
				color.red = 0x0808 + 0x0a0a * (i - (6*6*6+16));
				color.green = color.blue = color.red;
			}
			return XftColorAllocValue(xw.dpy, xw.vis,
			                          xw.cmap, &color, ncolor);
		} else
			name = seloptsch->colors[i];
	}

	return XftColorAllocName(xw.dpy, xw.vis, xw.cmap, name, ncolor);
}

static inline void
xsetalpha(Color *color)
{
    float const a = focused ? alpha : alpha_unfocused;
    color->color.alpha = (unsigned short)(0xffff * a);
    color->pixel &= 0x00FFFFFF;
    color->pixel |= (unsigned char)(0xff * a) << 24;
}

void
xloadalpha(void)
{
    xsetalpha(&dc.col[seloptsch->defaultbg]);
}

void
xloadcols(void)
{
	int i;
	static int loaded;
	Color *cp;

	if (loaded) {
		for (cp = dc.col; cp < &dc.col[dc.collen]; ++cp)
			XftColorFree(xw.dpy, xw.vis, xw.cmap, cp);
	} else {
		dc.collen = MAX(LEN(seloptsch->colors), 256);
		dc.col = xmalloc(dc.collen * sizeof(Color));
	}

	for (i = 0; i < dc.collen; i++)
		if (!xloadcolor(i, NULL, &dc.col[i])) {
			if (seloptsch->colors[i])
				die("could not allocate color '%s'\n", seloptsch->colors[i]);
			else
				die("could not allocate color %d\n", i);
		}

    xloadalpha();
    xinvalidatedrawn();
    loaded = 1;
}

int
xgetcolor(int x, unsigned char *r, unsigned char *g, unsigned char *b)
{
	if (!BETWEEN(x, 0, dc.collen - 1))
		return 1;

	*r = dc.col[x].color.red >> 8;
	*g = dc.col[x].color.green >> 8;
	*b = dc.col[x].color.blue >> 8;

	return 0;
}

int
xsetcolorname(int x, const char *name)
{
	Color ncolor;

	if (!BETWEEN(x, 0, dc.collen - 1))
		return 1;

	if (!xloadcolor(x, name, &ncolor))
		return 1;

	XftColorFree(xw.dpy, xw.vis, xw.cmap, &dc.col[x]);
	dc.col[x] = ncolor;
	xinvalidatedrawn();

	return 0;
}

/*
 * Absolute coordinates.
 */
void
xclear(int x1, int y1, int x2, int y2)
{
    Color c;
    if (x2 <= x1 || y2 <= y1)
        return;
    c = dc.col[IS_SET(MODE_REVERSE)? seloptsch->defaultfg : seloptsch->defaultbg];
    if (invertcolors) {
        c = invertedcolor(&c);
    }
    XftDrawRect(xw.draw, &c, x1, y1, x2-x1, y2-y1);
}

void
xhints(void)
{
	XClassHint class = {opt_name ? opt_name : termname,
	                    opt_class ? opt_class : termname};
	XWMHints wm = {.flags = InputHint, .input = 1};
	XSizeHints *sizeh;

	sizeh = XAllocSizeHints();

	sizeh->flags = PSize | PResizeInc | PBaseSize | PMinSize;
	sizeh->height = win.h;
	sizeh->width = win.w;
	sizeh->height_inc = 1;
	sizeh->width_inc = 1;
	sizeh->base_height = 2 * borderpx;
	sizeh->base_width = 2 * borderpx;
	sizeh->min_height = win.ch + 2 * borderpx;
	sizeh->min_width = win.cw + 2 * borderpx;
	if (xw.isfixed) {
		sizeh->flags |= PMaxSize;
		sizeh->min_width = sizeh->max_width = win.w;
		sizeh->min_height = sizeh->max_height = win.h;
	}
	if (xw.gm & (XValue|YValue)) {
		sizeh->flags |= USPosition | PWinGravity;
		sizeh->x = xw.l;
		sizeh->y = xw.t;
		sizeh->win_gravity = xgeommasktogravity(xw.gm);
	}

	XSetWMProperties(xw.dpy, xw.win, NULL, NULL, NULL, 0, sizeh, &wm,
			&class);
	XFree(sizeh);
}

int
xgeommasktogravity(int mask)
{
	switch (mask & (XNegative|YNegative)) {
	case 0:
		return NorthWestGravity;
	case XNegative:
		return NorthEastGravity;
	case YNegative:
		return SouthWestGravity;
	}

	return SouthEastGravity;
}

int
xloadfont(Font *f, FcPattern *pattern)
{
	FcPattern *configured;
	FcPattern *match;
	FcResult result;
	XGlyphInfo extents;
	int wantattr, haveattr;

	/*
	 * Manually configure instead of calling XftMatchFont
	 * so that we can use the configured pattern for
	 * "missing glyph" lookups.
	 */
	configured = FcPatternDuplicate(pattern);
	if (!configured)
		return 1;

	FcConfigSubstitute(NULL, configured, FcMatchPattern);
	XftDefaultSubstitute(xw.dpy, xw.scr, configured);

	match = FcFontMatch(NULL, configured, &result);
	if (!match) {
		FcPatternDestroy(configured);
		return 1;
	}

	if (!(f->match = XftFontOpenPattern(xw.dpy, match))) {
		FcPatternDestroy(configured);
		FcPatternDestroy(match);
		return 1;
	}

	if ((XftPatternGetInteger(pattern, "slant", 0, &wantattr) ==
	    XftResultMatch)) {
		/*
		 * Check if xft was unable to find a font with the appropriate
		 * slant but gave us one anyway. Try to mitigate.
		 */
		if ((XftPatternGetInteger(f->match->pattern, "slant", 0,
		    &haveattr) != XftResultMatch) || haveattr < wantattr) {
			f->badslant = 1;
			fputs("font slant does not match\n", stderr);
		}
	}

	if ((XftPatternGetInteger(pattern, "weight", 0, &wantattr) ==
	    XftResultMatch)) {
		if ((XftPatternGetInteger(f->match->pattern, "weight", 0,
		    &haveattr) != XftResultMatch) || haveattr != wantattr) {
			f->badweight = 1;
			fputs("font weight does not match\n", stderr);
		}
	}

	XftTextExtentsUtf8(xw.dpy, f->match,
		(const FcChar8 *) ascii_printable,
		strlen(ascii_printable), &extents);

	f->set = NULL;
	f->setcs = NULL;
	f->pattern = configured;

	f->ascent = f->match->ascent;
	f->descent = f->match->descent;
	f->lbearing = 0;
	f->rbearing = f->match->max_advance_width;

	f->height = f->ascent + f->descent;
	f->width = DIVCEIL(extents.xOff, strlen(ascii_printable));
	memset(f->ascii, 0, sizeof(f->ascii));
	for (int c = 0x20; c < 0x7f; c++)
		f->ascii[c] = XftCharIndex(xw.dpy, f->match, (FcChar32)c);
	f->space_glyph = 0;
	f->space_xoff = 0;
	if (f->ascii[' '] && f->ascii[' '] < 256) {
		XGlyphInfo spext;
		XftGlyphExtents(xw.dpy, f->match, &f->ascii[' '], 1, &spext);
		if (spext.width == 0 && spext.height == 0 && spext.xOff > 0) {
			f->space_glyph = f->ascii[' '];
			f->space_xoff = spext.xOff;
		}
	}

	return 0;
}

void
xloadfonts(const char *fontstr, double fontsize)
{
	FcPattern *pattern;
	double fontval;

	if (fontstr[0] == '-')
		pattern = XftXlfdParse(fontstr, False, False);
	else
		pattern = FcNameParse((const FcChar8 *)fontstr);

	if (!pattern)
		die("can't open font %s\n", fontstr);

	if (fontsize > 1) {
		FcPatternDel(pattern, FC_PIXEL_SIZE);
		FcPatternDel(pattern, FC_SIZE);
		FcPatternAddDouble(pattern, FC_PIXEL_SIZE, (double)fontsize);
		usedfontsize = fontsize;
	} else {
		if (FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &fontval) ==
				FcResultMatch) {
			usedfontsize = fontval;
		} else if (FcPatternGetDouble(pattern, FC_SIZE, 0, &fontval) ==
				FcResultMatch) {
			usedfontsize = -1;
		} else {
			/*
			 * Default font size is 12, if none given. This is to
			 * have a known usedfontsize value.
			 */
			FcPatternAddDouble(pattern, FC_PIXEL_SIZE, 12);
			usedfontsize = 12;
		}
		defaultfontsize = usedfontsize;
	}

	if (xloadfont(&dc.font, pattern))
		die("can't open font %s\n", fontstr);

	if (usedfontsize < 0) {
		FcPatternGetDouble(dc.font.match->pattern,
		                   FC_PIXEL_SIZE, 0, &fontval);
		usedfontsize = fontval;
		if (fontsize == 0)
			defaultfontsize = fontval;
	}

	/* Setting character width and height. */
	win.cw = ceilf(dc.font.width * cwscale);
	win.ch = ceilf(dc.font.height * chscale);
    win.cyo = ceilf(dc.font.height * (chscale - 1) / 2);

	FcPatternDel(pattern, FC_SLANT);
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ITALIC);
	if (xloadfont(&dc.ifont, pattern))
		die("can't open font %s\n", fontstr);

	FcPatternDel(pattern, FC_WEIGHT);
	FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);
	if (xloadfont(&dc.ibfont, pattern))
		die("can't open font %s\n", fontstr);

	FcPatternDel(pattern, FC_SLANT);
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
	if (xloadfont(&dc.bfont, pattern))
		die("can't open font %s\n", fontstr);

	FcPatternDestroy(pattern);
}

static int
fcpatternsamefont(FcPattern *a, FcPattern *b)
{
	FcChar8 *fa = NULL, *fb = NULL;
	int ia = 0, ib = 0;

	if (!a || !b)
		return 0;
	if (FcPatternGetString(a, FC_FILE, 0, &fa) != FcResultMatch ||
	    FcPatternGetString(b, FC_FILE, 0, &fb) != FcResultMatch)
		return 0;
	if (strcmp((const char *)fa, (const char *)fb) != 0)
		return 0;
	FcPatternGetInteger(a, FC_INDEX, 0, &ia);
	FcPatternGetInteger(b, FC_INDEX, 0, &ib);
	return ia == ib;
}

static int
xfontiscolor(XftFont *xf)
{
	FcBool iscol = FcFalse;
	return xf && FcPatternGetBool(xf->pattern, FC_COLOR, 0, &iscol) == FcResultMatch && iscol;
}

int
xloadsparefont(FcPattern *pattern, int flags)
{
   FcPattern *match;
   FcResult result;
   Font *primary;
   int i;
   FcChar8 *wantfam = NULL, *gotfam = NULL;

   match = FcFontMatch(NULL, pattern, &result);
   if (!match) {
       return 1;
   }
   FcPatternGetString(pattern, FC_FAMILY, 0, &wantfam);
   FcPatternGetString(match, FC_FAMILY, 0, &gotfam);

   /*
    * Skip sparefonts that resolve to the exact same font file+index as
    * the primary font or an already-loaded sparefont for this style.
    * When configured icon fonts are not installed, all missing entries
    * otherwise collapse to duplicate copies of the default sans font.
    */
   primary = (flags == FRC_ITALICBOLD) ? &dc.ibfont
           : (flags == FRC_ITALIC)     ? &dc.ifont
           : (flags == FRC_BOLD)       ? &dc.bfont
                                       : &dc.font;
   if (primary->match && fcpatternsamefont(match, primary->match->pattern)) {
       FcPatternDestroy(match);
       return (wantfam && gotfam && strcmp((const char *)wantfam, (const char *)gotfam) != 0) ? 2 : 0;
   }
   for (i = 0; i < frclen; i++) {
       if (frc[i].flags == flags && frc[i].font &&
           fcpatternsamefont(match, frc[i].font->pattern)) {
           FcPatternDestroy(match);
           return (wantfam && gotfam && strcmp((const char *)wantfam, (const char *)gotfam) != 0) ? 2 : 0;
       }
   }

   if (!(frc[frclen].font = XftFontOpenPattern(xw.dpy, match))) {
       FcPatternDestroy(match);
       return 1;
   }

   frc[frclen].flags = flags;
   /* Believe U+0000 glyph will present in each default font */
   frc[frclen].unicodep = 0;
   frc[frclen].iscolor = xfontiscolor(frc[frclen].font);
   frclen++;

   return 0;
}

void
xloadsparefonts(void)
{
   FcPattern *pattern;
   double sizeshift, fontval;
   int fc;
   char **fp;

   if (frclen != 0)
       die("can't embed spare fonts. cache isn't empty");

   /* Calculate count of spare fonts */
   fc = sizeof(sparefonts) / sizeof(*sparefonts);
   if (fc == 0)
       return;

   /* Allocate memory for cache entries. */
   if (frccap < 4 * fc) {
       frccap += 4 * fc - frccap;
       frc = xrealloc(frc, frccap * sizeof(Fontcache));
   }

   for (fp = sparefonts; fp - sparefonts < fc; ++fp) {

       if (**fp == '-')
           pattern = XftXlfdParse(*fp, False, False);
       else
           pattern = FcNameParse((FcChar8 *)*fp);

       if (!pattern)
           die("can't open spare font %s\n", *fp);

       if (defaultfontsize > 0) {
           sizeshift = usedfontsize - defaultfontsize;
           if (sizeshift != 0 &&
                   FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &fontval) ==
                   FcResultMatch) {
               fontval += sizeshift;
               FcPatternDel(pattern, FC_PIXEL_SIZE);
               FcPatternDel(pattern, FC_SIZE);
               FcPatternAddDouble(pattern, FC_PIXEL_SIZE, fontval);
           }
       }

       FcPatternAddBool(pattern, FC_SCALABLE, 1);
       FcPatternAddBool(pattern, FC_COLOR, FcFalse);

       FcConfigSubstitute(NULL, pattern, FcMatchPattern);
       XftDefaultSubstitute(xw.dpy, xw.scr, pattern);

       int r = xloadsparefont(pattern, FRC_NORMAL);
       if (r == 1)
           die("can't open spare font %s\n", *fp);
       if (r == 2) {
           /* Uninstalled family fell back to an already-loaded generic font;
            * its italic/bold variants collapse to the same generic fallbacks. */
           FcPatternDestroy(pattern);
           continue;
       }

       FcPatternDel(pattern, FC_SLANT);
       FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ITALIC);
       if (xloadsparefont(pattern, FRC_ITALIC) == 1)
           die("can't open spare font %s\n", *fp);

       FcPatternDel(pattern, FC_WEIGHT);
       FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);
       if (xloadsparefont(pattern, FRC_ITALICBOLD) == 1)
           die("can't open spare font %s\n", *fp);

       FcPatternDel(pattern, FC_SLANT);
       FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
       if (xloadsparefont(pattern, FRC_BOLD) == 1)
           die("can't open spare font %s\n", *fp);

       FcPatternDestroy(pattern);
   }
}

void
xunloadfont(Font *f)
{
	XftFontClose(xw.dpy, f->match);
	FcPatternDestroy(f->pattern);
	if (f->set)
		FcFontSetDestroy(f->set);
	if (f->setcs)
		FcCharSetDestroy(f->setcs);
	f->set = NULL;
	f->setcs = NULL;
}

void
xunloadfonts(void)
{
	/* Free the loaded fonts in the font cache.  */
	while (frclen > 0)
		XftFontClose(xw.dpy, frc[--frclen].font);
	memset(fgcache, 0, sizeof(fgcache));

	xunloadfont(&dc.font);
	xunloadfont(&dc.bfont);
	xunloadfont(&dc.ifont);
	xunloadfont(&dc.ibfont);
}

int
ximopen(Display *dpy)
{
	XIMCallback imdestroy = { .client_data = NULL, .callback = ximdestroy };
	XICCallback icdestroy = { .client_data = NULL, .callback = xicdestroy };

	xw.ime.xim = XOpenIM(xw.dpy, NULL, NULL, NULL);
	if (xw.ime.xim == NULL)
		return 0;

	if (XSetIMValues(xw.ime.xim, XNDestroyCallback, &imdestroy, NULL))
		fprintf(stderr, "XSetIMValues: "
		                "Could not set XNDestroyCallback.\n");

	xw.ime.spotlist = XVaCreateNestedList(0, XNSpotLocation, &xw.ime.spot,
	                                      NULL);

	if (xw.ime.xic == NULL) {
		xw.ime.xic = XCreateIC(xw.ime.xim, XNInputStyle,
		                       XIMPreeditNothing | XIMStatusNothing,
		                       XNClientWindow, xw.win,
		                       XNDestroyCallback, &icdestroy,
		                       NULL);
	}
	if (xw.ime.xic == NULL)
		fprintf(stderr, "XCreateIC: Could not create input context.\n");

	return 1;
}

void
ximinstantiate(Display *dpy, XPointer client, XPointer call)
{
	if (ximopen(dpy))
		XUnregisterIMInstantiateCallback(xw.dpy, NULL, NULL, NULL,
		                                 ximinstantiate, NULL);
}

void
ximdestroy(XIM xim, XPointer client, XPointer call)
{
	xw.ime.xim = NULL;
	XRegisterIMInstantiateCallback(xw.dpy, NULL, NULL, NULL,
	                               ximinstantiate, NULL);
	XFree(xw.ime.spotlist);
}

int
xicdestroy(XIC xim, XPointer client, XPointer call)
{
	xw.ime.xic = NULL;
	return 1;
}

int
countoptsch()
{ 
	int i, optsch_limit = 256;
	for(i = 0; optionschemes[i] != NULL && i < optsch_limit; i++)
		;
	return i;
}

static int
xerror(Display *dpy, XErrorEvent *e)
{
	char buf[256];

	/*
	 * Non-fatal protocol errors (notably BadLength from
	 * RenderAddGlyphs, produced by buggy libXft when uploading an
	 * oversized/color glyph) would otherwise reach Xlib's default
	 * handler, which calls exit(). Log and continue so that a single
	 * bad glyph cannot take down the whole terminal.
	 */
	XGetErrorText(dpy, e->error_code, buf, sizeof(buf));
	fprintf(stderr, "st: ignoring X error: %s "
		"(request %d.%d, serial %lu)\n",
		buf, e->request_code, e->minor_code, e->serial);
	return 0;
}

/*
 * Is this an Exceed TurboX (ETX) display? Its X server advertises the
 * HCL_ThinX extension; -DETX=0/1 (config.mk) overrides the detection.
 */
static int
xisetx(void)
{
#ifdef ETX
	return ETX;
#else
	int opcode, event, error;

	return XQueryExtension(xw.dpy, "HCL_ThinX", &opcode, &event, &error);
#endif
}

/* ETX-specific redraw pacing; any other display keeps the config values */
static void
xsetpacing(void)
{
	if (xisetx()) {
		maxlatency = etxmaxlatency;
		xfps = etxxfps;
		is_etx = 1;
	}
}

void
xinit(int cols, int rows)
{
	XGCValues gcvalues;
	Cursor cursor;
	Window parent, root;
	pid_t thispid = getpid();
	XColor xmousefg, xmousebg;
    XWindowAttributes attr;
    XVisualInfo vis;

	if (!(xw.dpy = XOpenDisplay(NULL)))
		die("can't open display\n");
	XSetErrorHandler(xerror);
	xsetpacing();
	xw.scr = XDefaultScreen(xw.dpy);

    if (!(opt_embed && (parent = strtol(opt_embed, NULL, 0)))) {
        parent = XRootWindow(xw.dpy, xw.scr);
        xw.depth = 32;
    } else {
        XGetWindowAttributes(xw.dpy, parent, &attr);
        xw.depth = attr.depth;
    }

    if (!XMatchVisualInfo(xw.dpy, xw.scr, xw.depth, TrueColor, &vis)) {
        /*
         * The requested depth is not available (many X servers offer no
         * 32-bit ARGB visual at all). XMatchVisualInfo leaves vis
         * untouched on failure, so using it here would dereference
         * garbage in XCreateColormap. Fall back to the screen default.
         */
        xw.depth = XDefaultDepth(xw.dpy, xw.scr);
        if (!XMatchVisualInfo(xw.dpy, xw.scr, xw.depth, TrueColor, &vis))
            die("no matching TrueColor visual\n");
    }
    xw.vis = vis.visual;

	/* font */
	if (!FcInit())
		die("could not init fontconfig.\n");

	usedfont = (opt_font == NULL)? font : opt_font;
	xloadfonts(usedfont, 0);

    /* spare fonts */
    xloadsparefonts();

	/* colors */
	if (seloptschname) {
		int optschcount = countoptsch();
		for (int i = 0; optionschemes[i] && i < optschcount; i++) {
			if (strcmp(optionschemes[i]->name, seloptschname) == 0) {
				seloptsch = optionschemes[i];
				break;
			}
		}
	}
    xw.cmap = XCreateColormap(xw.dpy, parent, xw.vis, None);
	xloadcols();

	/* adjust fixed window geometry */
	win.w = 2 * win.hborderpx + cols * win.cw;
	win.h = 2 * win.vborderpx + rows * win.ch;
	if (xw.gm & XNegative)
		xw.l += DisplayWidth(xw.dpy, xw.scr) - win.w - 2;
	if (xw.gm & YNegative)
		xw.t += DisplayHeight(xw.dpy, xw.scr) - win.h - 2;

	/* Events */
	xw.attrs.background_pixel = dc.col[seloptsch->defaultbg].pixel;
	xw.attrs.border_pixel = dc.col[seloptsch->defaultbg].pixel;
	xw.attrs.bit_gravity = NorthWestGravity;
	xw.attrs.event_mask = FocusChangeMask | KeyPressMask
		| ExposureMask | VisibilityChangeMask | StructureNotifyMask
		| ButtonMotionMask | ButtonPressMask | ButtonReleaseMask;
	xw.attrs.colormap = xw.cmap;

	root = XRootWindow(xw.dpy, xw.scr);
	if (!(opt_embed && (parent = strtol(opt_embed, NULL, 0))))
		parent = root;
	xw.win = XCreateWindow(xw.dpy, root, xw.l, xw.t,
            win.w, win.h, 0, xw.depth, InputOutput,
			xw.vis, CWBackPixel | CWBorderPixel | CWBitGravity
			| CWEventMask | CWColormap, &xw.attrs);
	if (parent != root)
		XReparentWindow(xw.dpy, xw.win, parent, xw.l, xw.t);

	/*
	 * On ETX with an opaque (<32-bit) visual, render directly onto xw.win
	 * instead of an offscreen pixmap + XCopyArea. This avoids etxproxy's
	 * ShadowCopyAny synchronous FlushShadow + 18 MB sse2_blt + OpenMP
	 * WalkPrevDrawHash per frame, and lets etxproxy track only the actual
	 * changed spans (or forward XRender via ThinX vector ops).
	 */
	xdirect = (is_etx && xw.depth < 32);
	memset(&gcvalues, 0, sizeof(gcvalues));
	gcvalues.graphics_exposures = False;
	if (xdirect) {
		xw.buf = xw.win;
		dc.gc = XCreateGC(xw.dpy, xw.win, GCGraphicsExposures, &gcvalues);
	} else {
		xw.buf = XCreatePixmap(xw.dpy, xw.win, win.w, win.h, xw.depth);
		dc.gc = XCreateGC(xw.dpy, xw.buf, GCGraphicsExposures, &gcvalues);
		XSetForeground(xw.dpy, dc.gc, dc.col[seloptsch->defaultbg].pixel);
		XFillRectangle(xw.dpy, xw.buf, dc.gc, 0, 0, win.w, win.h);
	}

	/* font spec buffer */
	xw.specbuf = xmalloc(cols * sizeof(GlyphFontSpec));

	/* Xft rendering context */
	xw.draw = XftDrawCreate(xw.dpy, xw.buf, xw.vis, xw.cmap);

	/* input methods */
	if (!ximopen(xw.dpy)) {
		XRegisterIMInstantiateCallback(xw.dpy, NULL, NULL, NULL,
	                                       ximinstantiate, NULL);
	}

	/* white cursor, black outline */
	cursor = XCreateFontCursor(xw.dpy, mouseshape);
	XDefineCursor(xw.dpy, xw.win, cursor);

	if (XParseColor(xw.dpy, xw.cmap, seloptsch->colors[mousefg], &xmousefg) == 0) {
		xmousefg.red   = 0xffff;
		xmousefg.green = 0xffff;
		xmousefg.blue  = 0xffff;
	}

	if (XParseColor(xw.dpy, xw.cmap, seloptsch->colors[mousebg], &xmousebg) == 0) {
		xmousebg.red   = 0x0000;
		xmousebg.green = 0x0000;
		xmousebg.blue  = 0x0000;
	}

	XRecolorCursor(xw.dpy, cursor, &xmousefg, &xmousebg);

	{
		static char *atom_names[] = {
			"_XEMBED", "WM_DELETE_WINDOW", "_NET_WM_NAME",
			"_NET_WM_ICON_NAME", "_NET_WM_PID", "_NET_WM_STATE",
			"_NET_WM_STATE_FULLSCREEN", "UTF8_STRING",
			"CLIPBOARD", "INCR", "TARGETS"
		};
		Atom atoms[LEN(atom_names)];
		XInternAtoms(xw.dpy, atom_names, LEN(atom_names), False, atoms);
		xw.xembed          = atoms[0];
		xw.wmdeletewin     = atoms[1];
		xw.netwmname       = atoms[2];
		xw.netwmiconname   = atoms[3];
		xw.netwmpid        = atoms[4];
		xw.netwmstate      = atoms[5];
		xw.netwmfullscreen = atoms[6];
		xsel.xtarget       = atoms[7];
		xw.clipboard       = atoms[8];
		xw.incr            = atoms[9];
		xw.targets         = atoms[10];
	}
	XSetWMProtocols(xw.dpy, xw.win, &xw.wmdeletewin, 1);

	XChangeProperty(xw.dpy, xw.win, xw.netwmpid, XA_CARDINAL, 32,
			PropModeReplace, (uchar *)&thispid, 1);

	win.mode = MODE_NUMLOCK;
	resettitle();
	xhints();
	XMapWindow(xw.dpy, xw.win);
	XSync(xw.dpy, False);

	clock_gettime(CLOCK_MONOTONIC, &xsel.tclick1);
	clock_gettime(CLOCK_MONOTONIC, &xsel.tclick2);
	xsel.primary = NULL;
	xsel.clipboard = NULL;
	if (xsel.xtarget == None)
		xsel.xtarget = XA_STRING;

	if (initialzoom != 0) {
		Arg zoomarg = { .f = usedfontsize + initialzoom };
		/* zoomarg.f = usedfontsize + initialzoom; */
		zoomabs(&zoomarg);
	}

    boxdraw_xinit(xw.dpy, xw.cmap, xw.draw, xw.vis);
}

/*
 * Resolve a rune that the primary font lacks. Returns an index into frc[],
 * or FG_NOTFOUND / FG_SKIP, and stores the glyph index in *gidx.
 * Callers memoize the result in fgcache, so this runs once per rune/style.
 */
static int
xfallbackglyph(Font *font, int frcflags, Rune rune, FT_UInt *gidx)
{
	FcResult fcres;
	FcPattern *fcpattern, *fontpattern;
	FcFontSet *fcsets[1];
	FcCharSet *fccharset, *matchcs;
	int f;

	*gidx = 0;

	/* Fonts opened earlier, in the same order the old code scanned them. */
	for (f = 0; f < frclen; f++) {
		if (frc[f].flags != frcflags)
			continue;
		if ((*gidx = XftCharIndex(xw.dpy, frc[f].font, rune))) {
			if (frc[f].iscolor) {
				*gidx = 0;
				return FG_SKIP;
			}
			return f;
		}
	}

	/*
	 * FcFontSort() also reports the union coverage of every candidate
	 * font. A rune outside it exists in no installed font, so skip the
	 * per-rune fontconfig match (measured 5-26 ms) that could only fail.
	 * This is what keeps `cat` of a binary file from stalling the terminal.
	 */
	if (!font->set)
		font->set = FcFontSort(0, font->pattern, 1, &font->setcs, &fcres);
	if (!font->set || !font->setcs || !FcCharSetHasChar(font->setcs, rune))
		return FG_NOTFOUND;

	fcpattern = FcPatternDuplicate(font->pattern);
	fccharset = FcCharSetCreate();
	FcCharSetAddChar(fccharset, rune);
	FcPatternAddCharSet(fcpattern, FC_CHARSET, fccharset);
	FcPatternAddBool(fcpattern, FC_SCALABLE, 1);
	FcPatternAddBool(fcpattern, FC_COLOR, FcFalse);
	FcConfigSubstitute(0, fcpattern, FcMatchPattern);
	FcDefaultSubstitute(fcpattern);
	fcsets[0] = font->set;
	fontpattern = FcFontSetMatch(0, fcsets, 1, fcpattern, &fcres);
	FcPatternDestroy(fcpattern);
	FcCharSetDestroy(fccharset);

	/* fontconfig returns its best match even when that font lacks the rune */
	if (!fontpattern)
		return FG_NOTFOUND;
	if (FcPatternGetCharSet(fontpattern, FC_CHARSET, 0, &matchcs) == FcResultMatch &&
	    !FcCharSetHasChar(matchcs, rune)) {
		FcPatternDestroy(fontpattern);
		return FG_NOTFOUND;
	}

	/* Reuse an open copy of the same font file rather than a duplicate. */
	for (f = 0; f < frclen; f++) {
		if (frc[f].flags == frcflags &&
		    fcpatternsamefont(fontpattern, frc[f].font->pattern))
			break;
	}
	if (f < frclen) {
		FcPatternDestroy(fontpattern);
	} else {
		if (frclen >= frccap) {
			frccap += 16;
			frc = xrealloc(frc, frccap * sizeof(Fontcache));
		}
		/* XftFontOpenPattern takes ownership of fontpattern */
		frc[frclen].font = XftFontOpenPattern(xw.dpy, fontpattern);
		if (!frc[frclen].font)
			die("XftFontOpenPattern failed seeking fallback font: %s\n",
				strerror(errno));
		frc[frclen].flags = frcflags;
		frc[frclen].unicodep = rune;
		frc[frclen].iscolor = xfontiscolor(frc[frclen].font);
		f = frclen++;
	}

	/*
	 * Colour (BGRA) fonts stay open so later runes they cover resolve
	 * without fontconfig, but are never drawn: libXft < 2.3.5 dies with
	 * BadLength in RenderAddGlyphs when uploading their glyphs.
	 */
	if (frc[f].iscolor)
		return FG_SKIP;
	if (!(*gidx = XftCharIndex(xw.dpy, frc[f].font, rune)))
		return FG_NOTFOUND;
	return f;
}

int
xmakeglyphfontspecs(XftGlyphFontSpec *specs, const Glyph *glyphs, int len, int x, int y)
{
	float winx = win.hborderpx + x * win.cw, winy = win.vborderpx + y * win.ch, xp, yp;
	ushort mode, prevmode = USHRT_MAX;
	Font *font = &dc.font;
	int frcflags = FRC_NORMAL;
	float runewidth = win.cw;
	Rune rune;
	FT_UInt glyphidx;
	uint h;
	int i, f, numspecs = 0;

    for (i = 0, xp = winx, yp = winy + font->ascent + win.cyo; i < len; ++i) {
		/* Fetch rune and mode for current glyph. */
		rune = glyphs[i].u;
		mode = glyphs[i].mode;

		/* Skip dummy wide-character spacing. */
		if (mode == ATTR_WDUMMY)
			continue;

		/* Determine font for glyph if different from previous glyph. */
		if (prevmode != mode) {
			prevmode = mode;
			font = &dc.font;
			frcflags = FRC_NORMAL;
			runewidth = win.cw * ((mode & ATTR_WIDE) ? 2.0f : 1.0f);
			if ((mode & ATTR_ITALIC) && (mode & ATTR_BOLD)) {
				font = &dc.ibfont;
				frcflags = FRC_ITALICBOLD;
			} else if (mode & ATTR_ITALIC) {
				font = &dc.ifont;
				frcflags = FRC_ITALIC;
			} else if (mode & ATTR_BOLD) {
				font = &dc.bfont;
				frcflags = FRC_BOLD;
			}
			yp = winy + font->ascent + win.cyo;
		}

		/*
		 * Fast path for plain spaces: DRAW_BG already paints the cell
		 * background, so emit a placeholder (font == NULL) that keeps
		 * specs 1:1 with xdrawline() while skipping XftCharIndex and
		 * the XRender glyph upload for the ~80% of cells that are blank.
		 */
		if (rune == ' ' && !(mode & ATTR_BOXDRAW)) {
			specs[numspecs].font = NULL;
			specs[numspecs].glyph = 0;
			specs[numspecs].x = (short)xp;
			specs[numspecs].y = (short)yp;
			xp += runewidth;
			numspecs++;
			continue;
		}

        if (mode & ATTR_BOXDRAW) {
            /* minor shoehorning: boxdraw uses only this ushort */
            glyphidx = boxdrawindex(&glyphs[i]);
        } else if (rune < 128) {
            glyphidx = font->ascii[rune];
        } else {
            /* Lookup character index with default font. */
            glyphidx = XftCharIndex(xw.dpy, font->match, rune);
        }
		if (glyphidx) {
			specs[numspecs].font = font->match;
			specs[numspecs].glyph = glyphidx;
			specs[numspecs].x = (short)xp;
			specs[numspecs].y = (short)yp;
			xp += runewidth;
			numspecs++;
			continue;
		}

		/*
		 * Rune missing from the primary font: one probe of fgcache;
		 * the full fallback search runs only on a cache miss.
		 */
		h = ((rune * 2654435761u) ^ (uint)frcflags) & (FGCACHE_SIZE - 1);
		if (!fgcache[h].valid || fgcache[h].rune != rune ||
		    fgcache[h].flags != frcflags) {
			fgcache[h].frcidx = xfallbackglyph(font, frcflags, rune,
			                                   &fgcache[h].glyphidx);
			fgcache[h].rune = rune;
			fgcache[h].flags = frcflags;
			fgcache[h].valid = 1;
		}
		f = fgcache[h].frcidx;
		specs[numspecs].font = (f >= 0) ? frc[f].font
		                     : (f == FG_NOTFOUND) ? font->match : NULL;
		specs[numspecs].glyph = fgcache[h].glyphidx;
		specs[numspecs].x = (short)xp;
		specs[numspecs].y = (short)yp;
		xp += runewidth;
		numspecs++;
	}

	return numspecs;
}

void
xdrawglyphfontspecs(const XftGlyphFontSpec *specs, Glyph base, int len, int x, int y, int dmode)
{
	int charlen = len * ((base.mode & ATTR_WIDE) ? 2 : 1);
	int winx = win.hborderpx + x * win.cw, winy = win.vborderpx + y * win.ch,
	    width = charlen * win.cw;
	Color *fg, *bg, *temp, revfg, revbg, truefg, truebg;
	XRenderColor colfg, colbg;

	/* Fallback on color display for attributes not supported by the font */
	if (base.mode & ATTR_ITALIC && base.mode & ATTR_BOLD) {
		if (dc.ibfont.badslant || dc.ibfont.badweight)
			base.fg = defaultattr;
	} else if ((base.mode & ATTR_ITALIC && dc.ifont.badslant) ||
	    (base.mode & ATTR_BOLD && dc.bfont.badweight)) {
		base.fg = defaultattr;
	}

	if (IS_TRUECOL(base.fg)) {
		colfg.alpha = 0xffff;
		colfg.red = TRUERED(base.fg);
		colfg.green = TRUEGREEN(base.fg);
		colfg.blue = TRUEBLUE(base.fg);
		XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg, &truefg);
		fg = &truefg;
	} else {
		fg = &dc.col[base.fg];
	}

	if (IS_TRUECOL(base.bg)) {
		colbg.alpha = 0xffff;
		colbg.green = TRUEGREEN(base.bg);
		colbg.red = TRUERED(base.bg);
		colbg.blue = TRUEBLUE(base.bg);
		XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colbg, &truebg);
		bg = &truebg;
	} else {
		bg = &dc.col[base.bg];
	}

	/* Change basic system colors [0-7] to bright system colors [8-15] */
	if (brightisbold && (base.mode & ATTR_BOLD_FAINT) == ATTR_BOLD && BETWEEN(base.fg, 0, 7))
		fg = &dc.col[base.fg + 8];

	if (IS_SET(MODE_REVERSE)) {
		if (fg == &dc.col[seloptsch->defaultfg]) {
			fg = &dc.col[seloptsch->defaultbg];
		} else {
			colfg.red = ~fg->color.red;
			colfg.green = ~fg->color.green;
			colfg.blue = ~fg->color.blue;
			colfg.alpha = fg->color.alpha;
			XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg,
					&revfg);
			fg = &revfg;
		}

		if (bg == &dc.col[seloptsch->defaultbg]) {
			bg = &dc.col[seloptsch->defaultfg];
		} else {
			colbg.red = ~bg->color.red;
			colbg.green = ~bg->color.green;
			colbg.blue = ~bg->color.blue;
			colbg.alpha = bg->color.alpha;
			XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colbg,
					&revbg);
			bg = &revbg;
		}
	}

	if ((base.mode & ATTR_BOLD_FAINT) == ATTR_FAINT) {
		colfg.red = fg->color.red / 2;
		colfg.green = fg->color.green / 2;
		colfg.blue = fg->color.blue / 2;
		colfg.alpha = fg->color.alpha;
		XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg, &revfg);
		fg = &revfg;
	}

	if (selectionstyle && (base.mode & ATTR_REVERSE)) {
        if (selectionstyle == 1) {
            temp = fg;
            fg = bg;
            bg = temp;
        } else {
            bg = &dc.col[selectionbg];
            if (selectionstyle != 2)
                fg = &dc.col[selectionfg];
        }
	}

	if (base.mode & ATTR_BLINK && win.mode & MODE_BLINK)
		fg = bg;

	if (base.mode & ATTR_INVISIBLE)
		fg = bg;

    if (invertcolors) {
        revfg = invertedcolor(fg);
        revbg = invertedcolor(bg);
        fg = &revfg;
        bg = &revbg;
    }

#ifdef ALPHA_ALL
    xsetalpha(bg);
#endif

    if (dmode & DRAW_BG) {
        /* Intelligent cleaning up of the borders. */
        if (x == 0) {
			xclear(0, (y == 0)? 0 : winy, win.hborderpx,
				   winy + win.ch +
				   ((winy + win.ch >= win.vborderpx + win.th)? win.h : 0));
        }
        if (winx + width >= win.hborderpx + win.tw) {
			xclear(winx + width, (y == 0)? 0 : winy, win.w,
				   ((winy + win.ch >= win.vborderpx + win.th)? win.h : (winy + win.ch)));
        }
		if (y == 0)
			xclear(winx, 0, winx + width, win.vborderpx);
		if (winy + win.ch >= win.vborderpx + win.th)
			xclear(winx, winy + win.ch, winx + width, win.h);
        /* Fill the background */
        XftDrawRect(xw.draw, bg, winx, winy, width, win.ch);
    }

    if (dmode & DRAW_FG) {
		if (base.mode & ATTR_BOXDRAW) {
			drawboxes(winx, winy, width / len, win.ch, fg, bg, specs, len);
		} else {
			/*
			 * Drop placeholder specs (font == NULL: spaces and
			 * colour-font glyphs) so XRender only sees real glyphs.
			 * A missing glyph keeps its font with glyph 0 and still
			 * renders the font's .notdef box, as it did before.
			 */
			XftGlyphFontSpec stackbuf[512];
			XftGlyphFontSpec *fgspecs = (len <= (int)LEN(stackbuf))
			    ? stackbuf : xmalloc((size_t)len * sizeof(*fgspecs));
			int k, nfg = 0;

			for (k = 0; k < len; k++) {
				if (specs[k].font)
					fgspecs[nfg++] = specs[k];
			}
			if (nfg > 0)
				XftDrawGlyphFontSpec(xw.draw, fg, fgspecs, nfg);
			if (fgspecs != stackbuf)
				free(fgspecs);
		}

        /* Render underline and strikethrough. */
        if (base.mode & ATTR_UNDERLINE) {
			XftDrawRect(xw.draw, fg, winx, winy + win.cyo + dc.font.ascent * chscale + 1,
						width, 1);
        }

        if (base.mode & ATTR_STRUCK) {
			XftDrawRect(xw.draw, fg, winx, winy + win.cyo + 2 * dc.font.ascent * chscale / 3,
						width, 1);
        }
    }
}

void
xdrawglyph(Glyph g, int x, int y)
{
	int numspecs;
	XftGlyphFontSpec spec;

	numspecs = xmakeglyphfontspecs(&spec, &g, 1, x, y);
	xdrawglyphfontspecs(&spec, g, numspecs, x, y, DRAW_BG | DRAW_FG);
}

static void
xdrawcursor_at(int cx, int cy, Glyph g)
{
	Color drawcol;

	if (IS_SET(MODE_HIDE))
		return;

	/*
	 * Select the right color for the right mode.
	 */
	g.mode &= ATTR_BOLD|ATTR_ITALIC|ATTR_UNDERLINE|ATTR_STRUCK|ATTR_WIDE|ATTR_BOXDRAW;

	if (IS_SET(MODE_REVERSE)) {
		g.mode |= ATTR_REVERSE;
		g.bg = seloptsch->defaultfg;
		if (selected(cx, cy)) {
			drawcol = dc.col[seloptsch->defaultcs];
			g.fg = seloptsch->defaultrcs;
		} else {
			drawcol = dc.col[seloptsch->defaultrcs];
			g.fg = seloptsch->defaultcs;
		}
	} else {
		if (selected(cx, cy)) {
			g.fg = seloptsch->defaultfg;
			g.bg = seloptsch->defaultrcs;
		} else {
			g.fg = seloptsch->defaultfg;
			g.bg = seloptsch->defaultrcs;
		}
		drawcol = dc.col[g.bg];
	}

	/* draw the new one */
	if (IS_SET(MODE_FOCUSED)) {
		switch (win.cursor) {
		case 7: /* st extension */
			g.u = 0x2603; /* snowman (U+2603) */
			/* FALLTHROUGH */
		case 0: /* Blinking Block */
		case 1: /* Blinking Block (Default) */
		case 2: /* Steady Block */
			xdrawglyph(g, cx, cy);
			break;
		case 3: /* Blinking Underline */
		case 4: /* Steady Underline */
			XftDrawRect(xw.draw, &drawcol,
                    win.hborderpx + cx * win.cw,
                    win.vborderpx + (cy + 1) * win.ch - \
						cursorthickness,
					win.cw, cursorthickness);
			break;
		case 5: /* Blinking bar */
		case 6: /* Steady bar */
			XftDrawRect(xw.draw, &drawcol,
					win.hborderpx + cx * win.cw,
					win.vborderpx + cy * win.ch,
					cursorthickness, win.ch);
			break;
		}
	} else {
		XftDrawRect(xw.draw, &drawcol,
				win.hborderpx + cx * win.cw,
				win.vborderpx + cy * win.ch,
				win.cw - 1, 1);
		XftDrawRect(xw.draw, &drawcol,
				win.hborderpx + cx * win.cw,
				win.vborderpx + cy * win.ch,
				1, win.ch - 1);
		XftDrawRect(xw.draw, &drawcol,
				win.hborderpx + (cx + 1) * win.cw - 1,
				win.vborderpx + cy * win.ch,
				1, win.ch - 1);
		XftDrawRect(xw.draw, &drawcol,
				win.hborderpx + cx * win.cw,
				win.vborderpx + (cy + 1) * win.ch - 1,
				win.cw, 1);
	}
}

void
xdrawcursor(int cx, int cy, Glyph g, int ox, int oy, Glyph og)
{
	/* remove the old cursor */
	if (selected(ox, oy))
		og.mode ^= ATTR_REVERSE;
	xdrawglyph(og, ox, oy);
	xdrawcursor_at(cx, cy, g);
}

void
xsetenv(void)
{
	char buf[sizeof(long) * 8 + 1];

	snprintf(buf, sizeof(buf), "%lu", xw.win);
	setenv("WINDOWID", buf, 1);
}

void
xseticontitle(char *p)
{
	static char *last_icontitle = NULL;
	XTextProperty prop;
	DEFAULT(p, opt_title);

	if (p[0] == '\0')
		p = opt_title;

	if (last_icontitle && strcmp(last_icontitle, p) == 0)
		return;
	free(last_icontitle);
	last_icontitle = xstrdup(p);

	if (Xutf8TextListToTextProperty(xw.dpy, &p, 1, XUTF8StringStyle,
	                                &prop) != Success)
		return;
	XSetWMIconName(xw.dpy, xw.win, &prop);
	XSetTextProperty(xw.dpy, xw.win, &prop, xw.netwmiconname);
	XFree(prop.value);
}

void
xfreetitlestack(void)
{
	for (int i = 0; i < LEN(titlestack); i++) {
		free(titlestack[i]);
		titlestack[i] = NULL;
	}
}

void
xsettitle(char *p, int pop)
{
 	XTextProperty prop;

	if (!pop && p && titlestack[tstki] && strcmp(titlestack[tstki], p) == 0)
		return;

	free(titlestack[tstki]);
	if (pop) {
		titlestack[tstki] = NULL;
		tstki = (tstki - 1 + TITLESTACKSIZE) % TITLESTACKSIZE;
		p = titlestack[tstki] ? titlestack[tstki] : opt_title;
	} else if (p) {
		titlestack[tstki] = xstrdup(p);
	} else {
		titlestack[tstki] = NULL;
		p = opt_title;
	}

	if (Xutf8TextListToTextProperty(xw.dpy, &p, 1, XUTF8StringStyle,
	                                &prop) != Success)
		return;
	XSetWMName(xw.dpy, xw.win, &prop);
	XSetTextProperty(xw.dpy, xw.win, &prop, xw.netwmname);
	XFree(prop.value);
}

void
xpushtitle(void)
{
	int tstkin = (tstki + 1) % TITLESTACKSIZE;

	free(titlestack[tstkin]);
	titlestack[tstkin] = titlestack[tstki] ? xstrdup(titlestack[tstki]) : NULL;
	tstki = tstkin;
}

int
xstartdraw(void)
{
	if (!IS_SET(MODE_VISIBLE))
		return 0;
	dirty_x1 = win.w;
	dirty_y1 = win.h;
	dirty_x2 = 0;
	dirty_y2 = 0;
	ndirty_rects = 0;
	dirty_overflow = 0;
	if (!drawn_valid)
		xdirtyrect(0, 0, win.w, win.h);
	return 1;
}

static uint64_t
xrowhash(const Glyph *line, int col)
{
	uint64_t h = 14695981039346656037ULL;
	const unsigned char *p = (const unsigned char *)line;
	size_t bytes = (size_t)col * sizeof(Glyph);
	size_t i;

	for (i = 0; i + 8 <= bytes; i += 8) {
		uint64_t v;
		memcpy(&v, p + i, 8);
		h ^= v;
		h *= 1099511628211ULL;
	}
	return h;
}

static int
rowhascontent(const Glyph *line, int col)
{
	int x;
	for (x = 0; x < col; x++) {
		if (line[x].u != ' ' || line[x].mode != 0)
			return 1;
	}
	return 0;
}

/*
 * Guess how far the screen content moved since the last frame by finding a
 * few distinctive rows (anchors) of the new frame in the shadow buffer.
 * Rows that occur more than twice (separators, repeated log lines) are
 * ambiguous and skipped, and at most MAXSCROLLCAND shifts are scored, so
 * the cost stays bounded on any screen. Returns 0 when no shift beats
 * in-place diffing by a useful margin.
 *
 * Correctness never depends on the guess: after the blit, every row that
 * differs from the shifted shadow buffer is repainted by the row diff.
 */
#define MAXSCROLLCAND 6
static int
xfindscroll(Line *lines, int col, int row, int matches0)
{
	int anchors[3] = { row / 2, row / 4, (3 * row) / 4 };
	int cand[MAXSCROLLCAND], hit[2];
	int a, i, y, oy, d, m, nhit, ay, off, ncand = 0;
	int best_d = 0, best = matches0;
	size_t rowbytes = (size_t)col * sizeof(Glyph);

	for (a = 0; a < 3; a++) {
		/*
		 * Search outward from each anchor for a distinctive non-blank row
		 * (1 or 2 matches in the previous frame). Stepping past ambiguous
		 * rows handles tables where repeated separator rules fall directly
		 * on an anchor index.
		 */
		int found = 0;
		for (off = 0; off < row / 4 && !found; off++) {
			int tries[2] = { anchors[a] + off, anchors[a] - off };
			int nt = (off == 0) ? 1 : 2;
			for (int t = 0; t < nt && !found; t++) {
				uint64_t ah;
				ay = tries[t];
				if (ay < 0 || ay >= row || !rowhascontent(lines[ay], col))
					continue;
				ah = cur_hash[ay];
				nhit = 0;
				for (oy = 0; oy < row && nhit <= 2; oy++) {
					if (oy == ay || !drawn_row_valid[oy] ||
					    drawn_hash[oy] != ah ||
					    memcmp(lines[ay], &drawn_buf[(size_t)oy * col], rowbytes))
						continue;
					if (nhit < 2)
						hit[nhit] = oy;
					nhit++;
				}
				if (nhit == 1 || nhit == 2) {
					for (i = 0; i < nhit && ncand < MAXSCROLLCAND; i++) {
						d = hit[i] - ay;
						for (m = 0; m < ncand && cand[m] != d; m++)
							;
						if (m == ncand)
							cand[ncand++] = d;
					}
					found = 1;
				}
			}
		}
	}

	/* score each candidate: rows reusable if we shift by d */
	for (i = 0; i < ncand; i++) {
		d = cand[i];
		m = 0;
		for (y = MAX(0, -d); y < MIN(row, row - d); y++) {
			if (drawn_row_valid[y + d] && drawn_hash[y + d] == cur_hash[y] &&
			    !memcmp(lines[y], &drawn_buf[(size_t)(y + d) * col], rowbytes))
				m++;
		}
		if (m > best) {
			best = m;
			best_d = d;
		}
	}
	/* the blit must save a few repaints to be worth an extra copy */
	return (best > matches0 + 2) ? best_d : 0;
}

void
xdrawscreen(Line *lines, int col, int row, int showcursor, int cx, int cy, int hassel)
{
	int y, x, best_d = 0, matches0 = 0;
	int rowbytes = col * (int)sizeof(Glyph);
	int cursor_kept = 0;

	if (drawn_col != col || drawn_row != row || !drawn_buf) {
		drawn_col = col;
		drawn_row = row;
		drawn_buf = xrealloc(drawn_buf, (size_t)row * (size_t)col * sizeof(Glyph));
		drawn_row_valid = xrealloc(drawn_row_valid, (size_t)row);
		drawn_hash = xrealloc(drawn_hash, (size_t)row * sizeof(*drawn_hash));
		cur_hash = xrealloc(cur_hash, (size_t)row * sizeof(*cur_hash));
		cur_row_same = xrealloc(cur_row_same, (size_t)row);
		drawn_eff_row = xrealloc(drawn_eff_row, (size_t)col * sizeof(Glyph));
		drawn_valid = 0;
		prev_cursor_drawn = 0;
	}

	/*
	 * Detect vertical scrolling against the cursor-free shadow buffer
	 * drawn_buf so we can shift unchanged rows inside xw.buf with one
	 * server-side XCopyArea instead of re-rasterizing all rows via Xft.
	 */
	if (drawn_valid && !hassel && row >= 4) {
		for (y = 0; y < row; y++) {
			cur_hash[y] = xrowhash(lines[y], col);
			if (drawn_row_valid[y] &&
			    drawn_hash[y] == cur_hash[y] &&
			    memcmp(lines[y], &drawn_buf[(size_t)y * col], rowbytes) == 0) {
				cur_row_same[y] = 1;
				matches0++;
			} else {
				cur_row_same[y] = 0;
			}
		}
		if (!xdirect && matches0 < row - 2)
			best_d = xfindscroll(lines, col, row, matches0);
	}

	/*
	 * Decide whether the previously painted cursor on xw.buf can stay
	 * untouched or must be erased before blitting/updating cells.
	 */
	if (drawn_valid && prev_cursor_drawn) {
		int cursor_row_same = 0;
		if (!cursor_dirty && best_d == 0 && showcursor && !IS_SET(MODE_HIDE) &&
		    prev_cx == cx && prev_cy == cy && !hassel &&
		    ((row >= 4) ? cur_row_same[prev_cy]
		                : (drawn_row_valid[prev_cy] &&
		                   memcmp(lines[prev_cy], &drawn_buf[(size_t)prev_cy * col], rowbytes) == 0))) {
			cursor_row_same = 1;
		}
		if (cursor_row_same) {
			cursor_kept = 1;
		} else {
			Glyph og = drawn_buf[(size_t)prev_cy * col + prev_cx];
			xdrawglyph(og, prev_cx, prev_cy);
			if (xdirect)
				xdirect_buf_est += 256;
			xdirtycells(prev_cx, prev_cy,
			            prev_cx + ((og.mode & ATTR_WIDE) ? 2 : 1),
			            prev_cy + 1, col, row);
			prev_cursor_drawn = 0;
		}
	}
	cursor_dirty = 0;

	/*
	 * Apply server-side scroll-blit on xw.buf and mirror it in drawn_buf,
	 * clamped to the exact matching band [band_top, band_bot] so fixed
	 * headers/footers (e.g. TUI status bars or prompt boxes) are preserved
	 * intact instead of being overwritten and repainted every scroll frame.
	 */
	if (best_d != 0) {
		int lo = MAX(0, -best_d), hi = MIN(row, row - best_d);
		int band_top = row, band_bot = -1;
		int gained = 0, lost = 0;

		for (y = lo; y < hi; y++) {
			if (!cur_row_same[y] && drawn_row_valid[y + best_d] &&
			    drawn_hash[y + best_d] == cur_hash[y] &&
			    memcmp(lines[y], &drawn_buf[(size_t)(y + best_d) * col], rowbytes) == 0) {
				if (y < band_top) band_top = y;
				band_bot = y;
				gained++;
			}
		}
		if (band_top <= band_bot) {
			for (y = band_top; y <= band_bot; y++) {
				if (cur_row_same[y])
					lost++;
			}
		}
		if (band_top <= band_bot && gained > lost + 2) {
			int count = band_bot - band_top + 1;
			int src_top = band_top + best_d;
			XCopyArea(xw.dpy, xw.buf, xw.buf, dc.gc,
			          win.hborderpx, win.vborderpx + src_top * win.ch,
			          win.tw, count * win.ch,
			          win.hborderpx, win.vborderpx + band_top * win.ch);
			memmove(&drawn_buf[(size_t)band_top * col],
			        &drawn_buf[(size_t)src_top * col],
			        (size_t)count * (size_t)col * sizeof(Glyph));
			memmove(&drawn_row_valid[band_top], &drawn_row_valid[src_top], (size_t)count);
			memmove(&drawn_hash[band_top], &drawn_hash[src_top], (size_t)count * sizeof(*drawn_hash));
			xdirtycells(0, band_top, col, band_bot + 1, col, row);
			for (y = band_top; y <= band_bot; y++) {
				cur_row_same[y] = (drawn_row_valid[y] &&
				                   drawn_hash[y] == cur_hash[y] &&
				                   memcmp(lines[y], &drawn_buf[(size_t)y * col], rowbytes) == 0);
			}
		} else {
			best_d = 0;
		}
	}

	/* Redraw only rows (and column spans) that actually changed. */
	xbatch_active = 1;
	xbg_pic = XftDrawPicture(xw.draw);
	xnbg_buckets = 0;
	xnfg_buckets = 0;
	xnbox_buckets = 0;
	xndeco = 0;
	for (y = 0; y < row; y++) {
		const Glyph *src = lines[y];
		Glyph *dst = &drawn_buf[(size_t)y * col];
		int known_same = (drawn_valid && !hassel && row >= 4 && cur_row_same[y]);

		if (known_same)
			continue;

		if (hassel) {
			memcpy(drawn_eff_row, lines[y], rowbytes);
			for (x = 0; x < col; x++) {
				if (selected(x, y))
					drawn_eff_row[x].mode ^= ATTR_REVERSE;
			}
			src = drawn_eff_row;
		}

		if (!drawn_valid || !drawn_row_valid[y]) {
			xdrawline(lines[y], 0, y, col);
			memcpy(dst, src, rowbytes);
			drawn_hash[y] = (!hassel && row >= 4 && drawn_valid)
			              ? cur_hash[y] : xrowhash(src, col);
			drawn_row_valid[y] = 1;
			xdirtycells(0, y, col, y + 1, col, row);
		} else if ((!hassel && row >= 4) || memcmp(src, dst, rowbytes) != 0) {
			/*
			 * Diff changed column spans, splitting across unchanged gaps
			 * >= 16 cells so distant edits on wide rows don't repaint
			 * unchanged cells in between. Frame-wide batching merges
			 * bg rects and fg glyphs across all spans and rows.
			 */
			int pos = 0;
			while (pos < col) {
				int x1, x2, scan, gap = 0;
				while (pos < col && memcmp(&src[pos], &dst[pos], sizeof(Glyph)) == 0)
					pos++;
				if (pos >= col)
					break;
				x1 = x2 = pos;
				for (scan = pos + 1; scan < col; scan++) {
					if (memcmp(&src[scan], &dst[scan], sizeof(Glyph)) != 0) {
						x2 = scan;
						gap = 0;
					} else if (++gap >= 16) {
						break;
					}
				}
				pos = scan;
				/* Expand by 1 cell each side for italic/glyph bearing overhang */
				if (x1 > 0) x1--;
				if (x2 + 1 < col) x2++;
				while (x1 > 0 && (lines[y][x1].mode & ATTR_WDUMMY))
					x1--;
				if (x2 + 1 < col && (lines[y][x2].mode & ATTR_WIDE))
					x2++;
				xdrawline(lines[y], x1, y, x2 + 1);
				xdirtycells(x1, y, x2 + 1, y + 1, col, row);
			}
			memcpy(dst, src, rowbytes);
			drawn_hash[y] = (!hassel && row >= 4) ? cur_hash[y] : xrowhash(src, col);
		}
		if (xdirect && xbatch_est_bytes() >= 4096)
			xdirect_flush_strip();
	}
	if (xdirect)
		xdirect_flush_strip();
	else
		xbatch_flush_all();
	xbatch_active = 0;
	drawn_valid = 1;

	if (showcursor && !cursor_kept && !IS_SET(MODE_HIDE)) {
		xdrawcursor_at(cx, cy, lines[cy][cx]);
		if (xdirect)
			xdirect_buf_est += 256;
		prev_cursor_drawn = 1;
		prev_cx = cx;
		prev_cy = cy;
		xdirtycells(cx, cy, cx + ((lines[cy][cx].mode & ATTR_WIDE) ? 2 : 1),
		            cy + 1, col, row);
	}
}

static inline int
xcoloreq(const Color *a, const Color *b)
{
	return a->color.red == b->color.red &&
	       a->color.green == b->color.green &&
	       a->color.blue == b->color.blue &&
	       a->color.alpha == b->color.alpha;
}

static void
xresolvecolors(Glyph base, int need_fg, int need_bg,
               Color **fgp, Color **bgp,
               Color *truefg, Color *truebg, Color *revfg, Color *revbg)
{
	Color *fg = NULL, *bg = NULL, *temp;
	XRenderColor colfg, colbg;
	int swap_sel = (selectionstyle == 1 && (base.mode & ATTR_REVERSE));
	int fg_needed = need_fg || swap_sel;
	int bg_needed = need_bg || swap_sel ||
	                (need_fg && ((base.mode & ATTR_INVISIBLE) ||
	                             ((base.mode & ATTR_BLINK) && (win.mode & MODE_BLINK))));

	if (fg_needed) {
		if (base.mode & ATTR_ITALIC && base.mode & ATTR_BOLD) {
			if (dc.ibfont.badslant || dc.ibfont.badweight)
				base.fg = defaultattr;
		} else if ((base.mode & ATTR_ITALIC && dc.ifont.badslant) ||
		    (base.mode & ATTR_BOLD && dc.bfont.badweight)) {
			base.fg = defaultattr;
		}

		if (IS_TRUECOL(base.fg)) {
			colfg.alpha = 0xffff;
			colfg.red = TRUERED(base.fg);
			colfg.green = TRUEGREEN(base.fg);
			colfg.blue = TRUEBLUE(base.fg);
			XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg, truefg);
			fg = truefg;
		} else {
			fg = &dc.col[base.fg];
		}

		if (brightisbold && (base.mode & ATTR_BOLD_FAINT) == ATTR_BOLD && BETWEEN(base.fg, 0, 7))
			fg = &dc.col[base.fg + 8];
	}

	if (bg_needed) {
		if (IS_TRUECOL(base.bg)) {
			colbg.alpha = 0xffff;
			colbg.green = TRUEGREEN(base.bg);
			colbg.red = TRUERED(base.bg);
			colbg.blue = TRUEBLUE(base.bg);
			XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colbg, truebg);
			bg = truebg;
		} else {
			bg = &dc.col[base.bg];
		}
	}

	if (IS_SET(MODE_REVERSE)) {
		if (fg_needed) {
			if (fg == &dc.col[seloptsch->defaultfg]) {
				fg = &dc.col[seloptsch->defaultbg];
			} else {
				colfg.red = ~fg->color.red;
				colfg.green = ~fg->color.green;
				colfg.blue = ~fg->color.blue;
				colfg.alpha = fg->color.alpha;
				XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg, revfg);
				fg = revfg;
			}
		}
		if (bg_needed) {
			if (bg == &dc.col[seloptsch->defaultbg]) {
				bg = &dc.col[seloptsch->defaultfg];
			} else {
				colbg.red = ~bg->color.red;
				colbg.green = ~bg->color.green;
				colbg.blue = ~bg->color.blue;
				colbg.alpha = bg->color.alpha;
				XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colbg, revbg);
				bg = revbg;
			}
		}
	}

	if (fg_needed && (base.mode & ATTR_BOLD_FAINT) == ATTR_FAINT) {
		colfg.red = fg->color.red / 2;
		colfg.green = fg->color.green / 2;
		colfg.blue = fg->color.blue / 2;
		colfg.alpha = fg->color.alpha;
		XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg, revfg);
		fg = revfg;
	}

	if (selectionstyle && (base.mode & ATTR_REVERSE)) {
		if (selectionstyle == 1) {
			temp = fg;
			fg = bg;
			bg = temp;
		} else {
			if (bg_needed)
				bg = &dc.col[selectionbg];
			if (fg_needed && selectionstyle != 2)
				fg = &dc.col[selectionfg];
		}
	}

	if (fg_needed) {
		if ((base.mode & ATTR_BLINK && win.mode & MODE_BLINK) ||
		    (base.mode & ATTR_INVISIBLE))
			fg = bg;
	}

	if (invertcolors) {
		if (fg_needed) {
			*revfg = invertedcolor(fg);
			fg = revfg;
		}
		if (bg_needed) {
			*revbg = invertedcolor(bg);
			bg = revbg;
		}
	}

#ifdef ALPHA_ALL
	if (bg_needed)
		xsetalpha(bg);
#endif

	if (fgp) *fgp = fg;
	if (bgp) *bgp = bg;
}

static void
xbatch_flush_bg_bucket(int bi)
{
	if (xbg_cnt[bi] <= 0)
		return;
	if (xbg_pic != None) {
		XRenderFillRectangles(xw.dpy, PictOpSrc, xbg_pic, &xbg_col[bi].color,
		                      xbg_rects[bi], xbg_cnt[bi]);
	} else {
		for (int k = 0; k < xbg_cnt[bi]; k++)
			XftDrawRect(xw.draw, &xbg_col[bi], xbg_rects[bi][k].x, xbg_rects[bi][k].y,
			            xbg_rects[bi][k].width, xbg_rects[bi][k].height);
	}
	xbg_cnt[bi] = 0;
}

static void
xbatch_flush_bg(void)
{
	for (int bi = 0; bi < xnbg_buckets; bi++)
		xbatch_flush_bg_bucket(bi);
	xnbg_buckets = 0;
}

static void
xbatch_flush_fg(void)
{
	/* Always ensure queued backgrounds are painted underneath before any foreground. */
	xbatch_flush_bg();
	for (int fi = 0; fi < xnfg_buckets; fi++) {
		if (xfg_cnt[fi] > 0) {
			XftDrawGlyphFontSpec(xw.draw, &xfg_col[fi], xfg_specs[fi], xfg_cnt[fi]);
			xfg_cnt[fi] = 0;
			xfg_wire[fi] = 0;
		}
	}
	xnfg_buckets = 0;
	for (int bi = 0; bi < xnbox_buckets; bi++) {
		if (xbox_cnt[bi] > 0) {
			/* Group box specs by row y (drawboxes_at takes a single y) */
			int start = 0;
			while (start < xbox_cnt[bi]) {
				int end = start + 1;
				short sy = xbox_winy[bi][start];
				while (end < xbox_cnt[bi] && xbox_winy[bi][end] == sy)
					end++;
				drawboxes_at(sy, win.cw, win.ch,
				             &xbox_col[bi], NULL, &xbox_specs[bi][start], end - start);
				start = end;
			}
			xbox_cnt[bi] = 0;
		}
	}
	xnbox_buckets = 0;
	for (int di = 0; di < xndeco; di++) {
		XftDrawRect(xw.draw, &xdeco_runs[di].col,
		            xdeco_runs[di].x, xdeco_runs[di].y, xdeco_runs[di].w, 1);
	}
	xndeco = 0;
}

static void
xbatch_flush_all(void)
{
	xbatch_flush_fg();
}

/* Accurate X11 wire-byte estimate for the currently queued batch. */
static size_t
xbatch_est_bytes(void)
{
	size_t est = 0;
	for (int bi = 0; bi < xnbg_buckets; bi++)
		if (xbg_cnt[bi] > 0)
			est += 20 + (size_t)xbg_cnt[bi] * 8;
	for (int fi = 0; fi < xnfg_buckets; fi++)
		if (xfg_cnt[fi] > 0)
			est += 40 + xfg_wire[fi];
	for (int bi = 0; bi < xnbox_buckets; bi++)
		if (xbox_cnt[bi] > 0)
			est += 40 + (size_t)xbox_cnt[bi] * 24;
	est += (size_t)xndeco * 28;
	return est;
}

/*
 * In direct-to-window ETX mode, align libX11 flushes to complete (bg + fg)
 * strips so a 16 KB libX11 auto-flush never splits a strip between
 * XRenderFillRectangles (blank bg) and XftDrawGlyphFontSpec (text).
 */
static void
xdirect_flush_strip(void)
{
	size_t est = xbatch_est_bytes();
	if (est == 0)
		return;
	if (xdirect_buf_est > 0 && xdirect_buf_est + est > 11000) {
		XFlush(xw.dpy);
		xdirect_buf_est = 0;
	}
	xbatch_flush_all();
	xdirect_buf_est += est;
}

/* Return 0-pixel space glyph ID for `xf` if its advance equals win.cw, else 0. */
static inline FT_UInt
xbridge_space_glyph(const XftFont *xf)
{
	if (xf == dc.font.match)
		return (dc.font.space_xoff == win.cw) ? dc.font.space_glyph : 0;
	if (xf == dc.bfont.match)
		return (dc.bfont.space_xoff == win.cw) ? dc.bfont.space_glyph : 0;
	if (xf == dc.ifont.match)
		return (dc.ifont.space_xoff == win.cw) ? dc.ifont.space_glyph : 0;
	if (xf == dc.ibfont.match)
		return (dc.ibfont.space_xoff == win.cw) ? dc.ibfont.space_glyph : 0;
	return 0;
}

void
xdrawline(Line line, int x1, int y1, int x2)
{
	int x, numspecs, specidx;
	int winy = win.vborderpx + y1 * win.ch;
	int winx1 = win.hborderpx + x1 * win.cw;
	int winx2 = win.hborderpx + x2 * win.cw;
	XftGlyphFontSpec *specs = xw.specbuf;
	int local_batch = !xbatch_active;

	if (local_batch) {
		xbg_pic = XftDrawPicture(xw.draw);
		xnbg_buckets = 0;
		xnfg_buckets = 0;
		xnbox_buckets = 0;
		xndeco = 0;
	}

	numspecs = xmakeglyphfontspecs(specs, &line[x1], x2 - x1, x1, y1);

	/* Pass 1: clean borders once and fill backgrounds merged by effective bg. */
	if (x1 == 0 && win.hborderpx > 0) {
		xclear(0, (y1 == 0) ? 0 : winy, win.hborderpx,
		       (winy + win.ch >= win.vborderpx + win.th) ? win.h : (winy + win.ch));
	}
	if (winx2 >= win.hborderpx + win.tw && win.hborderpx > 0) {
		xclear(winx2, (y1 == 0) ? 0 : winy, win.w,
		       (winy + win.ch >= win.vborderpx + win.th) ? win.h : (winy + win.ch));
	}
	if (y1 == 0 && win.vborderpx > 0)
		xclear(winx1, 0, winx2, win.vborderpx);
	if (winy + win.ch >= win.vborderpx + win.th && win.vborderpx > 0)
		xclear(winx1, winy + win.ch, winx2, win.h);

	{
		Color *run_bg = NULL, run_bg_val;
		Color truefg, truebg, revfg, revbg;
		int run_x = x1, run_cols = 0;
		uint32_t fast_bg = 0;
		int fast_valid = 0;

#define EMIT_BG_RUN(rx, rcols, rcolp) do { \
	int _px = win.hborderpx + (rx) * win.cw; \
	int _pw = (rcols) * win.cw; \
	int _bi; \
	for (_bi = 0; _bi < xnbg_buckets; _bi++) { \
		if (xcoloreq(&xbg_col[_bi], (rcolp))) \
			break; \
	} \
	if (_bi == xnbg_buckets) { \
		if (xnbg_buckets < MAX_BG_BUCKETS) { \
			xbg_col[_bi] = *(rcolp); \
			xbg_cnt[_bi] = 0; \
			xnbg_buckets++; \
		} else { \
			/* Evict fullest bg bucket to keep batching without ordering hazards */ \
			int _best = 0; \
			for (int _k = 1; _k < MAX_BG_BUCKETS; _k++) \
				if (xbg_cnt[_k] > xbg_cnt[_best]) _best = _k; \
			xbatch_flush_bg_bucket(_best); \
			_bi = _best; \
			xbg_col[_bi] = *(rcolp); \
			xbg_cnt[_bi] = 0; \
		} \
	} \
	if (xbg_cnt[_bi] >= BG_RECT_BATCH) \
		xbatch_flush_bg_bucket(_bi); \
	xbg_rects[_bi][xbg_cnt[_bi]].x = (short)_px; \
	xbg_rects[_bi][xbg_cnt[_bi]].y = (short)winy; \
	xbg_rects[_bi][xbg_cnt[_bi]].width = (unsigned short)_pw; \
	xbg_rects[_bi][xbg_cnt[_bi]].height = (unsigned short)win.ch; \
	xbg_cnt[_bi]++; \
} while (0)

		for (x = x1; x < x2; x++) {
			Glyph g = line[x];
			int cw_cells;
			if (g.mode == ATTR_WDUMMY)
				continue;
			cw_cells = (g.mode & ATTR_WIDE) ? 2 : 1;
			if (selected(x, y1))
				g.mode ^= ATTR_REVERSE;
			if (run_cols > 0 && fast_valid && !(g.mode & ATTR_REVERSE) && g.bg == fast_bg) {
				run_cols += cw_cells;
				continue;
			}
			Color *cbg;
			xresolvecolors(g, 0, 1, NULL, &cbg, &truefg, &truebg, &revfg, &revbg);
			if (run_cols > 0 && xcoloreq(run_bg, cbg)) {
				run_cols += cw_cells;
			} else {
				if (run_cols > 0)
					EMIT_BG_RUN(run_x, run_cols, run_bg);
				run_x = x;
				run_cols = cw_cells;
				run_bg_val = *cbg;
				run_bg = &run_bg_val;
			}
			if (!(g.mode & ATTR_REVERSE)) {
				fast_bg = g.bg;
				fast_valid = 1;
			} else {
				fast_valid = 0;
			}
		}
		if (run_cols > 0)
			EMIT_BG_RUN(run_x, run_cols, run_bg);
#undef EMIT_BG_RUN
	}

	/*
	 * Pass 2: accumulate foreground text glyphs, box-drawing cells, and
	 * underline/strikethrough runs into frame-wide color buckets.
	 */
	{
		Color truefg, truebg, revfg, revbg;
		Glyph prev_g = {0};
		Color prev_fg_val, prev_bg_val;
		int prev_g_valid = 0, prev_g_had_bg = 0;
		int ul_start = -1, st_start = -1;
		Color ul_col, st_col;
		int ul_y = winy + win.cyo + dc.font.ascent * chscale + 1;
		int st_y = winy + win.cyo + 2 * dc.font.ascent * chscale / 3;

#define EMIT_DECO(dx, dy, dw, dcol) do { \
	if (xndeco >= MAX_DECO_RUNS) \
		xbatch_flush_fg(); \
	xdeco_runs[xndeco].x = (dx); \
	xdeco_runs[xndeco].y = (dy); \
	xdeco_runs[xndeco].w = (dw); \
	xdeco_runs[xndeco].col = (dcol); \
	xndeco++; \
} while (0)

		for (x = x1, specidx = 0; x < x2 && specidx < numspecs; x++) {
			Glyph g = line[x];
			int cw_cells;
			Color *cfg, *cbg;
			int is_box;

			if (g.mode == ATTR_WDUMMY)
				continue;
			cw_cells = (g.mode & ATTR_WIDE) ? 2 : 1;
			if (selected(x, y1))
				g.mode ^= ATTR_REVERSE;
			is_box = (g.mode & ATTR_BOXDRAW) != 0;

			/* Skip spaces that have neither underline nor strikethrough. */
			if (!is_box && !specs[specidx].font &&
			    !(g.mode & (ATTR_UNDERLINE | ATTR_STRUCK))) {
				if (ul_start >= 0) {
					EMIT_DECO(win.hborderpx + ul_start * win.cw, ul_y, (x - ul_start) * win.cw, ul_col);
					ul_start = -1;
				}
				if (st_start >= 0) {
					EMIT_DECO(win.hborderpx + st_start * win.cw, st_y, (x - st_start) * win.cw, st_col);
					st_start = -1;
				}
				specidx++;
				continue;
			}

			/* Shades (U+2591..U+2593, bit 1<<14 in boxdraw_data.h) need bg too. */
			int need_bg_here = is_box && ((specs[specidx].glyph & (1 << 14)) != 0);
			if (prev_g_valid && !ATTRCMP(g, prev_g) && (!need_bg_here || prev_g_had_bg)) {
				cfg = &prev_fg_val;
				cbg = &prev_bg_val;
			} else {
				xresolvecolors(g, 1, need_bg_here, &cfg, &cbg, &truefg, &truebg, &revfg, &revbg);
				prev_g = g;
				prev_fg_val = *cfg;
				if (need_bg_here && cbg) {
					prev_bg_val = *cbg;
					prev_g_had_bg = 1;
				} else {
					prev_g_had_bg = 0;
				}
				prev_g_valid = 1;
			}

			if (is_box) {
				if (need_bg_here) {
					xbatch_flush_bg();
					drawboxes_at(winy, cw_cells * win.cw, win.ch, cfg, cbg,
					             &specs[specidx], 1);
				} else {
					int bi;
					for (bi = 0; bi < xnbox_buckets; bi++) {
						if (xcoloreq(&xbox_col[bi], cfg))
							break;
					}
					if (bi == xnbox_buckets) {
						if (xnbox_buckets < MAX_BOX_BUCKETS) {
							xbox_col[bi] = *cfg;
							xbox_cnt[bi] = 0;
							xnbox_buckets++;
						} else {
							xbatch_flush_fg();
							bi = 0;
							xbox_col[bi] = *cfg;
							xbox_cnt[bi] = 0;
							xnbox_buckets = 1;
						}
					}
					if (xbox_cnt[bi] >= BOX_SPEC_BATCH) {
						xbatch_flush_fg();
						bi = 0;
						xbox_col[bi] = *cfg;
						xbox_cnt[bi] = 0;
						xnbox_buckets = 1;
					}
					xbox_winy[bi][xbox_cnt[bi]] = (short)winy;
					xbox_specs[bi][xbox_cnt[bi]++] = specs[specidx];
				}
			} else if (specs[specidx].font) {
				int fi;
				for (fi = 0; fi < xnfg_buckets; fi++) {
					if (xcoloreq(&xfg_col[fi], cfg))
						break;
				}
				if (fi == xnfg_buckets) {
					if (xnfg_buckets < MAX_FG_BUCKETS) {
						xfg_col[fi] = *cfg;
						xfg_cnt[fi] = 0;
						xfg_wire[fi] = 0;
						xnfg_buckets++;
					} else {
						/* Flush backgrounds first, then evict fullest fg bucket */
						xbatch_flush_bg();
						int best = 0;
						for (int k = 1; k < MAX_FG_BUCKETS; k++)
							if (xfg_cnt[k] > xfg_cnt[best]) best = k;
						if (xfg_cnt[best] > 0)
							XftDrawGlyphFontSpec(xw.draw, &xfg_col[best],
							                     xfg_specs[best], xfg_cnt[best]);
						fi = best;
						xfg_col[fi] = *cfg;
						xfg_cnt[fi] = 0;
						xfg_wire[fi] = 0;
					}
				}
				if (xfg_cnt[fi] >= FG_SPEC_BATCH) {
					xbatch_flush_bg();
					XftDrawGlyphFontSpec(xw.draw, &xfg_col[fi], xfg_specs[fi], xfg_cnt[fi]);
					xfg_cnt[fi] = 0;
					xfg_wire[fi] = 0;
				}
				if (xfg_cnt[fi] > 0) {
					const XftGlyphFontSpec *prev = &xfg_specs[fi][xfg_cnt[fi] - 1];
					int dx = specs[specidx].x - prev->x;
					if (prev->font == specs[specidx].font &&
					    prev->y == specs[specidx].y &&
					    dx == win.cw) {
						xfg_wire[fi] += (specs[specidx].glyph < 256) ? 1 : 2;
					} else if (xdirect &&
					           prev->font == specs[specidx].font &&
					           prev->y == specs[specidx].y &&
					           prev->glyph < 256 && specs[specidx].glyph < 256 &&
					           dx > win.cw && dx <= 9 * win.cw &&
					           (dx % win.cw) == 0) {
						/*
						 * Bridge 1..8 cell gaps with 0x0 space glyphs (1 B/cell)
						 * instead of starting a new 8-12 B xGlyphElt header in
						 * XRenderCompositeGlyphs8. Draws 0 pixels, so it safely
						 * bridges across both spaces and cells of other fg colors.
						 */
						int gap = (dx / win.cw) - 1;
						FT_UInt spg = xbridge_space_glyph(specs[specidx].font);
						if (spg && xfg_cnt[fi] + gap + 1 <= FG_SPEC_BATCH) {
							short px = prev->x;
							short py = prev->y;
							for (int k = 1; k <= gap; k++) {
								xfg_specs[fi][xfg_cnt[fi]].font = specs[specidx].font;
								xfg_specs[fi][xfg_cnt[fi]].glyph = spg;
								xfg_specs[fi][xfg_cnt[fi]].x = (short)(px + k * win.cw);
								xfg_specs[fi][xfg_cnt[fi]].y = py;
								xfg_cnt[fi]++;
							}
							xfg_wire[fi] += (size_t)gap + 1;
						} else {
							xfg_wire[fi] += 12;
						}
					} else {
						xfg_wire[fi] += 12;
					}
				} else {
					xfg_wire[fi] += 12;
				}
				xfg_specs[fi][xfg_cnt[fi]++] = specs[specidx];
			}

			if (g.mode & ATTR_UNDERLINE) {
				if (ul_start >= 0 && !xcoloreq(&ul_col, cfg)) {
					EMIT_DECO(win.hborderpx + ul_start * win.cw, ul_y, (x - ul_start) * win.cw, ul_col);
					ul_start = -1;
				}
				if (ul_start < 0) {
					ul_start = x;
					ul_col = *cfg;
				}
			} else if (ul_start >= 0) {
				EMIT_DECO(win.hborderpx + ul_start * win.cw, ul_y, (x - ul_start) * win.cw, ul_col);
				ul_start = -1;
			}

			if (g.mode & ATTR_STRUCK) {
				if (st_start >= 0 && !xcoloreq(&st_col, cfg)) {
					EMIT_DECO(win.hborderpx + st_start * win.cw, st_y, (x - st_start) * win.cw, st_col);
					st_start = -1;
				}
				if (st_start < 0) {
					st_start = x;
					st_col = *cfg;
				}
			} else if (st_start >= 0) {
				EMIT_DECO(win.hborderpx + st_start * win.cw, st_y, (x - st_start) * win.cw, st_col);
				st_start = -1;
			}

			specidx++;
		}

		if (ul_start >= 0)
			EMIT_DECO(win.hborderpx + ul_start * win.cw, ul_y, (x2 - ul_start) * win.cw, ul_col);
		if (st_start >= 0)
			EMIT_DECO(win.hborderpx + st_start * win.cw, st_y, (x2 - st_start) * win.cw, st_col);
#undef EMIT_DECO
		if (local_batch)
			xbatch_flush_all();
	}
}

void
xfinishdraw(void)
{
	if (xdirect)
		return;
	if (dirty_x1 < dirty_x2 && dirty_y1 < dirty_y2) {
		/*
		 * On ETX, every XCopyArea(xw.buf -> xw.win) triggers etxproxy's
		 * ShadowCopyAny -> synchronous FlushShadow + OpenMP WalkPrevDrawHash,
		 * so always coalesce into 1 bounding-box copy. On local X11, keep
		 * multi-rect blits only for sparse (<= 4 rect) updates.
		 */
		if (!is_etx && !dirty_overflow && ndirty_rects > 0 && ndirty_rects <= 4) {
			int i;
			for (i = 0; i < ndirty_rects; i++) {
				XCopyArea(xw.dpy, xw.buf, xw.win, dc.gc,
				          dirty_rects[i].x, dirty_rects[i].y,
				          dirty_rects[i].width, dirty_rects[i].height,
				          dirty_rects[i].x, dirty_rects[i].y);
			}
		} else {
			XCopyArea(xw.dpy, xw.buf, xw.win, dc.gc,
			          dirty_x1, dirty_y1,
			          dirty_x2 - dirty_x1, dirty_y2 - dirty_y1,
			          dirty_x1, dirty_y1);
		}
	}
}

void
xximspot(int x, int y)
{
	if (xw.ime.xic == NULL)
		return;

	xw.ime.spot.x = borderpx + x * win.cw;
	xw.ime.spot.y = borderpx + (y + 1) * win.ch;

	XSetICValues(xw.ime.xic, XNPreeditAttributes, xw.ime.spotlist, NULL);
}

void
expose(XEvent *ev)
{
	XExposeEvent *e = &ev->xexpose;

	if (xdirect) {
		if (drawn_valid && drawn_row_valid && e->width > 0 && e->height > 0) {
			int y1 = (e->y - win.vborderpx) / win.ch;
			int y2 = (e->y + e->height - 1 - win.vborderpx) / win.ch;
			LIMIT(y1, 0, drawn_row - 1);
			LIMIT(y2, 0, drawn_row - 1);
			for (int y = y1; y <= y2; y++)
				drawn_row_valid[y] = 0;
			if (prev_cursor_drawn && prev_cy >= y1 && prev_cy <= y2)
				prev_cursor_drawn = 0;
			if (win.hborderpx > 0 || win.vborderpx > 0)
				xclear(e->x, e->y, e->x + e->width, e->y + e->height);
		} else {
			xinvalidatedrawn();
		}
		if (e->count == 0) {
			redraw();
			XFlush(xw.dpy);
			xdirect_buf_est = 0;
		}
		return;
	}

	if (drawn_valid) {
		/* xw.buf already holds the current frame: blit the exposed area
		 * directly instead of re-rasterizing the entire screen. */
		if (e->width > 0 && e->height > 0) {
			XCopyArea(xw.dpy, xw.buf, xw.win, dc.gc,
			          e->x, e->y, e->width, e->height, e->x, e->y);
		} else if (e->count == 0) {
			XCopyArea(xw.dpy, xw.buf, xw.win, dc.gc,
			          0, 0, win.w, win.h, 0, 0);
		}
		if (e->count == 0)
			XFlush(xw.dpy);
	} else if (e->count == 0) {
		redraw();
		XFlush(xw.dpy);
	}
}

void
visibility(XEvent *ev)
{
	XVisibilityEvent *e = &ev->xvisibility;
	int was_visible = IS_SET(MODE_VISIBLE);

	MODBIT(win.mode, e->state != VisibilityFullyObscured, MODE_VISIBLE);
	if (!was_visible && IS_SET(MODE_VISIBLE)) {
		xresetdrawthrottle = 1;
		xneeddraw = 1;
	}
}

void
unmap(XEvent *ev)
{
	win.mode &= ~MODE_VISIBLE;
}

void
xsetpointermotion(int set)
{
	MODBIT(xw.attrs.event_mask, set, PointerMotionMask);
	XChangeWindowAttributes(xw.dpy, xw.win, CWEventMask, &xw.attrs);
}

void
xsetmode(int set, unsigned int flags)
{
	int mode = win.mode;
	MODBIT(win.mode, set, flags);
	if ((win.mode & MODE_REVERSE) != (mode & MODE_REVERSE)) {
		xinvalidatedrawn();
		redraw();
	}
}

int
xsetcursor(int cursor)
{
	if (!BETWEEN(cursor, 0, 7)) /* 7: st extension */
		return 1;
	if (win.cursor != cursor) {
		win.cursor = cursor;
		cursor_dirty = 1;
		xneeddraw = 1;
	}
	return 0;
}

void
xseturgency(int add)
{
	XWMHints *h;

	add = !!add;
	if (add == win_urgent)
		return;
	win_urgent = add;

	h = XGetWMHints(xw.dpy, xw.win);
	if (!h)
		return;

	MODBIT(h->flags, add, XUrgencyHint);
	XSetWMHints(xw.dpy, xw.win, h);
	XFree(h);
}

void
xbell(void)
{
	if (!(IS_SET(MODE_FOCUSED)))
		xseturgency(1);
	if (bellvolume)
		XkbBell(xw.dpy, xw.win, bellvolume, (Atom)NULL);
}

void
focus(XEvent *ev)
{
	XFocusChangeEvent *e = &ev->xfocus;

	if (e->mode == NotifyGrab)
		return;

	if (ev->type == FocusIn) {
		if (xw.ime.xic)
			XSetICFocus(xw.ime.xic);
		win.mode |= MODE_FOCUSED;
		xseturgency(0);
		if (IS_SET(MODE_FOCUS))
			ttywrite("\033[I", 3, 0);
        if (!focused) {
            focused = true;
            xresetdrawthrottle = 1;
            if (xw.depth == 32 && alpha != alpha_unfocused) {
                xloadalpha();
                xinvalidatedrawn();
                redraw();
            } else {
                cursor_dirty = 1;
                xneeddraw = 1;
            }
        }
	} else {
		if (xw.ime.xic)
			XUnsetICFocus(xw.ime.xic);
		win.mode &= ~MODE_FOCUSED;
		if (IS_SET(MODE_FOCUS))
			ttywrite("\033[O", 3, 0);
        if (focused) {
            focused = false;
            if (xw.depth == 32 && alpha != alpha_unfocused) {
                xloadalpha();
                xinvalidatedrawn();
                redraw();
            } else {
                cursor_dirty = 1;
                xneeddraw = 1;
            }
        }
	}
}

int
match(uint mask, uint state)
{
	return mask == XK_ANY_MOD || mask == (state & ~ignoremod);
}

char*
kmap(KeySym k, uint state)
{
	Key *kp;
	int i;

	/* Check for mapped keys out of X11 function keys. */
	for (i = 0; i < LEN(mappedkeys); i++) {
		if (mappedkeys[i] == k)
			break;
	}
	if (i == LEN(mappedkeys)) {
		if ((k & 0xFFFF) < 0xFD00)
			return NULL;
	}

	for (kp = key; kp < key + LEN(key); kp++) {
		if (kp->k != k)
			continue;

		if (!match(kp->mask, state))
			continue;

		if (IS_SET(MODE_APPKEYPAD) ? kp->appkey < 0 : kp->appkey > 0)
			continue;
		if (IS_SET(MODE_NUMLOCK) && kp->appkey == 2)
			continue;

		if (IS_SET(MODE_APPCURSOR) ? kp->appcursor < 0 : kp->appcursor > 0)
			continue;

		return kp->s;
	}

	return NULL;
}

void
kpress(XEvent *ev)
{
	XKeyEvent *e = &ev->xkey;
	KeySym ksym = NoSymbol;
	char buf[64], *customkey;
	int len;
	Rune c;
	Status status;
	Shortcut *bp;

	if (IS_SET(MODE_KBDLOCK))
		return;

	if (xw.ime.xic) {
		len = XmbLookupString(xw.ime.xic, e, buf, sizeof buf, &ksym, &status);
		if (status == XBufferOverflow)
			return;
	} else {
		len = XLookupString(e, buf, sizeof buf, &ksym, NULL);
	}
	/* 1. shortcuts */
	for (bp = shortcuts; bp < shortcuts + LEN(shortcuts); bp++) {
		if (ksym == bp->keysym && match(bp->mod, e->state)) {
			if (bp -> func != autocomplete)
				autocomplete ((const Arg []) { ACMPL_DEACTIVATE });
			bp->func(&(bp->arg));
			xneeddraw = 1;
			return;
		}
	}

	if (!(
		len == 0 &&
		e -> state & ~ignoremod		// ACMPL_ISSUE: I'm not sure that this is the right way
				| ACMPL_MOD == ACMPL_MOD
	))
		autocomplete ((const Arg []) { ACMPL_DEACTIVATE });

	/* 2. custom keys from config.h */
	if ((customkey = kmap(ksym, e->state))) {
		kscrolldown(&((Arg){ .i = INT_MAX }));
		ttywrite(customkey, strlen(customkey), 1);
		xneeddraw = 1;
		return;
	}

	/* 3. composed string from input method */
	if (len == 0)
		return;
	if (len == 1 && e->state & Mod1Mask) {
		if (IS_SET(MODE_8BIT)) {
			if (*buf < 0177) {
				c = *buf | 0x80;
				len = utf8encode(c, buf);
			}
		} else {
			buf[1] = buf[0];
			buf[0] = '\033';
			len = 2;
		}
	}
	kscrolldown(&((Arg){ .i = INT_MAX }));
	ttywrite(buf, len, 1);
	xneeddraw = 1;
}

void
cmessage(XEvent *e)
{
	/*
	 * See xembed specs
	 *  http://standards.freedesktop.org/xembed-spec/xembed-spec-latest.html
	 */
	if (e->xclient.message_type == xw.xembed && e->xclient.format == 32) {
		if (e->xclient.data.l[1] == XEMBED_FOCUS_IN) {
			win.mode |= MODE_FOCUSED;
			if (!focused) {
				focused = true;
				xresetdrawthrottle = 1;
			}
			xseturgency(0);
			cursor_dirty = 1;
			xneeddraw = 1;
		} else if (e->xclient.data.l[1] == XEMBED_FOCUS_OUT) {
			win.mode &= ~MODE_FOCUSED;
			focused = false;
			cursor_dirty = 1;
			xneeddraw = 1;
		}
	} else if (e->xclient.data.l[0] == xw.wmdeletewin) {
		ttyhangup();
		exit(0);
	}
}

void
resize(XEvent *e)
{
	if (e->xconfigure.width == win.w && e->xconfigure.height == win.h)
		return;

	cresize(e->xconfigure.width, e->xconfigure.height);
	xneeddraw = 1;
}

int tinsync(uint);
int ttyread_pending();

/*
 * Compute effective draw pacing for the current window state:
 *  - minimized/unmapped/fully obscured (!MODE_VISIBLE): hiddenmaxlatency / hiddenxfps
 *  - visible but unfocused (!focused):                  unfocusedmaxlatency / unfocusedxfps
 *  - focused & visible:                                 configured maxlatency / xfps
 */
static void
xactivepacing(double *out_maxlat, unsigned int *out_xfps)
{
	double ml = maxlatency;
	unsigned int fps = xfps;

	if (!IS_SET(MODE_VISIBLE)) {
		if (hiddenmaxlatency > ml)
			ml = hiddenmaxlatency;
		if (hiddenxfps && (!fps || hiddenxfps < fps))
			fps = hiddenxfps;
	} else if (!focused) {
		if (unfocusedmaxlatency > ml)
			ml = unfocusedmaxlatency;
		if (unfocusedxfps && (!fps || unfocusedxfps < fps))
			fps = unfocusedxfps;
	}
	*out_maxlat = ml;
	*out_xfps = fps;
}

void
run(void)
{
	XEvent ev;
	int w = win.w, h = win.h;
	fd_set rfd, wfd;
	int xfd = XConnectionNumber(xw.dpy), ttyfd, xev, drawing;
	int xbackpressure = 0;
	struct timespec seltv, *tv, now, lastblink, trigger;
	struct timespec lastdraw = {0};
	double timeout;

	/* Waiting for window mapping */
	do {
		XNextEvent(xw.dpy, &ev);
		/*
		 * This XFilterEvent call is required because of XOpenIM. It
		 * does filter out the key event and some client message for
		 * the input method too.
		 */
		if (XFilterEvent(&ev, None))
			continue;
		if (ev.type == ConfigureNotify) {
			w = ev.xconfigure.width;
			h = ev.xconfigure.height;
		}
	} while (ev.type != MapNotify);

	ttyfd = ttynew(opt_line, shell, opt_io, opt_cmd);
	cresize(w, h);

	for (timeout = -1, drawing = 0, lastblink = (struct timespec){0};;) {
		fd_set *wfds = NULL;
		FD_ZERO(&rfd);
		FD_SET(ttyfd, &rfd);
		FD_SET(xfd, &rfd);
		if (ttywrite_pending() || xbackpressure) {
			FD_ZERO(&wfd);
			if (ttywrite_pending())
				FD_SET(ttyfd, &wfd);
			if (xbackpressure)
				FD_SET(xfd, &wfd);
			wfds = &wfd;
		}

		if (XPending(xw.dpy) || ttyread_pending())
			timeout = 0;  /* existing events might not set xfd */

		seltv.tv_sec = timeout / 1E3;
		seltv.tv_nsec = 1E6 * (timeout - 1E3 * seltv.tv_sec);
		tv = timeout >= 0 ? &seltv : NULL;

		if (pselect(MAX(xfd, ttyfd)+1, &rfd, wfds, NULL, tv, NULL) < 0) {
			if (errno == EINTR)
				continue;
			die("select failed: %s\n", strerror(errno));
		}

		if (wfds && FD_ISSET(ttyfd, wfds))
			ttywrite_flush();
		if (wfds && FD_ISSET(xfd, wfds))
			xbackpressure = 0;

		int ttyin = FD_ISSET(ttyfd, &rfd) || ttyread_pending();
		if (ttyin)
			ttyread();

		xev = 0;
		xneeddraw = 0;
		while (XPending(xw.dpy)) {
			XNextEvent(xw.dpy, &ev);
			if (XFilterEvent(&ev, None))
				continue;
			if (handler[ev.type])
				(handler[ev.type])(&ev);
		}
		xev = xneeddraw;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (xresetdrawthrottle) {
			lastdraw = (struct timespec){0};
			xresetdrawthrottle = 0;
		}
		double eff_maxlat;
		unsigned int eff_xfps;
		xactivepacing(&eff_maxlat, &eff_xfps);
		/*
		 * When the entire visible window is pinned in scrollback history
		 * (term.scr >= term.row) and incoming PTY data did not shift or
		 * evict those rows, bypass draw()/XFlush()/maxlatency timers
		 * completely until an X event, scroll, or ring overflow occurs.
		 */
		int drawin = ttyin &&
		             !(!xev && !cursor_dirty && drawn_valid && tscr_pinned());

		/*
		 * To reduce flicker and tearing, when new content or event
		 * triggers drawing, we first wait a bit to ensure we got
		 * everything, and if nothing new arrives - we draw.
		 * We start with trying to wait minlatency ms. If more content
		 * arrives sooner, we retry with shorter and shorter periods,
		 * and eventually draw even without idle after maxlatency ms.
		 * Sampling "now" after ttyread()/XPending() ensures that the
		 * first batch starts the deadline cleanly (avoiding single-read
		 * draw thrashing under X11 backpressure) while subsequent batches
		 * still honor maxlatency accurately.
		 */
		/*
		 * If twrite() paused right after ESU (?2026l) so the completed
		 * frame can be presented before a queued BSU, draw now instead
		 * of looping back and immediately consuming the next BSU.
		 */
		if ((drawin || xev) && !ttyread_pending()) {
			if (!drawing) {
				trigger = now;
				drawing = 1;
			}
			timeout = (eff_maxlat - TIMEDIFF(now, trigger)) \
			          / eff_maxlat * minlatency;
			if (timeout > 0)
				continue;  /* we have time, try to find idle */
		}
		if (!drawing && !drawin && !xev && !cursor_dirty && drawn_valid &&
		    !(blinktimeout && tattrset(ATTR_BLINK))) {
			timeout = -1;
			continue;
		}

		if (tinsync(su_timeout)) {
			/*
			 * on synchronized-update draw-suspension: don't reset
			 * drawing so that we draw ASAP once we can (just after
			 * ESU). it won't be too soon because we already can
			 * draw now but we skip. we set timeout > 0 to draw on
			 * SU-timeout even without new content.
			 */
			timeout = minlatency;
			continue;
		}

		/*
		 * Draw at most xfps times per second. A frame costs the X server
		 * a copy of the whole window (plus an encode of it on remote
		 * displays such as ETX or VNC), and a fast wheel or touchpad can
		 * otherwise ask for one frame per event. This delays a frame by
		 * at most 1000/xfps ms, and only when frames come back to back.
		 */
		if (eff_xfps && TIMEDIFF(now, lastdraw) < 1000.0 / eff_xfps) {
			timeout = 1000.0 / eff_xfps - TIMEDIFF(now, lastdraw);
			continue;
		}

		/*
		 * ETX-only backpressure guard: if the X11 proxy socket buffer is
		 * full (remote WAN stall), skip this intermediate frame instead of
		 * blocking inside XFlush() so input/PTY stay live and we paint the
		 * latest state as soon as xfd becomes writable again.
		 */
		if (is_etx) {
			struct pollfd xpfd = { .fd = xfd, .events = POLLOUT };
			if (poll(&xpfd, 1, 0) == 0) {
				xbackpressure = 1;
				drawing = 1;
				timeout = minlatency;
				continue;
			}
			xbackpressure = 0;
		}

		/* idle detected or maxlatency exhausted -> draw */
		timeout = -1;
		if (blinktimeout && tattrset(ATTR_BLINK)) {
			timeout = blinktimeout - TIMEDIFF(now, lastblink);
			if (timeout <= 0) {
				if (-timeout > blinktimeout) /* start visible */
					win.mode |= MODE_BLINK;
				win.mode ^= MODE_BLINK;
				tsetdirtattr(ATTR_BLINK);
				xinvalidatedrawn();
				lastblink = now;
				timeout = blinktimeout;
			}
		}

		draw();
		XFlush(xw.dpy);
		xdirect_buf_est = 0;
		lastdraw = now;
		drawing = 0;
	}
}

void
usage(void)
{
	die("usage: %s [-aiv] [-S colorscheme] [-c class] [-f font] [-g geometry]"
	    " [-n name] [-o file]\n"
	    "          [-T title] [-t title] [-w windowid]"
	    " [[-e] command [args ...]]\n"
	    "       %s [-aiv] [-S colorscheme] [-c class] [-f font] [-g geometry]"
	    " [-n name] [-o file]\n"
	    "          [-T title] [-t title] [-w windowid] -l line"
	    " [stty_args ...]\n", argv0, argv0);
}

int
main(int argc, char *argv[])
{
	xw.l = xw.t = 0;
	xw.isfixed = False;
	xsetcursor(cursorshape);

	ARGBEGIN {
	case 'a':
		allowaltscreen = 0;
		break;
    case 'A':
        opt_alpha = EARGF(usage());
        /* set alpha value of bg color */
        if (opt_alpha)
            alpha = strtof(opt_alpha, NULL);

        break;
	case 'c':
		opt_class = EARGF(usage());
		break;
	case 'e':
		if (argc > 0)
			--argc, ++argv;
		goto run;
	case 'f':
		opt_font = EARGF(usage());
		break;
	case 'g':
		xw.gm = XParseGeometry(EARGF(usage()),
				&xw.l, &xw.t, &cols, &rows);
		break;
	case 'i':
		xw.isfixed = 1;
		break;
	case 'o':
		opt_io = EARGF(usage());
		break;
	case 'l':
		opt_line = EARGF(usage());
		break;
	case 'n':
		opt_name = EARGF(usage());
		break;
	case 't':
	case 'T':
		opt_title = EARGF(usage());
		break;
	case 'w':
		opt_embed = EARGF(usage());
		break;
	case 'v':
		die("%s " VERSION "\n", argv0);
		break;
	case 'S':
		seloptschname = EARGF(usage());
		break;
	default:
		usage();
	} ARGEND;

run:
	if (argc > 0) /* eat all remaining arguments */
		opt_cmd = argv;

	if (!opt_title)
		opt_title = (opt_line || !opt_cmd) ? "st" : opt_cmd[0];

	setlocale(LC_CTYPE, "");
	XSetLocaleModifiers("");
	cols = MAX(cols, 1);
	rows = MAX(rows, 1);
	tnew(cols, rows);
	xinit(cols, rows);
	xsetenv();
	selinit();
	run();

	return 0;
}
