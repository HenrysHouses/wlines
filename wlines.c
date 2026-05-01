// Windows XP
#define WINVER 0x0501
#define _WIN32_WINNT 0x0501

// Unicode
#define UNICODE
#define _UNICODE
#undef _MBCS

// Use MinGW STDIO implementations
#define __USE_MINGW_ANSI_STDIO

#ifndef WLINES_VERSION
#define WLINES_VERSION "dev"
#endif

#include <shlwapi.h>
#include <windows.h>
#include <windowsx.h>
#include <stdarg.h>
#include <time.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <fcntl.h>
#include <io.h>

#define ASSERT_WIN32_RESULT(result)                                            \
  do {                                                                         \
    if (!(result)) {                                                           \
      fprintf(stderr, "Windows error %ld on line %d\n", GetLastError(),        \
              __LINE__);                                                       \
      exit(1);                                                                 \
    }                                                                          \
  } while (0);

static void debug_log(const char *fmt, ...) {
#ifdef DEBUG
  FILE *f = fopen("wlines_debug.log", "a");
  if (!f) return;
  time_t t = time(NULL);
  struct tm *tm = localtime(&t);
  if (tm) {
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] ", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
  }
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fprintf(f, "\n");
  fclose(f);
#else
  (void)fmt;
#endif
}

static LONG WINAPI unhandled_exception_handler(EXCEPTION_POINTERS *ep) {
  if (ep && ep->ExceptionRecord) {
    debug_log("Unhandled exception: code=0x%08x at addr=%p", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
  } else {
    debug_log("Unhandled exception: unknown ep");
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

#define FOREGROUND_TIMER_ID 1
#define SELECTED_INDEX_NO_RESULT ((size_t)-1)
#define LINE_HEIGHT(sz) ((sz) + 4)
#define DRAWTEXT_PARAMS (DT_NOCLIP | DT_NOPREFIX | DT_END_ELLIPSIS | DT_VCENTER | DT_SINGLELINE)
#define FONT_HMARGIN(sz) (int)(state->settings.fontSize / 6)
#define G_MARGIN 4

typedef enum {
  FM_COMPLETE,
  FM_KEYWORDS,
} filter_mode_t;

typedef struct {
  wchar_t *wndClass;
  int padding;
  wchar_t *fontName;
  wchar_t *promptText;
  int fontSize;
  filter_mode_t filterMode;
  bool caseSensitiveSearch;
  bool outputIndex;
  COLORREF bg, fg, bgSelect, fgSelect, bgEdit, fgEdit;
  int bgAlpha, fgAlpha, bgSelectAlpha, fgSelectAlpha, bgEditAlpha, fgEditAlpha;
  int lineCount;
  int selectedIndex;
  size_t width;
  bool centerWindow;
  bool horizontalLayout;
  int inputWidth;
  bool blur;
  bool border;
  COLORREF borderColor;
  int borderPadding;
  int fontQuality;
  COLORREF bgAntialias;
  bool hasBgAntialias;
  COLORREF acrylicColor;
  bool hasAcrylicColor;
  int acrylicAlpha;
  COLORREF outlineColor;
  bool hasOutline;
  bool autoOutline;
  COLORREF autoAcrylicColor;
  bool hasAutoAcrylicColor;
  int autoAcrylicAlpha;
} settings_t;

typedef struct {
  void *data;
  size_t count, cap;
} buf_t;

typedef struct {
  settings_t settings;
  HFONT font;
  HWND mainWnd, editWnd;
  WNDPROC editWndProc;
  size_t width, height;
  size_t contentWidth, contentHeight;
  size_t lineCount;
  bool hadForeground;
  size_t promptWidth;

  size_t entryCount;
  wchar_t **entries;
  wchar_t *stdinUtf16;
  buf_t textboxBuf;
  size_t searchResultCount;
  size_t *searchResults;      // index into `entries`
  size_t selectedResultIndex; // index into `searchResults`
  wchar_t *prevSearch;
} state_t;

state_t *g_state = NULL;
wchar_t **g_argv = NULL;
HDC g_bfhdc = NULL;
HBITMAP g_buffer_bitmap = NULL;
HBITMAP g_caret_bitmap = NULL;

void cleanup(void) {
  if (g_argv) LocalFree(g_argv);
  if (g_bfhdc) DeleteDC(g_bfhdc);
  if (g_buffer_bitmap) DeleteObject(g_buffer_bitmap);
  if (g_caret_bitmap) DeleteObject(g_caret_bitmap);
  if (g_state) {
    if (g_state->font) DeleteObject(g_state->font);
    if (g_state->stdinUtf16) free(g_state->stdinUtf16);
    if (g_state->entries) free(g_state->entries);
    if (g_state->searchResults) free(g_state->searchResults);
    if (g_state->textboxBuf.data) free(g_state->textboxBuf.data);
    if (g_state->prevSearch) free(g_state->prevSearch);
  }
}

void *xrealloc(void *ptr, size_t sz) {
  if (sz == 0) {
    free(ptr);
    return NULL;
  }
  ptr = realloc(ptr, sz);
  if (!ptr) {
    fprintf(stderr, "Out of memory\n");
    exit(1);
  }
  return ptr;
}

void bufEnsure(buf_t *buf, size_t sz) {
  if (buf->cap < sz) {
    if (!buf->cap) {
      buf->cap = 1024;
    }
    while (buf->cap < sz) {
      buf->cap <<= 1;
    }
    buf->data = xrealloc(buf->data, buf->cap);
  }
}

void *bufAdd(buf_t *buf, size_t sz) {
  bufEnsure(buf, buf->count + sz);
  buf->count += sz;
  return ((char *)buf->data) + (buf->count - sz);
}

void bufShrink(buf_t *buf) {
  buf->cap = buf->count;
  buf->data = xrealloc(buf->data, buf->cap);
}

void windowEventLoop(void) {
  MSG msg;
  while (GetMessageW(&msg, 0, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

void printUtf16AsUtf8(wchar_t *data) {
  const size_t len = wcslen(data);
  if (len == 0) {
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "\n", 1, &written, NULL);
    return;
  }
  const int bytecount = WideCharToMultiByte(CP_UTF8, 0, data, len, 0, 0, 0, 0);
  char *utf8 = xrealloc(0, bytecount);
  WideCharToMultiByte(CP_UTF8, 0, data, len, utf8, bytecount, 0, 0);

  HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD written;
  WriteFile(hStdOut, utf8, bytecount, &written, NULL);
  WriteFile(hStdOut, "\n", 1, &written, NULL);
  free(utf8);
}

wchar_t *getTextboxString(state_t *state) {
  const size_t length =
      CallWindowProc(state->editWndProc, state->editWnd, EM_LINELENGTH, 0, 0);
  bufEnsure(&state->textboxBuf, (length + 1) * sizeof(wchar_t));
  ((wchar_t *)state->textboxBuf.data)[0] = length; // EM_GETLINE requires this
  CallWindowProc(state->editWndProc, state->editWnd, EM_GETLINE, 0,
                 (LPARAM)state->textboxBuf.data);
  ((wchar_t *)state->textboxBuf.data)[length] = 0; // null term
  return state->textboxBuf.data;
}

void filterReduceByStr(state_t *state, const wchar_t *str) {
  if (wcslen(str) == 0) {
    return;
  }

  const size_t c = state->searchResultCount;
  state->searchResultCount = 0;
  if (state->settings.caseSensitiveSearch) {
    for (size_t i = 0; i < c; i++) {
      if (StrStrW(state->entries[state->searchResults[i]], str)) {
        state->searchResults[state->searchResultCount++] =
            state->searchResults[i];
      }
    }
  } else {
    for (size_t i = 0; i < c; i++) {
      if (StrStrIW(state->entries[state->searchResults[i]], str)) {
        state->searchResults[state->searchResultCount++] =
            state->searchResults[i];
      }
    }
  }
}

void filterReduceByKeywords(state_t *state, wchar_t *str) {
  // Iterate words and reduce results
  wchar_t *space;
  while ((space = wcschr(str, L' '))) {
    space[0] = 0;
    filterReduceByStr(state, str);
    str = space + 1;
  }
  filterReduceByStr(state, str);
}

void updateSearchResults(state_t *state) {
  wchar_t *str = getTextboxString(state);
  if (state->prevSearch && wcscmp(str, state->prevSearch) == 0) {
    return;
  }

  bool incremental = false;
  if (state->prevSearch && state->settings.filterMode == FM_COMPLETE) {
    if (wcsstr(str, state->prevSearch) == str) {
      incremental = true;
    }
  }

  if (!incremental) {
    state->searchResultCount = state->entryCount;
    for (size_t i = 0; i < state->entryCount; i++) {
      state->searchResults[i] = i;
    }
  }

  if (wcslen(str) > 0) {
    switch (state->settings.filterMode) {
    case FM_COMPLETE:
      filterReduceByStr(state, str);
      break;
    case FM_KEYWORDS:
      filterReduceByKeywords(state, str);
      break;
    }
  }

  if (state->prevSearch) free(state->prevSearch);
  state->prevSearch = _wcsdup(str);

  state->selectedResultIndex =
      state->searchResultCount > 0 ? 0 : SELECTED_INDEX_NO_RESULT;

  RedrawWindow(state->mainWnd, 0, 0, RDW_INVALIDATE);
}

LRESULT CALLBACK editWndProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  state_t *state = (state_t *)GetWindowLongPtrW(wnd, GWLP_USERDATA);
  if (!state) {
    return DefWindowProc(wnd, msg, wparam, lparam);
  }

  switch (msg) {
  case WM_SETFOCUS:;
    LRESULT res = CallWindowProc(state->editWndProc, wnd, msg, wparam, lparam);
    // Create custom caret with XOR mask to match text color
    // XOR(bg, fg) = mask. When XORed with bg, mask results in fg.
    const bool bgEditTransparent = state->settings.blur && state->settings.bgEditAlpha < 255;
    const COLORREF bg = bgEditTransparent ? state->settings.bg : state->settings.bgEdit;
    const COLORREF fg = state->settings.fgEdit;
    const COLORREF mask = (GetRValue(bg) ^ GetRValue(fg)) |
                           ((GetGValue(bg) ^ GetGValue(fg)) << 8) |
                           ((GetBValue(bg) ^ GetBValue(fg)) << 16);
    
    const int height = state->settings.fontSize - 2;
    if (g_caret_bitmap) DeleteObject(g_caret_bitmap);
    g_caret_bitmap = CreateBitmap(2, height, 1, 32, NULL);
    if (g_caret_bitmap) {
      HDC hdc = CreateCompatibleDC(NULL);
      SelectObject(hdc, g_caret_bitmap);
      HBRUSH hBrush = CreateSolidBrush(mask);
      RECT r = {0, 0, 2, height};
      FillRect(hdc, &r, hBrush);
      DeleteObject(hBrush);
      DeleteDC(hdc);
      
      CreateCaret(wnd, g_caret_bitmap, 0, 0); // Bitmap defines size
      ShowCaret(wnd);
    }
    return res;
  case WM_KILLFOCUS: // When focus is lost
    exit(1);
  case WM_CHAR:; // When a character is written
    LRESULT result = 0;
    switch (wparam) {
    case 0x01:; // Ctrl+A - Select everythinig
      const size_t length =
          CallWindowProc(state->editWndProc, wnd, EM_LINELENGTH, 0, 0);
      CallWindowProc(state->editWndProc, wnd, EM_SETSEL, 0, length);
      return 0;
    case 0x7f:; // Ctrl+Backspace - Simulate traditional behavior
      int start_sel = 0, end_sel = 0;
      CallWindowProc(state->editWndProc, wnd, EM_GETSEL, 0, (LPARAM)&end_sel);
      CallWindowProc(state->editWndProc, wnd, WM_KEYDOWN, VK_LEFT, 0);
      CallWindowProc(state->editWndProc, wnd, WM_KEYUP, VK_LEFT, 0);
      CallWindowProc(state->editWndProc, wnd, EM_GETSEL, (WPARAM)&start_sel, 0);
      CallWindowProc(state->editWndProc, wnd, EM_SETSEL, start_sel, end_sel);
      CallWindowProc(state->editWndProc, wnd, WM_CHAR, 0x08, 0); // Backspace
      break;
    case 0x09: // Tab - Autocomplete
      if (state->selectedResultIndex != SELECTED_INDEX_NO_RESULT) {
        const wchar_t *str =
            state->entries[state->searchResults[state->selectedResultIndex]];
        const size_t length = wcslen(str);
        SetWindowTextW(wnd, str);
        CallWindowProc(state->editWndProc, wnd, EM_SETSEL, length, length);
      }
      break;
    // Return - Ignore (handled in WM_KEYDOWN)
    case 0x0a:
    case 0x0d:
      return 0;
    default:
      result = CallWindowProc(state->editWndProc, wnd, msg, wparam, lparam);
    }
    updateSearchResults(state); // TODO: debounce on large entry set?
    return result;
  case WM_KEYDOWN: // When a key is pressed
    switch (wparam) {
    case VK_RETURN: // Enter - Output choice
      // If no results or shift is held: print input, else: print result
      if (state->selectedResultIndex == SELECTED_INDEX_NO_RESULT ||
          (GetKeyState(VK_SHIFT) & 0x8000)) {
        if (state->settings.outputIndex) {
          printf("-1\n");
        } else {
          printUtf16AsUtf8(getTextboxString(state));
        }
      } else {
        if (state->settings.outputIndex) {
          printf("%zu\n", state->searchResults[state->selectedResultIndex]);
        } else {
          printUtf16AsUtf8(
              state->entries[state->searchResults[state->selectedResultIndex]]);
        }
      }

      // Quit if control isn't held
      if (!(GetKeyState(VK_CONTROL) & 0x8000)) {
        exit(0);
      }
      return 0;
    case VK_ESCAPE: // Escape - Exit
      exit(1);
    case VK_UP: // Up - Previous result
      if (state->searchResultCount > 0) {
        state->selectedResultIndex =
            (state->selectedResultIndex - 1 + state->searchResultCount) %
            state->searchResultCount;
        RedrawWindow(state->mainWnd, 0, 0, RDW_INVALIDATE);
      }
      return 0;
    case VK_DOWN: // Down - Next result
      if (state->searchResultCount > 0) {
        state->selectedResultIndex =
            (state->selectedResultIndex + 1) % state->searchResultCount;
        RedrawWindow(state->mainWnd, 0, 0, RDW_INVALIDATE);
      }
      return 0;
    case VK_LEFT: // Left - Previous result (especially for horizontal layout)
      if (state->searchResultCount > 0) {
        if (state->selectedResultIndex > 0) {
          state->selectedResultIndex--;
        } else {
          state->selectedResultIndex = state->searchResultCount - 1;
        }
        RedrawWindow(state->mainWnd, 0, 0, RDW_INVALIDATE);
      }
      return 0;
    case VK_RIGHT: // Right - Next result (especially for horizontal layout)
      if (state->searchResultCount > 0) {
        state->selectedResultIndex =
            (state->selectedResultIndex + 1) % state->searchResultCount;
        RedrawWindow(state->mainWnd, 0, 0, RDW_INVALIDATE);
      }
      return 0;
    case VK_HOME: // Home - First result
      if (state->searchResultCount > 0 && state->selectedResultIndex > 0) {
        state->selectedResultIndex = 0;
        RedrawWindow(state->mainWnd, 0, 0, RDW_INVALIDATE);
      }
      return 0;
    case VK_END: // End - Last result
      if (state->searchResultCount > 0 && state->selectedResultIndex + 1 < state->searchResultCount) {
        state->selectedResultIndex = state->searchResultCount - 1;
        RedrawWindow(state->mainWnd, 0, 0, RDW_INVALIDATE);
      }
      return 0;
    case VK_PRIOR: // Page Up - Previous page (vertical layout only)
      if (!state->settings.horizontalLayout && state->searchResultCount > 0 && state->selectedResultIndex > 0) {
        const ssize_t n = (state->selectedResultIndex / state->lineCount - 1) *
                          state->lineCount;
        state->selectedResultIndex = max(0, n);
        RedrawWindow(state->mainWnd, 0, 0, RDW_INVALIDATE);
      }
      return 0;
    case VK_NEXT: // Page Down - Next page (vertical layout only)
      if (!state->settings.horizontalLayout && state->searchResultCount > 0 &&
          state->selectedResultIndex + 1 < state->searchResultCount) {
        const size_t n = (state->selectedResultIndex / state->lineCount + 1) *
                         state->lineCount;
        state->selectedResultIndex = min(state->searchResultCount - 1, n);
        RedrawWindow(state->mainWnd, 0, 0, RDW_INVALIDATE);
      }
      return 0;
    }
  }

  return CallWindowProc(state->editWndProc, wnd, msg, wparam, lparam);
}

static void drawTextOutlined(HDC hdc, const wchar_t *text, int len, RECT *rect, UINT format, COLORREF textColor, COLORREF outlineColor) {
  COLORREF oldColor = SetTextColor(hdc, outlineColor);
  int offsets[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
  for (int i = 0; i < 4; i++) {
    RECT tr = *rect;
    OffsetRect(&tr, offsets[i][0], offsets[i][1]);
    DrawTextW(hdc, text, len, &tr, format);
  }
  SetTextColor(hdc, textColor);
  DrawTextW(hdc, text, len, rect, format);
  SetTextColor(hdc, oldColor);
}

void forceForeground(HWND hwnd) {
  // Use trick from https://stackoverflow.com/a/59659421
  const DWORD foregroundThreadId =
      GetWindowThreadProcessId(GetForegroundWindow(), 0);
  const DWORD currentThreadId = GetCurrentThreadId();
  AttachThreadInput(foregroundThreadId, currentThreadId, true);
  BringWindowToTop(hwnd);
  ShowWindow(hwnd, SW_SHOW);
  SetForegroundWindow(hwnd);
  AttachThreadInput(foregroundThreadId, currentThreadId, false);
}

LRESULT CALLBACK mainWndProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  state_t *state = (state_t *)GetWindowLongPtrW(wnd, GWLP_USERDATA);
  if (!state) {
    return DefWindowProc(wnd, msg, wparam, lparam);
  }

  const int bp = state->settings.border ? state->settings.borderPadding : 0;
  const RECT wx = { .left = bp + G_MARGIN + state->settings.padding,
                    .top = bp + G_MARGIN + state->settings.padding,
                    .right = (int)state->width - (bp + G_MARGIN + state->settings.padding),
                    .bottom = (int)state->height - (bp + G_MARGIN + state->settings.padding) };
  const int contentWidth = wx.right - wx.left;
  const int contentHeight = wx.bottom - wx.top;
  const size_t page = (state->lineCount && state->searchResultCount > 0 && state->selectedResultIndex != SELECTED_INDEX_NO_RESULT) 
                      ? (state->selectedResultIndex / state->lineCount) : 0;
  const size_t pageStartI = page * state->lineCount;
  const int effectiveVerticalSpacing = (state->settings.horizontalLayout || state->lineCount == 0 || state->searchResultCount == 0) ? 0 : 4;
  const int entriesTop = wx.top + LINE_HEIGHT(state->settings.fontSize) + effectiveVerticalSpacing;

  switch (msg) {
  case WM_TIMER: // Repeating timer to make sure we're the foreground window
    if (wparam == FOREGROUND_TIMER_ID) {
      if (GetForegroundWindow() == wnd) {
        state->hadForeground = true;
      } else if (state->hadForeground) {
        exit(1);
      } else {
        forceForeground(state->mainWnd);
      }
    }
    break;
  case WM_PAINT:; // Paint window
    debug_log("WM_PAINT start, mainWnd=%p, size=%zu x %zu", state->mainWnd, state->width, state->height);
    // Begin
    PAINTSTRUCT ps = {0};
    HDC real_hdc = BeginPaint(wnd, &ps);

    // Use a draw buffer device
    if (!g_bfhdc) {
      // Create
      ASSERT_WIN32_RESULT(g_bfhdc = CreateCompatibleDC(real_hdc));
      ASSERT_WIN32_RESULT(g_buffer_bitmap = CreateCompatibleBitmap(
                              real_hdc, state->width, state->height));

      // Setup
      SelectObject(g_bfhdc, g_buffer_bitmap);
      SelectObject(g_bfhdc, state->font);
      SelectObject(g_bfhdc, GetStockObject(DC_PEN));
      SelectObject(g_bfhdc, GetStockObject(DC_BRUSH));
      SetBkMode(g_bfhdc, TRANSPARENT);
    }

    // Clear window - fill full window
    SetDCPenColor(g_bfhdc, state->settings.bg);
    SetDCBrushColor(g_bfhdc, state->settings.bg);
    Rectangle(g_bfhdc, 0, 0, state->width, state->height);

    // Draw border at edge
    if (state->settings.border) {
      SetDCPenColor(g_bfhdc, state->settings.borderColor);
      HGDIOBJ oldBrush = SelectObject(g_bfhdc, GetStockObject(NULL_BRUSH));
      Rectangle(g_bfhdc, 0, 0, state->width, state->height);
      SelectObject(g_bfhdc, oldBrush);
    }

    // Draw prompt
    if (state->settings.promptText) {
      RECT promptRect;

      if (state->settings.horizontalLayout) {
        // Horizontal: prompt on left side only
        promptRect = (RECT){
            .left = wx.left + FONT_HMARGIN(state->settings.fontSize),
            .top = wx.top,
            .right = wx.left + state->promptWidth - FONT_HMARGIN(state->settings.fontSize),
            .bottom = wx.top + LINE_HEIGHT(state->settings.fontSize),
        };
      } else {
        // Vertical: prompt takes half width
        promptRect = (RECT){
            .left = wx.left + FONT_HMARGIN(state->settings.fontSize),
            .top = wx.top,
            .right = wx.left + contentWidth/2 - FONT_HMARGIN(state->settings.fontSize),
            .bottom = wx.top + LINE_HEIGHT(state->settings.fontSize),
        };
      }

      // Use key color for prompt background in blur mode ONLY if alpha is used
      const bool promptTransparent = state->settings.blur && state->settings.bgSelectAlpha < 255;
      COLORREF promptBg = promptTransparent ? state->settings.bg : state->settings.bgSelect;
      SetDCPenColor(g_bfhdc, promptBg);
      SetDCBrushColor(g_bfhdc, promptBg);

      // Draw background for prompt width area
      Rectangle(g_bfhdc, wx.left, wx.top,
                wx.left + state->promptWidth,
                wx.top + LINE_HEIGHT(state->settings.fontSize));

      // Draw outline for prompt in blur mode if transparent
      if (promptTransparent) {
        SetDCPenColor(g_bfhdc, state->settings.bgSelect);
        HGDIOBJ oldBrush = SelectObject(g_bfhdc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(g_bfhdc, wx.left, wx.top,
                  wx.left + state->promptWidth,
                  wx.top + LINE_HEIGHT(state->settings.fontSize));
        SelectObject(g_bfhdc, oldBrush);
      }

      SetTextColor(g_bfhdc, state->settings.fgSelect);
      if (state->settings.hasOutline) {
        drawTextOutlined(g_bfhdc, state->settings.promptText, -1, &promptRect,
                         DRAWTEXT_PARAMS, state->settings.fgSelect, state->settings.outlineColor);
      } else {
        DrawTextW(g_bfhdc, state->settings.promptText, -1, &promptRect,
                  DRAWTEXT_PARAMS);
      }
    }

    // Draw input field background/outline
    const int scrollbarWidth = 10; // Reserve space for scrollbar

    // Calculate textbox bounds within content rect so both modes align
    int tbLeft = wx.left - G_MARGIN + state->promptWidth;
    size_t tbWidth;
    if (state->settings.horizontalLayout) {
      tbWidth = state->settings.inputWidth;
    } else {
      tbWidth = contentWidth - state->promptWidth - G_MARGIN - scrollbarWidth;
    }

    const bool editTransparent = state->settings.blur && state->settings.bgEditAlpha < 255;
    if (state->settings.blur) {
      // In blur mode: draw either an underline (transparent) or an outline (opaque blur)
      if (editTransparent) {
        HPEN hPen = CreatePen(PS_SOLID, 1, state->settings.bgEdit);
        HGDIOBJ oldPen = SelectObject(g_bfhdc, hPen);
        int lineY = wx.top + LINE_HEIGHT(state->settings.fontSize) - 1;
        MoveToEx(g_bfhdc, tbLeft + G_MARGIN, lineY, NULL);
        LineTo(g_bfhdc, tbLeft + G_MARGIN + tbWidth, lineY);
        SelectObject(g_bfhdc, oldPen);
        DeleteObject(hPen);
      } else {
        // Draw outline only (no fill) to match underline dimensions
        SetDCPenColor(g_bfhdc, state->settings.bgEdit);
        HGDIOBJ oldBrush = SelectObject(g_bfhdc, GetStockObject(NULL_BRUSH));
        Rectangle(g_bfhdc, tbLeft + G_MARGIN, wx.top,
                  tbLeft + G_MARGIN + tbWidth, wx.top + LINE_HEIGHT(state->settings.fontSize) - 1);
        SelectObject(g_bfhdc, oldBrush);
      }
    } else {
      // Non-blur mode: draw a filled input background using the same bounds
      SetDCPenColor(g_bfhdc, state->settings.bgEdit);
      SetDCBrushColor(g_bfhdc, state->settings.bgEdit);
      Rectangle(g_bfhdc, tbLeft + G_MARGIN, wx.top,
                tbLeft + G_MARGIN + tbWidth, wx.top + LINE_HEIGHT(state->settings.fontSize) - 1);
    }

    // Draw texts
    int vScrollShown = !state->settings.horizontalLayout && state->searchResultCount > state->lineCount;
    int textLeft = wx.left + FONT_HMARGIN(state->settings.fontSize) + (vScrollShown ? scrollbarWidth : 0);
    RECT textRect = {
        .left = textLeft,
        .top = entriesTop,
        .right = wx.right - G_MARGIN - scrollbarWidth - FONT_HMARGIN(state->settings.fontSize),
        .bottom = wx.bottom - G_MARGIN,
    };
    debug_log("layout: bp=%d entriesTop=%d contentHeight=%d textRect=[%d,%d,%d,%d] scrollbarWidth=%d", bp, entriesTop, contentHeight, textRect.left, textRect.top, textRect.right, textRect.bottom, scrollbarWidth);
    
    // Set default background color for text antialiasing
    COLORREF defaultAaBg = state->settings.hasBgAntialias ? state->settings.bgAntialias : state->settings.bg;
    SetBkColor(g_bfhdc, defaultAaBg);
    SetTextColor(g_bfhdc, state->settings.fg);

    const size_t count =
        min(state->lineCount, state->searchResultCount - pageStartI);

    if (state->settings.horizontalLayout) {
      // Horizontal layout: prompt on left, input field, then items displayed as
      // pages
      const int itemsStartX = wx.left + state->promptWidth + state->settings.inputWidth + state->settings.padding;
      const int availableWidth = wx.right - itemsStartX - state->settings.padding;
      const int itemWidth = 135; // 120px item + 15px spacing
      const int hm = FONT_HMARGIN(state->settings.fontSize);

      // Calculate how many items fit on screen
      const int itemsPerPage = availableWidth / itemWidth;
      const int itemsPerPageSafe = max(1, itemsPerPage);

      // Calculate page start index based on selected item
      size_t pageStartIdx =
          (state->selectedResultIndex / itemsPerPageSafe) * itemsPerPageSafe;

      // Render items starting from page start
      int currentX = itemsStartX;
      size_t itemIdx;
      for (itemIdx = pageStartIdx;
           itemIdx < state->searchResultCount &&
           currentX + itemWidth <= wx.right;
           itemIdx++) {
        RECT itemRect = {
            .left = currentX + hm,
            .top = wx.top + hm,
            .right = currentX + 120 - hm,
            .bottom = wx.bottom - hm,
        };

        // Set text color and color background for selected
        if (itemIdx == state->selectedResultIndex) {
          const bool selTransparent = state->settings.blur && state->settings.bgSelectAlpha < 255;
          COLORREF selBg = selTransparent ? state->settings.bg : state->settings.bgSelect;
          SetDCPenColor(g_bfhdc, selTransparent ? state->settings.fgSelect : selBg);
          SetDCBrushColor(g_bfhdc, selBg);
          Rectangle(g_bfhdc, currentX, wx.top, currentX + 120 - G_MARGIN,
                    wx.bottom - G_MARGIN);
          SetTextColor(g_bfhdc, state->settings.fgSelect);
          SetBkColor(g_bfhdc, selBg);
        } else {
          SetTextColor(g_bfhdc, state->settings.fg);
          SetBkColor(g_bfhdc, defaultAaBg);
        }

        // Draw this item
        if (state->settings.hasOutline) {
          drawTextOutlined(g_bfhdc, state->entries[state->searchResults[itemIdx]], -1,
                           &itemRect,
                           DT_END_ELLIPSIS | DT_NOPREFIX | DT_CENTER | DT_VCENTER |
                               DT_SINGLELINE, 
                           (itemIdx == state->selectedResultIndex) ? state->settings.fgSelect : state->settings.fg,
                           state->settings.outlineColor);
        } else {
          DrawTextW(g_bfhdc, state->entries[state->searchResults[itemIdx]], -1,
                    &itemRect,
                    DT_END_ELLIPSIS | DT_NOPREFIX | DT_CENTER | DT_VCENTER |
                        DT_SINGLELINE);
        }

        currentX += itemWidth;
      }

      // Scrollbar markers (non-interactable)
      // ... (rest of scrollbar logic unchanged)
      if (state->searchResultCount > (size_t)itemsPerPageSafe) {
        const int listBottom_h = textRect.bottom;
        const int barW = max(10, availableWidth * itemsPerPageSafe / (int)state->searchResultCount);
        
        // Calculate max page start for horizontal layout to avoid over-extension
        const size_t maxHPage = (state->searchResultCount - 1) / itemsPerPageSafe;
        const size_t maxPageStartIdx = maxHPage * itemsPerPageSafe;
        const int barX = itemsStartX + (maxPageStartIdx > 0 ? (availableWidth - barW) * (int)pageStartIdx / (int)maxPageStartIdx : 0);

        // Draw outline (window background color)
        SetDCPenColor(g_bfhdc, state->settings.bg);
        SetDCBrushColor(g_bfhdc, state->settings.bg);
        // Top bar outline
        Rectangle(g_bfhdc, barX - 1, wx.top - G_MARGIN / 2 - 1, barX + barW + 1, wx.top - G_MARGIN / 2 + 3);
        // Bottom bar outline (use adjusted listBottom)
        Rectangle(g_bfhdc, barX - 1, listBottom_h + G_MARGIN / 2 - 3, barX + barW + 1, listBottom_h + G_MARGIN / 2 + 1);

        // Draw thumb (selected color)
        SetDCPenColor(g_bfhdc, state->settings.bgSelect);
        SetDCBrushColor(g_bfhdc, state->settings.bgSelect);
        // Top bar
        Rectangle(g_bfhdc, barX, wx.top - G_MARGIN / 2, barX + barW, wx.top - G_MARGIN / 2 + 2);
        // Bottom bar
        Rectangle(g_bfhdc, barX, listBottom_h + G_MARGIN / 2 - 2, barX + barW, listBottom_h + G_MARGIN / 2);
      }
    } else {
      // Vertical layout (original behavior)
      for (size_t idx = pageStartI; idx < pageStartI + count; idx++) {
        // Set text color and color background
        if (idx == state->selectedResultIndex) {
          const bool selTransparent = state->settings.blur && state->settings.bgSelectAlpha < 255;
          COLORREF selBg = selTransparent ? state->settings.bg : state->settings.bgSelect;
          SetDCPenColor(g_bfhdc, selTransparent ? state->settings.fgSelect : selBg);
          SetDCBrushColor(g_bfhdc, selBg);
          
          // Offset selection rectangle to match text offset
          int selLeft = wx.left + (vScrollShown ? scrollbarWidth : 0);
          int selRight = wx.right - G_MARGIN - scrollbarWidth;
          
          Rectangle(g_bfhdc, selLeft, textRect.top, selRight,
                    textRect.top + LINE_HEIGHT(state->settings.fontSize));
          SetTextColor(g_bfhdc, state->settings.fgSelect);
          SetBkColor(g_bfhdc, selBg);
        } else {
          SetTextColor(g_bfhdc, state->settings.fg);
          SetBkColor(g_bfhdc, defaultAaBg);
        }

        // Calculate single line rect for vertical centering
        RECT lineRect = textRect;
        lineRect.bottom = lineRect.top + LINE_HEIGHT(state->settings.fontSize);

        // Draw this line
        if (state->settings.hasOutline) {
          drawTextOutlined(g_bfhdc, state->entries[state->searchResults[idx]], -1,
                           &lineRect, DRAWTEXT_PARAMS,
                           (idx == state->selectedResultIndex) ? state->settings.fgSelect : state->settings.fg,
                           state->settings.outlineColor);
        } else {
          DrawTextW(g_bfhdc, state->entries[state->searchResults[idx]], -1,
                    &lineRect, DRAWTEXT_PARAMS);
        }
        textRect.top += LINE_HEIGHT(state->settings.fontSize);
      }
      // ... (rest of scrollbar logic unchanged)
      if (state->searchResultCount > state->lineCount) {
        // Use the actual height of the items for the scrollbar track
        const int listHeight = (int)state->lineCount * LINE_HEIGHT(state->settings.fontSize);
        const int listBottom = entriesTop + listHeight;
        const int trackHeight = listHeight;
        
        // Calculate the maximum possible pageStartI to use as denominator
        const size_t maxPage = (state->searchResultCount - 1) / state->lineCount;
        const size_t maxPageStartI = maxPage * state->lineCount;
        
        const int barH = max(10, trackHeight * (int)state->lineCount / (int)state->searchResultCount);
        const int barY = entriesTop + (maxPageStartI > 0 ? (trackHeight - barH) * (int)pageStartI / (int)maxPageStartI : 0);

        debug_log("scrollbar vertical: entriesTop=%d listBottom=%d trackHeight=%d barH=%d barY=%d pageStartI=%zu maxPageStartI=%zu searchResultCount=%zu lineCount=%d", 
                  entriesTop, listBottom, trackHeight, barH, barY, pageStartI, maxPageStartI, state->searchResultCount, state->lineCount);

        // Draw outline (window background color)
        SetDCPenColor(g_bfhdc, state->settings.bg);
        SetDCBrushColor(g_bfhdc, state->settings.bg);
        // Left bar outline (at left edge of items)
        Rectangle(g_bfhdc, wx.left - 1, barY - 1, wx.left + 3, barY + barH + 1);
        // Right bar outline (at right edge of items)
        Rectangle(g_bfhdc, wx.right - G_MARGIN - 3, barY - 1, wx.right - G_MARGIN + 1, barY + barH + 1);

        // Draw thumb (selected color)
        SetDCPenColor(g_bfhdc, state->settings.bgSelect);
        SetDCBrushColor(g_bfhdc, state->settings.bgSelect);
        // Left bar
        Rectangle(g_bfhdc, wx.left, barY, wx.left + 2, barY + barH);
        // Right bar
        Rectangle(g_bfhdc, wx.right - G_MARGIN - 2, barY, wx.right - G_MARGIN, barY + barH);
      }
    }

    // Blit
    debug_log("WM_PAINT blitting, real_hdc=%p, buffer=%p", real_hdc, g_bfhdc);
    BitBlt(real_hdc, 0, 0, state->width, state->height, g_bfhdc, 0, 0, SRCCOPY);

    // End
    EndPaint(wnd, &ps);
    return 0;
  case WM_CTLCOLOREDIT:; // Textbox colors
    HDC hdc = (HDC)wparam;
    COLORREF bg = (state->settings.blur && state->settings.bgEditAlpha < 255) ? state->settings.bg : state->settings.bgEdit;
    SetTextColor(hdc, state->settings.fgEdit);
    SetBkColor(hdc, bg);
    SetDCBrushColor(hdc, bg);
    return (LRESULT)GetStockObject(DC_BRUSH);
  case WM_CLOSE:
    exit(1);
  case WM_LBUTTONDOWN:;
    const int mouseX = GET_X_LPARAM(lparam);
    const int mouseY = GET_Y_LPARAM(lparam);
    size_t newIdx = state->selectedResultIndex;

    if (state->settings.horizontalLayout) {
      if (mouseY < wx.top || mouseY > wx.top + LINE_HEIGHT(state->settings.fontSize)) {
        return 0;
      }
      const int itemsStartX = wx.left + state->promptWidth + state->settings.inputWidth + state->settings.padding;
      const int itemWidth = 135;
      const int availableWidth = wx.right - itemsStartX - state->settings.padding;
      const int itemsPerPage = max(1, availableWidth / itemWidth);
      const size_t pageStartIdx =
          (state->selectedResultIndex / itemsPerPage) * itemsPerPage;

      if (mouseX >= itemsStartX) {
        const int itemClicked = (mouseX - itemsStartX) / itemWidth;
        if (itemClicked < itemsPerPage) {
          newIdx = pageStartIdx + itemClicked;
          if (newIdx >= state->searchResultCount) {
            newIdx = state->searchResultCount - 1;
          }
        }
      }
    } else {
      if (mouseY >= entriesTop) {
        newIdx = pageStartI + (mouseY - entriesTop) / LINE_HEIGHT(state->settings.fontSize);
        if (newIdx >= state->searchResultCount) {
          newIdx = state->searchResultCount - 1;
        }
      } else {
        return 0;
      }
    }

    if (newIdx == state->selectedResultIndex) {
      if (state->settings.outputIndex) {
        printf("%zu\n", state->searchResults[state->selectedResultIndex]);
      } else {
        printUtf16AsUtf8(
            state->entries[state->searchResults[state->selectedResultIndex]]);
      }

      // Quit if control isn't held
      if (!(GetKeyState(VK_CONTROL) & 0x8000)) {
        exit(0);
      }
    } else {
      state->selectedResultIndex = newIdx;
      RedrawWindow(state->mainWnd, 0, 0, RDW_INVALIDATE);
    }
    return 0;
  case WM_MOUSEWHEEL:;
    const int ydelta = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
    state->selectedResultIndex =
        (state->selectedResultIndex - ydelta + state->searchResultCount) %
        state->searchResultCount;
    RedrawWindow(state->mainWnd, 0, 0, RDW_INVALIDATE);
    return 0;
  }

  return DefWindowProc(wnd, msg, wparam, lparam);
}

void createWindow(state_t *state) {
  // Register window class
  WNDCLASSEXW wc = {0};
  wc.cbSize = sizeof(WNDCLASSEXW);
  wc.lpfnWndProc = mainWndProc;
  wc.lpszClassName = state->settings.wndClass;
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  ASSERT_WIN32_RESULT(RegisterClassExW(&wc));

  // Create window on active display (where cursor is)
  POINT cursorPos;
  GetCursorPos(&cursorPos);
  HMONITOR hMonitor = MonitorFromPoint(cursorPos, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO mi = {.cbSize = sizeof(MONITORINFO)};
  GetMonitorInfoW(hMonitor, &mi);

  const int displayWidth = mi.rcMonitor.right - mi.rcMonitor.left;
  const int displayHeight = mi.rcMonitor.bottom - mi.rcMonitor.top;

  // Content dimensions (for element positioning)
  if (state->settings.width) {
    state->contentWidth = state->settings.width + G_MARGIN * 2;
  } else {
    state->contentWidth = displayWidth;
  }

  const int verticalSpacing = (state->settings.horizontalLayout || state->lineCount == 0) ? 0 : 4;
  if (state->settings.horizontalLayout) {
    state->contentHeight = LINE_HEIGHT(state->settings.fontSize) + state->settings.padding * 2 + G_MARGIN * 2;
  } else {
    state->contentHeight = LINE_HEIGHT(state->settings.fontSize) * (state->lineCount + 1) +
                    state->settings.padding * 2 + G_MARGIN * 2 + verticalSpacing;
  }

  // Window is content + border
  state->width = state->contentWidth;
  state->height = state->contentHeight;

  if (state->settings.border) {
    const int bp = state->settings.borderPadding;
    state->width += bp * 2;
    state->height += bp * 2;
  }

  // Ensure we don't exceed display dimensions
  if (state->width > (size_t)displayWidth) state->width = displayWidth;
  if (state->height > (size_t)displayHeight) state->height = displayHeight;

  int x = 0, y = 0;
  if (state->settings.centerWindow || state->settings.width) {
    // Center based on content
    x = mi.rcMonitor.left + (displayWidth - (int)state->width) / 2;
    y = mi.rcMonitor.top + (displayHeight - (int)state->height) / 2;
  } else {
    x = mi.rcMonitor.left;
    y = mi.rcMonitor.top;
  }

  // Handle auto-outline by sampling screen brightness
  bool needsOutline = false;
  if (state->settings.autoOutline || state->settings.hasAutoAcrylicColor) {
    state->settings.hasOutline = false; // Sensor takes control
    HDC hdcScreen = GetDC(NULL);
    if (hdcScreen) {
      // Sample 1: Bottom-left, Sample 2: Top-right
      COLORREF samples[2] = {
          GetPixel(hdcScreen, x - 2, y + (int)state->height + 2),
          GetPixel(hdcScreen, x + (int)state->width + 2, y - 2)};
      
      for (int i = 0; i < 2; i++) {
        if (samples[i] == CLR_INVALID) continue;
        int r = GetRValue(samples[i]);
        int g = GetGValue(samples[i]);
        int b = GetBValue(samples[i]);
        // Perceived luminance formula
        double lum = (0.299 * r + 0.587 * g + 0.114 * b);
        if (lum > 128) {
          needsOutline = true;
          break;
        }
      }

      if (needsOutline && state->settings.autoOutline) {
        state->settings.hasOutline = true;
        // Default to black outline if not manually set
        if (state->settings.outlineColor == 0 && GetRValue(state->settings.fg) > 128) {
           state->settings.outlineColor = RGB(0, 0, 0);
        }
      }
      ReleaseDC(NULL, hdcScreen);
    }
  }

  state->mainWnd = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, state->settings.wndClass, L"wlines",
      WS_POPUP, x, y, state->width, state->height, 0, 0, 0, 0);
  ASSERT_WIN32_RESULT(state->mainWnd);
  debug_log("createWindow: mainWnd=%p x=%d y=%d w=%zu h=%zu", state->mainWnd, x, y, state->width, state->height);

  // Set window transparency
  if (state->settings.blur && state->settings.bgAlpha < 255) {
    SetLayeredWindowAttributes(state->mainWnd, state->settings.bg, 255, LWA_COLORKEY | LWA_ALPHA);
  } else {
    SetLayeredWindowAttributes(state->mainWnd, 0, state->settings.bgAlpha, LWA_ALPHA);
  }
  debug_log("SetLayeredWindowAttributes: blur=%d bgAlpha=%d", state->settings.blur, state->settings.bgAlpha);

  if (state->settings.blur && state->settings.bgAlpha < 255) {
    // Background blur / Acrylic effect (Windows 10+)
    typedef enum {
      ACCENT_DISABLED = 0,
      ACCENT_ENABLE_GRADIENT = 1,
      ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
      ACCENT_ENABLE_BLURBEHIND = 3,
      ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
      ACCENT_INVALID_STATE = 5
    } ACCENT_STATE;

    typedef struct {
      ACCENT_STATE AccentState;
      int AccentFlags;
      int GradientColor;
      int AnimationId;
    } ACCENT_POLICY;

    typedef enum {
      WCA_ACCENT_POLICY = 19
    } WINDOWCOMPOSITIONATTRIB;

    typedef struct {
      WINDOWCOMPOSITIONATTRIB Attribute;
      PVOID Data;
      ULONG SizeOfData;
    } WINDOWCOMPOSITIONATTRIBDATA;

    typedef BOOL (WINAPI *pSetWindowCompositionAttribute)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);

    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (hUser) {
      pSetWindowCompositionAttribute setWindowCompositionAttribute = 
          (pSetWindowCompositionAttribute)GetProcAddress(hUser, "SetWindowCompositionAttribute");
      if (setWindowCompositionAttribute) {
        // GradientColor is AABBGGRR. state->settings.bg is already 0x00BBGGRR.
        int tintColor = (state->settings.bgAlpha << 24) | (state->settings.bg & 0xFFFFFF);
        if (needsOutline && state->settings.hasAutoAcrylicColor) {
          tintColor = (state->settings.autoAcrylicAlpha << 24) | (state->settings.autoAcrylicColor & 0xFFFFFF);
        } else if (state->settings.hasAcrylicColor) {
          tintColor = (state->settings.acrylicAlpha << 24) | (state->settings.acrylicColor & 0xFFFFFF);
        }
        ACCENT_POLICY accent = { ACCENT_ENABLE_ACRYLICBLURBEHIND, 0, tintColor, 0 };
        WINDOWCOMPOSITIONATTRIBDATA data;
        data.Attribute = WCA_ACCENT_POLICY;
        data.Data = &accent;
        data.SizeOfData = sizeof(accent);
        setWindowCompositionAttribute(state->mainWnd, &data);
        debug_log("SetWindowCompositionAttribute: applied acrylic policy, tintColor=%08x", tintColor);
      }
    }
  }

  // Calculate prompt width
  if (state->settings.promptText) {
    RECT promptRect = {
        .right = state->width / 2 - state->settings.padding,
        .bottom = state->settings.fontSize * 2,
    };
    const HDC tmpHDC = CreateCompatibleDC(NULL);
    SelectObject(tmpHDC, state->font);
    DrawTextW(tmpHDC, state->settings.promptText, -1, &promptRect,
              DRAWTEXT_PARAMS | DT_CALCRECT);
    DeleteDC(tmpHDC);
    state->promptWidth = promptRect.right - promptRect.left +
                         FONT_HMARGIN(state->settings.fontSize) * 2;
  }

  // Create textbox inside content rect (wx)
  const int bp = state->settings.border ? state->settings.borderPadding : 0;
  const RECT wx = { .left = bp + G_MARGIN + state->settings.padding,
                    .top = bp + G_MARGIN + state->settings.padding,
                    .right = (int)state->width - (bp + G_MARGIN + state->settings.padding),
                    .bottom = (int)state->height - (bp + G_MARGIN + state->settings.padding) };
  const int contentWidthLocal = wx.right - wx.left;

  size_t textboxLeft;
  size_t textboxWidth;

  if (state->settings.horizontalLayout) {
    textboxLeft = state->settings.padding + state->promptWidth;
    textboxWidth = state->settings.inputWidth;
  } else {
    textboxLeft = state->settings.padding + state->promptWidth;
    textboxWidth = contentWidthLocal - textboxLeft - G_MARGIN;
  }

  const int editX = wx.left + (int)state->promptWidth;
  const int editY = wx.top;
  state->editWnd = CreateWindowExW(
      0, L"EDIT", L"",
      WS_VISIBLE | WS_CHILD | ES_LEFT | ES_MULTILINE | ES_AUTOHSCROLL,
      editX, editY, (int)textboxWidth,
      LINE_HEIGHT(state->settings.fontSize) - 1, state->mainWnd, (HMENU)101, 0, 0);
  ASSERT_WIN32_RESULT(state->editWnd);
  debug_log("createWindow: editWnd=%p textboxLeft=%zu textboxWidth=%zu", state->editWnd, textboxLeft, textboxWidth);

  SendMessage(state->editWnd, WM_SETFONT, (WPARAM)state->font,
              MAKELPARAM(1, 0));
  SendMessage(state->editWnd, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
              MAKELPARAM(FONT_HMARGIN(state->settings.fontSize),
                         FONT_HMARGIN(state->settings.fontSize)));

  // Center text vertically using EM_SETRECT (requires ES_MULTILINE)
  RECT rect;
  GetClientRect(state->editWnd, &rect);
  int top = (LINE_HEIGHT(state->settings.fontSize) - 1 - state->settings.fontSize) / 2;
  rect.top = top;
  rect.bottom = LINE_HEIGHT(state->settings.fontSize) - 1;
  SendMessage(state->editWnd, EM_SETRECT, 0, (LPARAM)&rect);

  state->editWndProc = (WNDPROC)SetWindowLongPtr(state->editWnd, GWLP_WNDPROC,
                                                 (LONG_PTR)&editWndProc);

  // Add state pointer
  SetWindowLongPtrW(state->mainWnd, GWLP_USERDATA, (LONG_PTR)state);
  SetWindowLongPtrW(state->editWnd, GWLP_USERDATA, (LONG_PTR)state);

  // Remove default window styling
  LONG lStyle = GetWindowLong(state->mainWnd, GWL_STYLE);
  lStyle &= ~WS_OVERLAPPEDWINDOW;
  SetWindowLong(state->mainWnd, GWL_STYLE, lStyle);

  // Show and attempt to focus window
  ASSERT_WIN32_RESULT(UpdateWindow(state->mainWnd));
  forceForeground(state->mainWnd);
  SetFocus(state->editWnd);

  // Start foreground timer
  SetTimer(state->mainWnd, FOREGROUND_TIMER_ID, 50, 0);
}

void parseStdinEntries(state_t *state) {
  // If stdin is a terminal, don't wait for input - assume empty list
  if (_isatty(_fileno(stdin))) {
    state->stdinUtf16 = NULL;
    state->entryCount = 0;
    state->entries = NULL;
    state->searchResults = NULL;
    return;
  }

  // Read utf8 stdin
  char buf[65536];
  buf_t stdinUtf8 = {0};
  size_t lineLen;
  while ((lineLen = fread(buf, 1, sizeof(buf), stdin))) {
    memcpy(bufAdd(&stdinUtf8, lineLen), buf, lineLen);
  }

  // Convert to utf16
  const size_t charCount =
      MultiByteToWideChar(CP_UTF8, 0, stdinUtf8.data, stdinUtf8.count, 0, 0);
  state->stdinUtf16 = xrealloc(0, (charCount + 1) * sizeof(wchar_t));
  memset(state->stdinUtf16, 0, (charCount + 1) * sizeof(wchar_t));
  MultiByteToWideChar(CP_UTF8, 0, stdinUtf8.data, stdinUtf8.count, state->stdinUtf16,
                      charCount);
  free(stdinUtf8.data);

  // Read menu entries
  state->entryCount = 0;
  buf_t entryBuf = {0};
  size_t lineStartI = 0;
  for (size_t i = 0; i < charCount; i++) {
    if (state->stdinUtf16[i] == L'\r') {
      state->stdinUtf16[i] = L' '; // Strip carriage returns
    }
    if (state->stdinUtf16[i] == L'\n' || i == charCount - 1) {
      bufAdd(&entryBuf, sizeof(wchar_t *));
      ((wchar_t **)entryBuf.data)[state->entryCount] = &state->stdinUtf16[lineStartI];
      if (state->stdinUtf16[i] == L'\n') {
        state->stdinUtf16[i] = 0;
      } else if (i == charCount - 1) {
        state->stdinUtf16[i + 1] = 0;
      }
      lineStartI = i + 1;
      state->entryCount++;
    }
  }
  bufShrink(&entryBuf);
  state->entries = entryBuf.data;

  // Alloc result array
  state->searchResults = xrealloc(0, state->entryCount * sizeof(size_t));
}

void loadFont(state_t *state) {
  LOGFONTW lf = {0};
  lf.lfHeight = state->settings.fontSize; // Positive height matches cell height (matches CreateFontA behavior)
  lf.lfWeight = FW_NORMAL;
  lf.lfCharSet = DEFAULT_CHARSET;
  lf.lfOutPrecision = OUT_TT_ONLY_PRECIS;
  lf.lfQuality = state->settings.fontQuality;
  lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
  wcsncpy(lf.lfFaceName, state->settings.fontName, LF_FACESIZE - 1);

  state->font = CreateFontIndirectW(&lf);
  ASSERT_WIN32_RESULT(state->font);
}

// Blend two colors with alpha
COLORREF blendColor(COLORREF foreground, COLORREF background, int alpha) {
  if (alpha >= 255)
    return foreground;
  if (alpha <= 0)
    return background;

  // Extract RGB components (remember Windows uses BGR)
  int fg_r = foreground & 0xFF;
  int fg_g = (foreground >> 8) & 0xFF;
  int fg_b = (foreground >> 16) & 0xFF;

  int bg_r = background & 0xFF;
  int bg_g = (background >> 8) & 0xFF;
  int bg_b = (background >> 16) & 0xFF;

  // Linear interpolation
  int r = (fg_r * alpha + bg_r * (255 - alpha)) / 255;
  int g = (fg_g * alpha + bg_g * (255 - alpha)) / 255;
  int b = (fg_b * alpha + bg_b * (255 - alpha)) / 255;

  return RGB(r, g, b);
}

COLORREF parseColor(wchar_t *str, int *outAlpha) {
  if (str[0] == L'#') {
    str++;
  }

  size_t len = wcslen(str);
  if (len != 6 && len != 8) {
    fwprintf(stderr, L"Invalid color format, expected 6 or 8 digit hexadecimal "
                     L"(RRGGBB or RRGGBBAA).\n");
    exit(1);
  }

  // Parse the hex value
  unsigned long color = wcstoul(str, 0, 16);

  // Extract alpha if present (last 2 digits)
  if (len == 8) {
    *outAlpha = (int)(color & 0xFF);
    color = color >> 8;
  } else {
    *outAlpha = 255; // fully opaque by default
  }

  // Windows colors are BGR, swap R and B
  unsigned char *raw = (unsigned char *)&color;
  const unsigned char tmp = raw[0];
  raw[0] = raw[2];
  raw[2] = tmp;

  return (COLORREF)color;
}

void usage(void) {
  fprintf(stderr,
          "wlines " WLINES_VERSION "\n"
          "\n"
          "USAGE:\n"
          "\twlines.exe [FLAGS] [OPTIONS]\n"
          "\n"
          "FLAGS:\n"
          "\t-h    Show help and exit\n"
          "\t-cs   Case-sensitive filter\n"
          "\t-id   Output index of the selected line, or -1 when no match\n"
          "\t-blur Enable background blur (acrylic effect, Windows 10+)\n"
          "\t-auto-oc Enable automatic text outline based on background brightness\n"
          "\n"
          "OPTIONS:\n"
          "\t-l    <count>   Amount of lines to show in list\n"
          "\t-p    <text>    Prompt to show before input\n"
          "\t-fm   <mode>    Sets the desired filter mode (see list below)\n"
          "\t-si   <index>   Initial selected line index\n"
          "\t-px   <pixels>  Sets padding on window\n"
          "\t-wx   <pixels>  Sets width of the window and centers it on the "
          "screen\n"
          "\t-hl   <pixels>  Horizontal layout: input width in pixels, items "
          "displayed left-to-right\n"
          "\t-bg   <hex>     Background color\n"
          "\t-fg   <hex>     Foreground color\n"
          "\t-sbg  <hex>     Selected background color\n"
          "\t-sfg  <hex>     Selected foreground color\n"
          "\t-tbg  <hex>     Text input background color\n"
          "\t-tfg  <hex>     Text input foreground color\n"
          "\t-aabg <hex>     Explicit background color for text antialiasing\n"
          "\t-ac   <hex>     Acrylic tint color (requires -blur)\n"
          "\t-aac  <hex>     Auto acrylic tint color for bright backgrounds\n"
          "\t-oc   <hex>     Text outline color\n"
          "\t-f    <font>    Font name\n"
          "\t-fs   <size>    Font size\n"
          "\t-aa   <quality> Font quality (0: none, 1: antialias, 2: cleartype)\n"
          "\n"
          "FILTER MODES:\n"
          "\tcomplete        Filter on the entire search string (default)\n"
          "\tkeywords        Filter on all individual space-delimited words in "
          "the search string\n"
          "\n"
          "KEYBINDS:\n"
          "\tEnter           Output selected line\n"
          "\t[HELD] Ctrl     Don't quit after outputting\n"
          "\t[HELD] Shift    Output entered text, ignoring the selected line\n"
          "\tEscape          Exit without outputting anything\n"
          "\n"
          "\tArrow Up/Down   Select line\n"
          "\tPage Up/Down    Jump pages\n"
          "\tHome/End        Jump to first/last line\n"
          "\n"
          "\tMouse Click     Select or output line\n"
          "\tMouse Scroll    Select line\n"
          "\n");
  exit(1);
}

int main(void) {
  // Set console to UTF-8
  SetConsoleOutputCP(CP_UTF8);

  // Set stdin and stdout to binary mode to avoid CRLF translation
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);

  int argc;
  g_argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  wchar_t **argv = g_argv;

  // Turn off stdout buffering
  setvbuf(stdout, 0, _IONBF, 0);

  debug_log("main start argc=%d", argc);
  SetUnhandledExceptionFilter(unhandled_exception_handler);

  // Init state with default settings
  int bgAlpha, fgAlpha, bgSelectAlpha, fgSelectAlpha, bgEditAlpha, fgEditAlpha;

  g_state = xrealloc(NULL, sizeof(state_t));
  memset(g_state, 0, sizeof(state_t));
  atexit(cleanup);

  state_t *state = g_state;
  state->settings.wndClass = L"wlines_window";
  state->settings.padding = 4;
  state->settings.filterMode = FM_COMPLETE;
  state->settings.caseSensitiveSearch = false;
  state->settings.bg = parseColor(L"#000000", &bgAlpha);
  state->settings.fg = parseColor(L"#ffffff", &fgAlpha);
  state->settings.bgSelect = parseColor(L"#ffffff", &bgSelectAlpha);
  state->settings.fgSelect = parseColor(L"#000000", &fgSelectAlpha);
  state->settings.bgEdit = parseColor(L"#111111", &bgEditAlpha);
  state->settings.fgEdit = parseColor(L"#ffffff", &fgEditAlpha);
  state->settings.fontName = L"Courier New";
  state->settings.fontSize = 24;
  state->settings.lineCount = 15;
  state->settings.borderColor = 0xFFFFFF;
  state->settings.borderPadding = 4;
  state->settings.fontQuality = CLEARTYPE_QUALITY;
  state->settings.hasBgAntialias = false;
  state->settings.hasAcrylicColor = false;

  // Copy alpha values to state
  state->settings.bgAlpha = bgAlpha;
  state->settings.fgAlpha = fgAlpha;
  state->settings.bgSelectAlpha = bgSelectAlpha;
  state->settings.fgSelectAlpha = fgSelectAlpha;
  state->settings.bgEditAlpha = bgEditAlpha;
  state->settings.fgEditAlpha = fgEditAlpha;

  // Parse arguments
  for (int i = 1; i < argc; i++) {
    // Flags
    if (!wcscmp(argv[i], L"-h")) {
      usage();
    } else if (!wcscmp(argv[i], L"-cs")) {
      state->settings.caseSensitiveSearch = true;
    } else if (!wcscmp(argv[i], L"-id")) {
      state->settings.outputIndex = true;
    } else if (!wcscmp(argv[i], L"-blur")) {
      state->settings.blur = true;
    } else if (!wcscmp(argv[i], L"-auto-oc")) {
      state->settings.autoOutline = true;
    } else if (!wcscmp(argv[i], L"-border")) {
      state->settings.border = true;
    } else if (!wcscmp(argv[i], L"-hl")) {
      state->settings.horizontalLayout = true;
      state->settings.inputWidth = _wtoi(argv[++i]);
      if (state->settings.inputWidth < 1) {
        usage();
      }
    } else if (i + 1 == argc) {
      usage();
      // Options
    } else if (!wcscmp(argv[i], L"-l")) {
      state->settings.lineCount = _wtoi(argv[++i]);
      if (state->settings.lineCount < 1) {
        usage();
      }
    } else if (!wcscmp(argv[i], L"-p")) {
      state->settings.promptText = argv[++i];
    } else if (!wcscmp(argv[i], L"-fm")) {
      const wchar_t *modeStr = argv[++i];
      if (!wcscmp(modeStr, L"complete")) {
        state->settings.filterMode = FM_COMPLETE;
      } else if (!wcscmp(modeStr, L"keywords")) {
        state->settings.filterMode = FM_KEYWORDS;
      } else {
        usage();
      }
    } else if (!wcscmp(argv[i], L"-si")) {
      state->settings.selectedIndex = _wtoi(argv[++i]);
      if (state->settings.selectedIndex < 0) {
        usage();
      }
    } else if (!wcscmp(argv[i], L"-px")) {
      state->settings.padding = _wtoi(argv[++i]);
      if (state->settings.padding < 0) {
        usage();
      }
    } else if (!wcscmp(argv[i], L"-wx")) {
      state->settings.width = _wtoi(argv[++i]);
      state->settings.centerWindow = true;
      if (state->settings.width < 1) {
        usage();
      }
    } else if (!wcscmp(argv[i], L"-bg")) {
      state->settings.bg = parseColor(argv[++i], &state->settings.bgAlpha);
    } else if (!wcscmp(argv[i], L"-fg")) {
      state->settings.fg = parseColor(argv[++i], &state->settings.fgAlpha);
    } else if (!wcscmp(argv[i], L"-sbg")) {
      state->settings.bgSelect =
          parseColor(argv[++i], &state->settings.bgSelectAlpha);
    } else if (!wcscmp(argv[i], L"-sfg")) {
      state->settings.fgSelect =
          parseColor(argv[++i], &state->settings.fgSelectAlpha);
    } else if (!wcscmp(argv[i], L"-tbg")) {
      state->settings.bgEdit =
          parseColor(argv[++i], &state->settings.bgEditAlpha);
    } else if (!wcscmp(argv[i], L"-tfg")) {
      state->settings.fgEdit =
          parseColor(argv[++i], &state->settings.fgEditAlpha);
    } else if (!wcscmp(argv[i], L"-aabg")) {
      state->settings.bgAntialias = parseColor(argv[++i], (int[]){0});
      state->settings.hasBgAntialias = true;
    } else if (!wcscmp(argv[i], L"-ac")) {
      state->settings.acrylicColor = parseColor(argv[++i], &state->settings.acrylicAlpha);
      state->settings.hasAcrylicColor = true;
    } else if (!wcscmp(argv[i], L"-aac")) {
      state->settings.autoAcrylicColor = parseColor(argv[++i], &state->settings.autoAcrylicAlpha);
      state->settings.hasAutoAcrylicColor = true;
    } else if (!wcscmp(argv[i], L"-oc")) {
      state->settings.outlineColor = parseColor(argv[++i], (int[]){0});
      state->settings.hasOutline = true;
    } else if (!wcscmp(argv[i], L"-aa")) {
      int q = _wtoi(argv[++i]);
      if (q == 0) state->settings.fontQuality = NONANTIALIASED_QUALITY;
      else if (q == 1) state->settings.fontQuality = ANTIALIASED_QUALITY;
      else if (q == 2) state->settings.fontQuality = CLEARTYPE_QUALITY;
      else usage();
    } else if (!wcscmp(argv[i], L"-bc")) {
      state->settings.borderColor = parseColor(argv[++i], (int[]){0});
    } else if (!wcscmp(argv[i], L"-bp")) {
      state->settings.borderPadding = _wtoi(argv[++i]);
    } else if (!wcscmp(argv[i], L"-f")) {
      state->settings.fontName = argv[++i];
    } else if (!wcscmp(argv[i], L"-fs")) {
      state->settings.fontSize = _wtoi(argv[++i]);
      if (state->settings.fontSize < 1) {
        usage();
      }
    } else {
      usage();
    }
  }

  loadFont(state);
  parseStdinEntries(state);
  state->lineCount = min((size_t)state->settings.lineCount, state->entryCount);
  createWindow(state);
  debug_log("createWindow returned mainWnd=%p contentWidth=%zu contentHeight=%zu", state->mainWnd, state->contentWidth, state->contentHeight);
  updateSearchResults(state);
  if (state->entryCount > 0) {
    state->selectedResultIndex =
        min((size_t)state->settings.selectedIndex, state->entryCount - 1);
  } else {
    state->selectedResultIndex = SELECTED_INDEX_NO_RESULT;
  }
  windowEventLoop();

  return 0;
}
