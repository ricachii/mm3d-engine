/* MajorasMask3D.exe: windowless launcher with exactly what Play.bat does.
 *
 * Play.bat (the one of the release) runs, from the game folder:
 *   - checks private\mm3d_title_aot.dll;  logs\ is created and last_run.log is rotated to previous_run.log
 *   - sets TRIAEVUM_LOG_FILE, TRIAEVUM_CRASH_DIR and TRIAEVUM_REPORT_TITLE
 *   - runs tools\python\python.exe setup\...\prepare_shaders.py "<folder>." if that Python exists
 *   - adds the shader pack options when data\shader_corpus\mm3d.o3ps and all_pipelines.json exist
 *   - runs TriAevum.exe with the fixed arguments below (plus the launcher's own arguments, like %*),
 *     stdout and stderr going to logs\last_run.log; if it returns an error level >= 1, tells the user.
 * This program does the same without a console window. No input timeline and no test options.
 *
 * Part of the MM3D adaptation built on Volpe34's mm3d-pc (GPL-3.0-or-later). */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static wchar_t gDir[MAX_PATH * 2];  /* folder of this exe, with a trailing backslash */

static int exists(const wchar_t *relative) {
    wchar_t path[MAX_PATH * 3];
    swprintf(path, sizeof(path) / sizeof(path[0]), L"%ls%ls", gDir, relative);
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static void fail(const wchar_t *text) {
    MessageBoxW(NULL, text, L"Majora's Mask 3D", MB_OK | MB_ICONERROR);
}

/* The launcher's own arguments (everything after its name), as Play.bat's %* */
static const wchar_t *own_arguments(void) {
    const wchar_t *p = GetCommandLineW();
    if (*p == L'"') {
        for (++p; *p && *p != L'"'; ++p) {}
        if (*p) ++p;
    } else {
        while (*p && *p != L' ' && *p != L'\t') ++p;
    }
    while (*p == L' ' || *p == L'\t') ++p;
    return p;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show) {
    (void)inst; (void)prev; (void)cmd; (void)show;
    wchar_t self[MAX_PATH * 2];
    if (!GetModuleFileNameW(NULL, self, sizeof(self) / sizeof(self[0]))) return 1;
    wchar_t *slash = wcsrchr(self, L'\\');
    if (!slash) return 1;
    slash[1] = 0;
    wcscpy(gDir, self);
    SetCurrentDirectoryW(gDir);                                   /* cd /d "%~dp0" */

    if (!exists(L"private\\mm3d_title_aot.dll")) {
        fail(L"The game is not prepared yet: run Setup.bat first.");
        return 1;
    }

    wchar_t logs[MAX_PATH * 3], lastRun[MAX_PATH * 3], previousRun[MAX_PATH * 3];
    swprintf(logs, MAX_PATH * 3, L"%ls" L"logs", gDir);
    swprintf(lastRun, MAX_PATH * 3, L"%ls" L"logs\\last_run.log", gDir);
    swprintf(previousRun, MAX_PATH * 3, L"%ls" L"logs\\previous_run.log", gDir);
    CreateDirectoryW(logs, NULL);
    if (GetFileAttributesW(lastRun) != INVALID_FILE_ATTRIBUTES)
        MoveFileExW(lastRun, previousRun, MOVEFILE_REPLACE_EXISTING);

    SetEnvironmentVariableW(L"TRIAEVUM_LOG_FILE", lastRun);
    SetEnvironmentVariableW(L"TRIAEVUM_CRASH_DIR", logs);
    SetEnvironmentVariableW(L"TRIAEVUM_REPORT_TITLE", L"MM3D");

    if (exists(L"tools\\python\\python.exe")) {
        wchar_t command[MAX_PATH * 6];
        swprintf(command, sizeof(command) / sizeof(command[0]),
                 L"tools\\python\\python.exe setup\\TriAevum\\tools\\mm3d\\package\\prepare_shaders.py \"%ls.\"", gDir);
        wchar_t application[MAX_PATH * 3];
        swprintf(application, MAX_PATH * 3, L"%ls" L"tools\\python\\python.exe", gDir);
        STARTUPINFOW si = {sizeof(si)};
        PROCESS_INFORMATION pi;
        if (CreateProcessW(application, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, gDir, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }

    const wchar_t *shaders = L"";
    if (exists(L"data\\shader_corpus\\mm3d.o3ps") && exists(L"data\\shader_corpus\\all_pipelines.json"))
        shaders = L" --pica-aot-shader-pack data\\shader_corpus\\mm3d.o3ps"
                  L" --pica-pipeline-manifest data\\shader_corpus\\all_pipelines.json --pica-pipeline-prewarm";

    wchar_t runs[MAX_PATH * 3];
    swprintf(runs, MAX_PATH * 3, L"%ls" L"data\\shader_corpus\\runs", gDir);
    SHCreateDirectoryExW(NULL, runs, NULL);                       /* if not exist ... mkdir (with parents) */

    srand((unsigned)GetTickCount() ^ (unsigned)GetCurrentProcessId());
    wchar_t stamp[32];
    swprintf(stamp, 32, L"%d%d", rand() & 0x7FFF, rand() & 0x7FFF);   /* %RANDOM%%RANDOM% */

    const wchar_t *extra = own_arguments();
    size_t length = 2048 + wcslen(extra);
    wchar_t *command = (wchar_t *)calloc(length, sizeof(wchar_t));
    if (!command) return 1;
    swprintf(command, length,
             L"TriAevum.exe --title-plugin private\\mm3d_title_aot.dll --a32-process-manifest process-manifest.json"
             L" --resource-root resources --renderer nri --ui-profile oot3d --config data\\config\\TriAevum.json"
             L" --gameplay-timing native30_interpolated --presentation-rate 60 --save-data data\\savedata"
             L" --width 1280 --height 720"
             L" --pica-effective-shader-inventory data\\shader_corpus\\runs\\%ls_shaders.json"
             L" --pica-pipeline-inventory data\\shader_corpus\\runs\\%ls_pipelines.json%ls%ls%ls",
             stamp, stamp, shaders, *extra ? L" " : L"", extra);

    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};            /* inheritable handles */
    HANDLE log = CreateFileW(lastRun, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);        /* > logs\last_run.log 2>&1 */
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = log;
    si.hStdError = log;
    wchar_t application[MAX_PATH * 3];
    swprintf(application, MAX_PATH * 3, L"%ls" L"TriAevum.exe", gDir);
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(application, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, gDir, &si, &pi)) {
        fail(L"TriAevum.exe could not be started (is it in the same folder as this launcher?).");
        return 1;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    free(command);
    if ((int)code >= 1)                                           /* if errorlevel 1 */
        fail(L"The game stopped with an error. Send logs\\last_run.log (and any report in logs) to get help.");
    return (int)code;
}
