/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsDesks.h"

#include "arch/Arch.h"
#include "base/IEventQueue.h"
#include "base/IJob.h"
#include "base/Log.h"
#include "base/TMethodJob.h"
#include "deskflow/CursorHider.h"
#include "deskflow/IScreenSaver.h"
#include "deskflow/ScreenException.h"
#include "deskflow/win32/AppUtilWindows.h"
#include "mt/Lock.h"
#include "mt/Thread.h"
#include "platform/MSWindowsHook.h"
#include "platform/MSWindowsScreen.h"

#include <malloc.h>

// these are only defined when WINVER >= 0x0500
#if !defined(SPI_GETMOUSESPEED)
#define SPI_GETMOUSESPEED 112
#endif
#if !defined(SPI_SETMOUSESPEED)
#define SPI_SETMOUSESPEED 113
#endif
#if !defined(SPI_GETSCREENSAVERRUNNING)
#define SPI_GETSCREENSAVERRUNNING 114
#endif

#if !defined(MOUSEEVENTF_HWHEEL)
#define MOUSEEVENTF_HWHEEL 0x1000
#endif

// X button stuff
#if !defined(WM_XBUTTONDOWN)
#define WM_XBUTTONDOWN 0x020B
#define WM_XBUTTONUP 0x020C
#define WM_XBUTTONDBLCLK 0x020D
#define WM_NCXBUTTONDOWN 0x00AB
#define WM_NCXBUTTONUP 0x00AC
#define WM_NCXBUTTONDBLCLK 0x00AD
#define MOUSEEVENTF_XDOWN 0x0080
#define MOUSEEVENTF_XUP 0x0100
#define XBUTTON1 0x0001
#define XBUTTON2 0x0002
#endif
#if !defined(VK_XBUTTON1)
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
#endif

// <unused>; <unused>
#define DESKFLOW_MSG_SWITCH DESKFLOW_HOOK_LAST_MSG + 1
// <unused>; <unused>
#define DESKFLOW_MSG_ENTER DESKFLOW_HOOK_LAST_MSG + 2
// <unused>; <unused>
#define DESKFLOW_MSG_LEAVE DESKFLOW_HOOK_LAST_MSG + 3
// wParam = flags, HIBYTE(lParam) = virtual key, LOBYTE(lParam) = scan code
#define DESKFLOW_MSG_FAKE_KEY DESKFLOW_HOOK_LAST_MSG + 4
// flags, XBUTTON id
#define DESKFLOW_MSG_FAKE_BUTTON DESKFLOW_HOOK_LAST_MSG + 5
// x; y
#define DESKFLOW_MSG_FAKE_MOVE DESKFLOW_HOOK_LAST_MSG + 6
// xDelta; yDelta
#define DESKFLOW_MSG_FAKE_WHEEL DESKFLOW_HOOK_LAST_MSG + 7
// POINT*; <unused>
#define DESKFLOW_MSG_CURSOR_POS DESKFLOW_HOOK_LAST_MSG + 8
// IKeyState*; <unused>
#define DESKFLOW_MSG_SYNC_KEYS DESKFLOW_HOOK_LAST_MSG + 9
// install; <unused>
#define DESKFLOW_MSG_SCREENSAVER DESKFLOW_HOOK_LAST_MSG + 10
// dx; dy
#define DESKFLOW_MSG_FAKE_REL_MOVE DESKFLOW_HOOK_LAST_MSG + 11
// enable; <unused>
#define DESKFLOW_MSG_FAKE_INPUT DESKFLOW_HOOK_LAST_MSG + 12
// <unused>; <unused> — move the hider under the pointer after the main-thread warp
#define DESKFLOW_MSG_ANCHOR_CURSOR DESKFLOW_HOOK_LAST_MSG + 13

static void send_keyboard_input(WORD wVk, WORD wScan, DWORD dwFlags)
{
  INPUT inp;
  inp.type = INPUT_KEYBOARD;
  inp.ki.wVk = (dwFlags & KEYEVENTF_UNICODE) ? 0 : wVk; // 1..254 inclusive otherwise
  inp.ki.wScan = wScan;
  inp.ki.dwFlags = dwFlags & 0xF;
  inp.ki.time = 0;
  inp.ki.dwExtraInfo = 0;
  SendInput(1, &inp, sizeof(inp));
}

static void send_mouse_input(DWORD dwFlags, DWORD dx, DWORD dy, DWORD dwData)
{
  INPUT inp;
  inp.type = INPUT_MOUSE;
  inp.mi.dwFlags = dwFlags;
  inp.mi.dx = dx;
  inp.mi.dy = dy;
  inp.mi.mouseData = dwData;
  inp.mi.time = 0;
  inp.mi.dwExtraInfo = 0;
  SendInput(1, &inp, sizeof(inp));
}

// Every system cursor the pointer can switch to while it sits on another
// thread's window. ShowCursor does not change those sprites.
static const DWORD kSystemCursorIds[] = {
    OCR_NORMAL, OCR_IBEAM,  OCR_WAIT,    OCR_CROSS, OCR_UP,   OCR_SIZENWSE,    OCR_SIZENESW,
    OCR_SIZEWE, OCR_SIZENS, OCR_SIZEALL, OCR_NO,    OCR_HAND, OCR_APPSTARTING,
};

static HCURSOR s_blankCursor = nullptr;
static bool s_systemCursorsBlanked = false;
// The park warp delivers WM_MOUSEMOVE. That must not count as the user
// grabbing the physical mouse.
static bool s_suppressHiderDismiss = false;

static void blankSystemCursors()
{
  if (s_systemCursorsBlanked || s_blankCursor == nullptr)
    return;

  int replaced = 0;
  for (DWORD id : kSystemCursorIds) {
    HCURSOR copy = CopyCursor(s_blankCursor);
    if (copy == nullptr)
      continue;
    if (SetSystemCursor(copy, id))
      ++replaced;
    else
      DestroyCursor(copy);
  }

  s_systemCursorsBlanked = replaced > 0;
  if (replaced == 0)
    LOG_ERR("unable to blank system cursors");
  else
    LOG_DEBUG("blanked %d system cursors", replaced);
}

static void restoreSystemCursors()
{
  if (!s_systemCursorsBlanked)
    return;

  if (!SystemParametersInfo(SPI_SETCURSORS, 0, nullptr, 0))
    LOG_ERR("unable to restore system cursors: %lu", GetLastError());
  else
    LOG_DEBUG("restored system cursors");
  s_systemCursorsBlanked = false;
}

static void showCursorHider(HWND hwnd, int x, int y, int w, int h, bool claimHitTest)
{
  if (hwnd == nullptr)
    return;

  if (!claimHitTest) {
    // Full-monitor fallback used when low-level hooks did not install.
    // Leave it transparent so it does not eat clicks on the whole desktop.
    // System-cursor blanking still hides the arrow.
    SetWindowPos(hwnd, HWND_TOP, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return;
  }

  // WS_EX_TRANSPARENT is hit-tested as HTTRANSPARENT, so the pointer never
  // belongs to this thread and ShowCursor cannot hide it. A layered alpha
  // of 1 still hit-tests (same approach as the edge strips) without painting
  // a black rectangle over the desktop.
  LONG_PTR extra = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
  extra |= WS_EX_LAYERED | WS_EX_TOOLWINDOW;
  extra &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
  SetWindowLongPtr(hwnd, GWL_EXSTYLE, extra);
  SetLayeredWindowAttributes(hwnd, 0, 1, LWA_ALPHA);
  SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
  InvalidateRect(hwnd, nullptr, TRUE);
  UpdateWindow(hwnd);
}

static void hideCursorHider(HWND hwnd)
{
  if (hwnd == nullptr)
    return;

  // Hide before clearing WS_EX_LAYERED. Windows ignores that style change
  // on a window that is still visible.
  SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW);
  LONG_PTR extra = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
  extra |= WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW;
  extra &= ~static_cast<LONG_PTR>(WS_EX_LAYERED);
  SetWindowLongPtr(hwnd, GWL_EXSTYLE, extra);
  SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

static void pushBlankCursor(HWND hwnd)
{
  SetCursor(nullptr);
  if (hwnd != nullptr)
    SendMessage(hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
}

static LRESULT handleHiderMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, bool &handled)
{
  handled = false;
  switch (msg) {
  case WM_NCHITTEST:
    if (IsWindowVisible(hwnd)) {
      handled = true;
      return HTCLIENT;
    }
    break;

  case WM_SETCURSOR:
    if (IsWindowVisible(hwnd)) {
      SetCursor(nullptr);
      handled = true;
      return TRUE;
    }
    break;

  case WM_ERASEBKGND:
    if (IsWindowVisible(hwnd)) {
      RECT rc{};
      GetClientRect(hwnd, &rc);
      FillRect(reinterpret_cast<HDC>(wParam), &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      handled = true;
      return 1;
    }
    break;

  case WM_PAINT: {
    PAINTSTRUCT ps{};
    BeginPaint(hwnd, &ps);
    EndPaint(hwnd, &ps);
    handled = true;
    return 0;
  }

  default:
    break;
  }
  return 0;
}

//
// MSWindowsDesks
//

MSWindowsDesks::MSWindowsDesks(
    bool isPrimary, bool useHooks, const IScreenSaver *screensaver, IEventQueue *events, IJob *updateKeys
)
    : m_isPrimary(isPrimary),
      m_useHooks(useHooks),
      m_isOnScreen(m_isPrimary),
      m_screensaver(screensaver),
      m_deskReady(&m_mutex, false),
      m_updateKeys(updateKeys),
      m_events(events)
{

  m_cursor = createBlankCursor();
  s_blankCursor = m_cursor;
  m_deskClass = createDeskWindowClass(m_isPrimary);
  m_keyLayout = AppUtilWindows::instance().getCurrentKeyboardLayout();
  resetOptions();
}

MSWindowsDesks::~MSWindowsDesks()
{
  disable();
  restoreSystemCursors();
  destroyClass(m_deskClass);
  destroyCursor(m_cursor);
  s_blankCursor = nullptr;
  delete m_updateKeys;
}

void MSWindowsDesks::enable()
{
  m_threadID = GetCurrentThreadId();

  // set the active desk and (re)install the hooks
  checkDesk();

  // install the desk timer.  this timer periodically checks
  // which desk is active and reinstalls the hooks as necessary.
  // we wouldn't need this if windows notified us of a desktop
  // change but as far as i can tell it doesn't.
  m_timer = m_events->newTimer(0.2, nullptr);
  m_events->addHandler(EventTypes::Timer, m_timer, [this](const auto &) { handleCheckDesk(); });

  updateKeys();
}

void MSWindowsDesks::disable()
{
  // remove timer
  if (m_timer != nullptr) {
    m_events->removeHandler(EventTypes::Timer, m_timer);
    m_events->deleteTimer(m_timer);
    m_timer = nullptr;
  }

  // destroy desks
  removeDesks();

  m_isOnScreen = m_isPrimary;
}

void MSWindowsDesks::enter()
{
  sendMessage(DESKFLOW_MSG_ENTER, 0, 0);
}

void MSWindowsDesks::leave(HKL keyLayout)
{
  sendMessage(DESKFLOW_MSG_LEAVE, (WPARAM)keyLayout, 0);
}

void MSWindowsDesks::resetOptions()
{
  m_leaveForegroundOption = false;
  m_relativeMouseMoves = false;
  for (auto &entry : m_desks) {
    entry.second->m_hasRelativeRestorePosition = false;
  }
}

void MSWindowsDesks::setOptions(const OptionsList &options)
{
  for (uint32_t i = 0, n = (uint32_t)options.size(); i < n; i += 2) {
    if (options[i] == kOptionWin32KeepForeground) {
      m_leaveForegroundOption = (options[i + 1] != 0);
      LOG_VERBOSE("%s the foreground window", m_leaveForegroundOption ? "don\'t grab" : "grab");
    } else if (options[i] == kOptionRelativeMouseMoves) {
      m_relativeMouseMoves = (options[i + 1] != 0);
    }
  }
}

void MSWindowsDesks::updateKeys()
{
  sendMessage(DESKFLOW_MSG_SYNC_KEYS, 0, 0);
}

void MSWindowsDesks::setShape(
    int32_t x, int32_t y, int32_t width, int32_t height, int32_t xCenter, int32_t yCenter, bool isMultimon
)
{
  m_x = x;
  m_y = y;
  m_w = width;
  m_h = height;
  m_xCenter = xCenter;
  m_yCenter = yCenter;
  m_multimon = isMultimon;
}

void MSWindowsDesks::installScreensaverHooks(bool install)
{
  if (m_isPrimary && m_screensaverNotify != install) {
    m_screensaverNotify = install;
    sendMessage(DESKFLOW_MSG_SCREENSAVER, install, 0);
  }
}

void MSWindowsDesks::fakeInputBegin()
{
  sendMessage(DESKFLOW_MSG_FAKE_INPUT, 1, 0);
}

void MSWindowsDesks::fakeInputEnd()
{
  sendMessage(DESKFLOW_MSG_FAKE_INPUT, 0, 0);
}

void MSWindowsDesks::getCursorPos(int32_t &x, int32_t &y) const
{
  POINT pos{0, 0};
  sendMessage(DESKFLOW_MSG_CURSOR_POS, reinterpret_cast<WPARAM>(&pos), 0);
  x = pos.x;
  y = pos.y;
}

void MSWindowsDesks::fakeKeyEvent(WORD virtualKey, WORD scanCode, DWORD flags, bool /*isAutoRepeat*/) const
{
  sendMessage(DESKFLOW_MSG_FAKE_KEY, flags, MAKELPARAM(scanCode, virtualKey));
}

void MSWindowsDesks::fakeMouseButton(ButtonID button, bool press)
{
  // the system will swap the meaning of left/right for us if
  // the user has configured a left-handed mouse but we don't
  // want it to swap since we want the handedness of the
  // server's mouse.  so pre-swap for a left-handed mouse.
  if (GetSystemMetrics(SM_SWAPBUTTON)) {
    switch (button) {
    case kButtonLeft:
      button = kButtonRight;
      break;

    case kButtonRight:
      button = kButtonLeft;
      break;
    }
  }

  // map button id to button flag and button data
  DWORD data = 0;
  DWORD flags;
  switch (button) {
  case kButtonLeft:
    flags = press ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    break;

  case kButtonMiddle:
    flags = press ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    break;

  case kButtonRight:
    flags = press ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    break;

  case kButtonExtra0:
    data = XBUTTON1;
    flags = press ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
    break;

  case kButtonExtra1:
    data = XBUTTON2;
    flags = press ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
    break;

  default:
    return;
  }

  // do it
  sendMessage(DESKFLOW_MSG_FAKE_BUTTON, flags, data);
}

void MSWindowsDesks::postMouseApply() const
{
  if (m_mouseApplyPosted)
    return;
  if (m_activeDesk == nullptr || m_activeDesk->m_window == nullptr)
    return;
  if (PostThreadMessage(m_activeDesk->m_threadID, DESKFLOW_MSG_FAKE_MOVE, 0, 0) != 0)
    m_mouseApplyPosted = true;
}

void MSWindowsDesks::fakeMouseMove(int32_t x, int32_t y) const
{
  // Apply immediately when the desk thread is idle. While SendInput is in
  // progress, keep only the newest absolute position — do not queue one
  // message per sample and do not wait on a timer.
  Lock lock(&m_mutex);
  m_pendingAbsX = x;
  m_pendingAbsY = y;
  m_pendingAbs = true;
  postMouseApply();
}

void MSWindowsDesks::fakeMouseRelativeMove(int32_t dx, int32_t dy) const
{
  Lock lock(&m_mutex);
  m_pendingRelDx += dx;
  m_pendingRelDy += dy;
  postMouseApply();
}

void MSWindowsDesks::fakeMouseWheel(int32_t xDelta, int32_t yDelta) const
{
  sendMessage(DESKFLOW_MSG_FAKE_WHEEL, xDelta, yDelta);
}

void MSWindowsDesks::saveRelativeRestorePosition(Desk *desk) const
{
  POINT pos{0, 0};
  if (!GetCursorPos(&pos)) {
    LOG_DEBUG(
        "could not save relative restore position on desk \"%ls\", error: %lu", desk->m_name.c_str(), GetLastError()
    );
    return;
  }

  desk->m_relativeRestoreX = pos.x;
  desk->m_relativeRestoreY = pos.y;
  desk->m_hasRelativeRestorePosition = true;
  LOG_VERBOSE(
      "saved relative restore position on desk \"%ls\": %+d,%+d", desk->m_name.c_str(), desk->m_relativeRestoreX,
      desk->m_relativeRestoreY
  );
}

bool MSWindowsDesks::restoreRelativeCursorPosition(Desk *desk) const
{
  if (!desk->m_hasRelativeRestorePosition) {
    return false;
  }

  LOG_VERBOSE(
      "restoring relative cursor position on desk \"%ls\": %+d,%+d", desk->m_name.c_str(), desk->m_relativeRestoreX,
      desk->m_relativeRestoreY
  );
  deskMouseMove(desk->m_relativeRestoreX, desk->m_relativeRestoreY);
  return true;
}

void MSWindowsDesks::sendMessage(UINT msg, WPARAM wParam, LPARAM lParam, bool wait) const
{
  if (m_activeDesk != nullptr && m_activeDesk->m_window != nullptr) {
    PostThreadMessage(m_activeDesk->m_threadID, msg, wParam, lParam);
    if (wait)
      waitForDesk();
  }
}

HCURSOR
MSWindowsDesks::createBlankCursor() const
{
  // create a transparent cursor
  int cw = GetSystemMetrics(SM_CXCURSOR);
  int ch = GetSystemMetrics(SM_CYCURSOR);
  uint8_t *cursorAND = new uint8_t[ch * ((cw + 31) >> 2)];
  uint8_t *cursorXOR = new uint8_t[ch * ((cw + 31) >> 2)];
  memset(cursorAND, 0xff, ch * ((cw + 31) >> 2));
  memset(cursorXOR, 0x00, ch * ((cw + 31) >> 2));
  HCURSOR c = CreateCursor(MSWindowsScreen::getWindowInstance(), 0, 0, cw, ch, cursorAND, cursorXOR);
  delete[] cursorXOR;
  delete[] cursorAND;
  return c;
}

void MSWindowsDesks::destroyCursor(HCURSOR cursor) const
{
  if (cursor != nullptr) {
    DestroyCursor(cursor);
  }
}

ATOM MSWindowsDesks::createDeskWindowClass(bool isPrimary) const
{
  WNDCLASSEX classInfo;
  classInfo.cbSize = sizeof(classInfo);
  classInfo.style = CS_DBLCLKS | CS_NOCLOSE;
  classInfo.lpfnWndProc = isPrimary ? &MSWindowsDesks::primaryDeskProc : &MSWindowsDesks::secondaryDeskProc;
  classInfo.cbClsExtra = 0;
  classInfo.cbWndExtra = 0;
  classInfo.hInstance = MSWindowsScreen::getWindowInstance();
  classInfo.hIcon = nullptr;
  classInfo.hCursor = m_cursor;
  classInfo.hbrBackground = nullptr;
  classInfo.lpszMenuName = nullptr;
  classInfo.lpszClassName = L"DeskflowDesk";
  classInfo.hIconSm = nullptr;
  return RegisterClassEx(&classInfo);
}

void MSWindowsDesks::destroyClass(ATOM windowClass) const
{
  if (windowClass != 0) {
    UnregisterClass(MAKEINTATOM(windowClass), MSWindowsScreen::getWindowInstance());
  }
}

HWND MSWindowsDesks::createWindow(ATOM windowClass, const wchar_t *name) const
{
  HWND window = CreateWindowEx(
      WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW, MAKEINTATOM(windowClass), name, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
      MSWindowsScreen::getWindowInstance(), nullptr
  );
  if (window == nullptr) {
    LOG_ERR("failed to create window: %d", GetLastError());
    throw ScreenOpenFailureException();
  }
  return window;
}

void MSWindowsDesks::destroyWindow(HWND hwnd) const
{
  if (hwnd != nullptr) {
    DestroyWindow(hwnd);
  }
}

LRESULT CALLBACK MSWindowsDesks::primaryDeskProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  bool handled = false;
  const LRESULT result = handleHiderMessage(hwnd, msg, wParam, lParam, handled);
  if (handled)
    return result;
  return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK MSWindowsDesks::secondaryDeskProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  bool handled = false;
  const LRESULT result = handleHiderMessage(hwnd, msg, wParam, lParam, handled);
  if (handled)
    return result;

  // Local motion that leaves the hider means the user grabbed the physical
  // mouse. Hide our window and put the system cursors back. Judge that from
  // the pointer's position now, not from lParam: the park warp's message can
  // be delivered after the hider has moved.
  bool hide = false;
  if (!s_suppressHiderDismiss && msg == WM_MOUSEMOVE) {
    POINT pos{};
    RECT rc{};
    if (GetCursorPos(&pos) && GetWindowRect(hwnd, &rc)) {
      hide = dismissHiderOnLocalMove(
          static_cast<int32_t>(pos.x - rc.left), static_cast<int32_t>(pos.y - rc.top), rc.right - rc.left,
          rc.bottom - rc.top
      );
    }
  }

  if (hide && IsWindowVisible(hwnd)) {
    ReleaseCapture();
    hideCursorHider(hwnd);
    restoreSystemCursors();
  }

  return DefWindowProc(hwnd, msg, wParam, lParam);
}

#ifndef MOUSEEVENTF_VIRTUALDESK
#define MOUSEEVENTF_VIRTUALDESK 0x4000
#endif

void MSWindowsDesks::deskMouseMove(int32_t x, int32_t y) const
{
  // Coordinates from the server are in this machine's virtual-screen space
  // (SM_*VIRTUALSCREEN), which is not the primary monitor and is not always
  // rooted at (0, 0). Mapping them onto SM_CXSCREEN makes the cursor jump
  // and then "correct" on the next packet — that is the stutter after the
  // pointer crosses. MOUSEEVENTF_VIRTUALDESK addresses the whole desktop.
  int32_t originX = m_x;
  int32_t originY = m_y;
  int32_t w = m_w;
  int32_t h = m_h;
  if (w < 2)
    w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  if (h < 2)
    h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (w < 2)
    w = GetSystemMetrics(SM_CXSCREEN);
  if (h < 2)
    h = GetSystemMetrics(SM_CYSCREEN);
  if (w < 2)
    w = 2;
  if (h < 2)
    h = 2;

  const int32_t relX = x - originX;
  const int32_t relY = y - originY;
  auto clampNorm = [](int32_t value, int32_t span) -> DWORD {
    if (value < 0)
      value = 0;
    if (value > span - 1)
      value = span - 1;
    return static_cast<DWORD>((65535.0f * value) / (span - 1) + 0.5f);
  };
  send_mouse_input(
      MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, clampNorm(relX, w), clampNorm(relY, h), 0
  );
}

void MSWindowsDesks::deskMouseRelativeMove(int32_t dx, int32_t dy) const
{
  // relative moves are subject to cursor acceleration which we don't
  // want.so we disable acceleration, do the relative move, then
  // restore acceleration.  there's a slight chance we'll end up in
  // the wrong place if the user moves the cursor using this system's
  // mouse while simultaneously moving the mouse on the server
  // system.  that defeats the purpose of deskflow so we'll assume
  // that won't happen.  even if it does, the next mouse move will
  // correct the position.

  // save mouse speed & acceleration
  int oldSpeed[4];
  bool accelChanged =
      SystemParametersInfo(SPI_GETMOUSE, 0, oldSpeed, 0) && SystemParametersInfo(SPI_GETMOUSESPEED, 0, oldSpeed + 3, 0);

  // use 1:1 motion
  if (accelChanged) {
    int newSpeed[4] = {0, 0, 0, 1};
    accelChanged = SystemParametersInfo(SPI_SETMOUSE, 0, newSpeed, 0) ||
                   SystemParametersInfo(SPI_SETMOUSESPEED, 0, newSpeed + 3, 0);
  }

  // move relative to mouse position
  send_mouse_input(MOUSEEVENTF_MOVE, dx, dy, 0);

  // restore mouse speed & acceleration
  if (accelChanged) {
    SystemParametersInfo(SPI_SETMOUSE, 0, oldSpeed, 0);
    SystemParametersInfo(SPI_SETMOUSESPEED, 0, oldSpeed + 3, 0);
  }
}

/*!
 * Wraps the `ShowCursor` function and calls it repeatedly until the cursor visibility is at
 * the desired state. Windows maintains an internal counter for cursor visibility, and only
 * shows or hides the cursor when it reaches a certain threshold.
 *
 * That counter is per thread and only applies while the pointer is over a window this
 * thread created. Replacing the system cursor sprites hides the arrow everywhere else.
 */
void setCursorVisibility(bool visible)
{
  LOG_DEBUG("%s cursor", visible ? "showing" : "hiding");
  if (visible)
    restoreSystemCursors();
  else
    blankSystemCursors();

  const int max = 10;
  int attempts = 0;
  while (attempts++ < max) {
    const auto displayCounter = ShowCursor(visible ? TRUE : FALSE);
    LOG_VERBOSE("cursor display counter: %d", displayCounter);

    if (visible) {
      if (displayCounter < 0) {
        LOG_VERBOSE("cursor still hidden, retrying, attempt: %d", attempts);
      } else {
        LOG_VERBOSE("cursor is now visible, attempts: %d", attempts);
        return;
      }
    } else {
      if (displayCounter >= 0) {
        LOG_VERBOSE("cursor still visible, retrying, attempt: %d", attempts);
      } else {
        LOG_VERBOSE("cursor is now hidden, attempts: %d", attempts);
        return;
      }
    }
  }

  LOG_ERR("unable to set cursor visibility after %d attempts", attempts);
}

void MSWindowsDesks::deskEnter(Desk *desk)
{
  if (!m_isPrimary) {
    ReleaseCapture();
    if (m_relativeMouseMoves) {
      restoreRelativeCursorPosition(desk);
    }
  }

  // Drop the hider before restoring the system sprites, or its WM_SETCURSOR
  // handler puts the blank cursor back on top of the arrow.
  hideCursorHider(desk->m_window);
  setCursorVisibility(true);
  SetCursor(LoadCursor(nullptr, IDC_ARROW));

  // restore the foreground window
  // XXX -- this raises the window to the top of the Z-order.  we
  // want it to stay wherever it was to properly support X-mouse
  // (mouse over activation) but i've no idea how to do that.
  // the obvious workaround of using SetWindowPos() to move it back
  // after being raised doesn't work.
  DWORD thisThread = GetWindowThreadProcessId(desk->m_window, nullptr);
  DWORD thatThread = GetWindowThreadProcessId(desk->m_foregroundWindow, nullptr);
  AttachThreadInput(thatThread, thisThread, TRUE);
  SetForegroundWindow(desk->m_foregroundWindow);
  AttachThreadInput(thatThread, thisThread, FALSE);
  EnableWindow(desk->m_window, desk->m_lowLevel ? FALSE : TRUE);
  desk->m_foregroundWindow = nullptr;
}

void MSWindowsDesks::moveHiderUnderPointer(Desk *desk) const
{
  if (desk == nullptr || desk->m_window == nullptr)
    return;
  // Without low-level hooks the cursor jitters, so the hider already
  // covers every monitor. Do not shrink it to the pointer.
  if (m_isPrimary && !desk->m_lowLevel)
    return;

  POINT pos{m_xCenter, m_yCenter};
  if (!GetCursorPos(&pos)) {
    pos.x = m_xCenter;
    pos.y = m_yCenter;
  }

  const auto rect = cursorHiderRect(static_cast<int32_t>(pos.x), static_cast<int32_t>(pos.y), kCursorHiderSlop);
  const bool over = pointerOverHider(static_cast<int32_t>(pos.x), static_cast<int32_t>(pos.y), rect);
  s_suppressHiderDismiss = true;
  showCursorHider(desk->m_window, rect.x, rect.y, rect.w, rect.h, true);
  pushBlankCursor(desk->m_window);
  s_suppressHiderDismiss = false;
  LOG_DEBUG(
      "cursor hider at %d,%d %dx%d under pointer %d,%d", rect.x, rect.y, rect.w, rect.h, static_cast<int>(pos.x),
      static_cast<int>(pos.y)
  );
  if (showCursorCannotHide(true, over))
    LOG_ERR("cursor hider does not contain the pointer");
}

void MSWindowsDesks::parkPointerForHider(Desk *desk) const
{
  if (desk == nullptr || desk->m_window == nullptr)
    return;

  // Center the pointer on the hider. The primary low-level path used to
  // skip this, so the hardware cursor stayed on the leave edge (often the
  // last pixel) while a 1x1 window sat at the screen center.
  // The main thread is blocked in leave() and discards the mouse message
  // this posts, so the half-screen delta is not delivered.
  s_suppressHiderDismiss = true;
  if (!m_isPrimary) {
    // Watch for real local motion so the physical mouse still works.
    // Capture is not taken on the primary: stealing it would cancel an
    // OLE drag that already entered the edge strip.
    SetCapture(desk->m_window);
    // Let the blank system cursor settle before the warp, or the arrow
    // flashes at the center for a frame.
    ARCH->sleep(0.03);
  }
  LOG_VERBOSE("centering cursor on leave: %+d,%+d", m_xCenter, m_yCenter);
  deskMouseMove(m_xCenter, m_yCenter);
  s_suppressHiderDismiss = false;

  // If the warp did not land (the jump-zone hook can eat a move that is
  // still on the edge pixel), follow the pointer instead of leaving the
  // hider at the center.
  moveHiderUnderPointer(desk);
  if (!m_isPrimary)
    SetCapture(desk->m_window);
}

void MSWindowsDesks::anchorCursorHider() const
{
  sendMessage(DESKFLOW_MSG_ANCHOR_CURSOR, 0, 0);
}

void MSWindowsDesks::deskLeave(Desk *desk, HKL keyLayout)
{
  if (!m_isPrimary && m_relativeMouseMoves) {
    saveRelativeRestorePosition(desk);
  }

  // Blanks the system sprites first. ShowCursor alone leaves the arrow
  // on screen when the pointer is over another thread's window.
  setCursorVisibility(false);

  // Low-level hooks hold the pointer still, so a window around the
  // pointer is enough. Without them the pointer jitters and the hider
  // has to cover every monitor.
  const bool smallHider = !m_isPrimary || desk->m_lowLevel;
  if (smallHider) {
    // Low-level install disables the hider so it cannot activate. Enable it
    // while the pointer is away so it can own the hit and, on a client,
    // capture local motion. deskEnter disables it again.
    if (desk->m_window != nullptr)
      EnableWindow(desk->m_window, TRUE);
    const auto rect = cursorHiderRect(m_xCenter, m_yCenter, kCursorHiderSlop);
    showCursorHider(desk->m_window, rect.x, rect.y, rect.w, rect.h, true);
  } else {
    showCursorHider(desk->m_window, m_x, m_y, m_w, m_h, false);
  }

  if (m_isPrimary) {
    // map a window to hide the cursor and to use whatever keyboard
    // layout we choose rather than the keyboard layout of the last
    // active window.
    // switch to requested keyboard layout
    ActivateKeyboardLayout(keyLayout, 0);

    // if not using low-level hooks we have to also activate the
    // window to ensure we don't lose keyboard focus.
    // FIXME -- see if this can be avoided.  if so then always
    // disable the window (see handling of DESKFLOW_MSG_SWITCH).
    if (!desk->m_lowLevel) {
      SetActiveWindow(desk->m_window);
      pushBlankCursor(desk->m_window);
    }

    // if using low-level hooks then disable the foreground window
    // so it can't mess up any of our keyboard events.  the console
    // program, for example, will cause characters to be reported as
    // unshifted, regardless of the shift key state.  interestingly
    // we do see the shift key go down and up.
    //
    // note that we must enable the window to activate it and we
    // need to disable the window on deskEnter.
    else {
      // Keyboard activation still needs a foreground window. The hider was
      // enabled above so it can own the pointer.
      desk->m_foregroundWindow = getForegroundWindow();
      if (desk->m_foregroundWindow != nullptr) {
        SetActiveWindow(desk->m_window);
        DWORD thisThread = GetWindowThreadProcessId(desk->m_window, nullptr);
        DWORD thatThread = GetWindowThreadProcessId(desk->m_foregroundWindow, nullptr);

        AttachThreadInput(thatThread, thisThread, TRUE);
        SetForegroundWindow(desk->m_window);
        AttachThreadInput(thatThread, thisThread, FALSE);
      }
      parkPointerForHider(desk);
    }
  } else {
    parkPointerForHider(desk);
  }
}

void MSWindowsDesks::deskThread(const void *vdesk)
{
  MSG msg;

  // use given desktop for this thread
  Desk *desk = const_cast<Desk *>(static_cast<const Desk *>(vdesk));
  desk->m_threadID = GetCurrentThreadId();
  desk->m_window = nullptr;
  desk->m_foregroundWindow = nullptr;
  if (desk->m_desk != nullptr && SetThreadDesktop(desk->m_desk) != 0) {
    // create a message queue
    PeekMessage(&msg, nullptr, 0, 0, PM_NOREMOVE);

    // create a window.  we use this window to hide the cursor.
    try {
      desk->m_window = createWindow(m_deskClass, L"DeskflowDesk");
      LOG_DEBUG("desk %ls window is 0x%08x", desk->m_name.c_str(), desk->m_window);
    } catch (...) {
      // ignore
      LOG_DEBUG("can't create desk window for %ls", desk->m_name.c_str());
    }
  }

  // tell main thread that we're ready
  {
    Lock lock(&m_mutex);
    m_deskReady = true;
    m_deskReady.broadcast();
  }

  while (GetMessage(&msg, nullptr, 0, 0)) {
    switch (msg.message) {
    default:
      TranslateMessage(&msg);
      DispatchMessage(&msg);
      continue;

    case DESKFLOW_MSG_SWITCH:
      if (m_useHooks) {
        MSWindowsHook::uninstall();
        if (m_screensaverNotify) {
          MSWindowsHook::uninstallScreenSaver();
          MSWindowsHook::installScreenSaver();
        }
        switch (MSWindowsHook::install()) {
        case kHOOK_FAILED:
          // we won't work on this desk
          desk->m_lowLevel = false;
          break;

        case kHOOK_OKAY:
          desk->m_lowLevel = false;
          break;

        case kHOOK_OKAY_LL:
          desk->m_lowLevel = true;
          break;
        }

        // a window on the primary screen with low-level hooks
        // should never activate.
        if (desk->m_window)
          EnableWindow(desk->m_window, desk->m_lowLevel ? FALSE : TRUE);
      }
      break;

    case DESKFLOW_MSG_ENTER:
      m_isOnScreen = true;
      deskEnter(desk);
      break;

    case DESKFLOW_MSG_LEAVE:
      m_isOnScreen = false;
      m_keyLayout = (HKL)msg.wParam;
      deskLeave(desk, m_keyLayout);
      break;

    case DESKFLOW_MSG_ANCHOR_CURSOR:
      // The primary warps to the center after deskLeave returns. Cover
      // wherever the pointer actually is. Skip when the user already
      // dismissed the hider with the physical mouse.
      if (!m_isOnScreen && IsWindowVisible(desk->m_window))
        moveHiderUnderPointer(desk);
      break;

    case DESKFLOW_MSG_FAKE_KEY:
      // Note, this is intended to be HI/LOWORD and not HI/LOBYTE
      send_keyboard_input(HIWORD(msg.lParam), LOWORD(msg.lParam), (DWORD)msg.wParam);
      break;

    case DESKFLOW_MSG_FAKE_BUTTON:
      if (msg.wParam != 0) {
        send_mouse_input((DWORD)msg.wParam, 0, 0, (DWORD)msg.lParam);
      }
      break;

    case DESKFLOW_MSG_FAKE_MOVE:
    case DESKFLOW_MSG_FAKE_REL_MOVE:
      // Drain the latest sample, then anything that arrived during SendInput.
      // Holding the mutex across SendInput would deadlock if the hook calls back.
      for (;;) {
        int32_t absX = 0;
        int32_t absY = 0;
        int32_t relDx = 0;
        int32_t relDy = 0;
        bool hasAbs = false;
        bool hasRel = false;
        {
          Lock lock(&m_mutex);
          hasAbs = m_pendingAbs;
          hasRel = m_pendingRelDx != 0 || m_pendingRelDy != 0;
          absX = m_pendingAbsX;
          absY = m_pendingAbsY;
          relDx = m_pendingRelDx;
          relDy = m_pendingRelDy;
          m_pendingAbs = false;
          m_pendingRelDx = 0;
          m_pendingRelDy = 0;
          if (!hasAbs && !hasRel) {
            m_mouseApplyPosted = false;
            break;
          }
        }
        if (hasAbs)
          deskMouseMove(absX, absY);
        if (hasRel)
          deskMouseRelativeMove(relDx, relDy);
      }
      continue;

    case DESKFLOW_MSG_FAKE_WHEEL:
      // XXX -- add support for x-axis scrolling
      if (msg.lParam != 0) {
        send_mouse_input(MOUSEEVENTF_WHEEL, 0, 0, (DWORD)msg.lParam);
      }
      if (msg.wParam != 0) {
        send_mouse_input(MOUSEEVENTF_HWHEEL, 0, 0, (DWORD)msg.wParam);
      }
      break;

    case DESKFLOW_MSG_CURSOR_POS: {
      POINT *pos = reinterpret_cast<POINT *>(msg.wParam);
      if (!GetCursorPos(pos)) {
        pos->x = m_xCenter;
        pos->y = m_yCenter;
      }
      break;
    }

    case DESKFLOW_MSG_SYNC_KEYS:
      m_updateKeys->run();
      break;

    case DESKFLOW_MSG_SCREENSAVER:
      if (m_useHooks) {
        if (msg.wParam != 0) {
          MSWindowsHook::installScreenSaver();
        } else {
          MSWindowsHook::uninstallScreenSaver();
        }
      }
      break;

    case DESKFLOW_MSG_FAKE_INPUT:
      send_keyboard_input(
          DESKFLOW_HOOK_FAKE_INPUT_VIRTUAL_KEY, DESKFLOW_HOOK_FAKE_INPUT_SCANCODE, msg.wParam ? 0 : KEYEVENTF_KEYUP
      );
      break;
    }

    // notify that message was processed
    Lock lock(&m_mutex);
    m_deskReady = true;
    m_deskReady.broadcast();
  }

  // clean up
  deskEnter(desk);
  if (desk->m_window != nullptr) {
    DestroyWindow(desk->m_window);
  }
  if (desk->m_desk != nullptr) {
    closeDesktop(desk->m_desk);
  }
}

MSWindowsDesks::Desk *MSWindowsDesks::addDesk(const std::wstring &name, HDESK hdesk)
{
  Desk *desk = new Desk;
  desk->m_name = name;
  desk->m_desk = hdesk;
  desk->m_targetID = GetCurrentThreadId();
  desk->m_thread = new Thread(new TMethodJob<MSWindowsDesks>(this, &MSWindowsDesks::deskThread, desk));
  waitForDesk();
  m_desks.try_emplace(name, desk);
  return desk;
}

void MSWindowsDesks::removeDesks()
{
  for (Desks::iterator index = m_desks.begin(); index != m_desks.end(); ++index) {
    Desk *desk = index->second;
    PostThreadMessage(desk->m_threadID, WM_QUIT, 0, 0);
    desk->m_thread->wait();
    delete desk->m_thread;
    delete desk;
  }
  m_desks.clear();
  m_activeDesk = nullptr;
  m_activeDeskName = L"";
}

void MSWindowsDesks::checkDesk()
{
  // get current desktop.  if we already know about it then return.
  Desk *desk;
  HDESK hdesk = openInputDesktop();
  std::wstring name = getDesktopName(hdesk);
  Desks::const_iterator index = m_desks.find(name);
  if (index == m_desks.end()) {
    desk = addDesk(name, hdesk);
    // hold on to hdesk until thread exits so the desk can't
    // be removed by the system
  } else {
    closeDesktop(hdesk);
    desk = index->second;
  }

  // if active desktop changed then tell the old and new desk threads
  // about the change.  don't switch desktops when the screensaver is
  // active becaue we'd most likely switch to the screensaver desktop
  // which would have the side effect of forcing the screensaver to
  // stop.
  if (name != m_activeDeskName && !m_screensaver->isActive()) {
    // show cursor on previous desk
    bool wasOnScreen = m_isOnScreen;
    if (!wasOnScreen) {
      sendMessage(DESKFLOW_MSG_ENTER, 0, 0);
    }

    // always sync keys when switching desks to ensure keyboard modifier
    // states are correct.
    LOG_DEBUG("switched to desk \"%ls\"", name.c_str());
    bool syncKeys = false;
    if (isDeskAccessible(desk)) {
      LOG_DEBUG("desktop is accessible, syncing keyboard state after desk switch");
      syncKeys = true;
    } else {
      LOG_DEBUG("desktop is inaccessible");
    }

    // switch desk
    m_activeDesk = desk;
    m_activeDeskName = name;
    sendMessage(DESKFLOW_MSG_SWITCH, 0, 0);

    // hide cursor on new desk
    if (!wasOnScreen) {
      sendMessage(DESKFLOW_MSG_LEAVE, (WPARAM)m_keyLayout, 0);
    }

    // update keys if necessary
    if (syncKeys) {
      updateKeys();
    }
  } else if (name != m_activeDeskName) {
    // screen saver might have started
    PostThreadMessage(m_threadID, DESKFLOW_MSG_SCREEN_SAVER, TRUE, 0);
  }
}

bool MSWindowsDesks::isDeskAccessible(const Desk *desk) const
{
  return (desk != nullptr && desk->m_desk != nullptr);
}

void MSWindowsDesks::waitForDesk() const
{
  MSWindowsDesks *self = const_cast<MSWindowsDesks *>(this);

  Lock lock(&m_mutex);
  while (!(bool)m_deskReady) {
    m_deskReady.wait();
  }
  self->m_deskReady = false;
}

void MSWindowsDesks::handleCheckDesk()
{
  checkDesk();

  // also check if screen saver is running if on a modern OS and
  // this is the primary screen.
  if (m_isPrimary) {
    BOOL running;
    SystemParametersInfo(SPI_GETSCREENSAVERRUNNING, 0, &running, FALSE);
    PostThreadMessage(m_threadID, DESKFLOW_MSG_SCREEN_SAVER, running, 0);
  }
}

HDESK
MSWindowsDesks::openInputDesktop()
{
  return OpenInputDesktop(DF_ALLOWOTHERACCOUNTHOOK, TRUE, DESKTOP_CREATEWINDOW | DESKTOP_HOOKCONTROL | GENERIC_WRITE);
}

void MSWindowsDesks::closeDesktop(HDESK desk)
{
  if (desk != nullptr) {
    CloseDesktop(desk);
  }
}

std::wstring MSWindowsDesks::getDesktopName(HDESK desk)
{
  if (desk == nullptr) {
    return std::wstring();
  } else {
    DWORD size;
    GetUserObjectInformation(desk, UOI_NAME, nullptr, 0, &size);
    TCHAR *name = (TCHAR *)alloca(size + sizeof(TCHAR));
    GetUserObjectInformation(desk, UOI_NAME, name, size, &size);
    std::wstring result(name);
    return result;
  }
}

HWND MSWindowsDesks::getForegroundWindow() const
{
  // Ideally we'd return nullptr as much as possible, only returning
  // the actual foreground window when we know it's going to mess
  // up our keyboard input.  For now we'll just let the user
  // decide.
  if (m_leaveForegroundOption) {
    return nullptr;
  }
  return GetForegroundWindow();
}
