#include "screenshot.h"

#ifdef _WIN32
#include "common.h"

#include <objidl.h>  // gdiplus.h needs IStream and friends from here
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

#include <cstdio>
#include <fstream>
#include <iterator>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace {

bool GetPngEncoderClsid(CLSID* outClsid) {
    UINT count = 0;
    UINT bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || bytes == 0) return false;
    std::vector<BYTE> buffer(bytes);
    auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    if (Gdiplus::GetImageEncoders(count, bytes, codecs) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
            *outClsid = codecs[i].Clsid;
            return true;
        }
    }
    return false;
}

// Writes an HBITMAP to a PNG file, optionally downscaled to maxWidth.
bool SaveBitmapPng(HBITMAP bmp, const std::wstring& path, int maxWidth, std::string* error) {
    Gdiplus::GdiplusStartupInput startupInput;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startupInput, nullptr) != Gdiplus::Ok) {
        if (error) *error = "GDI+ startup failed";
        return false;
    }
    bool ok = false;
    CLSID pngClsid = {};
    if (!GetPngEncoderClsid(&pngClsid)) {
        if (error) *error = "PNG encoder not available";
    } else {
        Gdiplus::Bitmap source(bmp, nullptr);
        const int srcW = static_cast<int>(source.GetWidth());
        const int srcH = static_cast<int>(source.GetHeight());
        if (maxWidth > 0 && srcW > maxWidth) {
            int dstH = static_cast<int>(static_cast<long long>(srcH) * maxWidth / srcW);
            if (dstH < 1) dstH = 1;
            Gdiplus::Bitmap scaled(maxWidth, dstH, PixelFormat32bppARGB);
            {
                Gdiplus::Graphics g(&scaled);
                g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                g.DrawImage(&source, 0, 0, maxWidth, dstH);
            }
            ok = scaled.Save(path.c_str(), &pngClsid, nullptr) == Gdiplus::Ok;
        } else {
            ok = source.Save(path.c_str(), &pngClsid, nullptr) == Gdiplus::Ok;
        }
        if (!ok && error) *error = "failed to write PNG file";
    }
    Gdiplus::GdiplusShutdown(token);
    return ok;
}

}  // namespace

bool CaptureWindowPng(HWND hwnd, const std::wstring& path, int maxWidth,
                      std::vector<unsigned char>* pngOut, std::string* error) {
    if (!hwnd || !IsWindow(hwnd)) {
        if (error) *error = "window not found";
        return false;
    }
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
        if (error) *error = "window has no client area";
        return false;
    }

    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    HBITMAP bmp = CreateCompatibleBitmap(screenDc, w, h);
    HGDIOBJ oldBmp = SelectObject(memDc, bmp);
    // Ask the window to render itself; if that fails, fall back to copying what is on screen.
    if (!PrintWindow(hwnd, memDc, PW_CLIENTONLY | PW_RENDERFULLCONTENT)) {
        POINT origin = {0, 0};
        ClientToScreen(hwnd, &origin);
        BitBlt(memDc, 0, 0, w, h, screenDc, origin.x, origin.y, SRCCOPY);
    }
    SelectObject(memDc, oldBmp);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);

    const bool ok = SaveBitmapPng(bmp, path, maxWidth, error);
    DeleteObject(bmp);
    if (!ok) return false;

    if (pngOut) {
        std::ifstream in(path, std::ios::binary);
        pngOut->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (pngOut->empty()) {
            if (error) *error = "failed to read back PNG file";
            return false;
        }
    }
    return true;
}

std::wstring SnapshotDirectory() {
    const std::wstring dir = pc::GetLogsDirectoryWide() + L"\\screenshots";
    CreateDirectoryW(dir.c_str(), nullptr);  // no-op if it already exists
    return dir;
}

std::wstring NewSnapshotPath(const wchar_t* prefix) {
    SYSTEMTIME lt = {};
    GetLocalTime(&lt);
    wchar_t name[160] = {};
    swprintf_s(name, L"%ls_%04d%02d%02d_%02d%02d%02d_%03d.png", prefix,
               lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond, lt.wMilliseconds);
    return SnapshotDirectory() + L"\\" + name;
}

std::string Base64Encode(const std::vector<unsigned char>& data) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const unsigned v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back(kAlphabet[(v >> 6) & 63]);
        out.push_back(kAlphabet[v & 63]);
    }
    if (i + 1 == data.size()) {
        const unsigned v = data[i] << 16;
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out += "==";
    } else if (i + 2 == data.size()) {
        const unsigned v = (data[i] << 16) | (data[i + 1] << 8);
        out.push_back(kAlphabet[(v >> 18) & 63]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back(kAlphabet[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

#endif  // _WIN32
