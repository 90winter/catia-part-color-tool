#define NOMINMAX
#define UNICODE
#define _UNICODE

#include "pipeline.h"
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <process.h>
#include <stdio.h>
#include <vector>
#include <string>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#define ID_BTN_ADD_FILES   101
#define ID_BTN_ADD_DIR     102
#define ID_BTN_ADD_REC     103
#define ID_BTN_REMOVE      104
#define ID_BTN_CLEAR       105
#define ID_BTN_RUN         106
#define ID_LIST            107
#define ID_STATUS          108
#define ID_TITLE           109
#define ID_SUB             110
#define ID_ENTITY_STATUS   111

#define IDI_APPICON        200

#define WM_PROGRESS        (WM_APP + 1)
#define WM_BATCH_ITEM      (WM_APP + 10)
#define WM_BATCH_DONE      (WM_APP + 11)
#define WM_SCAN_DONE       (WM_APP + 12)

struct FileItem {
    std::wstring fullPath;
    std::wstring fileName;
    std::wstring folder;
    std::wstring sizeText;
    std::wstring statusText;
};

struct BatchUpdate {
    int index;
    int state; // 0 = running, 1 = success, 2 = failed, 3 = canceled
    std::wstring status;
};

static HWND g_wnd = NULL, g_list = NULL, g_status = NULL, g_entityStatus = NULL;
static HWND g_btnAddFiles = NULL, g_btnAddDir = NULL, g_btnAddRec = NULL;
static HWND g_btnRemove = NULL, g_btnClear = NULL, g_btnRun = NULL;
static HFONT g_titleFont = NULL, g_bodyFont = NULL, g_smallFont = NULL, g_boldFont = NULL;
static HBRUSH g_bgBrush = NULL;

static std::vector<FileItem> g_items;
static bool g_busy = false;
static bool g_closeRequested = false;
static int g_doneCount = 0;
static int g_failCount = 0;
static int g_totalCount = 0;
static int g_entityPct = 0;
static std::wstring g_currentFileName;
static HANDLE g_cancelEvent = NULL;
static HANDLE g_workerThread = NULL;

static std::wstring toLowerW(std::wstring s) {
    for (size_t i = 0; i < s.size(); ++i) {
        s[i] = (wchar_t)towlower(s[i]);
    }
    return s;
}

static std::wstring formatSize(unsigned long long bytes) {
    wchar_t buf[64];
    if (bytes >= 1024ULL * 1024ULL) {
        swprintf_s(buf, L"%.2f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        swprintf_s(buf, L"%.1f KB", (double)bytes / 1024.0);
    } else {
        swprintf_s(buf, L"%llu B", bytes);
    }
    return buf;
}

static bool hasCatPartExt(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    return _wcsicmp(path.c_str() + dot, L".CATPart") == 0;
}

static bool containsPath(const std::wstring& fullPath) {
    std::wstring low = toLowerW(fullPath);
    for (size_t i = 0; i < g_items.size(); ++i) {
        if (toLowerW(g_items[i].fullPath) == low) {
            return true;
        }
    }
    return false;
}

static void refreshSummaryStatus(const std::wstring& prefix = L"") {
    wchar_t buf[512];
    if (!prefix.empty()) {
        swprintf_s(buf, L"文件进度：%s  |  共 %d 个文件（已完成 %d，失败 %d）",
                   prefix.c_str(), (int)g_items.size(), g_doneCount, g_failCount);
    } else if (g_items.empty()) {
        swprintf_s(buf, L"文件进度：就绪。请点击“添加文件 / 添加文件夹”或直接拖入 .CATPart 文件（支持隐藏文件）。");
    } else {
        swprintf_s(buf, L"文件进度：共 %d 个文件  |  已完成 %d  |  失败 %d",
                   (int)g_items.size(), g_doneCount, g_failCount);
    }
    SetWindowTextW(g_status, buf);
}

static void refreshEntityStatus() {
    if (!g_entityStatus) return;
    wchar_t buf[512];
    if (g_busy && !g_currentFileName.empty()) {
        swprintf_s(buf, L"实体进度：%s — %d%%", g_currentFileName.c_str(), g_entityPct);
    } else if (g_entityPct > 0) {
        swprintf_s(buf, L"实体进度：%d%%", g_entityPct);
    } else {
        swprintf_s(buf, L"实体进度：等待开始");
    }
    SetWindowTextW(g_entityStatus, buf);
}

static void addFileToList(const std::wstring& path) {
    wchar_t fullBuf[32768];
    DWORD n = GetFullPathNameW(path.c_str(), 32768, fullBuf, NULL);
    std::wstring fullPath = (n > 0 && n < 32768) ? std::wstring(fullBuf) : path;

    DWORD attr = GetFileAttributesW(fullPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return;
    if (!hasCatPartExt(fullPath)) return;
    if (containsPath(fullPath)) return;

    WIN32_FILE_ATTRIBUTE_DATA fad;
    unsigned long long fsize = 0;
    if (GetFileAttributesExW(fullPath.c_str(), GetFileExInfoStandard, &fad)) {
        fsize = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    }

    size_t slash = fullPath.find_last_of(L"\\/");
    std::wstring name = (slash == std::wstring::npos) ? fullPath : fullPath.substr(slash + 1);
    std::wstring dir = (slash == std::wstring::npos) ? L"" : fullPath.substr(0, slash);

    FileItem item;
    item.fullPath = fullPath;
    item.fileName = name;
    item.folder = dir;
    item.sizeText = formatSize(fsize);
    item.statusText = L"待处理";

    int idx = (int)g_items.size();
    g_items.push_back(item);

    LVITEMW lv = {};
    lv.mask = LVIF_TEXT;
    lv.iItem = idx;
    lv.pszText = const_cast<LPWSTR>(g_items[idx].fileName.c_str());
    ListView_InsertItem(g_list, &lv);
    ListView_SetItemText(g_list, idx, 1, const_cast<LPWSTR>(g_items[idx].sizeText.c_str()));
    ListView_SetItemText(g_list, idx, 2, const_cast<LPWSTR>(g_items[idx].folder.c_str()));
    ListView_SetItemText(g_list, idx, 3, const_cast<LPWSTR>(g_items[idx].statusText.c_str()));
}

static void collectPartsInFolder(const std::wstring& dir, bool recursive, int depth,
                                 std::vector<std::wstring>& outPaths) {
    if (depth > 64) return;
    std::wstring base = dir;
    while (!base.empty() && (base[base.size() - 1] == L'\\' || base[base.size() - 1] == L'/')) {
        base.erase(base.size() - 1);
    }
    if (base.empty()) return;

    std::wstring pattern = base + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        std::wstring full = base + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Skip junctions / symlinks (reparse points) to prevent directory cycles
            if (recursive && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                collectPartsInFolder(full, true, depth + 1, outPaths);
            }
        } else {
            // Includes hidden files (FILE_ATTRIBUTE_HIDDEN) without filtering them out
            if (hasCatPartExt(full)) {
                outPaths.push_back(full);
            }
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

struct ScanTask {
    std::vector<std::wstring> roots;
    bool recursive;
    HWND notify;
};

static unsigned __stdcall ScanWorkerThread(void* raw) {
    ScanTask* task = static_cast<ScanTask*>(raw);
    if (!task) return 1;
    std::vector<std::wstring>* found = new std::vector<std::wstring>();
    for (size_t i = 0; i < task->roots.size(); ++i) {
        DWORD attr = GetFileAttributesW(task->roots[i].c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) continue;
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            collectPartsInFolder(task->roots[i], task->recursive, 0, *found);
        } else if (hasCatPartExt(task->roots[i])) {
            found->push_back(task->roots[i]);
        }
    }
    if (!PostMessageW(task->notify, WM_SCAN_DONE, 0, (LPARAM)found)) {
        delete found;
    }
    delete task;
    return 0;
}

static void startAsyncScan(HWND h, const std::vector<std::wstring>& roots, bool recursive) {
    ScanTask* task = new ScanTask;
    task->roots = roots;
    task->recursive = recursive;
    task->notify = h;
    SetWindowTextW(g_status, L"文件进度：正在后台扫描文件夹中的 .CATPart 文件，请稍候...");
    uintptr_t th = _beginthreadex(NULL, 0, ScanWorkerThread, task, 0, NULL);
    if (th) {
        CloseHandle((HANDLE)th);
    } else {
        delete task;
        refreshSummaryStatus();
    }
}

static void onAddFiles(HWND h) {
    if (g_busy) return;
    const DWORD BUF_LEN = 65536;
    std::vector<wchar_t> buf(BUF_LEN, 0);
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = h;
    ofn.lpstrFilter = L"CATPart 模型文件 (*.CATPart)\0*.CATPart\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = &buf[0];
    ofn.nMaxFile = BUF_LEN;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;

    wchar_t* p = &buf[0];
    std::wstring first = p;
    p += first.size() + 1;
    if (*p == 0) {
        addFileToList(first);
    } else {
        while (*p) {
            std::wstring fname = p;
            addFileToList(first + L"\\" + fname);
            p += fname.size() + 1;
        }
    }
    refreshSummaryStatus();
}

static void onAddFolder(HWND h, bool recursive) {
    if (g_busy) return;
    bool picked = false;
    std::wstring chosenPath;

    IFileOpenDialog* pfd = NULL;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd))) && pfd) {
        DWORD opts = 0;
        if (SUCCEEDED(pfd->GetOptions(&opts))) {
            pfd->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
        }
        pfd->SetTitle(recursive ? L"选择文件夹（将递归扫描子目录中的 .CATPart）" : L"选择文件夹（扫描当前目录中的 .CATPart）");
        if (SUCCEEDED(pfd->Show(h))) {
            IShellItem* psi = NULL;
            if (SUCCEEDED(pfd->GetResult(&psi)) && psi) {
                PWSTR pszPath = NULL;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)) && pszPath) {
                    chosenPath = pszPath;
                    CoTaskMemFree(pszPath);
                    picked = true;
                }
                psi->Release();
            }
        }
        pfd->Release();
    } else {
        BROWSEINFOW bi = {};
        bi.hwndOwner = h;
        bi.lpszTitle = recursive ? L"选择文件夹（将递归扫描子目录中的 .CATPart）" : L"选择文件夹（扫描当前目录中的 .CATPart）";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
        PIDLIST_ABSOLUTE pid = SHBrowseForFolderW(&bi);
        if (pid) {
            wchar_t path[MAX_PATH];
            if (SHGetPathFromIDListW(pid, path)) {
                chosenPath = path;
                picked = true;
            }
            CoTaskMemFree(pid);
        }
    }

    if (picked && !chosenPath.empty()) {
        std::vector<std::wstring> roots;
        roots.push_back(chosenPath);
        startAsyncScan(h, roots, recursive);
    }
}

static void onRemoveSelected() {
    if (g_busy) return;
    std::vector<int> sel;
    int pos = -1;
    while ((pos = ListView_GetNextItem(g_list, pos, LVNI_SELECTED)) != -1) {
        sel.push_back(pos);
    }
    for (int i = (int)sel.size() - 1; i >= 0; --i) {
        int idx = sel[i];
        if (idx >= 0 && idx < (int)g_items.size()) {
            ListView_DeleteItem(g_list, idx);
            g_items.erase(g_items.begin() + idx);
        }
    }
    refreshSummaryStatus();
}

static void onClearList() {
    if (g_busy) return;
    ListView_DeleteAllItems(g_list);
    g_items.clear();
    g_doneCount = 0;
    g_failCount = 0;
    refreshSummaryStatus();
    InvalidateRect(g_wnd, NULL, FALSE);
}

struct BatchThreadArgs {
    std::vector<std::wstring> paths;
    HWND notify;
    HANDLE cancelEvent;
};

static unsigned __stdcall BatchWorkerThread(void* raw) {
    BatchThreadArgs* args = static_cast<BatchThreadArgs*>(raw);
    if (!args) return 1;

    for (size_t i = 0; i < args->paths.size(); ++i) {
        if (args->cancelEvent && WaitForSingleObject(args->cancelEvent, 0) == WAIT_OBJECT_0) {
            BatchUpdate* upd = new BatchUpdate;
            upd->index = (int)i;
            upd->state = 3;
            upd->status = L"已取消";
            PostMessageW(args->notify, WM_BATCH_ITEM, 0, (LPARAM)upd);
            continue;
        }

        {
            BatchUpdate* upd = new BatchUpdate;
            upd->index = (int)i;
            upd->state = 0;
            upd->status = L"处理中...";
            PostMessageW(args->notify, WM_BATCH_ITEM, 0, (LPARAM)upd);
        }

        int exitCode = 1;
        std::wstring msg;
        BomJob* job = new BomJob;
        job->input = args->paths[i];
        job->mode = MODE_COLOR;
        job->size = 800;
        job->notify = args->notify;
        job->cancelEvent = args->cancelEvent;
        job->outExitCode = &exitCode;
        job->outMessage = &msg;

        RunBomJob(job);

        BatchUpdate* upd = new BatchUpdate;
        upd->index = (int)i;
        if (args->cancelEvent && WaitForSingleObject(args->cancelEvent, 0) == WAIT_OBJECT_0) {
            upd->state = 3;
            upd->status = L"已取消";
        } else if (exitCode == 0) {
            upd->state = 1;
            upd->status = msg.empty() ? L"已完成" : msg;
        } else {
            upd->state = 2;
            upd->status = L"失败: " + (msg.empty() ? L"处理异常" : msg);
        }
        PostMessageW(args->notify, WM_BATCH_ITEM, 0, (LPARAM)upd);
    }

    PostMessageW(args->notify, WM_BATCH_DONE, 0, 0);
    delete args;
    return 0;
}

static void setControlsBusy(bool busy) {
    g_busy = busy;
    EnableWindow(g_btnAddFiles, !busy);
    EnableWindow(g_btnAddDir, !busy);
    EnableWindow(g_btnAddRec, !busy);
    EnableWindow(g_btnRemove, !busy);
    EnableWindow(g_btnClear, !busy);
    EnableWindow(g_btnRun, TRUE);
    SetWindowTextW(g_btnRun, busy ? L"取消任务" : L"开始批量赋色");
}

static HWND addControl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
                       int x, int y, int w, int h, int id, HFONT font) {
    HWND c = CreateWindowExW(0, cls, text,
                             WS_CHILD | WS_VISIBLE | ((wcscmp(cls, L"STATIC") == 0) ? 0 : WS_TABSTOP) | style,
                             x, y, w, h, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    if (font) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        g_wnd = h;
        DragAcceptFiles(h, TRUE);
        g_bgBrush = CreateSolidBrush(RGB(246, 248, 251));

        g_titleFont = CreateFontW(-22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        g_boldFont  = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        g_bodyFont  = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        g_smallFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");

        addControl(h, L"STATIC", L"CATPart 批量实体赋色工具", 0, 24, 16, 480, 28, ID_TITLE, g_titleFont);
        addControl(h, L"STATIC", L"按几何特征（体积/面积/惯量）生成稳定颜色，先重置显示属性再赋色，直接保存回原文件（含隐藏文件/隐藏实体）",
                   0, 24, 48, 860, 20, ID_SUB, g_smallFont);

        g_btnAddFiles = addControl(h, L"BUTTON", L"添加文件...", BS_OWNERDRAW, 24, 80, 108, 34, ID_BTN_ADD_FILES, g_bodyFont);
        g_btnAddDir   = addControl(h, L"BUTTON", L"添加文件夹...", BS_OWNERDRAW, 140, 80, 118, 34, ID_BTN_ADD_DIR, g_bodyFont);
        g_btnAddRec   = addControl(h, L"BUTTON", L"添加文件夹(递归)...", BS_OWNERDRAW, 266, 80, 152, 34, ID_BTN_ADD_REC, g_bodyFont);
        g_btnRemove   = addControl(h, L"BUTTON", L"移除选中", BS_OWNERDRAW, 426, 80, 96, 34, ID_BTN_REMOVE, g_bodyFont);
        g_btnClear    = addControl(h, L"BUTTON", L"清空列表", BS_OWNERDRAW, 530, 80, 96, 34, ID_BTN_CLEAR, g_bodyFont);
        g_btnRun      = addControl(h, L"BUTTON", L"开始批量赋色", BS_OWNERDRAW | BS_DEFPUSHBUTTON, 748, 78, 148, 38, ID_BTN_RUN, g_boldFont);

        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
                                 24, 128, 872, 360, h, (HMENU)(INT_PTR)ID_LIST, GetModuleHandleW(NULL), NULL);
        SendMessageW(g_list, WM_SETFONT, (WPARAM)g_bodyFont, TRUE);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.fmt = LVCFMT_LEFT;

        col.pszText = const_cast<LPWSTR>(L"文件名称");
        col.cx = 230;
        ListView_InsertColumn(g_list, 0, &col);

        col.pszText = const_cast<LPWSTR>(L"大小");
        col.cx = 90;
        ListView_InsertColumn(g_list, 1, &col);

        col.pszText = const_cast<LPWSTR>(L"文件夹");
        col.cx = 340;
        ListView_InsertColumn(g_list, 2, &col);

        col.pszText = const_cast<LPWSTR>(L"状态");
        col.cx = 190;
        ListView_InsertColumn(g_list, 3, &col);

        g_status = addControl(h, L"STATIC", L"", 0, 24, 520, 872, 20, ID_STATUS, g_smallFont);
        g_entityStatus = addControl(h, L"STATIC", L"", 0, 24, 566, 872, 20, ID_ENTITY_STATUS, g_smallFont);
        refreshSummaryStatus();
        refreshEntityStatus();
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        SetBkMode(dc, TRANSPARENT);
        int id = GetDlgCtrlID((HWND)l);
        SetTextColor(dc, (id == ID_TITLE) ? RGB(22, 40, 56) : RGB(84, 100, 116));
        return (INT_PTR)g_bgBrush;
    }

    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(h, &r);
        FillRect((HDC)w, &r, g_bgBrush);
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);

        // Bar 1: File batch progress
        RECT rail1 = { 24, 502, 896, 510 };
        HBRUSH g = CreateSolidBrush(RGB(221, 229, 236));
        FillRect(dc, &rail1, g);

        int filePct = 0;
        if (g_totalCount > 0) {
            filePct = (g_doneCount + g_failCount) * 100 / g_totalCount;
            if (filePct > 100) filePct = 100;
        }
        if (filePct > 0) {
            RECT fill1 = rail1;
            fill1.right = rail1.left + (rail1.right - rail1.left) * filePct / 100;
            HBRUSH b1 = CreateSolidBrush(RGB(40, 113, 168));
            FillRect(dc, &fill1, b1);
            DeleteObject(b1);
        }

        // Bar 2: Entity progress within current file
        RECT rail2 = { 24, 548, 896, 556 };
        FillRect(dc, &rail2, g);
        DeleteObject(g);

        int entPct = g_entityPct;
        if (entPct < 0) entPct = 0;
        if (entPct > 100) entPct = 100;
        if (entPct > 0) {
            RECT fill2 = rail2;
            fill2.right = rail2.left + (rail2.right - rail2.left) * entPct / 100;
            HBRUSH b2 = CreateSolidBrush(RGB(34, 148, 118));
            FillRect(dc, &fill2, b2);
            DeleteObject(b2);
        }

        EndPaint(h, &ps);
        return 0;
    }

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* d = (DRAWITEMSTRUCT*)l;
        if (d->CtlType != ODT_BUTTON) break;

        bool primary = (d->CtlID == ID_BTN_RUN);
        bool disabled = (d->itemState & ODS_DISABLED) != 0;
        bool pressed = (d->itemState & ODS_SELECTED) != 0;

        COLORREF bg = primary
            ? (disabled ? RGB(150, 184, 212) : (pressed ? RGB(28, 88, 136) : RGB(40, 113, 168)))
            : (disabled ? RGB(240, 243, 246) : (pressed ? RGB(226, 234, 242) : RGB(255, 255, 255)));
        COLORREF border = primary
            ? bg
            : (disabled ? RGB(214, 222, 230) : RGB(194, 206, 218));
        COLORREF fg = primary
            ? RGB(255, 255, 255)
            : (disabled ? RGB(140, 152, 164) : RGB(36, 52, 68));

        HBRUSH br = CreateSolidBrush(bg);
        HPEN pen = CreatePen(PS_SOLID, 1, border);
        HGDIOBJ oldBr = SelectObject(d->hDC, br);
        HGDIOBJ oldPen = SelectObject(d->hDC, pen);
        RoundRect(d->hDC, d->rcItem.left, d->rcItem.top, d->rcItem.right, d->rcItem.bottom, 6, 6);
        SelectObject(d->hDC, oldBr);
        SelectObject(d->hDC, oldPen);
        DeleteObject(br);
        DeleteObject(pen);

        wchar_t text[128] = {0};
        GetWindowTextW(d->hwndItem, text, 128);
        SetBkMode(d->hDC, TRANSPARENT);
        SetTextColor(d->hDC, fg);
        DrawTextW(d->hDC, text, -1, &d->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return TRUE;
    }

    case WM_DROPFILES: {
        if (g_busy) {
            DragFinish((HDROP)w);
            return 0;
        }
        HDROP drop = (HDROP)w;
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
        std::vector<std::wstring> roots;
        for (UINT i = 0; i < count; ++i) {
            UINT len = DragQueryFileW(drop, i, NULL, 0);
            std::wstring path(len + 1, L'\0');
            DragQueryFileW(drop, i, &path[0], len + 1);
            path.resize(len);
            roots.push_back(path);
        }
        DragFinish(drop);
        if (!roots.empty()) {
            startAsyncScan(h, roots, true);
        }
        return 0;
    }

    case WM_SCAN_DONE: {
        std::vector<std::wstring>* found = (std::vector<std::wstring>*)l;
        if (found) {
            for (size_t i = 0; i < found->size(); ++i) {
                addFileToList((*found)[i]);
            }
            delete found;
        }
        refreshSummaryStatus();
        return 0;
    }

    case WM_CONTEXTMENU: {
        if (g_busy) return 0;
        if ((HWND)w == g_list || (HWND)w == h) {
            POINT pt;
            pt.x = (short)LOWORD(l);
            pt.y = (short)HIWORD(l);
            if (pt.x == -1 && pt.y == -1) {
                RECT rc;
                GetWindowRect(g_list, &rc);
                pt.x = (rc.left + rc.right) / 2;
                pt.y = (rc.top + rc.bottom) / 2;
            }
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, ID_BTN_ADD_FILES, L"添加文件...");
            AppendMenuW(menu, MF_STRING, ID_BTN_ADD_DIR, L"添加文件夹...");
            AppendMenuW(menu, MF_STRING, ID_BTN_ADD_REC, L"添加文件夹(递归)...");
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(menu, MF_STRING, ID_BTN_REMOVE, L"移除选中");
            AppendMenuW(menu, MF_STRING, ID_BTN_CLEAR, L"清空列表");
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, NULL);
            DestroyMenu(menu);
            return 0;
        }
        break;
    }

    case WM_COMMAND: {
        switch (LOWORD(w)) {
        case ID_BTN_ADD_FILES: onAddFiles(h); return 0;
        case ID_BTN_ADD_DIR:   onAddFolder(h, false); return 0;
        case ID_BTN_ADD_REC:   onAddFolder(h, true); return 0;
        case ID_BTN_REMOVE:    onRemoveSelected(); return 0;
        case ID_BTN_CLEAR:     onClearList(); return 0;

        case ID_BTN_RUN: {
            if (g_busy) {
                if (g_cancelEvent) SetEvent(g_cancelEvent);
                EnableWindow(g_btnRun, FALSE);
                SetWindowTextW(g_btnRun, L"正在取消...");
                refreshSummaryStatus(L"正在中止当前任务");
                return 0;
            }
            if (g_items.empty()) {
                MessageBoxW(h, L"请先添加至少一个 .CATPart 文件。", L"提示", MB_OK | MB_ICONWARNING);
                return 0;
            }

            g_doneCount = 0;
            g_failCount = 0;
            g_totalCount = (int)g_items.size();
            for (size_t i = 0; i < g_items.size(); ++i) {
                g_items[i].statusText = L"等待中";
                ListView_SetItemText(g_list, (int)i, 3, const_cast<LPWSTR>(g_items[i].statusText.c_str()));
            }

            g_cancelEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
            BatchThreadArgs* args = new BatchThreadArgs;
            args->notify = h;
            args->cancelEvent = g_cancelEvent;
            for (size_t i = 0; i < g_items.size(); ++i) {
                args->paths.push_back(g_items[i].fullPath);
            }

            setControlsBusy(true);
            refreshSummaryStatus(L"正在批量赋色");
            InvalidateRect(h, NULL, FALSE);

            uintptr_t th = _beginthreadex(NULL, 0, BatchWorkerThread, args, 0, NULL);
            if (th) {
                g_workerThread = (HANDLE)th;
            } else {
                delete args;
                if (g_cancelEvent) { CloseHandle(g_cancelEvent); g_cancelEvent = NULL; }
                setControlsBusy(false);
                MessageBoxW(h, L"无法启动后台处理线程。", L"错误", MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        }
        break;
    }

    case WM_PROGRESS: {
        if (w == 0) {
            g_entityPct = 0;
            refreshEntityStatus();
            RECT r = { 24, 548, 896, 556 };
            InvalidateRect(h, &r, FALSE);
        } else if (w == 1) {
            g_entityPct = (int)l;
            refreshEntityStatus();
            RECT r = { 24, 548, 896, 556 };
            InvalidateRect(h, &r, FALSE);
        } else if (w == 3 || w == 4 || w == 5) {
            std::wstring* p = (std::wstring*)l;
            delete p;
            g_entityPct = (w == 3) ? 100 : 0;
            refreshEntityStatus();
            RECT r = { 24, 548, 896, 556 };
            InvalidateRect(h, &r, FALSE);
        }
        return 0;
    }

    case WM_BATCH_ITEM: {
        BatchUpdate* upd = (BatchUpdate*)l;
        if (upd) {
            if (upd->index >= 0 && upd->index < (int)g_items.size()) {
                g_items[upd->index].statusText = upd->status;
                ListView_SetItemText(g_list, upd->index, 3, const_cast<LPWSTR>(g_items[upd->index].statusText.c_str()));
                ListView_EnsureVisible(g_list, upd->index, FALSE);
                if (upd->state == 0) {
                    g_currentFileName = g_items[upd->index].fileName;
                    g_entityPct = 0;
                    refreshSummaryStatus(L"正在处理: " + g_currentFileName);
                    refreshEntityStatus();
                } else {
                    if (upd->state == 1) {
                        g_doneCount++;
                        g_entityPct = 100;
                    } else if (upd->state == 2) {
                        g_failCount++;
                        g_entityPct = 0;
                    }
                    refreshSummaryStatus(L"正在批量赋色");
                    refreshEntityStatus();
                }
            }
            delete upd;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }

    case WM_BATCH_DONE: {
        if (g_workerThread) {
            WaitForSingleObject(g_workerThread, INFINITE);
            CloseHandle(g_workerThread);
            g_workerThread = NULL;
        }
        bool canceled = (g_cancelEvent && WaitForSingleObject(g_cancelEvent, 0) == WAIT_OBJECT_0);
        if (g_cancelEvent) {
            CloseHandle(g_cancelEvent);
            g_cancelEvent = NULL;
        }
        setControlsBusy(false);
        g_currentFileName.clear();
        refreshSummaryStatus(canceled ? L"任务已取消" : L"全部处理完成");
        refreshEntityStatus();
        InvalidateRect(h, NULL, FALSE);

        if (g_closeRequested) {
            DestroyWindow(h);
            return 0;
        }

        wchar_t summary[256];
        swprintf_s(summary, L"%s\n\n总计：%d 个文件\n成功：%d\n失败：%d",
                   canceled ? L"批量赋色任务已取消。" : L"批量赋色完成！",
                   g_totalCount, g_doneCount, g_failCount);
        MessageBoxW(h, summary, canceled ? L"已取消" : L"完成",
                    MB_OK | (g_failCount > 0 ? MB_ICONWARNING : MB_ICONINFORMATION));
        return 0;
    }

    case WM_CLOSE: {
        if (g_busy) {
            if (g_closeRequested) {
                return 0; // Already waiting for safe stop
            }
            if (MessageBoxW(h, L"批量赋色正在进行中，是否安全停止任务并退出？\n（若当前文件已进入提交阶段，将等待提交完成后再退出，避免损坏原文件）",
                            L"确认退出", MB_YESNO | MB_ICONQUESTION) != IDYES) {
                return 0;
            }
            g_closeRequested = true;
            if (g_cancelEvent) SetEvent(g_cancelEvent);
            EnableWindow(g_btnRun, FALSE);
            EnableMenuItem(GetSystemMenu(h, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
            SetWindowTextW(g_status, L"正在安全停止当前任务并保护原文件，请稍候...");
            return 0;
        }
        DestroyWindow(h);
        return 0;
    }

    case WM_DESTROY:
        if (g_titleFont) DeleteObject(g_titleFont);
        if (g_boldFont)  DeleteObject(g_boldFont);
        if (g_bodyFont)  DeleteObject(g_bodyFont);
        if (g_smallFont) DeleteObject(g_smallFont);
        if (g_bgBrush)   DeleteObject(g_bgBrush);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 2 && (_wcsicmp(argv[1], L"/c") == 0 || _wcsicmp(argv[1], L"-c") == 0)) {
        if (argc < 3) {
            LocalFree(argv);
            CoUninitialize();
            return 2;
        }
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* dummy = NULL;
            freopen_s(&dummy, "CONOUT$", "w", stdout);
            freopen_s(&dummy, "CONOUT$", "w", stderr);
        }

        wchar_t tempDir[MAX_PATH + 1] = {0};
        GetTempPathW(MAX_PATH, tempDir);
        SYSTEMTIME st = {};
        GetLocalTime(&st);
        wchar_t receiptPath[MAX_PATH + 80] = {0};
        swprintf_s(receiptPath, L"%sCATPart_Color_Receipt_%04d%02d%02d_%02d%02d%02d_%lu.txt",
                   tempDir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                   (unsigned long)GetCurrentProcessId());
        FILE* rf = _wfopen(receiptPath, L"wb");
        if (rf) {
            fwrite("\xEF\xBB\xBF", 1, 3, rf);
            fprintf(rf, "index\texit_code\tcommitted\tinput\tmessage\r\n");
        }

        int overallExit = 0;
        for (int i = 2; i < argc; ++i) {
            int ec = 1;
            std::wstring msg;
            BomJob* job = new BomJob;
            job->input = argv[i];
            job->mode = MODE_COLOR;
            job->size = 800;
            job->outExitCode = &ec;
            job->outMessage = &msg;
            RunBomJob(job);
            if (ec != 0) overallExit = 1;

            int uLen1 = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, NULL, 0, NULL, NULL);
            std::string uIn(uLen1 > 0 ? uLen1 : 1, '\0');
            if (uLen1 > 0) WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, &uIn[0], uLen1, NULL, NULL);

            int uLen2 = WideCharToMultiByte(CP_UTF8, 0, msg.c_str(), -1, NULL, 0, NULL, NULL);
            std::string uMsg(uLen2 > 0 ? uLen2 : 1, '\0');
            if (uLen2 > 0) WideCharToMultiByte(CP_UTF8, 0, msg.c_str(), -1, &uMsg[0], uLen2, NULL, NULL);

            if (rf) {
                fprintf(rf, "%d\t%d\t%s\t%s\t%s\r\n",
                        i - 1, ec, (ec == 0 ? "1" : "0"), uIn.c_str(), uMsg.c_str());
            }
            fprintf(ec == 0 ? stdout : stderr, "[%s] (ec=%d) %s | %s\n",
                    (ec == 0 ? "OK" : "FAIL"), ec, uIn.c_str(), uMsg.c_str());
        }
        if (rf) {
            fclose(rf);
            fwprintf(stdout, L"Receipt: %s\n", receiptPath);
        }
        LocalFree(argv);
        CoUninitialize();
        return overallExit;
    }
    if (argv) LocalFree(argv);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"CAAColorBatchWindow";
    RegisterClassExW(&wc);

    HWND h = CreateWindowW(wc.lpszClassName, L"CATPart 批量实体赋色工具",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, 936, 636, NULL, NULL, inst, NULL);

    if (wc.hIcon)   SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)wc.hIcon);
    if (wc.hIconSm) SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)wc.hIconSm);

    ShowWindow(h, show);
    UpdateWindow(h);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return 0;
}
