# st — High-Throughput Simple Terminal

A customized fork of [st](https://git.suckless.org/st) (`0.9.2`) with scrollback, box drawing, synchronized updates, color schemes, autocomplete, and extensive performance and reliability engineering for large-output floods, wide displays, and remote X11 sessions (such as Exceed TurboX / ETX).

## Background (Previous Status Before `5b3983e`)

Originally, this fork combined common community patches (1,000,000-line lazy scrollback, mouse wheel scrollback, alpha focus highlight, `font2` fallback fonts, `boxdraw`, `appsync`, `externalpipe`, `optionscheme`, and `autocomplete`) on top of suckless `st`. While fine for light interactive use, several bottlenecks caused severe lag, multi-GB memory blowup, and occasional freezes in real workloads:

- **4 KB PTY reads & full-window repaints:** `ttyread()` read at most ~4 KB per event-loop pass, every scroll repainted every row, and `xfinishdraw()` copied the entire window pixmap even for a single changed character.
- **Multi-GB scrollback blowup & resize stalls:** Each scrollback line allocated a full-width `Glyph[]` (`16 B × cols`, e.g. 5.5 KB/line at 344 columns -> **4.4 GB RSS** on a 64 MB flood), and `tresize()` reallocated every history line (causing >1 s stalls on large scrollback).
- **Missing-glyph freezes & fontconfig churn:** Uncovered Unicode runes triggered repeated fontconfig queries (freezing `st` for ~10 s on 20 lines of missing runes), and duplicate fallback fonts were scanned repeatedly.
- **PTY write / ETX proxy freezes & stability bugs:** Blocking `ttywriteraw()` and blocking `XFlush()` on saturated remote X11 sockets could freeze the terminal; `ESC M` reverse index could expose unwritten `NULL` history slots; Alt+wheel scrollback was swallowed when apps enabled mouse reporting or Caps Lock was active.

## Key Improvements (Since `5b3983e`)

Starting from commit `5b3983e35510e2c28bd594061eeb4a42d283d92a` through `a9a5021`, `st` was profiled and redesigned across PTY ingestion, scrollback storage, X11/ETX rendering, draw pacing, and protocol handling:

### 1. PTY Reading, Parsing & Flood-Only Direct-to-History Fast Path
- **64 KB non-blocking PTY drain (`5b3983e`):** `ttyread()` drains all immediately ready PTY chunks up to 64 KB per wakeup instead of looping through `pselect()` + `XPending()` every 4 KB.
- **Register-hoisted text & escape fast paths (`5b3983e`, `434ed9f`):** `tputascii()`, UTF-8 box-drawing run fast path (`tputbox()`), inline `isboxdraw_fast()`, lazy `utf8encode()`, inline CSI/SGR integer parsing, batch-fill `REP` (`CSI Ps b`), and lightweight `csireset()` / `tclearregion()`.
- **Flood-only direct-to-history ingestion (`0aada01`):** Under normal interactive conditions (`< 4096 B` reads or `< term.row` lines), `st` runs the standard per-line path untouched. During real floods (`>= 4 KB` queued and `>= term.row` complete lines overflowing the window), `twrite_bulk()` routes scrolled-off plain ASCII, wrapped lines, SGR-styled ASCII, and no-op `\033[K` lines straight into compact scrollback history (or skips them on the alternate screen) without rotating `term.line[]`, clearing rows, or expanding to 16-byte `Glyph` structs, rendering only the final `term.row - 1` visible lines.

### 2. Compact Unified Slab Scrollback & O(rows) Resize
- **Trimmed, compact unified 256 KB byte slabs (`5b3983e`, `434ed9f`, `0aada01`):**
  - Default-attribute ASCII lines are stored as **1 byte/cell** (`HIST_ASCII` / `HIST_ASCII_WRAP`; 0 bytes for blank lines).
  - Styled ASCII lines are stored as packed 12-byte `HistRun` attribute runs + raw ASCII bytes (`HIST_SGR_ASCII`, always strictly smaller than 16-byte `Glyph[]`, typically 6–14× smaller on colored logs).
  - Non-ASCII UTF-8 / wide / box-drawing lines fall back to aligned `Glyph[]` (`HIST_GLYPH`) in the same refcounted slab allocator.
  - Lines are inflated lazily into scratch on `tvisline()` / `externalpipe()` without widening stored history.
- **O(rows) resize & lossless narrow->wide resizing (`5b3983e`):** `tresize()` never touches scrollback history, eliminating multi-second resize stalls and preserving full line widths across narrow->wide resizes.
- **Alt-screen & ring-buffer safety (`5b3983e`, `434ed9f`, `0aada01`):** Alternate-screen scrolls (`tscrollup()`/`tscrolldown()` under `MODE_ALTSCREEN`) no longer allocate or pollute primary scrollback; fixed `ESC M` reverse-index `NULL`-slot crash; clean `CSI 3 J` (`ED 3`) scrollback clearing.

### 3. Incremental X11 Rendering & Low-Latency Tab Switching
- **Shadow-buffered incremental renderer `xdrawscreen()` (`5b3983e`, `434ed9f`, `d8cab1e`):** Diffs visible rows via 64-bit hashes against a shadow buffer, shifts scrolled bands server-side via `XCopyArea` (clamped to preserve fixed headers/footers), diffs intra-row changed spans (`gap >= 16`) without repainting unchanged cells in between, batches background `XRenderFillRectangles()`, foreground `XftDrawGlyphFontSpec()`, and box-drawing segments across the entire frame by color, coalesces local `XCopyArea` blits when `ndirty_rects > 4`, and caches primary-font ASCII glyph indices.
- **ETX direct-to-window Xft, strip-aligned flushing & space-glyph bridging (`d8cab1e`, `a9a5021`):** On Exceed TurboX (`is_etx && xw.depth < 32`), renders directly onto `xw.win` with zero `XCopyArea` calls — eliminating `etxproxy`'s `ShadowCopyAny -> FlushShadow/_hclPutImage` + 4-thread OpenMP `WalkPrevDrawHash` freeze on `vim` `hlsearch`/`incsearch` and full-screen jumps while keeping full FreeType/Xft anti-aliasing and native ThinX `hclGlyphs`/`hclCompositeRects` forwarding. Flushes complete `(bg + fg)` strips aligned below `libX11`'s 16 KB buffer limit so no `writev()` splits backgrounds from glyphs (preventing blank-background flashes), and bridges `1..8`-cell horizontal gaps between 8-bit same-`fg` glyphs with 0×0-pixel space glyphs (`1 B/cell` vs. 8–12 B `xGlyphElt` headers) to cut `XRenderCompositeGlyphs8` wire bytes by ~21–32% and keep more updates below ETX's 4 KB / 50 ms shadow threshold.
- **Fallback font deduplication & negative cache (`5b3983e`, `434ed9f`):** Deduplicates `sparefonts` by font file, caches per-rune lookups, skips fontconfig entirely for runes outside `FcFontSort()` union coverage, and skips color-emoji glyphs safely on older `libXft`.
- **Fast Expose / Focus / Tab switching (`434ed9f`, `d8cab1e`):** `Expose` blits directly from `xw.buf` on local X11 (or invalidates only the exposed rows in `xdirect` mode), `FocusIn`/`FocusOut` and `XEMBED_FOCUS` on 24-bit visuals only dirty the cursor, urgency hints and X11 atoms are cached/batched, and identical window title updates are skipped.

### 4. Adaptive Refresh Pacing, Remote ETX Guard & Pinned-History Draw Bypass
- **ETX display auto-detection & backpressure guard (`5b3983e`, `72d0ccf`):** Automatically detects Exceed TurboX (`HCL_ThinX`) at startup to use `etxmaxlatency = 50` ms / `etxxfps = 30` fps (keeping `8 ms` / uncapped on local displays), and skips intermediate frames when `xfd` is not `POLLOUT`-writable instead of blocking in `XFlush()`.
- **Lower refresh rate when unfocused or minimized (`0aada01`):** Automatically lowers draw pacing to `10 fps` (`100 ms`) when unfocused (`FocusOut` / `XEMBED_FOCUS_OUT`) and `4 ticks/s` (`250 ms`, 0 visible draws) when minimized/unmapped/obscured (`!MODE_VISIBLE`), and resets the draw throttle immediately on focus or visibility regain.
- **Zero-draw bypass when scrolled back in history (`92d8896`):** When viewing pure scrollback (`term.scr >= term.row`) and incoming PTY output does not shift or evict the visible history slice, `run()` bypasses `draw()`, `XFlush()`, and `maxlatency` timers completely (verified via monotonic `hist_seq - scr` tracking, including exact-`HISTSIZE` wraps).

### 5. Non-Blocking PTY I/O, Reliability, Protocol & Mouse Fixes
- **Non-blocking queued PTY writes & safe `sigchld()` (`72d0ccf`):** Replaced blocking `ttywriteraw()` with an `O_NONBLOCK` queued write buffer driven by `run()`'s `pselect()` `wfd` set, and made `sigchld()` async-signal-safe with `sigaction()` + `waitpid(..., WNOHANG)`.
- **Modern VT query support & guards (`434ed9f`):** Implemented `DECRQM` (`?2026` sync output), `DA2`, `XTVERSION`, and `XTGETTCAP` (`RGB`/`Tc`/`Sync`/`TN`), fixed synchronized-update ESU (`?2026l`) frame presentation, guarded Kitty keyboard queries (`CSI ? u` / `> u` / `< u`) from triggering cursor save/restore, and cleanly handled `OSC 8`, `SGR 58/59`, and `SGR 4:x`.
- **Mouse scrollback (`5b3983e`):** Alt/Ctrl+wheel scroll `st`'s primary scrollback even when applications request mouse reports, translate cleanly on the alternate screen, and ignore `LockMask` (Caps Lock).

### 6. Generic In-Terminal Benchmark (`termbench`)
- Added [`termbench.c`](./termbench.c) (`af4d9d4`, `160042f`), a standalone C99 generic terminal benchmark built alongside `st` (`make termbench`) using raw `~OPOST` PTY output and `CSI 6 n` (CPR) barriers to measure isolated PTY read speed (write ingest vs. CPR-verified drain MB/s, post-write backlog tail, write-chunk scaling at `64 KB`/`4 KB`/`512 B`, per-write backpressure stall percentiles), interactive/burst latency, and streaming throughput across standard VT encodings.

## Benchmark Results

All measurements below were collected on `Linux 4.18 x86_64` under a private `Xvfb` (24-bit, 100 dpi, no Composite), taking the **median of 3 interleaved runs** comparing the pre-`5b3983e` binary (`st.prev`) against the current build (`92d8896`), alongside `urxvt` and `xterm`.

### 64 MB Bulk Flood (`344×67` wide window)

| Workload (64 MB, `344×67`) | `st` (pre-`5b3983e`) | `st` (current) | Speedup | Peak RSS (Before -> After) | Memory Reduction |
|---|---:|---:|---:|---:|---:|
| Plain ASCII (primary screen) | 3,793.9 ms (16.9 MB/s) | **390.9 ms (163.7 MB/s)** | **9.7×** | 4,433.8 MB -> **92.6 MB** | **47.9×** |
| 24-bit SGR (primary screen) | 3,031.0 ms (21.1 MB/s) | **585.5 ms (109.3 MB/s)** | **5.2×** | 2,694.2 MB -> **89.8 MB** | **30.0×** |
| Plain ASCII (alt screen) | 3,787.9 ms (16.9 MB/s) | **331.3 ms (193.2 MB/s)** | **11.4×** | 4,433.8 MB -> **30.2 MB** | **146.8×** |

### `termbench --quick` (`120×40` window, median of 3 interleaved runs)

#### PTY Read Speed (CPR-verified Drain MB/s — higher is better)

| Metric | `st` (pre-`5b3983e`) | `st` (current) | vs. Previous | `urxvt` | `xterm` |
|---|---:|---:|---:|---:|---:|
| NUL drain (no-op, 64 KB) | 104.5 | 65.5 | 0.63× | 211.1 | 67.2 |
| CSI no-op (`SGR 0`, 64 KB) | 49.4 | **103.0** | **2.09×** | 98.0 | 37.8 |
| ASCII in-place (64 KB wr) | 64.7 | **171.5** | **2.65×** | 77.9 | 5.7 |
| ASCII in-place (4 KB wr) | 57.1 | **176.3** | **3.09×** | 80.7 | 6.8 |
| ASCII in-place (512 B wr) | 51.0 | **145.7** | **2.86×** | 84.0 | 5.9 |
| ASCII scroll (alt, 64 KB) | 30.9 | **233.6** | **7.55×** | 72.7 | 3.3 |
| ASCII scroll (primary, 64 KB) | 31.9 | **179.6** | **5.64×** | 77.7 | 6.0 |

#### Streaming Throughput by Content Type (primary screen, Drain MB/s — higher is better)

| Workload | `st` (pre-`5b3983e`) | `st` (current) | vs. Previous | `urxvt` | `xterm` |
|---|---:|---:|---:|---:|---:|
| Long wrapping ASCII (240 col) | 36.1 | **153.9** | **4.27×** | 79.3 | 8.4 |
| ASCII + 16-color SGR | 37.8 | **96.9** | **2.56×** | 84.5 | 7.8 |
| ASCII + 24-bit SGR (RGB) | 39.3 | **110.3** | **2.80×** | 118.9 | 7.7 |
| Multibyte UTF-8 (2/3-byte) | 36.7 | **51.6** | **1.41×** | 74.1 | 9.0 |
| Dense CSI escapes (SGR/EL) | 26.1 | **95.5** | **3.66×** | 92.3 | 5.8 |

#### Interactive & Burst Latency (CPR p50 ms — lower is better)

| Benchmark | `st` (pre-`5b3983e`) | `st` (current) | `urxvt` | `xterm` |
|---|---:|---:|---:|---:|
| Idle CPR RTT (`CSI 6 n`) | 0.026 | **0.025** | 0.023 | 0.024 |
| 1-char edit + CPR | 0.025 | **0.023** | 0.025 | 0.024 |
| 2-corner edit `(1,1)&(C,R)` | 0.050 | **0.024** | 0.026 | 0.025 |
| 64 KB PTY burst (in-place) | 1.170 | **0.321** | 0.938 | 8.550 |
| Full-screen ASCII | 0.100 | **0.039** | 0.101 | 0.653 |
| Full-screen UTF-8 | 0.126 | **0.105** | 0.112 | 0.691 |
| Full-screen 24-bit SGR | 0.271 | **0.158** | 0.208 | 1.227 |
| Scroll region (`DECSTBM`) | 0.023 | **0.025** | 0.028 | 0.066 |
| Sync (`?2026`) full-screen SGR | 0.280 | **0.159** | 0.190 | 1.107 |

#### Adaptive Refresh Throttling & Scrollback Draw Bypass (live `Xvfb` checks)

| Scenario (200 Hz live output) | Draw Calls | Visible Draws |
|---|---:|---:|
| Focused & visible (1.0 s) | 205 | 205 |
| Unfocused / `FocusOut` (1.0 s) | 10 | 10 |
| Minimized / `UnmapNotify` (1.0 s) | 4 | 0 |
| Refocused / `FocusIn` (1.0 s) | 209 | 209 |
| Scrolled back in pure history, `term.scr >= term.row` (0.8 s) | **0** | **0** |

#### ETX X11 Wire Traffic (`344×67`, `-DETX=1`, scattered `hlsearch` × 5 + 5 full-screen jumps)

| X11 Wire Metric (`344×67`, `-DETX=1`) | Pre-ETX-fix (`fae35a2`) | Direct + Strip (`d8cab1e`) | + Space Bridging (`a9a5021`) | Total Improvement |
|---|---:|---:|---:|---:|
| `CopyArea` -> window (`vim` peak/frame) | 18 (up to 62/fr) | **0** | **0** | **eliminated** |
| `RenderFillRectangles` requests | 19,613 (550 KB) | 419 (166 KB) | **312 (164 KB)** | **62.9× fewer reqs** |
| `RenderCompositeGlyphs8` requests | 24,253 (1,442 KB) | 767 (784 KB) | **568 (531 KB)** | **42.7× fewer reqs / 2.7× less B** |
| Total X11 wire bytes | 2,053 KB | 1,011 KB | **755 KB** | **2.7× less data** |
| Text `writev()` ending BG-only | 0 | **0** | **0** | flicker-free |

## Included Base Patches

- `scrollback`, `scrollback-mouse`, `scrollback-clamp`
- `anysize`, `alpha`, `alpha-focus-highlight`, `alpha-all`, `vertcenter`
- `fix-keyboard-input`, `clipboard`, `right-click-paste`, `selection-colors`
- `desktop-entry`, `font2`, `bold-is-not-bright`, `invert`, `optionscheme`
- `external-pipe`, `external-pipe-eternal`, `boxdraw`, `appsync`, `autocomplete`
- `fix-wide-glyph-truncation`, `initial-zoom`

## Requirements & Building

Requires Xlib, Xft, Xrender, fontconfig, and freetype2 development headers.

```sh
make all              # builds both st and termbench
make clean install    # installs st, st-autocomplete, man page, and terminfo
```

To force ETX pacing on or off at build time instead of runtime `HCL_ThinX` detection, set `ETXFLAGS = -DETX=1` or `ETXFLAGS = -DETX=0` in `config.mk`.

If running `./st` without `make install`, compile the terminfo entry once:

```sh
tic -sx st.info
```

## Running `termbench`

```sh
./termbench                       # full generic suite (80 frames, 400 ms/stream)
./termbench --pty-only            # PTY read speed table only
./termbench --pty-only --mb 16    # fixed 16 MB payload per PTY read test
./termbench --quick --json        # fast run + machine-readable JSON
```

## Appearance & Configuration

- **Default font:** `DejaVu Sans Mono:size=12` (configured in `config.def.h`, with deduplicated `sparefonts[]` fallbacks for Symbols Nerd Font, Devicons, FontAwesome, Symbola, and Unifont).
- **Color schemes:** Select at startup with `-S <scheme>` (`default`, `light`, `plan9`, `monokai`, `solarized`, `nord`, `gruvbox`, `cyberpunk` in `config.def.h`).
- **Refresh pacing knobs (`config.def.h`):** `minlatency`, `maxlatency`, `xfps`, `etxmaxlatency`, `etxxfps`, `unfocusedmaxlatency`, `unfocusedxfps`, `hiddenmaxlatency`, and `hiddenxfps`.

## Credits

- Original `st` by [suckless.org](https://st.suckless.org/) (based on Aurélien Aptel's `bt`).
- Performance engineering, diagnosis, benchmarking, and test verification from `5b3983e` onward:
  - **Claude (Claude Opus 5.5)** — initial lag diagnosis & fix (`5b3983e`), incremental `xdrawscreen()` renderer, fallback font caching, and earlier optimization/reliability rounds (`434ed9f`, `af4d9d4`, `72d0ccf`).
  - **Codex (Gemini 4 Neon)** — generic PTY read-speed `termbench` redesign (`160042f`), compact unified byte-slab scrollback & flood-only direct-to-history fast path (`0aada01`), alt-screen scrollback isolation, unfocused/minimized refresh throttling, pinned-history draw bypass (`92d8896`), and ETX direct-to-window strip-aligned XRender batching & 0-pixel space-glyph bridging (`d8cab1e`, `a9a5021`).
