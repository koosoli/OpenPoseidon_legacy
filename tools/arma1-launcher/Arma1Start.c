// Arma1Start.exe -- small starter for OpenPoseidon with Arma 1 missions.
//
// Sits next to OpenPoseidon.exe in the Cold War Assault folder. Finds the Arma 1
// (ArmA Gold) install by itself, lets the player pick a campaign or a mission and
// starts OpenPoseidon.exe with --arma1 / --arma1-campaign / --arma1-mission.
// Nothing is copied; the Arma 1 folder is only passed on the command line.
//
// Arma 1 folder lookup, first hit wins (a folder counts when it has an AddOns subfolder):
//   1. arma1-pfad.txt next to this exe (one line, the folder)
//   2. ..\ARMA Gold and ..\..\ARMA Gold relative to this exe (Steam "common" siblings)
//   3. every Steam library in libraryfolders.vdf: <lib>\steamapps\common\ARMA Gold
//   4. folder picker; the choice is saved to arma1-pfad.txt
//
// Extra arguments given to this exe are passed through to OpenPoseidon.exe.

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define BIG 4096

static wchar_t g_exeDir[MAX_PATH];
static wchar_t g_arma1[BIG];
static wchar_t g_choice[512]; // extra args for the chosen start
static int g_started = 0;

enum
{
    ID_CA = 101,
    ID_RA,
    ID_MERC,
    ID_M09,
    ID_MISSION_EDIT,
    ID_MISSION_GO,
    ID_MENU,
    ID_CHANGE_DIR,
    ID_DIR_LABEL
};

static int DirExists(const wchar_t* p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int FileExists(const wchar_t* p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void StripTrailing(wchar_t* p)
{
    size_t n = wcslen(p);
    while (n > 0 && (p[n - 1] == L'\\' || p[n - 1] == L'/' || p[n - 1] == L' ' || p[n - 1] == L'\r' ||
                     p[n - 1] == L'\n' || p[n - 1] == L'"'))
        p[--n] = 0;
    // leading quote / spaces
    size_t s = 0;
    while (p[s] == L' ' || p[s] == L'"')
        s++;
    if (s)
        memmove(p, p + s, (wcslen(p + s) + 1) * sizeof(wchar_t));
}

// Valid Arma 1 folder = has AddOns. Normalises into g_arma1 on success.
static int TryArma1(const wchar_t* candidate)
{
    wchar_t full[BIG], test[BIG];
    if (!GetFullPathNameW(candidate, BIG, full, NULL))
        return 0;
    StripTrailing(full);
    if (!full[0])
        return 0;
    _snwprintf(test, BIG, L"%s\\AddOns", full);
    test[BIG - 1] = 0;
    if (!DirExists(test))
        return 0;
    wcsncpy(g_arma1, full, BIG - 1);
    g_arma1[BIG - 1] = 0;
    return 1;
}

static void SavedPathFile(wchar_t* out)
{
    _snwprintf(out, BIG, L"%s\\arma1-pfad.txt", g_exeDir);
    out[BIG - 1] = 0;
}

static int FromSavedFile(void)
{
    wchar_t path[BIG];
    SavedPathFile(path);
    FILE* f = _wfopen(path, L"rb");
    if (!f)
        return 0;
    char buf[BIG];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    char* start = buf;
    if (n >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
        start += 3;
    char* nl = strpbrk(start, "\r\n");
    if (nl)
        *nl = 0;
    wchar_t w[BIG];
    if (!MultiByteToWideChar(CP_UTF8, 0, start, -1, w, BIG))
        return 0;
    StripTrailing(w);
    return w[0] && TryArma1(w);
}

static void SaveChoice(void)
{
    wchar_t path[BIG];
    SavedPathFile(path);
    FILE* f = _wfopen(path, L"wb");
    if (!f)
        return;
    char utf8[BIG * 3];
    int n = WideCharToMultiByte(CP_UTF8, 0, g_arma1, -1, utf8, sizeof(utf8), NULL, NULL);
    if (n > 1)
        fwrite(utf8, 1, (size_t)(n - 1), f);
    fputs("\r\n", f);
    fclose(f);
}

static int FromSiblings(void)
{
    wchar_t c[BIG];
    const wchar_t* rel[] = {L"\\..\\ARMA Gold", L"\\..\\..\\ARMA Gold", L"\\..\\ArmA Armed Assault",
                            L"\\..\\..\\ArmA Armed Assault"};
    for (int i = 0; i < 4; i++)
    {
        _snwprintf(c, BIG, L"%s%s", g_exeDir, rel[i]);
        c[BIG - 1] = 0;
        if (TryArma1(c))
            return 1;
    }
    return 0;
}

// libraryfolders.vdf: lines like   "path"		"D:\\Steam-Games"
static int FromSteamLibraries(void)
{
    wchar_t steam[BIG];
    DWORD size = sizeof(steam);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, NULL, steam, &size) !=
        ERROR_SUCCESS)
    {
        size = sizeof(steam);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath", RRF_RT_REG_SZ,
                         NULL, steam, &size) != ERROR_SUCCESS)
            return 0;
    }
    for (wchar_t* p = steam; *p; p++)
        if (*p == L'/')
            *p = L'\\';
    StripTrailing(steam);

    wchar_t c[BIG];
    _snwprintf(c, BIG, L"%s\\steamapps\\common\\ARMA Gold", steam);
    c[BIG - 1] = 0;
    if (TryArma1(c))
        return 1;

    wchar_t vdf[BIG];
    _snwprintf(vdf, BIG, L"%s\\steamapps\\libraryfolders.vdf", steam);
    vdf[BIG - 1] = 0;
    FILE* f = _wfopen(vdf, L"rb");
    if (!f)
        return 0;
    char line[BIG];
    int found = 0;
    while (!found && fgets(line, sizeof(line), f))
    {
        char* key = strstr(line, "\"path\"");
        if (!key)
            continue;
        char* q1 = strchr(key + 6, '"');
        if (!q1)
            continue;
        char* q2 = strchr(q1 + 1, '"');
        if (!q2)
            continue;
        *q2 = 0;
        // unescape \\ -> backslash
        char lib[BIG];
        size_t o = 0;
        for (char* s = q1 + 1; *s && o < sizeof(lib) - 1; s++)
        {
            if (s[0] == '\\' && s[1] == '\\')
                s++;
            lib[o++] = *s;
        }
        lib[o] = 0;
        wchar_t wlib[BIG];
        if (!MultiByteToWideChar(CP_UTF8, 0, lib, -1, wlib, BIG))
            continue;
        StripTrailing(wlib);
        _snwprintf(c, BIG, L"%s\\steamapps\\common\\ARMA Gold", wlib);
        c[BIG - 1] = 0;
        found = TryArma1(c);
    }
    fclose(f);
    return found;
}

static int PickFolder(HWND owner)
{
    for (;;)
    {
        BROWSEINFOW bi;
        ZeroMemory(&bi, sizeof(bi));
        bi.hwndOwner = owner;
        bi.lpszTitle = L"Arma 1 (ArmA Gold) Ordner auswaehlen - der Ordner mit dem Unterordner \"AddOns\"";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
        if (!pidl)
            return 0;
        wchar_t path[MAX_PATH];
        int ok = SHGetPathFromIDListW(pidl, path);
        CoTaskMemFree(pidl);
        if (ok && TryArma1(path))
        {
            SaveChoice();
            return 1;
        }
        MessageBoxW(owner,
                    L"In diesem Ordner gibt es keinen Unterordner \"AddOns\".\n"
                    L"Bitte den Arma-1-Installationsordner waehlen (z. B. ...\\steamapps\\common\\ARMA Gold).",
                    L"Arma 1 Start", MB_ICONWARNING);
    }
}

static int FindArma1(void)
{
    return FromSavedFile() || FromSiblings() || FromSteamLibraries() ||
           TryArma1(L"D:\\Steam-Games\\steamapps\\common\\ARMA Gold") ||
           TryArma1(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\ARMA Gold");
}

static void UpdateDirLabel(HWND wnd)
{
    wchar_t t[BIG + 32];
    _snwprintf(t, BIG + 32, L"Arma 1: %s", g_arma1);
    t[BIG + 31] = 0;
    SetDlgItemTextW(wnd, ID_DIR_LABEL, t);
}

static HFONT g_font;

static HWND Add(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static void Choose(HWND wnd, const wchar_t* args)
{
    wcsncpy(g_choice, args, 511);
    g_choice[511] = 0;
    g_started = 1;
    DestroyWindow(wnd);
}

static LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        int x = 16, w = 420, y = 14, h = 32, gap = 8;
        Add(wnd, L"STATIC", L"Was soll gestartet werden?", 0, x, y, w, 20, 0);
        y += 28;
        Add(wnd, L"BUTTON", L"Kampagne  ca  (Armed Assault)", BS_PUSHBUTTON, x, y, w, h, ID_CA);
        y += h + gap;
        Add(wnd, L"BUTTON", L"Kampagne  ra  (Queen's Gambit)", BS_PUSHBUTTON, x, y, w, h, ID_RA);
        y += h + gap;
        Add(wnd, L"BUTTON", L"Kampagne  merc", BS_PUSHBUTTON, x, y, w, h, ID_MERC);
        y += h + gap;
        Add(wnd, L"BUTTON", L"Mission  M09 Convoy Attack", BS_PUSHBUTTON, x, y, w, h, ID_M09);
        y += h + gap + 6;
        Add(wnd, L"STATIC", L"Andere Mission (z. B. m01-firstblood.sara):", 0, x, y, w, 20, 0);
        y += 22;
        Add(wnd, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, x, y, 300, 26, ID_MISSION_EDIT);
        Add(wnd, L"BUTTON", L"Mission starten", BS_PUSHBUTTON, x + 308, y, w - 308, 26, ID_MISSION_GO);
        y += 26 + gap + 6;
        Add(wnd, L"BUTTON", L"Nur Hauptmenue (Arma-1-Daten eingebunden)", BS_PUSHBUTTON, x, y, w, h, ID_MENU);
        y += h + gap + 10;
        Add(wnd, L"STATIC", L"", SS_PATHELLIPSIS, x, y, w, 20, ID_DIR_LABEL);
        y += 24;
        Add(wnd, L"BUTTON", L"Arma-1-Ordner aendern ...", BS_PUSHBUTTON, x, y, 200, 26, ID_CHANGE_DIR);
        UpdateDirLabel(wnd);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case ID_CA:
            Choose(wnd, L"--arma1-campaign ca");
            return 0;
        case ID_RA:
            Choose(wnd, L"--arma1-campaign ra");
            return 0;
        case ID_MERC:
            Choose(wnd, L"--arma1-campaign merc");
            return 0;
        case ID_M09:
            Choose(wnd, L"--arma1-mission m09convoyattack.sara");
            return 0;
        case ID_MISSION_GO:
        {
            wchar_t m[256], a[512];
            GetDlgItemTextW(wnd, ID_MISSION_EDIT, m, 256);
            StripTrailing(m);
            if (!m[0])
            {
                MessageBoxW(wnd, L"Bitte einen Missionsnamen eingeben.", L"Arma 1 Start", MB_ICONINFORMATION);
                return 0;
            }
            _snwprintf(a, 512, L"--arma1-mission \"%s\"", m);
            a[511] = 0;
            Choose(wnd, a);
            return 0;
        }
        case ID_MENU:
            Choose(wnd, L"");
            return 0;
        case ID_CHANGE_DIR:
            if (PickFolder(wnd))
                UpdateDirLabel(wnd);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

// Everything after argv[0] in the launcher's own command line.
static const wchar_t* PassThroughArgs(void)
{
    const wchar_t* c = GetCommandLineW();
    if (*c == L'"')
    {
        c++;
        while (*c && *c != L'"')
            c++;
        if (*c)
            c++;
    }
    else
        while (*c && *c != L' ' && *c != L'\t')
            c++;
    while (*c == L' ' || *c == L'\t')
        c++;
    return c;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev;
    (void)cmd;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    GetModuleFileNameW(NULL, g_exeDir, MAX_PATH);
    wchar_t* slash = wcsrchr(g_exeDir, L'\\');
    if (slash)
        *slash = 0;

    wchar_t game[BIG], dll[BIG];
    _snwprintf(game, BIG, L"%s\\OpenPoseidon.exe", g_exeDir);
    _snwprintf(dll, BIG, L"%s\\wgpu_renderer.dll", g_exeDir);
    game[BIG - 1] = dll[BIG - 1] = 0;
    if (!FileExists(game) || !FileExists(dll))
    {
        MessageBoxW(NULL,
                    L"OpenPoseidon.exe und wgpu_renderer.dll muessen im selben Ordner liegen wie Arma1Start.exe.\n\n"
                    L"Bitte alle Dateien aus dem ZIP in den Cold-War-Assault-Ordner kopieren, z. B.\n"
                    L"D:\\Steam-Games\\steamapps\\common\\ARMA Cold War Assault\\Remastered",
                    L"Arma 1 Start", MB_ICONERROR);
        return 1;
    }

    if (!FindArma1())
    {
        MessageBoxW(NULL,
                    L"Die Arma-1-Installation (ArmA Gold) wurde nicht automatisch gefunden.\n"
                    L"Bitte im naechsten Fenster den Ordner auswaehlen.",
                    L"Arma 1 Start", MB_ICONINFORMATION);
        if (!PickFolder(NULL))
            return 1;
    }

    NONCLIENTMETRICSW ncm;
    ZeroMemory(&ncm, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.lpszClassName = L"Arma1StartWnd";
    RegisterClassW(&wc);

    RECT r = {0, 0, 452, 456};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"Open Poseidon - Arma 1 starten", style,
                               (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww,
                               wh, NULL, NULL, inst, NULL);
    ShowWindow(wnd, show);
    UpdateWindow(wnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0)
    {
        if (!IsDialogMessageW(wnd, &m))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    if (!g_started)
        return 0;

    static wchar_t cmdline[BIG * 2];
    const wchar_t* extra = PassThroughArgs();
    _snwprintf(cmdline, BIG * 2, L"\"%s\" --arma1 \"%s\" %s %s", game, g_arma1, g_choice, extra);
    cmdline[BIG * 2 - 1] = 0;

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(game, cmdline, NULL, NULL, FALSE, 0, NULL, g_exeDir, &si, &pi))
    {
        wchar_t err[512];
        _snwprintf(err, 512, L"OpenPoseidon.exe konnte nicht gestartet werden (Fehler %lu).", GetLastError());
        err[511] = 0;
        MessageBoxW(NULL, err, L"Arma 1 Start", MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
