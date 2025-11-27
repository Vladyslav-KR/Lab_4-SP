#define NOMINMAX

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <iostream>
#include <string>
#include <iomanip>
#include <limits>
#include <vector>


#pragma comment(lib, "Advapi32.lib")

// ========================= Допоміжні функції =========================

void PrintFileTime(const char* label, const FILETIME& ft) {
    FILETIME localFt;
    SYSTEMTIME st;

    if (!FileTimeToLocalFileTime(&ft, &localFt)) {
        std::cout << label << ": (error converting time)\n";
        return;
    }
    if (!FileTimeToSystemTime(&localFt, &st)) {
        std::cout << label << ": (error converting time)\n";
        return;
    }

    std::cout << label << ": "
        << std::setfill('0')
        << std::setw(2) << st.wDay << "."
        << std::setw(2) << st.wMonth << "."
        << st.wYear << " "
        << std::setw(2) << st.wHour << ":"
        << std::setw(2) << st.wMinute << ":"
        << std::setw(2) << st.wSecond
        << "\n";
}

double GetTimeSeconds() {
    static LARGE_INTEGER freq = {};
    static bool init = false;
    if (!init) {
        QueryPerformanceFrequency(&freq);
        init = true;
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / freq.QuadPart;
}

bool CanOpenForAccess(const std::string& path, DWORD desiredAccess) {
    HANDLE h = CreateFileA(
        path.c_str(),
        desiredAccess,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

// ========================= Частина 1: атрибути файлу =========================

void ShowFileInfo(const std::string& path) {
    std::cout << "\n=== File info for: " << path << " ===\n";

    DWORD attrs = GetFileAttributesA(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        std::cout << "Error: cannot get file attributes. GetLastError = "
            << GetLastError() << "\n";
        return;
    }

    std::cout << "Attributes:\n";
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) std::cout << "  - DIRECTORY\n";
    if (attrs & FILE_ATTRIBUTE_READONLY)  std::cout << "  - READONLY\n";
    if (attrs & FILE_ATTRIBUTE_HIDDEN)    std::cout << "  - HIDDEN\n";
    if (attrs & FILE_ATTRIBUTE_SYSTEM)    std::cout << "  - SYSTEM\n";
    if (attrs & FILE_ATTRIBUTE_ARCHIVE)   std::cout << "  - ARCHIVE\n";
    if (attrs & FILE_ATTRIBUTE_COMPRESSED)std::cout << "  - COMPRESSED\n";
    if (attrs & FILE_ATTRIBUTE_ENCRYPTED) std::cout << "  - ENCRYPTED\n";
    std::cout << "\n";

    HANDLE hFile = CreateFileA(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        std::cout << "Error: cannot open file. GetLastError = "
            << GetLastError() << "\n";
        return;
    }

    LARGE_INTEGER size;
    if (GetFileSizeEx(hFile, &size))
        std::cout << "File size: " << size.QuadPart << " bytes\n";
    else
        std::cout << "File size: error\n";

    FILETIME ftCreate{}, ftAccess{}, ftWrite{};
    if (GetFileTime(hFile, &ftCreate, &ftAccess, &ftWrite)) {
        PrintFileTime("Creation time      ", ftCreate);
        PrintFileTime("Last access time   ", ftAccess);
        PrintFileTime("Last write time    ", ftWrite);
    }

    // Owner (SID)
    PSECURITY_DESCRIPTOR pSD = nullptr;
    PSID pOwnerSid = nullptr;

    DWORD secRes = GetSecurityInfo(
        hFile,
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION,
        &pOwnerSid,
        nullptr,
        nullptr,
        nullptr,
        &pSD
    );

    if (secRes == ERROR_SUCCESS && pOwnerSid) {
        char name[256];
        char domain[256];
        DWORD nameSize = sizeof(name);
        DWORD domainSize = sizeof(domain);
        SID_NAME_USE sidType;

        if (LookupAccountSidA(nullptr, pOwnerSid, name, &nameSize,
            domain, &domainSize, &sidType)) {
            std::cout << "Owner: " << domain << "\\" << name << "\n";
        }
        else {
            std::cout << "Owner: (cannot resolve SID)\n";
        }
    }

    if (pSD) LocalFree(pSD);

    std::cout << "\nAccess:\n";
    std::cout << "  Can READ:  " << (CanOpenForAccess(path, GENERIC_READ) ? "YES" : "NO") << "\n";
    std::cout << "  Can WRITE: " << (CanOpenForAccess(path, GENERIC_WRITE) ? "YES" : "NO") << "\n";

    CloseHandle(hFile);
}

// ========================= Частина 2: копіювання (C stdio) =========================

bool CopyBufferedC(const std::string& src, const std::string& dst, double& seconds) {
    const size_t BUF_SIZE = 1024 * 1024; // 1MB

    FILE* in = fopen(src.c_str(), "rb");
    if (!in) {
        std::cout << "CopyBufferedC: cannot open source file\n";
        return false;
    }
    FILE* out = fopen(dst.c_str(), "wb");
    if (!out) {
        std::cout << "CopyBufferedC: cannot open dest file\n";
        fclose(in);
        return false;
    }

    char* buffer = new char[BUF_SIZE];

    double t1 = GetTimeSeconds();

    size_t bytesRead;
    while ((bytesRead = fread(buffer, 1, BUF_SIZE, in)) > 0) {
        size_t written = fwrite(buffer, 1, bytesRead, out);
        if (written != bytesRead) {
            std::cout << "CopyBufferedC: fwrite failed\n";
            delete[] buffer;
            fclose(in);
            fclose(out);
            return false;
        }
    }

    double t2 = GetTimeSeconds();
    seconds = t2 - t1;

    delete[] buffer;
    fclose(in);
    fclose(out);
    return true;
}

// ========================= Частина 2: копіювання (WinAPI) =========================

bool CopyWinAPI(const std::string& src, const std::string& dst, double& seconds) {
    const DWORD BUF_SIZE = 1024 * 1024; // 1MB

    HANDLE hIn = CreateFileA(
        src.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

    if (hIn == INVALID_HANDLE_VALUE) {
        std::cout << "CopyWinAPI: cannot open source file. GetLastError="
            << GetLastError() << "\n";
        return false;
    }

    HANDLE hOut = CreateFileA(
        dst.c_str(), GENERIC_WRITE,
        0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);

    if (hOut == INVALID_HANDLE_VALUE) {
        std::cout << "CopyWinAPI: cannot open dest file. GetLastError="
            << GetLastError() << "\n";
        CloseHandle(hIn);
        return false;
    }

    char* buffer = new char[BUF_SIZE];
    double t1 = GetTimeSeconds();

    DWORD bytesRead;
    while (ReadFile(hIn, buffer, BUF_SIZE, &bytesRead, nullptr) && bytesRead > 0) {
        DWORD written;
        if (!WriteFile(hOut, buffer, bytesRead, &written, nullptr) ||
            written != bytesRead) {
            std::cout << "CopyWinAPI: WriteFile failed. GetLastError="
                << GetLastError() << "\n";
            delete[] buffer;
            CloseHandle(hIn);
            CloseHandle(hOut);
            return false;
        }
    }

    double t2 = GetTimeSeconds();
    seconds = t2 - t1;

    delete[] buffer;
    CloseHandle(hIn);
    CloseHandle(hOut);
    return true;
}

void RunCopyComparison() {
    std::string src;
    std::cout << "\nEnter path to a large file (>100MB):\n> ";
    std::getline(std::cin, src);
    if (src.empty()) {
        std::cout << "No file.\n";
        return;
    }

    std::string dstC = src + ".Ccopy.bin";
    std::string dstAPI = src + ".APIcopy.bin";

    std::cout << "\n--- C stdio copy ---\n";
    double tC = 0;
    if (CopyBufferedC(src, dstC, tC))
        std::cout << "Time: " << tC << " sec\n";
    else
        std::cout << "Failed.\n";

    std::cout << "\n--- WinAPI copy ---\n";
    double tA = 0;
    if (CopyWinAPI(src, dstAPI, tA))
        std::cout << "Time: " << tA << " sec\n";
    else
        std::cout << "Failed.\n";

    std::cout << "\n=== RESULT ===\n";
    std::cout << "C stdio: " << tC << " sec\n";
    std::cout << "WinAPI : " << tA << " sec\n";
}

// ========================= Частина 3: асинхронне I/O =========================

struct AsyncCopyContext {
    std::string src;
    std::string dst;

    HANDLE hIn = INVALID_HANDLE_VALUE;
    HANDLE hOut = INVALID_HANDLE_VALUE;

    OVERLAPPED ov{};
    HANDLE eventHandle = nullptr;

    char* buffer = nullptr;
    bool finished = false;
    LARGE_INTEGER offset{};
};

void CloseAsyncContext(AsyncCopyContext& ctx) {
    if (ctx.hIn != INVALID_HANDLE_VALUE) CloseHandle(ctx.hIn);
    if (ctx.hOut != INVALID_HANDLE_VALUE) CloseHandle(ctx.hOut);
    if (ctx.eventHandle) CloseHandle(ctx.eventHandle);
    delete[] ctx.buffer;

    ctx.hIn = ctx.hOut = INVALID_HANDLE_VALUE;
    ctx.eventHandle = nullptr;
    ctx.buffer = nullptr;
    ctx.finished = true;
}

// Запуск першого асинхронного читання
bool StartAsyncRead(AsyncCopyContext& ctx, DWORD bufferSize) {
    ZeroMemory(&ctx.ov, sizeof(ctx.ov));
    ctx.ov.Offset = ctx.offset.LowPart;
    ctx.ov.OffsetHigh = ctx.offset.HighPart;
    ctx.ov.hEvent = ctx.eventHandle;

    DWORD bytesRead = 0;
    BOOL ok = ReadFile(ctx.hIn, ctx.buffer, bufferSize, &bytesRead, &ctx.ov);
    if (!ok) {
        DWORD err = GetLastError();
        if (err == ERROR_IO_PENDING) {
            // все нормально, операція триває
            return true;
        }
        else {
            std::cout << "ReadFile async error for file '" << ctx.src
                << "'. GetLastError=" << err << "\n";
            return false;
        }
    }
    else {
        // операція завершилась миттєво (рідко, але можливо)
        if (bytesRead == 0) {
            return false; // EOF
        }
        // одразу записати і запустити наступне читання
        DWORD written = 0;
        if (!WriteFile(ctx.hOut, ctx.buffer, bytesRead, &written, nullptr)
            || written != bytesRead) {
            std::cout << "WriteFile sync error for file '" << ctx.src << "'\n";
            return false;
        }
        ctx.offset.QuadPart += bytesRead;
        return StartAsyncRead(ctx, bufferSize);
    }
}

// Головна функція для пункту 3
void RunAsyncMultiCopy() {
    const DWORD BUF_SIZE = 1024 * 1024; // 1MB
    const DWORD MAX_FILES = 4;

    int n;
    std::cout << "\nHow many files to copy asynchronously (1-" << MAX_FILES << ")? ";
    std::cin >> n;
    if (!std::cin || n <= 0) {
        std::cout << "Invalid number.\n";
        // очистити \n
        std::cin.clear();
        std::cin.ignore((std::numeric_limits<std::streamsize>::max)(), '\n');
        return;
    }
    if (n > (int)MAX_FILES) n = MAX_FILES;

    std::cin.ignore((std::numeric_limits<std::streamsize>::max)(), '\n');

    std::vector<AsyncCopyContext> contexts;
    contexts.reserve(n);

    for (int i = 0; i < n; ++i) {
        AsyncCopyContext ctx;

        std::cout << "Enter source path for file " << (i + 1) << ":\n> ";
        std::getline(std::cin, ctx.src);
        if (ctx.src.empty()) {
            std::cout << "Skipped (empty path).\n";
            continue;
        }
        ctx.dst = ctx.src + ".ASYNCcopy.bin";

        ctx.hIn = CreateFileA(
            ctx.src.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
            nullptr
        );
        if (ctx.hIn == INVALID_HANDLE_VALUE) {
            std::cout << "Cannot open source file '" << ctx.src
                << "'. GetLastError=" << GetLastError() << "\n";
            continue;
        }

        ctx.hOut = CreateFileA(
            ctx.dst.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (ctx.hOut == INVALID_HANDLE_VALUE) {
            std::cout << "Cannot open dest file '" << ctx.dst
                << "'. GetLastError=" << GetLastError() << "\n";
            CloseHandle(ctx.hIn);
            continue;
        }

        ctx.eventHandle = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if (!ctx.eventHandle) {
            std::cout << "Cannot create event.\n";
            CloseAsyncContext(ctx);
            continue;
        }

        ctx.buffer = new char[BUF_SIZE];
        ctx.finished = false;
        ctx.offset.QuadPart = 0;

        // Запускаємо перше асинхронне читання
        if (!StartAsyncRead(ctx, BUF_SIZE)) {
            std::cout << "First async read failed for file '" << ctx.src << "'.\n";
            CloseAsyncContext(ctx);
            continue;
        }

        contexts.push_back(ctx);
    }

    if (contexts.empty()) {
        std::cout << "No valid files to process.\n";
        return;
    }

    std::vector<HANDLE> events;
    events.reserve(contexts.size());
    for (auto& ctx : contexts) {
        events.push_back(ctx.eventHandle);
    }

    std::cout << "\nStarting asynchronous copy of " << contexts.size()
        << " file(s)...\n";

    double tStart = GetTimeSeconds();

    size_t active = contexts.size();
    while (active > 0) {
        DWORD waitRes = WaitForMultipleObjects(
            static_cast<DWORD>(events.size()),
            events.data(),
            FALSE,           // any one
            INFINITE
        );

        if (waitRes < WAIT_OBJECT_0 ||
            waitRes >= WAIT_OBJECT_0 + events.size()) {
            std::cout << "WaitForMultipleObjects error. GetLastError="
                << GetLastError() << "\n";
            break;
        }

        DWORD index = waitRes - WAIT_OBJECT_0;
        AsyncCopyContext& ctx = contexts[index];

        if (ctx.finished) {
            // вже завершений, просто скидаємо подію
            ResetEvent(ctx.eventHandle);
            continue;
        }

        DWORD bytesRead = 0;
        BOOL ok = GetOverlappedResult(
            ctx.hIn,
            &ctx.ov,
            &bytesRead,
            FALSE
        );

        if (!ok) {
            DWORD err = GetLastError();
            std::cout << "GetOverlappedResult error for file '" << ctx.src
                << "'. GetLastError=" << err << "\n";
            CloseAsyncContext(ctx);
            ctx.finished = true;
            active--;
            continue;
        }

        if (bytesRead == 0) {
            // EOF
            std::cout << "Finished async copy for '" << ctx.src << "'\n";
            CloseAsyncContext(ctx);
            ctx.finished = true;
            active--;
            continue;
        }

        // Пишемо прочитаний блок (синхронно, для простоти)
        DWORD written = 0;
        if (!WriteFile(ctx.hOut, ctx.buffer, bytesRead, &written, nullptr) ||
            written != bytesRead) {
            std::cout << "WriteFile error for '" << ctx.src << "'. GetLastError="
                << GetLastError() << "\n";
            CloseAsyncContext(ctx);
            ctx.finished = true;
            active--;
            continue;
        }

        ctx.offset.QuadPart += bytesRead;
        ResetEvent(ctx.eventHandle);

        // Запускаємо наступне асинхронне читання
        if (!StartAsyncRead(ctx, BUF_SIZE)) {
            // ймовірно EOF
            CloseAsyncContext(ctx);
            ctx.finished = true;
            active--;
        }
    }

    double tEnd = GetTimeSeconds();
    std::cout << "\nTotal async copy time: " << (tEnd - tStart) << " sec\n";
    std::cout << "Async copy completed.\n";
}

// ========================= MAIN =========================

int main() {
    std::cout << "Lab 4 — File System & IO\n";

    while (true) {
        std::cout << "\nMenu:\n";
        std::cout << "1 - Show file attributes (part 1)\n";
        std::cout << "2 - Compare C stdio vs WinAPI copy (part 2)\n";
        std::cout << "3 - Asynchronous copy of multiple files (part 3)\n";
        std::cout << "0 - Exit\n";
        std::cout << "Your choice: ";

        int choice;
        if (!(std::cin >> choice)) return 0;

        std::cin.ignore((std::numeric_limits<std::streamsize>::max)(), '\n');

        if (choice == 0) break;

        else if (choice == 1) {
            std::string path;
            std::cout << "\nEnter file path:\n> ";
            std::getline(std::cin, path);
            if (!path.empty())
                ShowFileInfo(path);
        }
        else if (choice == 2) {
            RunCopyComparison();
        }
        else if (choice == 3) {
            RunAsyncMultiCopy();
        }
        else {
            std::cout << "Invalid choice.\n";
        }
    }

    return 0;
}
