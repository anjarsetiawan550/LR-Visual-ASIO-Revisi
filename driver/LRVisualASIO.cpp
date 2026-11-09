// LR Visual ASIO - driver ASIO virtual (WDM/WASAPI -> ASIO), 64-bit.
// Mode normal : WASAPI Exclusive event-driven (latensi terendah, seperti driver interface).
// Safe mode   : WASAPI Shared + auto-konversi (kompatibel, tidak mengunci perangkat).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <olectl.h>
#include <initguid.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <shellapi.h>
#include <atomic>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <new>
#include <cstdio>
#include <cstdarg>
#include "sdk/asiosys.h"   // Steinberg ASIO SDK 2.3 (resmi)
#include "sdk/asio.h"
#include "sdk/iasiodrv.h"

// {B1E7C0A4-5D3F-4C2E-9A61-7F8D2E4C9B10}
static const GUID CLSID_LRVisualASIO = { 0xb1e7c0a4, 0x5d3f, 0x4c2e, { 0x9a, 0x61, 0x7f, 0x8d, 0x2e, 0x4c, 0x9b, 0x10 } };
static const wchar_t* kClsid = L"{B1E7C0A4-5D3F-4C2E-9A61-7F8D2E4C9B10}";
static const wchar_t* kName = L"LR Visual ASIO";
static HINSTANCE g_hInst = nullptr;
static std::atomic<long> g_objs{ 0 };
class LRAsio;
static LRAsio* volatile g_owner = nullptr;   // instance pemegang status bersama (DAW bisa membuat >1 instance)

// ---------------------------------------------------------------- log diagnosa: %LOCALAPPDATA%\LRVisual\driver.log
static std::wstring LogPath() {
    wchar_t dir[MAX_PATH]; DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH); if (!n || n >= MAX_PATH) return L"";
    std::wstring d = std::wstring(dir) + L"\\LRVisual"; CreateDirectoryW(d.c_str(), nullptr); return d + L"\\driver.log";
}
static void LogRotate() {
    std::wstring p = LogPath(); WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!p.empty() && GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 200000) DeleteFileW(p.c_str());
}
static void Log(const char* fmt, ...) {
    std::wstring p = LogPath(); if (p.empty()) return;
    FILE* f = _wfopen(p.c_str(), L"a"); if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t); fprintf(f, "%02d:%02d:%02d.%03d  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f);
}
static LONG Peak16(const int32_t* d, size_t n) { int64_t m = 0; for (size_t i = 0; i < n; ++i) { int64_t v = d[i]; if (v < 0) v = -v; if (v > m) m = v; } return (LONG)(m >> 16); }

// ---------------------------------------------------------------- status bersama (dibaca GUI)
struct SharedStatus { volatile LONG loaded, active, sampleRate, bufferSize, inLatency, outLatency, inCh, outCh, safeMode, heartbeat, xruns, inPeak, outPeak, inOpen, fallback, outFrames, inFrames; };

// ---------------------------------------------------------------- konfigurasi (ditulis GUI)
struct Config {
    int sampleRate = 48000, bufferSize = 128; bool safe = false, force = false; std::wstring outId, inId;
    bool operator==(const Config& o) const { return sampleRate == o.sampleRate && bufferSize == o.bufferSize && safe == o.safe && outId == o.outId && inId == o.inId; }
};
static const int kRates[6] = { 44100, 48000, 88200, 96000, 176400, 192000 };
static int NormBuf(long b) { int v = (int)std::clamp<long>(b, 32, 2048), p = 32; while (p * 2 <= v) p *= 2; return p; }
static void SaveBufferSize(int v, DWORD force) {   // DAW mengubah buffer -> simpan agar aplikasi LR Visual ASIO ikut berubah
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\LRVisual\\ASIO", 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        DWORD d = (DWORD)v; RegSetValueExW(k, L"BufferSize", 0, REG_DWORD, (const BYTE*)&d, sizeof(d)); RegSetValueExW(k, L"BufferForce", 0, REG_DWORD, (const BYTE*)&force, sizeof(force)); RegCloseKey(k);
    }
}
static bool ValidRate(double r) { for (int x : kRates) if (std::abs(r - x) < 0.5) return true; return false; }
static DWORD RegDw(HKEY k, const wchar_t* n, DWORD def) { DWORD v = 0, sz = 4, t = 0; return (RegQueryValueExW(k, n, nullptr, &t, (LPBYTE)&v, &sz) == ERROR_SUCCESS && t == REG_DWORD) ? v : def; }
static std::wstring RegStr(HKEY k, const wchar_t* n) {
    wchar_t buf[512] = {}; DWORD sz = sizeof(buf) - 2, t = 0;
    if (RegQueryValueExW(k, n, nullptr, &t, (LPBYTE)buf, &sz) == ERROR_SUCCESS && t == REG_SZ) return buf;
    return L"";
}
static Config LoadConfig() {
    Config c; HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\LRVisual\\ASIO", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        c.sampleRate = (int)RegDw(k, L"SampleRate", 48000); c.bufferSize = (int)RegDw(k, L"BufferSize", 128);
        c.safe = RegDw(k, L"SafeMode", 0) != 0; c.force = RegDw(k, L"BufferForce", 0) != 0; c.outId = RegStr(k, L"OutputDevice"); c.inId = RegStr(k, L"InputDevice");
        RegCloseKey(k);
    }
    if (!ValidRate(c.sampleRate)) c.sampleRate = 48000;
    int b = std::clamp(c.bufferSize, 32, 2048), p = 32; while (p * 2 <= b) p *= 2; c.bufferSize = p;
    return c;
}

// ---------------------------------------------------------------- util audio
static uint64_t NowNs() { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart); }
static void ComInit() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); } // sengaja tanpa Uninitialize (objek WASAPI hidup lintas thread)
static IMMDeviceEnumerator* Enum() { IMMDeviceEnumerator* e = nullptr; CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&e); return e; }
static IMMDevice* GetDev(IMMDeviceEnumerator* en, EDataFlow fl, const std::wstring& id) {
    IMMDevice* d = nullptr;
    if (!id.empty() && SUCCEEDED(en->GetDevice(id.c_str(), &d))) { DWORD s = 0; if (SUCCEEDED(d->GetState(&s)) && s == DEVICE_STATE_ACTIVE) return d; d->Release(); d = nullptr; }
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(fl, eConsole, &d))) return d;
    return nullptr;
}
static int ChannelsOf(IMMDeviceEnumerator* en, EDataFlow fl, const std::wstring& id) {
    IMMDevice* d = GetDev(en, fl, id); if (!d) return 0; int n = 0; IAudioClient* c = nullptr;
    if (SUCCEEDED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&c))) {
        WAVEFORMATEX* m = nullptr; if (SUCCEEDED(c->GetMixFormat(&m)) && m) { n = std::clamp<int>(m->nChannels, 1, 8); CoTaskMemFree(m); } c->Release();
    }
    d->Release(); return n;
}
static void MakeFmt(WAVEFORMATEXTENSIBLE& w, int sr, int ch, int container, int valid, bool flt) {
    memset(&w, 0, sizeof(w)); w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; w.Format.nChannels = (WORD)ch; w.Format.nSamplesPerSec = sr;
    w.Format.wBitsPerSample = (WORD)container; w.Format.nBlockAlign = (WORD)(ch * container / 8); w.Format.nAvgBytesPerSec = sr * w.Format.nBlockAlign;
    w.Format.cbSize = 22; w.Samples.wValidBitsPerSample = (WORD)valid;
    w.dwChannelMask = ch == 1 ? SPEAKER_FRONT_CENTER : ch == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : ((1u << ch) - 1);
    w.SubFormat = flt ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
}

enum class Kind { I16, I24, I32, F32 };
// int32 interleaved (ASIO) -> format perangkat
static void ToDev(const int32_t* s, BYTE* d, size_t frames, int srcCh, int dstCh, Kind k, int valid) {
    for (size_t f = 0; f < frames; ++f) for (int c = 0; c < dstCh; ++c) {
        int32_t v = c < srcCh ? s[f * srcCh + c] : 0; size_t i = f * dstCh + c;
        switch (k) {
        case Kind::I32: ((int32_t*)d)[i] = valid == 24 ? (int32_t)((uint32_t)v & 0xFFFFFF00u) : v; break;
        case Kind::I24: { BYTE* p = d + i * 3; uint32_t u = (uint32_t)v; p[0] = (BYTE)(u >> 8); p[1] = (BYTE)(u >> 16); p[2] = (BYTE)(u >> 24); break; }
        case Kind::I16: ((int16_t*)d)[i] = (int16_t)(v >> 16); break;
        case Kind::F32: ((float*)d)[i] = (float)((double)v / 2147483648.0); break;
        }
    }
}
// format perangkat -> int32 interleaved (ASIO)
static void FromDev(const BYTE* s, int32_t* d, size_t frames, int srcCh, int dstCh, Kind k) {
    for (size_t f = 0; f < frames; ++f) for (int c = 0; c < dstCh; ++c) {
        int32_t v = 0;
        if (c < srcCh) {
            size_t i = f * srcCh + c;
            switch (k) {
            case Kind::I32: v = ((const int32_t*)s)[i]; break;
            case Kind::I24: { const BYTE* p = s + i * 3; v = (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24)); break; }
            case Kind::I16: v = (int32_t)((uint32_t)(uint16_t)((const int16_t*)s)[i] << 16); break;
            case Kind::F32: { double x = ((const float*)s)[i]; x = std::clamp(x, -1.0, 1.0); v = (int32_t)(x * 2147483647.0); break; }
            }
        }
        d[f * dstCh + c] = v;
    }
}

// ---------------------------------------------------------------- ring buffer frame (SPSC)
struct Ring {
    std::vector<int32_t> d; size_t cap = 0; int ch = 0; std::atomic<uint64_t> r{ 0 }, w{ 0 };
    void init(size_t frames, int c) { cap = frames ? frames : 1; ch = c; d.assign(cap * (size_t)std::max(c, 1), 0); r = 0; w = 0; }
    size_t avail() const { return (size_t)(w.load(std::memory_order_acquire) - r.load(std::memory_order_acquire)); }
    size_t space() const { return cap - avail(); }
    void push(const int32_t* s, size_t n) {
        uint64_t wp = w.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) memcpy(&d[(size_t)((wp + i) % cap) * ch], s + i * ch, (size_t)ch * 4);
        w.store(wp + n, std::memory_order_release);
    }
    size_t pop(int32_t* dst, size_t n) {
        size_t k = std::min(avail(), n); uint64_t rp = r.load(std::memory_order_relaxed);
        for (size_t i = 0; i < k; ++i) memcpy(dst + i * ch, &d[(size_t)((rp + i) % cap) * ch], (size_t)ch * 4);
        r.store(rp + k, std::memory_order_release);
        if (k < n) memset(dst + k * ch, 0, (n - k) * (size_t)ch * 4);
        return k;
    }
    void skip(size_t n) { r.store(r.load(std::memory_order_relaxed) + n, std::memory_order_release); }
};

// ---------------------------------------------------------------- satu aliran WASAPI
struct Stream {
    IMMDevice* dev = nullptr; IAudioClient* ac = nullptr; IAudioRenderClient* rc = nullptr; IAudioCaptureClient* cc = nullptr;
    HANDLE evt = nullptr; UINT32 frames = 0; int devCh = 0; Kind kind = Kind::I32; int valid = 32; bool exclusive = false, render = true;

    void CloseClient() {
        if (rc) { rc->Release(); rc = nullptr; } if (cc) { cc->Release(); cc = nullptr; } if (ac) { ac->Release(); ac = nullptr; }
        if (evt) { CloseHandle(evt); evt = nullptr; } frames = 0;
    }
    void Close() { CloseClient(); if (dev) { dev->Release(); dev = nullptr; } }

    const char* T() const { return render ? "OUT" : "IN"; }
    bool Open(IMMDeviceEnumerator* en, const std::wstring& id, bool isRender, int sr, int B, bool wantEx) {
        Close(); render = isRender; dev = GetDev(en, isRender ? eRender : eCapture, id);
        if (!dev) { Log("%s: perangkat tidak ditemukan", T()); return false; }
        for (int a = wantEx ? 0 : 1; a < 2; ++a) { // exclusive gagal -> otomatis shared
            if (TryInit(a == 0, sr, B)) { Log("%s: OK mode=%s ch=%d frames=%u kind=%d valid=%d sr=%d", T(), exclusive ? "EXCLUSIVE" : "SHARED", devCh, (unsigned)frames, (int)kind, valid, sr); return true; }
            Log("%s: mode %s gagal", T(), a == 0 ? "EXCLUSIVE" : "SHARED"); CloseClient();
        }
        return false;
    }
    bool TryInit(bool ex, int sr, int B) {
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac))) return false;
        WAVEFORMATEX* mix = nullptr; ac->GetMixFormat(&mix); int mixCh = mix ? mix->nChannels : 2; if (mix) CoTaskMemFree(mix);
        mixCh = std::clamp(mixCh, 1, 8);
        REFERENCE_TIME defP = 0, minP = 0; ac->GetDevicePeriod(&defP, &minP);
        WAVEFORMATEXTENSIBLE wf{}; HRESULT hr = E_FAIL; int useCh = mixCh;
        if (ex) {
            static const struct { int c, v; bool f; Kind k; } C[] = { {32,32,false,Kind::I32}, {32,24,false,Kind::I32}, {24,24,false,Kind::I24}, {16,16,false,Kind::I16}, {32,32,true,Kind::F32} };
            int chs[2] = { mixCh, 2 }; bool found = false;
            for (int ci = 0; ci < 2 && !found; ++ci) {
                if (ci == 1 && chs[1] == chs[0]) break;
                for (int i = 0; i < 5 && !found; ++i) {
                    MakeFmt(wf, sr, chs[ci], C[i].c, C[i].v, C[i].f);
                    if (ac->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX*)&wf, nullptr) == S_OK) { found = true; useCh = chs[ci]; kind = C[i].k; valid = C[i].v; }
                }
            }
            if (!found) { Log("%s: format exclusive tidak didukung (mixCh=%d sr=%d)", T(), mixCh, sr); return false; }
            auto tryPer = [&](REFERENCE_TIME per) -> HRESULT {
                if (!ac && FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac))) return E_FAIL;
                HRESULT h = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, per, per, (WAVEFORMATEX*)&wf, nullptr);
                if (h == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                    UINT32 f = 0; ac->GetBufferSize(&f); ac->Release(); ac = nullptr;
                    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac))) return E_FAIL;
                    REFERENCE_TIME p2 = (REFERENCE_TIME)(1e7 * f / sr + 0.5);
                    h = ac->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, p2, p2, (WAVEFORMATEX*)&wf, nullptr);
                }
                if (FAILED(h) && ac) { ac->Release(); ac = nullptr; }
                return h;
            };
            REFERENCE_TIME per = (REFERENCE_TIME)(1e7 * B / sr + 0.5); if (per < minP) per = minP;
            Log("%s: exclusive periode diminta=%.2f ms (minimum perangkat=%.2f ms, default=%.2f ms)", T(), per / 10000.0, minP / 10000.0, defP / 10000.0);
            hr = tryPer(per); if (FAILED(hr) && per != defP) hr = tryPer(defP);
        } else {
            bool done = false;
            IAudioClient3* c3 = nullptr;   // Windows 10+: shared mode latensi rendah (periode kecil, tanpa resampler)
            if (SUCCEEDED(ac->QueryInterface(__uuidof(IAudioClient3), (void**)&c3)) && c3) {
                WAVEFORMATEX* m = nullptr;
                if (SUCCEEDED(ac->GetMixFormat(&m)) && m) {
                    bool flt = m->wBitsPerSample == 32 && (m->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || (m->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ((WAVEFORMATEXTENSIBLE*)m)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
                    UINT32 defF = 0, fund = 0, minF = 0, maxF = 0;
                    if (flt && (int)m->nSamplesPerSec == sr && SUCCEEDED(c3->GetSharedModeEnginePeriod(m, &defF, &fund, &minF, &maxF)) && fund > 0) {
                        UINT32 want = std::min<UINT32>(std::max<UINT32>((UINT32)B, minF), maxF);
                        want = ((want + fund - 1) / fund) * fund; want = std::min<UINT32>(std::max<UINT32>(want, minF), maxF);
                        if (SUCCEEDED(c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, want, m, nullptr))) {
                            kind = Kind::F32; valid = 32; useCh = m->nChannels; hr = S_OK; done = true;
                            Log("%s: shared latensi-rendah periode=%u smp (min=%u max=%u)", T(), want, minF, maxF);
                        }
                    }
                    CoTaskMemFree(m);
                }
                c3->Release();
                if (!done) { ac->Release(); ac = nullptr; if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac))) return false; }
            }
            if (!done) {
                MakeFmt(wf, sr, mixCh, 32, 32, true); kind = Kind::F32; valid = 32; useCh = mixCh;
                hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, defP, 0, (WAVEFORMATEX*)&wf, nullptr);
                Log("%s: shared biasa (periode default=%.2f ms)", T(), defP / 10000.0);
            }
        }
        if (FAILED(hr) || !ac) { Log("%s: Initialize gagal hr=0x%08X", T(), (unsigned)hr); return false; }
        evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!evt || FAILED(ac->SetEventHandle(evt)) || FAILED(ac->GetBufferSize(&frames))) return false;
        HRESULT hs = render ? ac->GetService(__uuidof(IAudioRenderClient), (void**)&rc) : ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cc);
        if (FAILED(hs)) return false;
        devCh = useCh; exclusive = ex; return true;
    }
};

// ---------------------------------------------------------------- driver ASIO
class LRAsio : public IASIO {
    LONG ref_ = 1;
    Config cfg; ASIOCallbacks* cb = nullptr; bool timeInfo = false, inited = false, buffersCreated = false, inOk = false;
    long B = 128, bufMin = 32, bufMax = 2048, bufPref = 128; double sr = 48000; int inCh = 0, outCh = 0; long latIn = 0, latOut = 0; std::string err;
    struct Chan { bool active = false; std::vector<int32_t> mem; int32_t* p[2] = { nullptr, nullptr }; };
    std::vector<Chan> ins, outs; std::vector<int32_t> inTmp, outTmp;
    Stream out, in; Ring inRing, outRing;
    std::atomic<bool> running{ false }; HANDLE stopEvt = nullptr, thR = nullptr, thC = nullptr;
    std::atomic<uint64_t> samplePos{ 0 }, lastNs{ 0 }; long curIdx = 0;
    HANDLE svcStop = nullptr, thS = nullptr, hMap = nullptr; SharedStatus* st = nullptr;

    void SetErr(const char* s) { err = s; }
    void CreateStatus() {
        SECURITY_DESCRIPTOR sd; InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION); SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);
        SECURITY_ATTRIBUTES sa{ sizeof(sa), &sd, FALSE };
        hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, 128, L"LRVisualASIO_Status");
        bool existed = GetLastError() == ERROR_ALREADY_EXISTS;   // instance lain sudah membuatnya: jangan dikosongkan
        if (hMap) { st = (SharedStatus*)MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, 128); if (st && !existed) memset((void*)st, 0, sizeof(*st)); }
    }
    bool CanWrite() const { return st && (!g_owner || g_owner == this); }
    void Publish() {
        if (!st) return;
        if (buffersCreated) g_owner = this;
        if (!CanWrite()) return;
        st->inOpen = inOk ? 1 : 0;
        st->fallback = (!cfg.safe && buffersCreated && !out.exclusive) ? 1 : 0;   // exclusive gagal -> turun ke shared
        st->outFrames = buffersCreated ? (LONG)out.frames : 0; st->inFrames = (buffersCreated && inOk) ? (LONG)in.frames : 0;
        st->sampleRate = (LONG)sr; st->bufferSize = B; st->inLatency = latIn; st->outLatency = latOut; st->inCh = inCh; st->outCh = outCh;
        st->safeMode = (cfg.safe || (buffersCreated && !out.exclusive)) ? 1 : 0; st->active = running ? 1 : 0; st->loaded = inited ? 1 : 0;
    }
    bool OpenStreams() {
        ComInit(); IMMDeviceEnumerator* en = Enum(); if (!en) { SetErr("Gagal membuat enumerator audio."); return false; }
        bool ok = out.Open(en, cfg.outId, true, (int)sr, B, !cfg.safe);
        inOk = false; if (ok && inCh > 0) { inOk = in.Open(en, cfg.inId, false, (int)sr, B, !cfg.safe); if (!inOk) in.Close(); }
        en->Release();
        if (ok && inCh > 0 && !inOk) Log("IN: gagal dibuka, input dibisukan");
        if (!ok) { SetErr("Gagal membuka perangkat output (sedang dipakai aplikasi lain atau format tidak didukung)."); return false; }
        outRing.init((size_t)(out.frames + B) * 2 + 64, outCh);
        inRing.init((size_t)std::max<long>(B, inOk ? (long)in.frames : 0) * 8 + 64, inCh);
        latOut = B + (long)out.frames; latIn = B + (inOk ? (long)in.frames : 0);
        return true;
    }
    void CloseStreams() { out.Close(); in.Close(); inOk = false; }

    void RunHostBlock() {
        long idx = curIdx;
        if (inCh > 0) {
            if (inOk) {
                size_t a = inRing.avail(), keep = (size_t)B + in.frames; if (a > keep * 3) inRing.skip(a - keep); // drift clock: buang kelebihan
                if (inRing.pop(inTmp.data(), (size_t)B) < (size_t)B && st) InterlockedIncrement(&st->xruns);
                for (int c = 0; c < inCh; ++c) if (ins[c].active) { int32_t* d = ins[c].p[idx]; for (long i = 0; i < B; ++i) d[i] = inTmp[(size_t)i * inCh + c]; }
            } else for (int c = 0; c < inCh; ++c) if (ins[c].active) memset(ins[c].p[idx], 0, (size_t)B * 4);
            if (inOk && CanWrite()) { LONG pk = Peak16(inTmp.data(), (size_t)B * inCh); if (pk > st->inPeak) st->inPeak = pk; }
        }
        if (cb) {
            if (timeInfo) {
                ASIOTime t; memset(&t, 0, sizeof(t)); uint64_t p = samplePos.load(), ns = NowNs();
                t.timeInfo.speed = 1.0; t.timeInfo.systemTime.hi = (unsigned long)(ns >> 32); t.timeInfo.systemTime.lo = (unsigned long)(ns & 0xFFFFFFFFu);
                t.timeInfo.samplePosition.hi = (unsigned long)(p >> 32); t.timeInfo.samplePosition.lo = (unsigned long)(p & 0xFFFFFFFFu);
                t.timeInfo.sampleRate = sr; t.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
                cb->bufferSwitchTimeInfo(&t, idx, ASIOTrue);
            } else cb->bufferSwitch(idx, ASIOTrue);
        }
        for (int c = 0; c < outCh; ++c) {
            if (outs[c].active) { const int32_t* s = outs[c].p[idx]; for (long i = 0; i < B; ++i) outTmp[(size_t)i * outCh + c] = s[i]; }
            else for (long i = 0; i < B; ++i) outTmp[(size_t)i * outCh + c] = 0;
        }
        if (CanWrite()) { LONG pk = Peak16(outTmp.data(), (size_t)B * outCh); if (pk > st->outPeak) st->outPeak = pk; }
        outRing.push(outTmp.data(), (size_t)B);
        samplePos += (uint64_t)B; lastNs = NowNs(); curIdx ^= 1;
    }

    static HANDLE Boost() { DWORD ti = 0; HANDLE h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &ti); if (h) AvSetMmThreadPriority(h, AVRT_PRIORITY_CRITICAL); else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL); return h; }

    void RenderLoop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED); HANDLE mm = Boost();
        std::vector<int32_t> tmp((size_t)out.frames * outCh);
        { BYTE* d = nullptr; if (SUCCEEDED(out.rc->GetBuffer(out.frames, &d))) out.rc->ReleaseBuffer(out.frames, AUDCLNT_BUFFERFLAGS_SILENT); }
        out.ac->Start();
        HANDLE hs[2] = { out.evt, stopEvt };
        while (running) {
            DWORD w = WaitForMultipleObjects(2, hs, FALSE, 1000);
            if (w == WAIT_OBJECT_0 + 1) break;
            if (w != WAIT_OBJECT_0) { if (st) InterlockedIncrement(&st->xruns); continue; }
            UINT32 n = out.frames;
            if (!out.exclusive) { UINT32 pad = 0; if (FAILED(out.ac->GetCurrentPadding(&pad))) continue; n = out.frames - pad; }
            if (n == 0) continue;
            while (outRing.avail() < n) RunHostBlock();
            BYTE* d = nullptr;
            if (SUCCEEDED(out.rc->GetBuffer(n, &d))) { outRing.pop(tmp.data(), n); ToDev(tmp.data(), d, n, outCh, out.devCh, out.kind, out.valid); out.rc->ReleaseBuffer(n, 0); }
            if (st) InterlockedIncrement(&st->heartbeat);
        }
        out.ac->Stop(); if (mm) AvRevertMmThreadCharacteristics(mm); CoUninitialize();
    }
    void CaptureLoop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED); HANDLE mm = Boost();
        std::vector<int32_t> tmp; bool warned = false; in.ac->Start(); HANDLE hs[2] = { in.evt, stopEvt };
        while (running) {
            DWORD w = WaitForMultipleObjects(2, hs, FALSE, 1000);
            if (w == WAIT_OBJECT_0 + 1) break; if (w != WAIT_OBJECT_0) continue;
            for (;;) {
                UINT32 n = 0; if (FAILED(in.cc->GetNextPacketSize(&n)) || n == 0) break;
                BYTE* d = nullptr; DWORD fl = 0; if (FAILED(in.cc->GetBuffer(&d, &n, &fl, nullptr, nullptr))) break;
                tmp.resize((size_t)n * inCh);
                if (fl & AUDCLNT_BUFFERFLAGS_SILENT) { std::fill(tmp.begin(), tmp.end(), 0); if (!warned) { warned = true; Log("IN: perangkat mengirim paket SILENT (cek izin mikrofon Windows / mute)"); } }
                else FromDev(d, tmp.data(), n, in.devCh, inCh, in.kind);
                inRing.push(tmp.data(), std::min<size_t>(n, inRing.space()));
                in.cc->ReleaseBuffer(n);
            }
        }
        in.ac->Stop(); if (mm) AvRevertMmThreadCharacteristics(mm); CoUninitialize();
    }
    static DWORD WINAPI RenderThunk(LPVOID p) { ((LRAsio*)p)->RenderLoop(); return 0; }
    static DWORD WINAPI CaptureThunk(LPVOID p) { ((LRAsio*)p)->CaptureLoop(); return 0; }
    static DWORD WINAPI ServiceThunk(LPVOID p) { ((LRAsio*)p)->ServiceLoop(); return 0; }

    // thread layanan: detak status + pantau perubahan pengaturan dari GUI -> minta DAW reset driver
    void ServiceLoop() {
        HKEY k = nullptr; RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\LRVisual\\ASIO", 0, nullptr, 0, KEY_READ | KEY_NOTIFY, nullptr, &k, nullptr);
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (k) RegNotifyChangeKeyValue(k, TRUE, REG_NOTIFY_CHANGE_LAST_SET, ev, TRUE);
        HANDLE hs[2] = { svcStop, ev };
        for (int tick = 0;; ++tick) {
            DWORD w = WaitForMultipleObjects(k ? 2 : 1, hs, FALSE, 50);
            if (w == WAIT_OBJECT_0) break;
            if (CanWrite()) { InterlockedIncrement(&st->heartbeat); st->inPeak = st->inPeak * 7 / 8; st->outPeak = st->outPeak * 7 / 8; }
            if (tick % 5 == 0) Publish();
            if (w == WAIT_OBJECT_0 + 1) {
                Sleep(300); RegNotifyChangeKeyValue(k, TRUE, REG_NOTIFY_CHANGE_LAST_SET, ev, TRUE);
                if (!(LoadConfig() == cfg) && cb && buffersCreated && cb->asioMessage) {
                    cb->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
                }
            }
        }
        if (k) RegCloseKey(k); CloseHandle(ev);
    }
    void DisposeInternal() { CloseStreams(); ins.clear(); outs.clear(); buffersCreated = false; cb = nullptr; Publish(); }
    void Shutdown() {
        stop(); DisposeInternal();
        if (thS) { SetEvent(svcStop); WaitForSingleObject(thS, 3000); CloseHandle(thS); thS = nullptr; }
        if (svcStop) { CloseHandle(svcStop); svcStop = nullptr; }
        if (stopEvt) { CloseHandle(stopEvt); stopEvt = nullptr; }
        if (g_owner == this) g_owner = nullptr;
        if (st) { if (g_objs.load() <= 1) memset((void*)st, 0, sizeof(*st)); UnmapViewOfFile((void*)st); st = nullptr; }
        if (hMap) { CloseHandle(hMap); hMap = nullptr; }
    }

public:
    LRAsio() { g_objs++; } ~LRAsio() { Shutdown(); g_objs--; }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == CLSID_LRVisualASIO) { *ppv = static_cast<IASIO*>(this); AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override { ULONG r = (ULONG)InterlockedDecrement(&ref_); if (!r) delete this; return r; }

    ASIOBool init(void*) override {
        if (inited) return ASIOTrue;
        LogRotate(); cfg = LoadConfig(); sr = cfg.sampleRate; B = cfg.bufferSize;
        ComInit(); IMMDeviceEnumerator* en = Enum();
        if (en) { outCh = ChannelsOf(en, eRender, cfg.outId); inCh = ChannelsOf(en, eCapture, cfg.inId);
            en->Release(); }
        Log("init: sr=%d buf=%d safe=%d outCh=%d inCh=%d", cfg.sampleRate, cfg.bufferSize, cfg.safe ? 1 : 0, outCh, inCh);
        if (outCh <= 0) { SetErr("Tidak ada perangkat output WDM yang aktif."); return ASIOFalse; }
        stopEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr); svcStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        CreateStatus(); thS = CreateThread(nullptr, 0, ServiceThunk, this, 0, nullptr);
        inited = true; Publish(); return ASIOTrue;
    }
    void getDriverName(char* name) override { strcpy(name, "LR Visual ASIO"); }
    long getDriverVersion() override { return 1; }
    void getErrorMessage(char* s) override { strncpy(s, err.c_str(), 123); s[123] = 0; }

    ASIOError start() override {
        if (!buffersCreated || !cb) return ASE_NotPresent; if (running) return ASE_OK;
        ResetEvent(stopEvt); samplePos = 0; curIdx = 0; running = true; Log("start: B=%ld sr=%.0f inOpen=%d", B, sr, inOk ? 1 : 0);
        inRing.init(inRing.cap, inCh); outRing.init(outRing.cap, outCh);
        if (inOk) thC = CreateThread(nullptr, 0, CaptureThunk, this, 0, nullptr);
        thR = CreateThread(nullptr, 0, RenderThunk, this, 0, nullptr);
        if (!thR) { running = false; return ASE_HWMalfunction; }
        Publish(); return ASE_OK;
    }
    ASIOError stop() override {
        if (running.exchange(false)) {
            SetEvent(stopEvt);
            if (thR) { WaitForSingleObject(thR, 3000); CloseHandle(thR); thR = nullptr; }
            if (thC) { WaitForSingleObject(thC, 3000); CloseHandle(thC); thC = nullptr; }
        }
        Publish(); return ASE_OK;
    }
    ASIOError getChannels(long* i, long* o) override { if (!i || !o) return ASE_InvalidParameter; *i = inCh; *o = outCh; return ASE_OK; }
    ASIOError getLatencies(long* i, long* o) override {
        if (!i || !o) return ASE_InvalidParameter;
        *i = buffersCreated ? latIn : B * 2; *o = buffersCreated ? latOut : B * 2; return ASE_OK;
    }
    ASIOError getBufferSize(long* mn, long* mx, long* pref, long* gran) override {
        if (!mn || !mx || !pref || !gran) return ASE_InvalidParameter; long cur = (long)std::clamp<long>(cfg.bufferSize, bufMin, bufMax);
        if (cfg.force) { *mn = *mx = *pref = cur; *gran = 0; }            // buffer baru dari aplikasi LR: DAW wajib mengikuti
        else { *mn = bufMin; *mx = bufMax; *pref = cur; *gran = -1; }     // normal: DAW bebas memilih
        return ASE_OK;   // host boleh memilih; kelipatan pangkat 2 (granularity -1)
    }
    ASIOError canSampleRate(ASIOSampleRate r) override { return ValidRate(r) ? ASE_OK : ASE_NoClock; }
    ASIOError getSampleRate(ASIOSampleRate* r) override { if (!r) return ASE_InvalidParameter; *r = sr; return ASE_OK; }
    ASIOError setSampleRate(ASIOSampleRate r) override {
        if (!ValidRate(r)) return ASE_NoClock; if (buffersCreated && std::abs(r - sr) > 0.5) return ASE_InvalidMode;
        sr = r; Publish(); return ASE_OK;
    }
    ASIOError getClockSources(ASIOClockSource* c, long* n) override {
        if (!c || !n) return ASE_InvalidParameter; c[0].index = 0; c[0].associatedChannel = -1; c[0].associatedGroup = -1; c[0].isCurrentSource = ASIOTrue;
        strcpy(c[0].name, "Internal"); *n = 1; return ASE_OK;
    }
    ASIOError setClockSource(long r) override { return r == 0 ? ASE_OK : ASE_InvalidParameter; }
    ASIOError getSamplePosition(ASIOSamples* p, ASIOTimeStamp* t) override {
        if (!p || !t) return ASE_InvalidParameter; if (!running) { p->hi = p->lo = t->hi = t->lo = 0; return ASE_SPNotAdvancing; }
        uint64_t s = samplePos.load(), n = lastNs.load();
        p->hi = (unsigned long)(s >> 32); p->lo = (unsigned long)(s & 0xFFFFFFFFu); t->hi = (unsigned long)(n >> 32); t->lo = (unsigned long)(n & 0xFFFFFFFFu); return ASE_OK;
    }
    ASIOError getChannelInfo(ASIOChannelInfo* i) override {
        if (!i) return ASE_InvalidParameter; int cnt = i->isInput ? inCh : outCh; if (i->channel < 0 || i->channel >= cnt) return ASE_InvalidParameter;
        auto& v = i->isInput ? ins : outs; i->isActive = ((size_t)i->channel < v.size() && v[i->channel].active) ? ASIOTrue : ASIOFalse;
        i->channelGroup = 0; i->type = ASIOSTInt32LSB; wsprintfA(i->name, i->isInput ? "LR In %d" : "LR Out %d", (int)i->channel + 1); return ASE_OK;
    }
    ASIOError createBuffers(ASIOBufferInfo* infos, long n, long bufSize, ASIOCallbacks* c) override {
        if (!inited || !infos || n <= 0 || !c) return ASE_InvalidParameter; if (buffersCreated) return ASE_InvalidMode;
        if (bufSize < bufMin || bufSize > bufMax) return ASE_InvalidParameter;
        { int nb = NormBuf(bufSize); if (nb != cfg.bufferSize || cfg.force) { cfg.bufferSize = nb; cfg.force = false; SaveBufferSize(nb, 0); Log("buffer dipakai DAW: %ld -> disimpan %d", bufSize, nb); } }   // cfg diubah DULU agar tidak memicu reset
        B = bufSize; cb = c; ins.assign(inCh, Chan()); outs.assign(outCh, Chan());
        for (long i = 0; i < n; ++i) {
            ASIOBufferInfo& bi = infos[i]; auto& v = bi.isInput ? ins : outs;
            if (bi.channelNum < 0 || bi.channelNum >= (long)v.size()) { DisposeInternal(); return ASE_InvalidParameter; }
            Chan& ch = v[bi.channelNum]; ch.mem.assign((size_t)B * 2, 0); ch.p[0] = ch.mem.data(); ch.p[1] = ch.mem.data() + B; ch.active = true;
            bi.buffers[0] = ch.p[0]; bi.buffers[1] = ch.p[1];
        }
        timeInfo = c->asioMessage && c->bufferSwitchTimeInfo && c->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1;
        inTmp.assign((size_t)B * std::max(inCh, 1), 0); outTmp.assign((size_t)B * std::max(outCh, 1), 0);
        if (!OpenStreams()) { DisposeInternal(); return ASE_HWMalfunction; }
        buffersCreated = true; Publish();
        Log("createBuffers OK: B=%ld sr=%.0f latIn=%ld latOut=%ld mode=%s", B, sr, latIn, latOut, out.exclusive ? "EXCLUSIVE" : "SHARED"); return ASE_OK;
    }
    ASIOError disposeBuffers() override { stop(); DisposeInternal(); return ASE_OK; }
    ASIOError controlPanel() override {
        HKEY k = nullptr; std::wstring dir;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\LRVisual\\ASIO", 0, KEY_READ, &k) == ERROR_SUCCESS) { dir = RegStr(k, L"InstallDir"); RegCloseKey(k); }
        if (dir.empty()) return ASE_NotPresent;
        ShellExecuteW(nullptr, L"open", (dir + L"\\LRVisualASIO.exe").c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL); return ASE_OK;
    }
    ASIOError future(long sel, void*) override {
        switch (sel) {
        case kAsioCanTimeInfo: return ASE_SUCCESS;   // bufferSwitchTimeInfo didukung
        default: return ASE_NotPresent;              // timecode / input monitor / transport tidak didukung
        }
    }
    ASIOError outputReady() override { return ASE_NotPresent; }
};

// ---------------------------------------------------------------- COM boilerplate
class Factory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID r, void** p) override { if (r == IID_IUnknown || r == IID_IClassFactory) { *p = this; return S_OK; } *p = nullptr; return E_NOINTERFACE; }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; } STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID r, void** p) override {
        if (outer) return CLASS_E_NOAGGREGATION; LRAsio* d = new (std::nothrow) LRAsio(); if (!d) return E_OUTOFMEMORY;
        HRESULT hr = d->QueryInterface(r, p); d->Release(); return hr;
    }
    STDMETHODIMP LockServer(BOOL) override { return S_OK; }
};
static Factory g_factory;

static bool SetKey(HKEY root, const std::wstring& path, const wchar_t* name, const wchar_t* val) {
    HKEY k = nullptr; if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    LONG r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val, (DWORD)((wcslen(val) + 1) * 2)); RegCloseKey(k); return r == ERROR_SUCCESS;
}
STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* ppv) { if (clsid == CLSID_LRVisualASIO) return g_factory.QueryInterface(riid, ppv); *ppv = nullptr; return CLASS_E_CLASSNOTAVAILABLE; }
STDAPI DllCanUnloadNow() { return g_objs == 0 ? S_OK : S_FALSE; }
STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH]; GetModuleFileNameW(g_hInst, path, MAX_PATH);
    std::wstring c = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsid;
    bool ok = SetKey(HKEY_LOCAL_MACHINE, c, nullptr, kName) && SetKey(HKEY_LOCAL_MACHINE, c + L"\\InprocServer32", nullptr, path) && SetKey(HKEY_LOCAL_MACHINE, c + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
    std::wstring a = std::wstring(L"SOFTWARE\\ASIO\\") + kName;
    ok = ok && SetKey(HKEY_LOCAL_MACHINE, a, L"CLSID", kClsid) && SetKey(HKEY_LOCAL_MACHINE, a, L"Description", kName);
    return ok ? S_OK : SELFREG_E_CLASS;
}
STDAPI DllUnregisterServer() {
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kClsid).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(L"SOFTWARE\\ASIO\\") + kName).c_str()); return S_OK;
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) { if (reason == DLL_PROCESS_ATTACH) { g_hInst = h; DisableThreadLibraryCalls(h); } return TRUE; }
