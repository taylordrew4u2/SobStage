#pragma once

// A stand-in for the Windows SDK headers WasapiAsioBackend.cpp includes,
// covering exactly the surface it uses.
//
// The aim is not a Windows emulator. It is to let the real, unmodified backend
// source compile and run on a machine that has no Windows, driven by a virtual
// audio endpoint layer (FakeWasapi.h) configurable to behave like the hardware
// people actually own -- in particular microphones that accept only 16- or
// 24-bit PCM in exclusive mode, which is most of them.
//
// COM interfaces here are ordinary C++ abstract classes, which is what they are
// on Windows too: the vtable layout is the ABI. Signatures match the SDK
// exactly, so a mismatch in the backend fails here the same way it would there.

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <string>

// --- Primitive types --------------------------------------------------------
using BYTE    = unsigned char;
using WORD    = uint16_t;
using DWORD   = uint32_t;
using UINT    = unsigned int;
using UINT32  = uint32_t;
using UINT64  = uint64_t;
using ULONG   = uint32_t;
using LONG    = int32_t;
using BOOL    = int;
using WCHAR   = wchar_t;
using LPWSTR  = wchar_t*;
using LPCWSTR = const wchar_t*;
using LPCSTR  = const char*;
using HANDLE  = void*;
// 32-bit, as on Windows. Typing this as `long` on a 64-bit Unix makes every
// 0x8889xxxx error code positive, so FAILED() reads every failure as success --
// which is exactly the kind of thing this simulation exists to catch.
using HRESULT = int32_t;
using REFERENCE_TIME = int64_t;

#define STDMETHODCALLTYPE
#define WINAPI

constexpr BOOL TRUE_VALUE = 1;
#define TRUE 1
#define FALSE 0

// --- HRESULTs ---------------------------------------------------------------
constexpr HRESULT S_OK           = 0;
constexpr HRESULT S_FALSE        = 1;
constexpr HRESULT E_FAIL         = static_cast<HRESULT> (0x80004005);
constexpr HRESULT E_POINTER      = static_cast<HRESULT> (0x80004003);
constexpr HRESULT E_NOINTERFACE  = static_cast<HRESULT> (0x80004002);
constexpr HRESULT E_INVALIDARG   = static_cast<HRESULT> (0x80070057);
constexpr HRESULT E_OUTOFMEMORY  = static_cast<HRESULT> (0x8007000E);
constexpr HRESULT E_ACCESSDENIED = static_cast<HRESULT> (0x80070005);

constexpr bool SUCCEEDED (HRESULT hr) { return hr >= 0; }
constexpr bool FAILED (HRESULT hr) { return hr < 0; }

// --- GUIDs ------------------------------------------------------------------
struct GUID
{
    uint32_t a, b, c, d;

    bool operator== (const GUID& o) const { return a == o.a && b == o.b && c == o.c && d == o.d; }
    bool operator!= (const GUID& o) const { return ! (*this == o); }
};

using IID = GUID;
using CLSID = GUID;
using REFIID = const GUID&;
using REFCLSID = const GUID&;

namespace mmasim {

/// Per-interface identity. MSVC's __uuidof is a compiler intrinsic backed by
/// declspec(uuid); this is the same idea expressed in plain C++.
template <typename T> struct SimUuid;

template <typename T> const GUID& uuidOf() { return SimUuid<T>::value; }

/// IID_PPV_ARGS accepts either a raw T** or a ComPtr's address proxy, exactly
/// as the SDK's IID_PPV_ARGS_Helper overloads do. The ComPtr overloads live in
/// wrl/client.h, where the proxy type is declared.
template <typename T> const GUID& uuidOfPtr (T**) { return SimUuid<T>::value; }
template <typename T> void** ppvHelper (T** p) { return reinterpret_cast<void**> (p); }

} // namespace mmasim

// MSVC provides __uuidof as a compiler intrinsic and MinGW as a macro of its
// own; a simulated build wants neither, so clear it first rather than redefine
// over the top of one.
#ifdef __uuidof
 #undef __uuidof
#endif
#define __uuidof(T) (::mmasim::uuidOf<T>())

/// Matches the SDK macro: expands to the interface's IID plus the void** the
/// call fills. The pointer expression is a plain address-of, so evaluating it
/// twice is harmless.
#define IID_PPV_ARGS(pp) (::mmasim::uuidOfPtr (pp)), (::mmasim::ppvHelper (pp))

// --- IUnknown ---------------------------------------------------------------
struct IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void** object) = 0;
    virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
    virtual ULONG STDMETHODCALLTYPE Release() = 0;

protected:
    ~IUnknown() = default;
};

namespace mmasim { template <> struct SimUuid<IUnknown> { static constexpr GUID value { 1, 0, 0, 0 }; }; }

// --- COM lifecycle ----------------------------------------------------------
constexpr DWORD CLSCTX_ALL = 23;
constexpr DWORD COINIT_MULTITHREADED = 0;
constexpr DWORD COINIT_APARTMENTTHREADED = 2;

// A thread already in one apartment asking for the other; and a COM call from
// a thread in no apartment while no thread in the process is in the MTA.
constexpr HRESULT RPC_E_CHANGED_MODE  = static_cast<HRESULT> (0x80010106);
constexpr HRESULT CO_E_NOTINITIALIZED = static_cast<HRESULT> (0x800401F0);

HRESULT CoInitializeEx (void* reserved, DWORD flags);
void CoUninitialize();
HRESULT CoCreateInstance (REFCLSID clsid, IUnknown* outer, DWORD context, REFIID riid, void** out);
void CoTaskMemFree (void* p);
void* CoTaskMemAlloc (size_t bytes);

// --- Events -----------------------------------------------------------------
constexpr DWORD WAIT_OBJECT_0 = 0;
constexpr DWORD WAIT_TIMEOUT  = 258;
constexpr DWORD INFINITE      = 0xFFFFFFFF;

HANDLE CreateEventW (void* attributes, BOOL manualReset, BOOL initialState, LPCWSTR name);
BOOL CloseHandle (HANDLE handle);
DWORD WaitForSingleObject (HANDLE handle, DWORD milliseconds);
BOOL SetEvent (HANDLE handle);

// --- String conversion ------------------------------------------------------
constexpr UINT CP_UTF8 = 65001;

int WideCharToMultiByte (UINT codePage, DWORD flags, LPCWSTR wide, int wideLength,
                         char* out, int outBytes, const char* defaultChar, BOOL* usedDefault);

int MultiByteToWideChar (UINT codePage, DWORD flags, LPCSTR narrow, int narrowLength,
                         LPWSTR out, int outChars);

// --- Registry ---------------------------------------------------------------
using HKEY = struct HKEY__*;

constexpr LONG ERROR_SUCCESS = 0;
constexpr LONG ERROR_FILE_NOT_FOUND = 2;
constexpr DWORD KEY_READ = 0x20019;

// The SDK spells this as a cast of a sentinel value; the shim keeps the shape.
#define HKEY_LOCAL_MACHINE (reinterpret_cast<HKEY> (static_cast<uintptr_t> (0x80000002)))

LONG RegOpenKeyExA (HKEY key, LPCSTR subKey, DWORD options, DWORD desired, HKEY* out);
LONG RegCloseKey (HKEY key);

// --- Property variants ------------------------------------------------------
struct PROPERTYKEY
{
    GUID fmtid;
    DWORD pid;

    bool operator== (const PROPERTYKEY& o) const { return fmtid == o.fmtid && pid == o.pid; }
};

struct PROPVARIANT
{
    unsigned short vt;
    LPWSTR pwszVal;
};

constexpr unsigned short VT_LPWSTR = 31;
constexpr DWORD STGM_READ = 0;

void PropVariantInit (PROPVARIANT* v);
HRESULT PropVariantClear (PROPVARIANT* v);

// --- Wave formats -----------------------------------------------------------
constexpr WORD WAVE_FORMAT_EXTENSIBLE = 0xFFFE;

constexpr DWORD SPEAKER_FRONT_LEFT   = 0x1;
constexpr DWORD SPEAKER_FRONT_RIGHT  = 0x2;
constexpr DWORD SPEAKER_FRONT_CENTER = 0x4;

#pragma pack(push, 1)
struct WAVEFORMATEX
{
    WORD wFormatTag;
    WORD nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD nBlockAlign;
    WORD wBitsPerSample;
    WORD cbSize;
};

struct WAVEFORMATEXTENSIBLE
{
    WAVEFORMATEX Format;

    union
    {
        WORD wValidBitsPerSample;
        WORD wSamplesPerBlock;
        WORD wReserved;
    } Samples;

    DWORD dwChannelMask;
    GUID SubFormat;
};
#pragma pack(pop)

// The two subformat GUIDs the backend negotiates between.
constexpr GUID KSDATAFORMAT_SUBTYPE_PCM        { 0x00000001, 0x0000, 0x0010, 0x8000 };
constexpr GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT { 0x00000003, 0x0000, 0x0010, 0x8000 };
