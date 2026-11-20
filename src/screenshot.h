#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <cstddef>
#include <string>
#include <vector>

// Renders the client area of `hwnd` (which may belong to another process) into a PNG file.
// maxWidth > 0 downscales the image (keeping aspect ratio) when it is wider than that.
// When pngOut is non-null, the saved file's bytes are returned there as well.
bool CaptureWindowPng(HWND hwnd, const std::wstring& path, int maxWidth,
                      std::vector<unsigned char>* pngOut, std::string* error);

// <logs>\screenshots, created if missing.
std::wstring SnapshotDirectory();

// <SnapshotDirectory>\<prefix>_YYYYMMDD_HHMMSS_mmm.png
std::wstring NewSnapshotPath(const wchar_t* prefix);

std::string Base64Encode(const std::vector<unsigned char>& data);

#endif  // _WIN32
