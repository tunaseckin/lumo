// Lumo's graphics backend for Windows and Linux. Lumo draws everything itself into
// one pixel buffer (shapes, and text from the built-in font in gfx_font.h); the system
// only shows that buffer in a window and reports keys and the mouse.
//   Windows: Win32 + GDI, system DLLs only.
//   Linux:   X11, loaded with dlopen when a window opens, so lumo still runs without a display.
// Built into lumo by `make` (see Makefile); lumo.c calls the gfx_* functions.
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx_font.h"

static uint32_t *px;   // 0xRRGGBB, top row first
static int PW, PH;     // buffer size in pixels
static double W, H, S; // window size in points, pixels per point
static bool closed;
static bool held[128], pressed_now[128], pressed_frame[128];
static bool clicked_now, clicked_frame;
static double last_time, delta;
static long frames;

// keys are tracked by a small code: the named keys get 1-8, everything else is its ASCII letter
enum { K_LEFT = 1, K_RIGHT, K_UP, K_DOWN, K_ENTER, K_ESCAPE, K_BACKSPACE, K_TAB };

static int key_code(const char *name) {
  static const char *named[] = {"left", "right", "up", "down", "enter", "escape", "backspace", "tab"};
  for (int i = 0; i < 8; i++)
    if (!strcmp(named[i], name)) return i + 1;
  if (!strcmp(name, "space")) return ' ';
  return (unsigned char)name[0] < 128 && !name[1] ? name[0] : 0;
}

static void key_event(int code, bool down) {
  if (code <= 0 || code >= 128) return;
  if (down && !held[code]) pressed_now[code] = true;
  held[code] = down;
}

/* ---------- drawing ---------- */

static void blend(int x, int y, double a, const double *c) {
  if (a <= 0) return;
  uint32_t *p = &px[y * PW + x];
  if (a >= 1) {
    *p = (uint32_t)(c[0] * 255 + 0.5) << 16 | (uint32_t)(c[1] * 255 + 0.5) << 8 | (uint32_t)(c[2] * 255 + 0.5);
    return;
  }
  double r = (*p >> 16 & 255), g = (*p >> 8 & 255), b = (*p & 255);
  r += (c[0] * 255 - r) * a;
  g += (c[1] * 255 - g) * a;
  b += (c[2] * 255 - b) * a;
  *p = (uint32_t)(r + 0.5) << 16 | (uint32_t)(g + 0.5) << 8 | (uint32_t)(b + 0.5);
}

static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// visits the pixels a shape may touch: (x0, y0)-(x1, y1) in points, clipped to the buffer
#define EACH_PIXEL(x0, y0, x1, y1)                                                              \
  for (int py = (int)fmax(floor((y0) * S), 0), py_end = (int)fmin(ceil((y1) * S), PH); py < py_end; py++) \
    for (int px_ = (int)fmax(floor((x0) * S), 0), px_end = (int)fmin(ceil((x1) * S), PW); px_ < px_end; px_++)

void gfx_clear(const double *c) {
  for (int y = 0; y < PH; y++)
    for (int x = 0; x < PW; x++) blend(x, y, c[3], c);
}

// coverage from a signed distance in pixels (negative inside): a one-pixel soft edge
#define COVER(d) clamp01(0.5 - (d))

void gfx_rect(double x, double y, double w, double h, double radius, const double *c) {
  if (w <= 0 || h <= 0) return;
  radius = fmin(fmax(radius, 0), fmin(w, h) / 2) * S;
  double cx = (x + w / 2) * S, cy = (y + h / 2) * S, hx = w / 2 * S, hy = h / 2 * S;
  EACH_PIXEL(x, y, x + w, y + h) {
    double qx = fabs(px_ + 0.5 - cx) - (hx - radius), qy = fabs(py + 0.5 - cy) - (hy - radius), a;
    if (radius > 0) {
      double ox = fmax(qx, 0), oy = fmax(qy, 0);
      a = COVER(sqrt(ox * ox + oy * oy) + fmin(fmax(qx, qy), 0) - radius);
    } else {
      a = COVER(qx) * COVER(qy); // exact for axis-aligned edges
    }
    blend(px_, py, a * c[3], c);
  }
}

void gfx_circle(double x, double y, double r, const double *c) {
  if (r <= 0) return;
  EACH_PIXEL(x - r, y - r, x + r, y + r) {
    double dx = px_ + 0.5 - x * S, dy = py + 0.5 - y * S;
    blend(px_, py, COVER(sqrt(dx * dx + dy * dy) - r * S) * c[3], c);
  }
}

void gfx_line(double x1, double y1, double x2, double y2, double width, const double *c) { // round caps
  double hw = fmax(width, 0) / 2, ax = x1 * S, ay = y1 * S, bx = x2 * S - ax, by = y2 * S - ay;
  double len2 = bx * bx + by * by;
  EACH_PIXEL(fmin(x1, x2) - hw - 1, fmin(y1, y2) - hw - 1, fmax(x1, x2) + hw + 1, fmax(y1, y2) + hw + 1) {
    double qx = px_ + 0.5 - ax, qy = py + 0.5 - ay;
    double t = len2 > 0 ? clamp01((qx * bx + qy * by) / len2) : 0;
    double dx = qx - t * bx, dy = qy - t * by;
    blend(px_, py, COVER(sqrt(dx * dx + dy * dy) - fmax(hw * S, 0.5)) * c[3], c);
  }
}

static int next_cp(const char **s) { // decodes one UTF-8 character
  const unsigned char *p = (const unsigned char *)*s;
  int cp = *p++, extra = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;
  if (extra) cp &= 0x3f >> extra;
  while (extra-- && (*p & 0xc0) == 0x80) cp = cp << 6 | (*p++ & 0x3f);
  *s = (const char *)p;
  return cp;
}

static int glyph_index(int cp) {
  for (int i = 0; i < (int)(sizeof font_glyphs / sizeof *font_glyphs); i++)
    if (font_glyphs[i].cp == cp) return i;
  return cp == '?' ? -1 : glyph_index('?');
}

double gfx_text_width(const char *s, double size) {
  double w = 0;
  while (*s) {
    int g = glyph_index(next_cp(&s));
    if (g >= 0) w += font_glyphs[g].adv;
  }
  return w * size / FONT_EM;
}

static double sdf_at(int g, double u, double v) { // bilinear sample, 0 outside the glyph's box
  int w = font_glyphs[g].w, h = font_glyphs[g].h;
  const unsigned char *d = font_sdf + font_glyphs[g].off;
  u -= 0.5;
  v -= 0.5;
  int x0 = (int)floor(u), y0 = (int)floor(v);
  double fx = u - x0, fy = v - y0, sum = 0;
  for (int k = 0; k < 4; k++) {
    int xx = x0 + (k & 1), yy = y0 + (k >> 1);
    double wt = (k & 1 ? fx : 1 - fx) * (k >> 1 ? fy : 1 - fy);
    sum += wt * (xx >= 0 && yy >= 0 && xx < w && yy < h ? d[yy * w + xx] : 0);
  }
  return sum;
}

void gfx_text(const char *s, double x, double y, double size, const double *c) { // y is the top of the line
  double k = size / FONT_EM;              // points per atlas pixel
  double edge = FONT_SPREAD / 127.0 * k * S; // pixels per unit of stored distance
  while (*s) {
    int g = glyph_index(next_cp(&s));
    if (g < 0) continue;
    double gx = x + font_glyphs[g].x * k, gy = y + font_glyphs[g].y * k;
    EACH_PIXEL(gx, gy, gx + font_glyphs[g].w * k, gy + font_glyphs[g].h * k) {
      double d = sdf_at(g, ((px_ + 0.5) / S - gx) / k, ((py + 0.5) / S - gy) / k);
      blend(px_, py, clamp01((d - 128) * edge + 0.5) * c[3], c);
    }
    x += font_glyphs[g].adv * k;
  }
}

void gfx_pixels(const unsigned char *rgba, int w, int h) { // stretched over the window, crisp pixels
  for (int y = 0; y < PH; y++) {
    const unsigned char *row = rgba + (size_t)(y * h / PH) * w * 4;
    for (int x = 0; x < PW; x++) {
      const unsigned char *p = row + (size_t)(x * w / PW) * 4;
      px[y * PW + x] = (uint32_t)p[0] << 16 | p[1] << 8 | p[2];
    }
  }
}

/* ---------- PNG screenshots (uncompressed deflate: simple, and only for tests and recordings) ---------- */

static uint32_t crc_table[256];
static uint32_t crc(uint32_t c, const unsigned char *b, size_t n) {
  if (!crc_table[1])
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t v = i;
      for (int k = 0; k < 8; k++) v = v & 1 ? 0xedb88320 ^ (v >> 1) : v >> 1;
      crc_table[i] = v;
    }
  for (size_t i = 0; i < n; i++) c = crc_table[(c ^ b[i]) & 255] ^ (c >> 8);
  return c;
}

static void put32(unsigned char *b, uint32_t v) { b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v; }

static void chunk(FILE *f, const char *type, const unsigned char *data, size_t n) {
  unsigned char head[8];
  put32(head, (uint32_t)n);
  memcpy(head + 4, type, 4);
  fwrite(head, 1, 8, f);
  fwrite(data, 1, n, f);
  uint32_t c = ~crc(crc(0xffffffff, head + 4, 4), data, n);
  put32(head, c);
  fwrite(head, 1, 4, f);
}

bool gfx_screenshot(const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  size_t row = (size_t)PW * 3 + 1, raw_n = row * PH, blocks = (raw_n + 65534) / 65535;
  unsigned char *raw = malloc(raw_n), *z = malloc(raw_n + blocks * 5 + 6);
  for (int y = 0; y < PH; y++) {
    unsigned char *r = raw + y * row;
    *r++ = 0; // filter: none
    for (int x = 0; x < PW; x++) {
      uint32_t p = px[y * PW + x];
      *r++ = p >> 16;
      *r++ = p >> 8;
      *r++ = p;
    }
  }
  size_t n = 0;
  uint32_t a = 1, b = 0;
  z[n++] = 0x78;
  z[n++] = 1;
  for (size_t i = 0; i < raw_n; i += 65535) {
    size_t len = raw_n - i < 65535 ? raw_n - i : 65535;
    z[n++] = i + len == raw_n;
    z[n++] = len & 255;
    z[n++] = len >> 8;
    z[n++] = ~len & 255;
    z[n++] = (~len >> 8) & 255;
    memcpy(z + n, raw + i, len);
    n += len;
  }
  for (size_t i = 0; i < raw_n; i++) {
    a = (a + raw[i]) % 65521;
    b = (b + a) % 65521;
  }
  put32(z + n, b << 16 | a);
  n += 4;
  unsigned char ihdr[13] = {0};
  put32(ihdr, PW);
  put32(ihdr + 4, PH);
  ihdr[8] = 8; // 8 bits per channel
  ihdr[9] = 2; // RGB
  fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
  chunk(f, "IHDR", ihdr, 13);
  chunk(f, "IDAT", z, n);
  chunk(f, "IEND", NULL, 0);
  free(raw);
  free(z);
  return fclose(f) == 0;
}

/* ---------- per-frame bookkeeping shared by both platforms ---------- */

static double now_s(void);
static void sleep_s(double s);
static void present(void);
static void poll_events(void);

bool gfx_frame(void) {
  present();
  // LUMO_SNAPSHOT=out.png LUMO_SNAPSHOT_FRAME=n saves frame n and quits (for tests and screenshots)
  // LUMO_RECORD=dir saves every second frame as dir/00001.png ... (for GIFs; time then runs at a steady 1/60 s)
  frames++;
  const char *rec = getenv("LUMO_RECORD");
  if (rec && frames % 2 == 0) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%05ld.png", rec, frames / 2);
    gfx_screenshot(path);
  }
  const char *snap = getenv("LUMO_SNAPSHOT");
  if (snap) {
    const char *at = getenv("LUMO_SNAPSHOT_FRAME");
    if (frames >= (at ? atol(at) : 2)) exit(gfx_screenshot(snap) ? 0 : 1);
  }
  double wait = last_time + 1.0 / 60 - now_s();
  if (wait > 0) sleep_s(wait);
  memset(pressed_now, 0, sizeof pressed_now);
  clicked_now = false;
  poll_events();
  memcpy(pressed_frame, pressed_now, sizeof pressed_now);
  clicked_frame = clicked_now;
  double t = now_s();
  delta = rec ? 1.0 / 60 : t - last_time;
  last_time = t;
  return !closed;
}

bool gfx_key(const char *name) { int k = key_code(name); return k && held[k]; }
bool gfx_pressed(const char *name) { int k = key_code(name); return k && pressed_frame[k]; }
double gfx_delta(void) { return delta; }

// lumo.c accepts the macOS system sound names; elsewhere they map to the system's own sounds
static int sound_kind(const char *name) { // 0 unknown, 1 plain, 2 alert, 3 bright
  static const char *names[] = {"Basso", "Funk", "Sosumi", "Frog", "Pop", "Tink", "Morse", "Bottle", "Purr", "Blow", "Submarine",
                                "Glass", "Hero", "Ping"};
  for (int i = 0; i < 14; i++)
    if (!strcmp(names[i], name)) return i < 3 ? 2 : i < 11 ? 1 : 3;
  return 0;
}

#if defined(_WIN32)
/* ---------- Windows ---------- */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

static HWND hwnd;
static HDC mem_dc;

static double now_s(void) {
  static LARGE_INTEGER freq;
  LARGE_INTEGER t;
  if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t);
  return (double)t.QuadPart / freq.QuadPart;
}

static void sleep_s(double s) { Sleep((DWORD)(s * 1000)); }

static int win_key(WPARAM vk) {
  switch (vk) {
    case VK_LEFT: return K_LEFT;
    case VK_RIGHT: return K_RIGHT;
    case VK_UP: return K_UP;
    case VK_DOWN: return K_DOWN;
    case VK_RETURN: return K_ENTER;
    case VK_ESCAPE: return K_ESCAPE;
    case VK_BACK: return K_BACKSPACE;
    case VK_TAB: return K_TAB;
    case VK_SPACE: return ' ';
  }
  UINT ch = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_CHAR) & 0x7fff;
  return ch < 128 ? tolower(ch) : 0;
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CLOSE: closed = true; return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: key_event(win_key(wp), true); return 0;
    case WM_KEYUP: case WM_SYSKEYUP: key_event(win_key(wp), false); return 0;
    case WM_LBUTTONDOWN: clicked_now = true; SetCapture(h); return 0;
    case WM_LBUTTONUP: ReleaseCapture(); return 0;
    case WM_KILLFOCUS: memset(held, 0, sizeof held); return 0; // no stuck keys
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      BitBlt(dc, 0, 0, PW, PH, mem_dc, 0, 0, SRCCOPY);
      EndPaint(h, &ps);
      return 0;
    }
  }
  return DefWindowProcW(h, msg, wp, lp);
}

bool gfx_window(int w, int h, const char *title) {
  SetProcessDPIAware(); // draw at the screen's real resolution instead of being stretched
  HDC screen = GetDC(NULL);
  S = GetDeviceCaps(screen, LOGPIXELSX) / 96.0;
  ReleaseDC(NULL, screen);
  W = w;
  H = h;
  PW = (int)(w * S + 0.5);
  PH = (int)(h * S + 0.5);
  timeBeginPeriod(1); // so Sleep can wait for the next frame precisely

  WNDCLASSW wc = {0};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleW(NULL);
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
  wc.lpszClassName = L"Lumo";
  RegisterClassW(&wc);
  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT r = {0, 0, PW, PH};
  AdjustWindowRect(&r, style, FALSE);
  wchar_t wtitle[256];
  if (!MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 256)) wcscpy(wtitle, L"Lumo");
  int ww = r.right - r.left, wh = r.bottom - r.top;
  hwnd = CreateWindowW(L"Lumo", wtitle, style, (GetSystemMetrics(SM_CXSCREEN) - ww) / 2,
                       (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh, NULL, NULL, wc.hInstance, NULL);
  if (!hwnd) return false;

  BITMAPINFO bi = {0};
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = PW;
  bi.bmiHeader.biHeight = -PH; // top row first
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  mem_dc = CreateCompatibleDC(NULL);
  HBITMAP dib = CreateDIBSection(mem_dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
  if (!dib) return false;
  SelectObject(mem_dc, dib);
  ShowWindow(hwnd, SW_SHOW);
  SetForegroundWindow(hwnd);
  last_time = now_s();
  return true;
}

static void present(void) {
  HDC dc = GetDC(hwnd);
  BitBlt(dc, 0, 0, PW, PH, mem_dc, 0, 0, SRCCOPY);
  ReleaseDC(hwnd, dc);
}

static void poll_events(void) {
  MSG m;
  while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
}

void gfx_mouse(double *x, double *y, bool *down, bool *clicked) {
  POINT p;
  GetCursorPos(&p);
  ScreenToClient(hwnd, &p);
  *x = p.x / S;
  *y = p.y / S;
  *down = (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0;
  *clicked = clicked_frame;
}

bool gfx_sound(const char *name) {
  static const UINT kinds[] = {0, MB_OK, MB_ICONHAND, MB_ICONASTERISK};
  int k = sound_kind(name);
  if (k) MessageBeep(kinds[k]);
  return k != 0;
}

#elif defined(GFX_HEADLESS)
/* ---------- no window at all: for checking the drawing code on any machine ---------- */
#include <time.h>
#include <unistd.h>
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
static void sleep_s(double s) { usleep((useconds_t)(s * 1e6)); }
bool gfx_window(int w, int h, const char *title) {
  (void)title;
  W = w, H = h, S = 1, PW = w, PH = h;
  px = calloc((size_t)PW * PH, 4);
  last_time = now_s();
  return true;
}
static void present(void) {}
static void poll_events(void) {}
void gfx_mouse(double *x, double *y, bool *down, bool *clicked) { *x = *y = 0; *down = *clicked = false; }
bool gfx_sound(const char *name) { return sound_kind(name) != 0; }

#else
/* ---------- Linux (X11) ---------- */
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>

// The few Xlib types and constants used here (Xlib's ABI has been fixed since the 1990s), so
// building lumo needs no X11 headers and lumo only needs libX11 once a window opens.
typedef struct _XDisplay Display;
typedef struct _XVisual Visual;
typedef struct _XGC *GC;
typedef struct _XImage XImage;
typedef unsigned long Window, Atom, KeySym;
typedef struct {
  int type;
  unsigned long serial;
  int send_event;
  Display *display;
  Window window, root, subwindow;
  unsigned long time;
  int x, y, x_root, y_root;
  unsigned state, keycode; // keycode is the button number in button events
  int same_screen;
} XKeyEvent;
typedef struct {
  int type;
  unsigned long serial;
  int send_event;
  Display *display;
  Window window;
  Atom message_type;
  int format;
  long l[5];
} XClientMessageEvent;
typedef union { int type; XKeyEvent xkey; XClientMessageEvent xclient; long pad[24]; } XEvent;
typedef struct { long flags; int x, y, width, height, min_width, min_height, max_width, max_height; } XSizeHints; // leading fields
enum { KeyPress = 2, KeyRelease = 3, ButtonPress = 4, FocusOut = 10, Expose = 12, ClientMessage = 33 };
enum { ZPixmap = 2, PropModeReplace = 0, PMinSize = 1 << 4, PMaxSize = 1 << 5, Button1Mask = 1 << 8 };
#define EVENT_MASK (1L << 0 | 1L << 1 | 1L << 2 | 1L << 15 | 1L << 21) // keys, button press, exposure, focus

#define XFNS                                                                                                  \
  X(Display *, XOpenDisplay, (const char *))                                                                   \
  X(int, XDefaultScreen, (Display *))                                                                          \
  X(Window, XRootWindow, (Display *, int))                                                                     \
  X(Visual *, XDefaultVisual, (Display *, int))                                                                \
  X(int, XDefaultDepth, (Display *, int))                                                                      \
  X(unsigned long, XBlackPixel, (Display *, int))                                                              \
  X(Window, XCreateSimpleWindow, (Display *, Window, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long)) \
  X(int, XStoreName, (Display *, Window, const char *))                                                        \
  X(int, XChangeProperty, (Display *, Window, Atom, Atom, int, int, const unsigned char *, int))               \
  X(Atom, XInternAtom, (Display *, const char *, int))                                                         \
  X(int, XSetWMProtocols, (Display *, Window, Atom *, int))                                                    \
  X(int, XSelectInput, (Display *, Window, long))                                                              \
  X(int, XMapWindow, (Display *, Window))                                                                      \
  X(GC, XCreateGC, (Display *, Window, unsigned long, void *))                                                 \
  X(XImage *, XCreateImage, (Display *, Visual *, unsigned, int, int, char *, unsigned, unsigned, int, int))    \
  X(int, XPutImage, (Display *, Window, GC, XImage *, int, int, int, int, unsigned, unsigned))                  \
  X(int, XPending, (Display *))                                                                                \
  X(int, XNextEvent, (Display *, XEvent *))                                                                    \
  X(KeySym, XLookupKeysym, (XKeyEvent *, int))                                                                 \
  X(int, XFlush, (Display *))                                                                                  \
  X(int, XBell, (Display *, int))                                                                              \
  X(int, XQueryPointer, (Display *, Window, Window *, Window *, int *, int *, int *, int *, unsigned *))        \
  X(int, XkbSetDetectableAutoRepeat, (Display *, int, int *))                                                  \
  X(XSizeHints *, XAllocSizeHints, (void))                                                                     \
  X(void, XSetWMNormalHints, (Display *, Window, XSizeHints *))                                                \
  X(int, XFree, (void *))
#define X(ret, f, args) static ret(*p##f) args;
XFNS
#undef X

static Display *dpy;
static Window win;
static GC gc;
static XImage *img;
static Atom wm_delete;

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void sleep_s(double s) { usleep((useconds_t)(s * 1e6)); }

static bool load_x11(void) {
  void *lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
  if (!lib) return false;
#define X(ret, f, args) if (!(*(void **)&p##f = dlsym(lib, #f))) return false;
  XFNS
#undef X
  return true;
}

bool gfx_window(int w, int h, const char *title) {
  if (!load_x11()) {
    fprintf(stderr, "graphics need X11 (libX11.so.6), which isn't installed here\n");
    return false;
  }
  if (!(dpy = pXOpenDisplay(NULL))) {
    fprintf(stderr, "can't connect to a display (is DISPLAY set? graphics need a desktop session)\n");
    return false;
  }
  int scr = pXDefaultScreen(dpy);
  if (pXDefaultDepth(dpy, scr) < 24) return false;
  W = w, H = h, S = 1, PW = w, PH = h;
  win = pXCreateSimpleWindow(dpy, pXRootWindow(dpy, scr), 0, 0, PW, PH, 0, pXBlackPixel(dpy, scr), pXBlackPixel(dpy, scr));
  pXStoreName(dpy, win, title);
  pXChangeProperty(dpy, win, pXInternAtom(dpy, "_NET_WM_NAME", 0), pXInternAtom(dpy, "UTF8_STRING", 0), 8,
                   PropModeReplace, (const unsigned char *)title, (int)strlen(title));
  XSizeHints *hints = pXAllocSizeHints(); // fixed size, like the other platforms
  hints->flags = PMinSize | PMaxSize;
  hints->min_width = hints->max_width = PW;
  hints->min_height = hints->max_height = PH;
  pXSetWMNormalHints(dpy, win, hints);
  pXFree(hints);
  wm_delete = pXInternAtom(dpy, "WM_DELETE_WINDOW", 0);
  pXSetWMProtocols(dpy, win, &wm_delete, 1);
  pXSelectInput(dpy, win, EVENT_MASK);
  pXkbSetDetectableAutoRepeat(dpy, 1, NULL); // a held key sends one release, not one per repeat
  pXMapWindow(dpy, win);
  gc = pXCreateGC(dpy, win, 0, NULL);
  px = calloc((size_t)PW * PH, 4);
  img = pXCreateImage(dpy, pXDefaultVisual(dpy, scr), 24, ZPixmap, 0, (char *)px, PW, PH, 32, 0);
  if (!img) return false;
  last_time = now_s();
  return true;
}

static void present(void) {
  pXPutImage(dpy, win, gc, img, 0, 0, 0, 0, PW, PH);
  pXFlush(dpy);
}

static int x_key(XKeyEvent *e) {
  KeySym k = pXLookupKeysym(e, 0); // keysyms below 128 are ASCII
  switch (k) {
    case 0xff51: return K_LEFT;
    case 0xff53: return K_RIGHT;
    case 0xff52: return K_UP;
    case 0xff54: return K_DOWN;
    case 0xff0d: case 0xff8d: return K_ENTER;
    case 0xff1b: return K_ESCAPE;
    case 0xff08: return K_BACKSPACE;
    case 0xff09: return K_TAB;
  }
  return k >= 32 && k < 127 ? (int)k : 0;
}

static void poll_events(void) {
  while (pXPending(dpy)) {
    XEvent e;
    pXNextEvent(dpy, &e);
    switch (e.type) {
      case KeyPress: key_event(x_key(&e.xkey), true); break;
      case KeyRelease: key_event(x_key(&e.xkey), false); break;
      case ButtonPress: if (e.xkey.keycode == 1) clicked_now = true; break;
      case FocusOut: memset(held, 0, sizeof held); break;
      case Expose: present(); break;
      case ClientMessage: if ((Atom)e.xclient.l[0] == wm_delete) closed = true; break;
    }
  }
}

void gfx_mouse(double *x, double *y, bool *down, bool *clicked) {
  Window root, child;
  int rx, ry, wx = 0, wy = 0;
  unsigned mask = 0;
  pXQueryPointer(dpy, win, &root, &child, &rx, &ry, &wx, &wy, &mask);
  *x = wx;
  *y = wy;
  *down = (mask & Button1Mask) != 0;
  *clicked = clicked_frame;
}

bool gfx_sound(const char *name) {
  int k = sound_kind(name);
  if (k) pXBell(dpy, 0);
  return k != 0;
}
#endif
