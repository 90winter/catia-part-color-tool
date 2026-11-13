#pragma once
#include <windows.h>
#include <string>

enum JobMode {
    MODE_BOM = 0,
    MODE_COLOR = 1
};

struct BomJob {
    std::wstring input;
    std::wstring outputDir;
    int size;
    HWND notify;
    HANDLE cancelEvent;
    int* outExitCode;
    std::wstring* outMessage;
    double density;
    JobMode mode;

    BomJob() : size(800), notify(NULL), cancelEvent(NULL), outExitCode(NULL), outMessage(NULL), density(0.0), mode(MODE_COLOR) {}
};

// Takes ownership of a heap-allocated BomJob.
// WM_APP+1 notification stages:
//   0 = Start
//   1 = Progress (lParam = percentage)
//   2 = Excel generation phase
//   3 = Success (lParam = heap std::wstring* owned by receiver)
//   4 = Failure (lParam = heap std::wstring* owned by receiver)
//   5 = Canceled (lParam = heap std::wstring* owned by receiver)
unsigned __stdcall RunBomJob(void* raw);
