#pragma once

#include <Windows.h>

// Breaks the WM_ACTIVATEAPP feedback loop between the RTX Remix bridge client
// and the game.
//
// The bridge client subclasses the game window and, on every
// WM_ACTIVATEAPP(TRUE) it sees, posts another WM_ACTIVATEAPP(TRUE) back to the
// window. The game's own handler reacts to activation with
// SetWindowPos(HWND_TOPMOST) + ShowWindow(SW_MAXIMIZE) + SendMessage(WM_SIZE),
// so the message queue never drains and the game never reaches its render loop
// (window looks "responding", Remix logs simply stop).
//
// We subclass the window on top of the bridge and drop repeated
// WM_ACTIVATEAPP messages whose state did not change. Neither the bridge nor
// the game sees the duplicates, so nothing gets re-posted.
//
// Call after any device call that may let the bridge (re)subclass the window:
// CreateDevice/CreateDeviceEx/Reset/ResetEx.
void InstallActivateAppFilter(HWND hwnd);
