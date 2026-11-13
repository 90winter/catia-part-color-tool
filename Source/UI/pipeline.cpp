#define NOMINMAX
#define UNICODE
#define _UNICODE

#include "pipeline.h"
#include <process.h>
#include <share.h>
#include <stdio.h>
#include <stdlib.h>
#include <float.h>
#include <algorithm>
#include <vector>
#include <string>

namespace {

const UINT MSG = WM_APP + 1;

const DWORD WORKER_HEARTBEAT_TIMEOUT_MS = 180000;

// Total time budget of the CAA worker chain (30 minutes).  Kept in sync with
// TASK_TOTAL_TIMEOUT_MS below so a single constant documents the whole chain.
const DWORD WORKER_TOTAL_TIMEOUT_MS = 1800000;

// Total time budget of the whole job (CAA worker + Node/native Excel writer +
// copy to the destination folder): 30 minutes.
const DWORD TASK_TOTAL_TIMEOUT_MS = 1800000;

const DWORD NODE_TIMEOUT_MS = 120000;

static void send(HWND h, WPARAM stage, LPARAM value = 0) {
    if (h) PostMessageW(h, MSG, stage, value);
}

static void finish(const BomJob& j, WPARAM stage, const std::wstring& value) {
    if (j.outMessage) {
        *j.outMessage = value;
    }
    if (j.notify) {
        std::wstring* p = new std::wstring(value);
        if (!PostMessageW(j.notify, MSG, stage, (LPARAM)p)) {
            delete p;
        }
    }
}

static std::wstring quote(const std::wstring& s) {
    std::wstring r = L"\"";
    size_t slash = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (c == L'\\') {
            ++slash;
            continue;
        }
        if (c == L'\"') {
            r.append(slash * 2 + 1, L'\\');
            r += c;
        } else {
            r.append(slash, L'\\');
            r += c;
        }
        slash = 0;
    }
    r.append(slash * 2, L'\\');
    return r + L"\"";
}

static bool full(const std::wstring& in, std::wstring& out) {
    DWORD n = GetFullPathNameW(in.c_str(), 0, NULL, NULL);
    if (!n) return false;
    std::vector<wchar_t> b(n + 1);
    DWORD k = GetFullPathNameW(in.c_str(), (DWORD)b.size(), &b[0], NULL);
    if (!k || k >= b.size()) return false;
    out = &b[0];
    return true;
}

// Check if path is a valid directory (allow reparse points / junctions)
static bool isDirectory(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return (a != INVALID_FILE_ATTRIBUTES) && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Check if path is a regular file (allow reparse points / junctions)
static bool regular(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string readFile(const std::wstring& p) {
    FILE* f = _wfsopen(p.c_str(), L"rb", _SH_DENYNO);
    if (!f) return "";
    std::string s;
    char b[8192];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) != 0) {
        s.append(b, n);
    }
    fclose(f);
    return s;
}

static bool writeTextFile(const std::wstring& path, const std::string& content) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
    return true;
}

static std::string replaceAll(std::string str, const std::string& from, const std::string& to) {
    if (from.empty()) return str;
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
    return str;
}

static std::wstring utf8w(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    if (!n) return L"";
    std::wstring r(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &r[0], n);
    return r;
}

static std::string wideNarrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, NULL, 0, NULL, NULL);
    if (n <= 0) return "";
    std::string s(n, '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, &s[0], n, NULL, NULL);
    s.resize(n - 1);
    return s;
}

static std::string safeNarrowPath(const std::wstring& w) {
    if (w.empty()) return "";
    bool asciiOnly = true;
    for (size_t i = 0; i < w.size(); ++i) {
        if (w[i] > 127) {
            asciiOnly = false;
            break;
        }
    }
    if (asciiOnly) {
        return wideNarrow(w);
    }
    wchar_t shortBuf[MAX_PATH + 1] = {0};
    DWORD res = GetShortPathNameW(w.c_str(), shortBuf, MAX_PATH);
    if (res > 0 && res <= MAX_PATH) {
        return wideNarrow(shortBuf);
    }
    return wideNarrow(w);
}

static std::string wideUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    if (n <= 0) return "";
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, NULL, NULL);
    return s;
}

// Locate application root directory dynamically
static std::wstring getAppRoot() {
    wchar_t buf[MAX_PATH + 1] = {0};
    DWORD len = GetModuleFileNameW(NULL, buf, MAX_PATH);
    if (len == 0 || len > MAX_PATH) return L"";
    std::wstring p(buf);
    size_t lastSlash = p.find_last_of(L"\\/");
    if (lastSlash == std::wstring::npos) return L"";
    std::wstring dir = p.substr(0, lastSlash);

    // 1. Check if Runtime directory is directly under current directory
    DWORD attr = GetFileAttributesW((dir + L"\\Runtime").c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return dir;
    }

    // 2. If running from Source\UI or similar subfolder
    size_t p1 = dir.find_last_of(L"\\/");
    if (p1 != std::wstring::npos) {
        std::wstring parent1 = dir.substr(0, p1);
        size_t p2 = parent1.find_last_of(L"\\/");
        if (p2 != std::wstring::npos) {
            std::wstring parent2 = parent1.substr(0, p2);
            attr = GetFileAttributesW((parent2 + L"\\Runtime").c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                return parent2;
            }
        }
        attr = GetFileAttributesW((parent1 + L"\\Runtime").c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            return parent1;
        }
    }

    return dir;
}

static std::vector<std::vector<std::string> > csvRows(const std::string& data) {
    std::vector<std::vector<std::string> > rows;
    std::vector<std::string> row;
    std::string cell;
    bool q = false;
    size_t start = (data.size() >= 3 && (unsigned char)data[0] == 0xef &&
                    (unsigned char)data[1] == 0xbb && (unsigned char)data[2] == 0xbf) ? 3 : 0;

    for (size_t i = start; i < data.size(); i++) {
        char c = data[i];
        if (q) {
            if (c == '"') {
                if (i + 1 < data.size() && data[i + 1] == '"') {
                    cell += '"';
                    ++i;
                } else {
                    q = false;
                }
            } else {
                cell += c;
            }
        } else if (c == '"') {
            q = true;
        } else if (c == ',') {
            row.push_back(cell);
            cell.clear();
        } else if (c == '\n') {
            if (!cell.empty() && cell[cell.size() - 1] == '\r') {
                cell.erase(cell.size() - 1);
            }
            row.push_back(cell);
            cell.clear();
            rows.push_back(row);
            row.clear();
        } else {
            cell += c;
        }
    }
    if (!cell.empty() || !row.empty()) {
        row.push_back(cell);
        rows.push_back(row);
    }
    return rows;
}

static std::wstring findOne(const std::wstring& dir, const wchar_t* pattern) {
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &d);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring found;
    do {
        if (!(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            if (!found.empty()) {
                found = L"";
                break;
            }
            found = d.cFileName;
        }
    } while (FindNextFileW(h, &d));
    FindClose(h);
    return found;
}

static int totalCount(const std::string& log) {
    size_t p = log.find("GetSolidBodies=");
    if (p == std::string::npos) return -1;
    size_t e = log.find('\n', p);
    if (e == std::string::npos) return -1;
    size_t c = log.find("count=", p);
    if (c == std::string::npos || c > e) return -1;
    int n = atoi(log.c_str() + c + 6);
    return n > 0 ? n : -1;
}

static bool validXlsx(const std::wstring& path) {
    FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!f) return false;
    unsigned char b[4] = {};
    size_t n = fread(b, 1, 4, f);
    fclose(f);
    return n == 4 && b[0] == 0x50 && b[1] == 0x4b && b[2] == 3 && b[3] == 4;
}

static int groupCount(const std::string& s) {
    size_t p = s.rfind("group_count=");
    if (p == std::string::npos) return -1;
    int n = atoi(s.c_str() + p + 12);
    return n >= 0 ? n : -1;
}

static int phaseProgress(const std::string& s) {
    if (s.find("color_save_ok=1") != std::string::npos) {
        return 100;
    }
    if (s.find("SaveAs=") != std::string::npos) {
        return 95;
    }
    size_t p = s.rfind("color_body_done=");
    if (p != std::string::npos) {
        int a = 0, b = 0;
        if (sscanf_s(s.c_str() + p, "color_body_done=%d/%d", &a, &b) == 2 && b > 0) {
            return 50 + 40 * a / b;
        }
    }
    p = s.rfind("render_group_done=");
    if (p != std::string::npos) {
        int a = 0, b = 0;
        if (sscanf_s(s.c_str() + p, "render_group_done=%d/%d", &a, &b) == 2 && b > 0) {
            return 50 + 50 * a / b;
        }
    }
    p = s.rfind("render_group_begin=");
    if (p != std::string::npos) {
        int a = 0, b = 0;
        if (sscanf_s(s.c_str() + p, "render_group_begin=%d/%d", &a, &b) == 2 && b > 0) {
            int base = 50 + 50 * (a > 0 ? a - 1 : 0) / b;
            return std::min(99, base + 1);
        }
    }
    p = s.rfind("measure_done=");
    if (p != std::string::npos) {
        int a = 0, b = 0;
        if (sscanf_s(s.c_str() + p, "measure_done=%d/%d", &a, &b) == 2 && b > 0) {
            return 50 * a / b;
        }
    }
    p = s.rfind("measure_begin=");
    if (p != std::string::npos) {
        int a = 0, b = 0;
        if (sscanf_s(s.c_str() + p, "measure_begin=%d/%d", &a, &b) == 2 && b > 0) {
            int base = 50 * (a > 0 ? a - 1 : 0) / b;
            return std::min(49, base + 1);
        }
    }
    return 0;
}

static bool finalMarker(const std::string& s) {
    size_t end = s.find_last_not_of("\r\n \t");
    if (end == std::string::npos) return false;
    size_t begin = s.find_last_of("\n", end);
    std::string line = s.substr(begin == std::string::npos ? 0 : begin + 1,
                                end - (begin == std::string::npos ? 0 : begin + 1) + 1);
    return line == "failures=0";
}

// Status values the CAA worker may emit for one grouped entity row.
//   ok                          : measurement + preview fully succeeded
//   area_warning                : surface area could not be measured
//   render_warning              : preview picture fell back to a blank image
//   inertia_warning             : principal moments of inertia unavailable
// plus every combination of the three warnings (sorted alphabetically).
static bool isValidGroupStatus(const std::string& st) {
    return st == "ok" ||
           st == "area_warning" ||
           st == "render_warning" ||
           st == "inertia_warning" ||
           st == "area_render_warning" ||
           st == "area_inertia_warning" ||
           st == "render_inertia_warning" ||
           st == "area_render_inertia_warning";
}

static bool validateRows(const std::vector<std::vector<std::string> >& rows, int expected, const std::wstring& dir) {
    if (expected < 0 || rows.size() != (size_t)expected + 1) return false;
    const std::vector<std::string>& h = rows[0];
    int image = -1, status = -1;
    for (size_t i = 0; i < h.size(); i++) {
        if (h[i] == "image") image = (int)i;
        if (h[i] == "status") status = (int)i;
    }
    if (image < 0 || status < 0) return false;
    for (size_t i = 1; i < rows.size(); i++) {
        if (rows[i].size() <= (size_t)std::max(image, status) ||
            !isValidGroupStatus(rows[i][status])) {
            return false;
        }
        std::wstring name = utf8w(rows[i][image]);
        if (name.empty() || name == L"." || name == L".." ||
            name.find_first_of(L"\\/:") != std::wstring::npos ||
            !regular(dir + L"\\" + name)) {
            return false;
        }
    }
    return true;
}

// Build the child environment block and keep parent environment clean.
static std::vector<wchar_t> environment(const std::wstring& input, const std::wstring& output) {
    SetEnvironmentVariableW(L"CAA_INPUT_PATH", input.c_str());
    SetEnvironmentVariableW(L"CAA_OUTPUT_DIR", output.c_str());

    std::vector<wchar_t> block;
    LPWCH p = GetEnvironmentStringsW();
    if (p) {
        for (LPWCH x = p; *x; x += wcslen(x) + 1) {
            size_t len = wcslen(x);
            block.insert(block.end(), x, x + len);
            block.push_back(0);
        }
        FreeEnvironmentStringsW(p);
    }
    // Fallback: the API must never leave the child without our variables.
    if (block.empty()) {
        const std::wstring a = L"CAA_INPUT_PATH=" + input;
        const std::wstring b = L"CAA_OUTPUT_DIR=" + output;
        block.insert(block.end(), a.begin(), a.end());
        block.push_back(0);
        block.insert(block.end(), b.begin(), b.end());
        block.push_back(0);
    }
    block.push_back(0);

    // Immediately clean up parent environment variables to avoid pollution
    SetEnvironmentVariableW(L"CAA_INPUT_PATH", NULL);
    SetEnvironmentVariableW(L"CAA_OUTPUT_DIR", NULL);

    return block;
}

// Write one UTF-8 diagnostic block straight into an open log handle.
static void writeLogHandle(HANDLE file, const std::string& text) {
    if (file == NULL || file == INVALID_HANDLE_VALUE || text.empty()) return;
    DWORD written = 0;
    WriteFile(file, text.data(), (DWORD)text.size(), &written, NULL);
    FlushFileBuffers(file);
}

static void terminateChildJob(HANDLE& hJob, PROCESS_INFORMATION& pi) {
    if (hJob) {
        TerminateJobObject(hJob, 1);
        CloseHandle(hJob);
        hJob = NULL;
    }
    if (pi.hProcess) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 3000);
    }
}

// Launch a child process with stdio redirected into `logfile`.
static bool launch(const std::wstring& cmd, const std::wstring& logfile, std::vector<wchar_t>* env,
                   PROCESS_INFORMATION& pi, const wchar_t* workDir = NULL, HANDLE* outJob = NULL) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE file = CreateFileW(logfile.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;

    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (input == INVALID_HANDLE_VALUE) {
        CloseHandle(file);
        return false;
    }

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = file;
    si.hStdError = file;
    si.hStdInput = input;

    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(0);
    ZeroMemory(&pi, sizeof(pi));

    HANDLE hJob = NULL;
    if (outJob != NULL) {
        *outJob = NULL;
        hJob = CreateJobObjectW(NULL, NULL);
        if (!hJob) {
            DWORD jobErr = GetLastError();
            char jbuf[160];
            sprintf_s(jbuf, sizeof(jbuf),
                      "[launch_failed] CreateJobObjectW_err=%lu\r\n", (unsigned long)jobErr);
            writeLogHandle(file, std::string(jbuf));
            CloseHandle(file);
            CloseHandle(input);
            return false;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
        ZeroMemory(&jeli, sizeof(jeli));
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
            DWORD jobErr = GetLastError();
            CloseHandle(hJob);
            char jbuf[160];
            sprintf_s(jbuf, sizeof(jbuf),
                      "[launch_failed] SetInformationJobObject_err=%lu\r\n", (unsigned long)jobErr);
            writeLogHandle(file, std::string(jbuf));
            CloseHandle(file);
            CloseHandle(input);
            return false;
        }
    }

    // Diagnostic header (written before the child starts)
    {
        std::string head = "[launch] cmd=";
        head += wideUtf8(cmd);
        head += "\r\n";
        if (workDir != NULL) {
            head += "[launch] cwd=";
            head += wideUtf8(workDir);
            head += "\r\n";
        }
        if (env != NULL) {
            head += "[launch] env=unicode-block\r\n";
        } else {
            head += "[launch] env=inherit\r\n";
        }
        if (hJob != NULL) {
            head += "[launch] job_object=enabled\r\n";
        }
        writeLogHandle(file, head);
    }

    DWORD creationFlags = CREATE_NO_WINDOW | (env ? CREATE_UNICODE_ENVIRONMENT : 0);
    if (hJob != NULL) {
        creationFlags |= CREATE_SUSPENDED;
    }

    BOOL ok = CreateProcessW(NULL, &mutableCmd[0], NULL, NULL, TRUE,
                             creationFlags,
                             (env && !env->empty()) ? &(*env)[0] : NULL,
                             workDir, &si, &pi);
    if (!ok) {
        char buf[128];
        sprintf_s(buf, sizeof(buf), "[launch_failed] GetLastError=%lu\r\n",
                  (unsigned long)GetLastError());
        writeLogHandle(file, std::string(buf));
        if (hJob) {
            CloseHandle(hJob);
            hJob = NULL;
        }
    } else {
        if (hJob != NULL) {
            if (!AssignProcessToJobObject(hJob, pi.hProcess)) {
                // Child is still suspended: kill it, otherwise a failed bind
                // would leave an unreachable zombie process behind.
                DWORD bindErr = GetLastError();
                char bbuf[160];
                sprintf_s(bbuf, sizeof(bbuf),
                          "[launch_failed] AssignProcessToJobObject GetLastError=%lu\r\n",
                          (unsigned long)bindErr);
                writeLogHandle(file, std::string(bbuf));
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 3000);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                pi.hThread = NULL;
                pi.hProcess = NULL;
                CloseHandle(hJob);
                hJob = NULL;
                CloseHandle(input);
                CloseHandle(file);
                return false;
            }
            if (ResumeThread(pi.hThread) == (DWORD)-1) {
                // The suspended child could not be resumed: tear everything down.
                DWORD resumeErr = GetLastError();
                char rbuf[160];
                sprintf_s(rbuf, sizeof(rbuf),
                          "[launch_failed] ResumeThread GetLastError=%lu\r\n",
                          (unsigned long)resumeErr);
                writeLogHandle(file, std::string(rbuf));
                TerminateJobObject(hJob, 1);
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 3000);
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                pi.hThread = NULL;
                pi.hProcess = NULL;
                CloseHandle(hJob);
                hJob = NULL;
                CloseHandle(input);
                CloseHandle(file);
                return false;
            }
            writeLogHandle(file, "[launch] job_object=bound\r\n");
            *outJob = hJob;
        }
    }
    CloseHandle(input);
    CloseHandle(file);
    return ok != FALSE;
}

// Append one UTF-8 line to a log file (used for the Excel dual-engine trace).
static void appendLog(const std::wstring& path, const std::wstring& line) {
    FILE* f = _wfopen(path.c_str(), L"ab");
    if (!f) return;
    std::string out = wideUtf8(line) + "\r\n";
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
}

static bool removeDirTree(const std::wstring& path) {
    if (path.empty()) return false;
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &d);
    if (h == INVALID_HANDLE_VALUE) {
        return (RemoveDirectoryW(path.c_str()) != FALSE);
    }
    bool ok = true;
    do {
        if (!wcscmp(d.cFileName, L".") || !wcscmp(d.cFileName, L"..")) continue;
        std::wstring item = path + L"\\" + d.cFileName;
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!removeDirTree(item)) {
                ok = false;
            }
        } else {
            SetFileAttributesW(item.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(item.c_str())) {
                ok = false;
            }
        }
    } while (FindNextFileW(h, &d));
    FindClose(h);
    if (!RemoveDirectoryW(path.c_str())) {
        ok = false;
    }
    return ok;
}

static bool cleanStage(const std::wstring& stage, const std::wstring& temp) {
    std::wstring a, b;
    if (!full(stage, a) || !full(temp, b)) return false;
    if (!b.empty() && b[b.size() - 1] != L'\\') b += L'\\';
    if (a.size() <= b.size() || _wcsnicmp(a.c_str(), b.c_str(), b.size()) != 0 || !isDirectory(a)) {
        return false;
    }

    for (int attempt = 0; attempt < 5; attempt++) {
        WIN32_FIND_DATAW d;
        HANDLE h = FindFirstFileW((a + L"\\*").c_str(), &d);
        if (h == INVALID_HANDLE_VALUE) {
            Sleep(100);
            continue;
        }
        bool ok = true;
        do {
            if (!wcscmp(d.cFileName, L".") || !wcscmp(d.cFileName, L"..")) continue;
            std::wstring item = a + L"\\" + d.cFileName;
            if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!removeDirTree(item)) ok = false;
            } else {
                SetFileAttributesW(item.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (!DeleteFileW(item.c_str())) ok = false;
            }
        } while (FindNextFileW(h, &d));
        FindClose(h);
        if (ok) ok = (RemoveDirectoryW(a.c_str()) != FALSE);
        if (ok) return true;
        Sleep(100);
    }
    return false;
}

static std::wstring findNodePath(const std::wstring& appRoot) {
    std::wstring p = appRoot + L"\\Runtime\\Node\\node.exe";
    if (regular(p)) return p;

    p = appRoot + L"\\Runtime\\node.exe";
    if (regular(p)) return p;

    wchar_t envBuf[MAX_PATH + 1] = {0};
    if (GetEnvironmentVariableW(L"NODE_EXE", envBuf, MAX_PATH) > 0 && regular(envBuf)) {
        return envBuf;
    }

    static const wchar_t* const standardPaths[] = {
        L"C:\\Program Files\\nodejs\\node.exe",
        L"C:\\Program Files (x86)\\nodejs\\node.exe"
    };
    for (size_t i = 0; i < sizeof(standardPaths) / sizeof(standardPaths[0]); ++i) {
        if (regular(standardPaths[i])) return standardPaths[i];
    }

    wchar_t searchBuf[MAX_PATH + 1] = {0};
    DWORD found = SearchPathW(NULL, L"node.exe", NULL, MAX_PATH, searchBuf, NULL);
    if (found > 0 && found <= MAX_PATH && regular(searchBuf)) {
        return searchBuf;
    }
    return L"";
}

static std::wstring findCatstartPath() {
    wchar_t envBuf[MAX_PATH + 1] = {0};
    if (GetEnvironmentVariableW(L"CATSTART_PATH", envBuf, MAX_PATH) > 0 && regular(envBuf)) {
        return envBuf;
    }
    if (GetEnvironmentVariableW(L"CATIA_INSTALL_DIR", envBuf, MAX_PATH) > 0) {
        std::wstring p = std::wstring(envBuf) + L"\\code\\bin\\CATSTART.exe";
        if (regular(p)) return p;
    }

    static const wchar_t* const candidatePaths[] = {
        L"C:\\Program Files\\Dassault Systemes\\B26\\win_b64\\code\\bin\\CATSTART.exe",
        L"C:\\Program Files\\Dassault Systemes\\B28\\win_b64\\code\\bin\\CATSTART.exe",
        L"C:\\Program Files\\Dassault Systemes\\B27\\win_b64\\code\\bin\\CATSTART.exe",
        L"C:\\Program Files\\Dassault Systemes\\B29\\win_b64\\code\\bin\\CATSTART.exe",
        L"C:\\Program Files\\Dassault Systemes\\B30\\win_b64\\code\\bin\\CATSTART.exe",
        L"C:\\Program Files\\Dassault Systemes\\B31\\win_b64\\code\\bin\\CATSTART.exe",
        L"C:\\Program Files\\Dassault Systemes\\B25\\win_b64\\code\\bin\\CATSTART.exe",
        L"C:\\Program Files\\Dassault Systemes\\B24\\win_b64\\code\\bin\\CATSTART.exe",
        L"C:\\Program Files\\Dassault Systemes\\B21\\win_b64\\code\\bin\\CATSTART.exe",
        L"D:\\Program Files\\Dassault Systemes\\B26\\win_b64\\code\\bin\\CATSTART.exe",
        L"D:\\Program Files\\Dassault Systemes\\B28\\win_b64\\code\\bin\\CATSTART.exe",
        L"D:\\Program Files\\Dassault Systemes\\B30\\win_b64\\code\\bin\\CATSTART.exe",
        L"E:\\Program Files\\Dassault Systemes\\B26\\win_b64\\code\\bin\\CATSTART.exe",
        L"E:\\Program Files\\Dassault Systemes\\B28\\win_b64\\code\\bin\\CATSTART.exe"
    };
    for (size_t i = 0; i < sizeof(candidatePaths) / sizeof(candidatePaths[0]); ++i) {
        if (regular(candidatePaths[i])) return candidatePaths[i];
    }

    wchar_t searchBuf[MAX_PATH + 1] = {0};
    DWORD found = SearchPathW(NULL, L"CATSTART.exe", NULL, MAX_PATH, searchBuf, NULL);
    if (found > 0 && found <= MAX_PATH && regular(searchBuf)) {
        return searchBuf;
    }
    return L"";
}

static bool requestSafeWorkerStop(const std::wstring& stage,
                                  HANDLE workerJob,
                                  PROCESS_INFORMATION& pi,
                                  bool& outExitedNormally) {
    outExitedNormally = false;
    std::wstring cancelFlag = stage + L"\\cancel.flag";
    HANDLE hFlag = CreateFileW(cancelFlag.c_str(), GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFlag != INVALID_HANDLE_VALUE) {
        CloseHandle(hFlag);
    }

    std::wstring commitLock = stage + L"\\commit.lock";
    // If worker is in CommitWriteBack (commit.lock exists), never hard-kill it; wait for it to finish.
    if (GetFileAttributesW(commitLock.c_str()) != INVALID_FILE_ATTRIBUTES) {
        while (GetFileAttributesW(commitLock.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (WaitForSingleObject(pi.hProcess, 250) == WAIT_OBJECT_0) {
                outExitedNormally = true;
                return true;
            }
        }
        if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0) {
            outExitedNormally = true;
            return true;
        }
    } else {
        // Give worker a brief window to notice cancel.flag and exit cleanly.
        if (WaitForSingleObject(pi.hProcess, 1500) == WAIT_OBJECT_0) {
            outExitedNormally = true;
            return true;
        }
        if (GetFileAttributesW(commitLock.c_str()) != INVALID_FILE_ATTRIBUTES) {
            while (GetFileAttributesW(commitLock.c_str()) != INVALID_FILE_ATTRIBUTES) {
                if (WaitForSingleObject(pi.hProcess, 250) == WAIT_OBJECT_0) {
                    outExitedNormally = true;
                    return true;
                }
            }
            if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0) {
                outExitedNormally = true;
                return true;
            }
        }
    }

    terminateChildJob(workerJob, pi);
    return false;
}

} // namespace

unsigned __stdcall RunBomJob(void* raw) {
    BomJob* job = static_cast<BomJob*>(raw);
    if (!job) return 1;
    BomJob j = *job;
    delete job;

    // Global (whole chain) budget: CAA worker + Excel writer + file copy.
    DWORD taskStartTick = GetTickCount();
    DWORD taskDeadlineTick = taskStartTick + TASK_TOTAL_TIMEOUT_MS;

    send(j.notify, 0);

    std::wstring appRoot = getAppRoot();
    if (appRoot.empty() || !isDirectory(appRoot + L"\\Runtime")) {
        if (j.outExitCode) *j.outExitCode = 1;
        finish(j, 4, L"无法定位软件运行环境 (Runtime 目录缺失)。");
        return 1;
    }

    std::wstring catPath = findCatstartPath();
    if (catPath.empty()) {
        if (j.outExitCode) *j.outExitCode = 1;
        finish(j, 4, L"未检测到 CATIA 安装环境 (未找到 CATSTART.exe)。\n\n请确认目标机器已安装 64位 CATIA V5-6R（如 B26/B28 等版本），或设置系统环境变量 CATSTART_PATH 指向 CATSTART.exe。");
        return 1;
    }

    std::wstring nodePath = findNodePath(appRoot);
    std::wstring scriptPath = appRoot + L"\\Runtime\\Export\\build_filtered_bom.mjs";
    bool hasNodeEngine = !nodePath.empty() && regular(scriptPath);

    std::wstring stage, temp;
    wchar_t tb[MAX_PATH + 1];
    DWORD tn = GetTempPathW(MAX_PATH, tb);
    if (tn > 0 && tn <= MAX_PATH && full(tb, temp)) {
        if (!isDirectory(temp)) {
            CreateDirectoryW(temp.c_str(), NULL);
        }
    }
    if (!isDirectory(temp) || temp.find(L' ') != std::wstring::npos) {
        std::wstring fallback = L"C:\\Users\\Public\\CAA_BOM_V3_Temp";
        CreateDirectoryW(fallback.c_str(), NULL);
        if (isDirectory(fallback)) {
            temp = fallback;
        }
    }
    if (temp.empty() || !isDirectory(temp)) {
        if (j.outExitCode) *j.outExitCode = 1;
        finish(j, 4, L"无法确定安全的临时目录。");
        return 1;
    }

    if (!regular(j.input) || (j.mode == MODE_BOM && !isDirectory(j.outputDir)) ||
        (j.size != 600 && j.size != 800 && j.size != 1200)) {
        if (j.outExitCode) *j.outExitCode = 1;
        finish(j, 4, L"输入文件、输出目录或分辨率设置无效。");
        return 1;
    }

    if (j.mode == MODE_COLOR) {
        DWORD inAttr = GetFileAttributesW(j.input.c_str());
        if (inAttr != INVALID_FILE_ATTRIBUTES && (inAttr & FILE_ATTRIBUTE_READONLY)) {
            if (j.outExitCode) *j.outExitCode = 1;
            finish(j, 4, L"目标 CATPart 文件为只读状态，无法直接修改并保存颜色。请先取消只读属性后再试。");
            return 1;
        }
    }

    static LONG serial = 0;
    DWORD lastStageErr = 0;
    std::wstring lastAttemptStage;
    for (int i = 0; i < 50; i++) {
        wchar_t suffix[100];
        swprintf_s(suffix, L"CAA_BOM_V3_%lu_%lu_%ld",
                   GetCurrentProcessId(), GetTickCount(), InterlockedIncrement(&serial));
        stage = temp;
        if (stage[stage.size() - 1] != L'\\') stage += L'\\';
        stage += suffix;
        if (CreateDirectoryW(stage.c_str(), NULL)) {
            lastStageErr = 0;
            break;
        }
        lastStageErr = GetLastError();
        lastAttemptStage = stage;
        stage.clear();
    }

    if (stage.empty()) {
        wchar_t errBuf[256];
        swprintf_s(errBuf, L"无法创建专用临时工作目录 (错误码: %lu, 尝试路径: %s)。",
                   lastStageErr, lastAttemptStage.c_str());
        if (j.outExitCode) *j.outExitCode = 1;
        finish(j, 4, errBuf);
        return 1;
    }

    // Create subdirectories for isolated CATIA temp and error logs
    CreateDirectoryW((stage + L"\\cat_temp").c_str(), NULL);
    CreateDirectoryW((stage + L"\\cat_report").c_str(), NULL);

    // Dynamically generate isolated CATEnv in stage directory pointing to current appRoot
    std::wstring catEnvTemplatePath = appRoot + L"\\Runtime\\CATEnv\\CAA-BOM-Tiny-Release-B26.txt";
    std::string envTemplate = readFile(catEnvTemplatePath);
    if (envTemplate.empty()) {
        cleanStage(stage, temp);
        if (j.outExitCode) *j.outExitCode = 1;
        finish(j, 4, L"无法读取 CATIA 运行环境模板文件。");
        return 1;
    }

    std::string appRootNarrow = safeNarrowPath(appRoot);
    std::string resolvedEnv = replaceAll(envTemplate, "C:\\CATPart_BOM_Tool\\Runtime", appRootNarrow + "\\Runtime");
    resolvedEnv = replaceAll(resolvedEnv, "{{APP_RUNTIME}}", appRootNarrow + "\\Runtime");

    // Deduce CATIA win_b64 installation path from catPath
    std::wstring catWinB64 = catPath;
    size_t codeBinPos = catWinB64.rfind(L"\\code\\bin");
    if (codeBinPos != std::wstring::npos) {
        catWinB64 = catWinB64.substr(0, codeBinPos);
    }
    std::string catWinB64Narrow = safeNarrowPath(catWinB64);
    if (!catWinB64Narrow.empty()) {
        resolvedEnv = replaceAll(resolvedEnv, "C:\\Program Files\\Dassault Systemes\\B26\\win_b64", catWinB64Narrow);
    }

    // CATEnv isolation
    std::string stageNarrow = safeNarrowPath(stage);
    resolvedEnv = replaceAll(resolvedEnv, "CATTemp=CSIDL_LOCAL_APPDATA\\DassaultSystemes\\CATTemp", "CATTemp=" + stageNarrow + "\\cat_temp");
    resolvedEnv = replaceAll(resolvedEnv, "CATErrorLog=CSIDL_LOCAL_APPDATA\\DassaultSystemes\\CATTemp\\error.log", "CATErrorLog=" + stageNarrow + "\\cat_temp\\error.log");
    resolvedEnv = replaceAll(resolvedEnv, "CATReport=CSIDL_LOCAL_APPDATA\\DassaultSystemes\\CATReport", "CATReport=" + stageNarrow + "\\cat_report");

    std::wstring stagedEnvPath = stage + L"\\CAA-BOM-V3-Release-B26.txt";
    if (!writeTextFile(stagedEnvPath, resolvedEnv)) {
        cleanStage(stage, temp);
        if (j.outExitCode) *j.outExitCode = 1;
        finish(j, 4, L"无法生成运行时 CATEnv 环境配置。");
        return 1;
    }

    std::vector<wchar_t> env = environment(j.input, stage);
    wchar_t num[20];
    swprintf_s(num, L"%d", j.size);
    std::wstring inner = L"CAAStandalonePartWorker.exe @env @env " + std::wstring(num);
    if (j.mode == MODE_COLOR) {
        inner += L" 1010 color";
    } else if (j.density > 0.0 && _finite(j.density)) {
        wchar_t densBuf[64];
        swprintf_s(densBuf, L" %.15g", j.density);
        inner += densBuf;
    }
    std::wstring cmd = quote(catPath) + L" -run " + quote(inner) +
                       L" -env CAA-BOM-V3-Release-B26 -direnv " + quote(stage) + L" -nowindow";

    PROCESS_INFORMATION pi;
    std::wstring workerLog = stage + L"\\worker_launcher.log";
    HANDLE workerJob = NULL;
    if (!launch(cmd, workerLog, &env, pi, NULL, &workerJob)) {
        cleanStage(stage, temp);
        if (j.outExitCode) *j.outExitCode = 1;
        finish(j, 4, L"无法启动 CAA 计算进程；日志保留于：" + stage);
        return 1;
    }

    DWORD startTick = GetTickCount();
    DWORD lastActiveTick = startTick;
    size_t lastLogSize = 0;
    int prev = -1, total = -1;
    std::wstring logfile, csvfile;
    // Last observed worker heartbeat payload + parsed stage/detail (for the
    // diagnostic message shown when the watchdog fires).
    std::string lastHbContent, lastHbStage, lastHbDetail;

    for (;;) {
        // 1. Check user cancellation
        if (j.cancelEvent && WaitForSingleObject(j.cancelEvent, 0) == WAIT_OBJECT_0) {
            bool exitedNormally = false;
            requestSafeWorkerStop(stage, workerJob, pi, exitedNormally);
            if (exitedNormally) {
                std::wstring logName = findOne(stage, L"capture_*.log");
                std::string lc = logName.empty() ? "" : readFile(stage + L"\\" + logName);
                if (lc.find("color_save_ok=1") != std::string::npos) {
                    break; // Commit finished cleanly right as cancel arrived; report actual success
                }
            }
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            std::wstring name = findOne(stage, L"capture_*.log");
            if (!name.empty()) {
                CopyFileW((stage + L"\\" + name).c_str(), (temp + L"\\CAA_BOM_V3_Canceled_last.log").c_str(), FALSE);
            }
            CopyFileW(workerLog.c_str(), (temp + L"\\CAA_BOM_V3_Canceled_worker_launcher.log").c_str(), FALSE);
            cleanStage(stage, temp);
            if (j.outExitCode) *j.outExitCode = 1;
            finish(j, 5, L"任务已由用户取消（未提交回写，原文件未修改）。");
            return 1;
        }

        // 2. Check timeouts (global task budget + worker heartbeat liveness)
        DWORD now = GetTickCount();
        if ((now - taskStartTick > TASK_TOTAL_TIMEOUT_MS) ||
            (now - lastActiveTick > WORKER_HEARTBEAT_TIMEOUT_MS)) {
            bool isTotal = (now - taskStartTick > TASK_TOTAL_TIMEOUT_MS);
            unsigned long elapsedSec = (unsigned long)((now - taskStartTick) / 1000);
            bool exitedNormally = false;
            requestSafeWorkerStop(stage, workerJob, pi, exitedNormally);
            if (exitedNormally) {
                std::wstring logName = findOne(stage, L"capture_*.log");
                std::string lc = logName.empty() ? "" : readFile(stage + L"\\" + logName);
                if (lc.find("color_save_ok=1") != std::string::npos) {
                    break; // Commit finished cleanly; report actual success
                }
            }
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);

            wchar_t secBuf[32];
            swprintf_s(secBuf, L"%lu", elapsedSec);
            std::wstring detailLine = L"[timeout] reason=";
            detailLine += isTotal ? L"total" : L"heartbeat";
            detailLine += L" elapsed_s=";
            detailLine += secBuf;
            detailLine += L" lastStage=";
            detailLine += utf8w(lastHbStage);
            detailLine += L" lastDetail=";
            detailLine += utf8w(lastHbDetail);
            appendLog(workerLog, detailLine);

            if (j.outExitCode) *j.outExitCode = 1;
            std::wstring msg = L"CAA 测量分析超时中止（原因：";
            msg += isTotal ? L"任务总耗时超过 30 分钟上限" : L"测量进程心跳超时，疑似卡死";
            msg += L"；已耗时 ";
            msg += secBuf;
            msg += L" 秒；最后阶段：";
            msg += utf8w(lastHbStage);
            msg += L"；细节：";
            msg += utf8w(lastHbDetail);
            msg += L"）；中间日志保留于：";
            msg += stage;
            finish(j, 4, msg);
            return 1;
        }

        // 3. Watch the worker heartbeat file (heartbeat_<wid>.txt).  Any change
        //    of its content proves the worker thread is still alive even while
        //    a single geometry operation takes minutes.
        std::wstring hbName = findOne(stage, L"heartbeat_*.txt");
        if (!hbName.empty()) {
            std::string hbContent = readFile(stage + L"\\" + hbName);
            if (!hbContent.empty() && hbContent != lastHbContent) {
                lastHbContent = hbContent;
                lastActiveTick = GetTickCount();

                size_t sp = hbContent.find("stage=");
                if (sp != std::string::npos) {
                    size_t se = hbContent.find('\n', sp);
                    std::string v = hbContent.substr(sp + 6,
                        (se == std::string::npos ? hbContent.size() : se) - (sp + 6));
                    while (!v.empty() && (v[v.size() - 1] == '\r' || v[v.size() - 1] == ' ' ||
                                          v[v.size() - 1] == '\t')) {
                        v.erase(v.size() - 1);
                    }
                    lastHbStage = v;
                }
                size_t dp = hbContent.find("detail=");
                if (dp != std::string::npos) {
                    size_t de = hbContent.find('\n', dp);
                    std::string v = hbContent.substr(dp + 7,
                        (de == std::string::npos ? hbContent.size() : de) - (dp + 7));
                    while (!v.empty() && (v[v.size() - 1] == '\r' || v[v.size() - 1] == ' ' ||
                                          v[v.size() - 1] == '\t')) {
                        v.erase(v.size() - 1);
                    }
                    lastHbDetail = v;
                }
            }
        }

        DWORD w = WaitForSingleObject(pi.hProcess, 250);
        std::wstring name = findOne(stage, L"capture_*.log");
        if (!name.empty()) {
            logfile = stage + L"\\" + name;
            std::string content = readFile(logfile);
            if (content.size() != lastLogSize) {
                lastLogSize = content.size();
                lastActiveTick = GetTickCount();
            }
            total = totalCount(content);
            std::wstring id = name.substr(8, name.size() - 12);
            csvfile = stage + L"\\Entity_Analysis_" + id + L".csv";
            if (total > 0) {
                int pct = std::min(100, phaseProgress(content));
                if (pct != prev) {
                    send(j.notify, 1, (LPARAM)pct);
                    prev = pct;
                }
            }
        } else {
            std::string wlog = readFile(workerLog);
            if (wlog.size() != lastLogSize) {
                lastLogSize = wlog.size();
                lastActiveTick = GetTickCount();
            }
        }

        if (w == WAIT_OBJECT_0 || w == WAIT_FAILED) break;
    }

    if (workerJob) {
        CloseHandle(workerJob);
        workerJob = NULL;
    }

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // Strict Hidden Entity (NoShow) Handling (Exit Code 3 or log marker)
    std::string logContent = logfile.empty() ? "" : readFile(logfile);
    if (code == 3 || logContent.find("hidden_body_count=") != std::string::npos) {
        std::wstring countStr = L"1";
        std::wstring namesStr = L"";
        size_t p = logContent.find("hidden_body_count=");
        if (p != std::string::npos) {
            size_t endLine = logContent.find('\n', p);
            std::string line = (endLine != std::string::npos) ? logContent.substr(p, endLine - p) : logContent.substr(p);
            size_t namePos = line.find("names=");
            if (namePos != std::string::npos) {
                std::string cPart = line.substr(18, namePos - 18);
                while (!cPart.empty() && (cPart[cPart.size() - 1] == ' ' || cPart[cPart.size() - 1] == '\t')) {
                    cPart.erase(cPart.size() - 1);
                }
                std::string nPart = line.substr(namePos + 6);
                while (!nPart.empty() && (nPart[nPart.size() - 1] == '\r' || nPart[nPart.size() - 1] == ' ')) {
                    nPart.erase(nPart.size() - 1);
                }
                if (!cPart.empty()) countStr = utf8w(cPart);
                if (!nPart.empty()) namesStr = utf8w(nPart);
            }
        }
        if (namesStr.empty()) {
            namesStr = L"隐藏实体";
        }

        cleanStage(stage, temp);
        std::wstring msg = (j.mode == MODE_COLOR
                            ? L"改色已中止：检测到模型中存在 "
                            : L"导出已中止：检测到模型中存在 ") + countStr +
                           L" 个隐藏状态（NoShow）的实体（" + namesStr +
                           L"）。\n\n本程序要求模型内全部实体均处于显示状态才能继续操作。请先在 CATIA 中将所有隐藏实体设为显示状态（或清理无关隐藏实体）后再试。";
        if (j.outExitCode) *j.outExitCode = 3;
        finish(j, 4, msg);
        return 3;
    }

    bool savedOk = (logContent.find("color_save_ok=1") != std::string::npos);
    if (!savedOk || logfile.empty()) {
        if (j.outExitCode) *j.outExitCode = 1;
        std::wstring errMsg = L"CAA 实体赋色或保存失败（原文件未提交修改）；日志保留于：" + stage;
        if (logContent.find("recovery_copy=") != std::string::npos) {
            errMsg = L"保存后回写原文件失败（已保留恢复副本，原文件未覆盖）；详情见日志：" + stage;
        }
        finish(j, 4, errMsg);
        return 1;
    }
    std::wstring summary = L"已完成";
    size_t cbPos = logContent.find("colored_bodies=");
    if (cbPos != std::string::npos) {
        int cb = atoi(logContent.c_str() + cbPos + 15);
        if (cb > 0) {
            wchar_t buf[64];
            swprintf_s(buf, L"已完成 (%d 个实体)", cb);
            summary = buf;
        }
    }
    if (code != 0 || !finalMarker(logContent)) {
        summary += L" (清理异常)";
    } else {
        cleanStage(stage, temp);
    }
    if (j.outExitCode) *j.outExitCode = 0;
    finish(j, 3, summary);
    return 0;
}