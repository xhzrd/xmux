#include "xmux.hpp"

#include <iostream>
#include <string>
#include <thread>
#include <tlhelp32.h>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <functional>
#include <windows.h>

/*
 * xmux hooking/embedding helper
 *
 * Big picture:
 *  - Launches a child process and parents its main window
 *    into a "console" parent window so the video appears inside your terminal UI.
 *  - Patches window styles to remove chrome and make it behave like a child.
 *  - Hooks child windows WndProcs to block dragging/capture-based moves.
 *  - Monitors parent process lifetimes and keeps the child window geometry in sync.
 *
 * Important notes:
 *  - WinAPI is stateful and full of edge-cases. This code fights the target
 *    window by repeatedly forcing styles for a while (race with target window).
 *  - Many functions expect valid HWNDs; always check for nullptr before using.
 *  - Thread-safety: global maps and gFoundHWND are mutated from different threads/callbacks.
 *    Access patterns here rely on single-threaded usage for some globals; be careful if you
 *    later make things multi-threaded.
 *
 * TODOS / improvements:
 *  - Consider locking for gOriginalProcs if concurrent modifications are possible.
 *  - Use Unicode (W) APIs consistently if you plan to support non-ASCII window titles.
 */

/* ----------------------------------------------------------------------------
 * Custom WndProc
 *
 * Rationale:
 *  - Some apps (mpv for example) might try to re-enable dragging or react to capture changes.
 *  - Replacing WndProc lets us intercept WM_NCHITTEST and system commands like SC_MOVE.
 *
 * WARNING:
 *  - Replacing window procs is fragile: the target window or another hook may replace it too.
 *  - Always store the original WndProc and call CallWindowProc to forward unhandled messages.
 *  - The hooked proc must use the same calling convention and must be careful about recursion.
 * ----------------------------------------------------------------------------
 */
LRESULT CALLBACK xmux::LockedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST:
            // Tell Windows the mouse is in client area only → disables the non-client drag behavior.
            // This effectively blocks the caption/title bar dragging on many apps.
            return HTCLIENT;

        case WM_SYSCOMMAND:
            // Some apps call SendMessage(WM_SYSCOMMAND, SC_MOVE, ...) to move themselves.
            // Mask wParam & 0xFFF0 per MSDN guidance and block SC_MOVE to prevent repositioning.
            if ((wParam & 0xFFF0) == SC_MOVE) {
                std::cout << "[BLOCK] Attempted move via SC_MOVE on hwnd: " << hwnd << std::endl;
                return 0; // swallow the message
            }
            break;

        case WM_CAPTURECHANGED:
            // Useful for debugging and handling cases where the target grabs/releases mouse capture.
            std::cout << "[CAPTURE CHANGED] hwnd: " << hwnd << std::endl;
            break;

        default:
            break;
    }

    // If we stored an original WndProc for this HWND, forward the message to it.
    // This preserves the app's normal behavior for messages we don't explicitly handle.
    auto it = gOriginalProcs.find(hwnd);
    if (it != gOriginalProcs.end()) {
        return CallWindowProcA(it->second, hwnd, msg, wParam, lParam);
    }

    // Fallback: default processing.
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ----------------------------------------------------------------------------
 * hookAllChildren
 *
 * Recursively replaces WndProc for 'hwnd' and all descendant windows.
 * Useful when the app creates a bunch of child windows — we want to hook them all.
 *
 * NOTES:
 *  - SetWindowLongPtr returns the previous WndProc. We cast and store it so we can forward.
 *  - Class names and debug logging are helpful when diagnosing why a window still moves.
 * ----------------------------------------------------------------------------
 */
void xmux::hookAllChildren(HWND hwnd) {
    // Replace the window procedure for this HWND and store the original in the map.
    WNDPROC original = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)LockedWndProc);
    gOriginalProcs[hwnd] = original;

    // Debug: print class name for easier tracing.
    char class_name[256];
    GetClassNameA(hwnd, class_name, sizeof(class_name));
    std::cout << "[hook] Hooking: " << hwnd << " Class: " << class_name << std::endl;

    // Recurse for all child windows of this HWND.
    HWND child = nullptr;
    while ((child = FindWindowExA(hwnd, child, nullptr, nullptr)) != nullptr) {
        hookAllChildren(child);
    }
}

/* ----------------------------------------------------------------------------
 * getParentProcessId
 *
 * Returns the parent process ID (PPID) for the current process.
 * We snapshot processes and walk entries to find current PID's parent.
 *
 * Edge cases & gotchas:
 *  - On some platforms or sandboxed environments, parent process might not be available.
 *  - If CreateToolhelp32Snapshot fails or iteration fails, we return 0.
 * ----------------------------------------------------------------------------
 */
DWORD xmux::getParentProcessId() {
    DWORD ppid = 0;
    DWORD pid = GetCurrentProcessId();

    // Snapshot all processes
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);
    if (Process32First(hSnapshot, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                ppid = pe.th32ParentProcessID;
                break;
            }
        } while (Process32Next(hSnapshot, &pe));
    }

    CloseHandle(hSnapshot);
    return ppid;
}

/* ----------------------------------------------------------------------------
 * findWindowByPID
 *
 * Bruteforce scan for top-level windows and match by process id (PID).
 * Returns the first visible window owned by the PID.
 *
 * NOTE:
 *  - This is a very naive scan (FindWindowEx looping through all top-level windows).
 *  - It returns the first visible one and logs its class. If the process creates
 *    windows on different threads or later, this might miss them.
 * ----------------------------------------------------------------------------
 */
HWND xmux::findWindowByPID(DWORD pid) {
    HWND hwnd = nullptr;
    char class_name[256];

    do {
        hwnd = FindWindowExA(nullptr, hwnd, nullptr, nullptr); // enumerate top-level windows
        if (!hwnd) break;

        DWORD wnd_pid = 0;
        GetWindowThreadProcessId(hwnd, &wnd_pid);
        if (wnd_pid == pid && IsWindowVisible(hwnd)) {
            GetClassNameA(hwnd, class_name, sizeof(class_name));
            std::string cls = class_name;
            std::cout << "[xmux::info] Found a window with the className: " << class_name << "\n";
            return hwnd;
        }
    } while (hwnd != nullptr);
    return nullptr;
}

/* ----------------------------------------------------------------------------
 * Helper-based enumerator to find a window via EnumWindows callback.
 *
 * Strategy:
 *  - EnumWindows is sometimes more reliable than findWindowByPID loop above.
 *  - We stash a found HWND in a global and stop enumeration by returning FALSE.
 * ----------------------------------------------------------------------------
 */
BOOL CALLBACK xmux::EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    DWORD wnd_pid = 0;
    GetWindowThreadProcessId(hwnd, &wnd_pid);

    DWORD target_pid = (DWORD)lParam;

    if (wnd_pid == target_pid && IsWindowVisible(hwnd)) {
        // Optional: skip certain classes or titles if necessary
        gFoundHWND = hwnd;
        return FALSE; // stop enumeration early
    }

    return TRUE; // continue enumeration
}

HWND xmux::findWindowByPIDRecursive(DWORD pid) {
    gFoundHWND = nullptr;
    EnumWindows(EnumWindowsProc, (LPARAM)pid);
    return gFoundHWND;
}

/* ----------------------------------------------------------------------------
 * getThreadsInProcess
 *
 * Returns all thread IDs belonging to a process. Useful when window belongs to
 * a thread other than the main one and you want to EnumThreadWindows().
 * ----------------------------------------------------------------------------
 */
std::vector<DWORD> xmux::getThreadsInProcess(DWORD pid) {
    std::vector<DWORD> threadIds;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return threadIds;

    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);

    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == pid) {
                threadIds.push_back(entry.th32ThreadID);
            }
        } while (Thread32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return threadIds;
}

/* ----------------------------------------------------------------------------
 * EnumThreadWindowsProc & findWindowByPIDFullScan
 *
 * Walks windows created by each thread in the process. Sometimes windows
 * are created on different threads than the main thread, so this helps find them.
 * ----------------------------------------------------------------------------
 */
BOOL CALLBACK xmux::EnumThreadWindowsProc(HWND hwnd, LPARAM lParam) {
    (void)(lParam);
    if (IsWindowVisible(hwnd)) {
        gFoundHWND = hwnd;
        return FALSE; // found an HWND for this thread, stop enumeration
    }
    return TRUE;
}

HWND xmux::findWindowByPIDFullScan(DWORD pid) {
    gFoundHWND = nullptr;
    std::vector<DWORD> threads = getThreadsInProcess(pid);

    for (DWORD tid : threads) {
        EnumThreadWindows(tid, EnumThreadWindowsProc, 0);
        if (gFoundHWND) break;
    }

    return gFoundHWND;
}

/* ----------------------------------------------------------------------------
 * getAllChildPIDs
 *
 * Builds a recursive list (vector) of the parent PID + all descendant PIDs
 * by scanning the process snapshot and walking the parent-child hierarchy.
 *
 * Use-case:
 *  - Many programs spawn helper processes (ffmpeg, helper daemons). We want to
 *    search windows owned by any descendant child to find the real window.
 * ----------------------------------------------------------------------------
 */
std::vector<DWORD> xmux::getAllChildPIDs(DWORD parent_pid) {
    std::vector<DWORD> pids;
    pids.push_back(parent_pid); // include the original PID

    // Build a parent->children map so we can do recursive expansion.
    std::unordered_map<DWORD, std::vector<DWORD>> children_map;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    if (snapshot == INVALID_HANDLE_VALUE) return pids;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);

    if (Process32First(snapshot, &pe)) {
        do {
            children_map[pe.th32ParentProcessID].push_back(pe.th32ProcessID);
        } while (Process32Next(snapshot, &pe));
    }

    CloseHandle(snapshot);

    // Recursive lambda to push all descendants into pids.
    std::function<void(DWORD)> collect = [&](DWORD pid) {
        for (DWORD child : children_map[pid]) {
            pids.push_back(child);
            collect(child);
        }
    };

    collect(parent_pid);
    return pids;
}

/* ----------------------------------------------------------------------------
 * findWindowByAnyPID
 *
 * Given a list of PIDs, enumerate all top-level windows and return the first visible
 * window whose owning PID is in the list. This helps when child or helper processes
 * create the visible window (common in complex apps).
 *
 * Note:
 *  - This lambda-based EnumWindows call uses a small struct to capture state and store found HWND.
 *  - We print the window title in wide form for better readability when titles use Unicode.
 * ----------------------------------------------------------------------------
 */
HWND xmux::findWindowByAnyPID(const std::vector<DWORD>& pids) {
    struct EnumData {
        const std::vector<DWORD>* pids;
        HWND found = nullptr;
    };

    EnumData data { &pids, nullptr };

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* info = reinterpret_cast<EnumData*>(lParam);
        DWORD pid;
        GetWindowThreadProcessId(hwnd, &pid);

        // If this window belongs to any of the PIDs we care about and it's visible, pick it.
        if (std::find(info->pids->begin(), info->pids->end(), pid) != info->pids->end()) {
            if (IsWindowVisible(hwnd)) {
                wchar_t title[256];
                GetWindowTextW(hwnd, title, 256);
                std::wcout << L"[debug] Found HWND: " << hwnd << L" Title: " << title << L"\n";
                info->found = hwnd;
                return FALSE; // Found it — stop enumeration
            }
        }

        return TRUE; // Keep looking
    }, reinterpret_cast<LPARAM>(&data));

    return data.found;
}

/* ----------------------------------------------------------------------------
 * xmux class methods (core)
 *
 * These are member functions; the actual class likely resides in xmux.hpp.
 * Below are the definitions from your original file with clarifying comments.
 * ----------------------------------------------------------------------------
 */

// Constructor: store the target parent PID and attempt to find the parent HWND now.
// - mPID: parent process id (console parent in your use-case)
// - mCommand: command string to launch as child
xmux::xmux(int parentPID, const std::string command) {
    mPID = parentPID;
    mCommand = command;
    // Try to find the parent window for the given PID immediately during construction.
    // This may return nullptr; launch() checks and handles that.
    mParentHWND = findWindowByPID(parentPID);
}

// Destructor: ensure we stop threads/processes and clean up handles.
xmux::~xmux() {
    stop();
}

// launch: main setup flow to start child process, find its window, hook it, and start monitor threads.
bool xmux::launch(bool showNormal) {
    if (!mParentHWND) {
        std::cerr << "[xmux::error] Parent HWND not found. PID: " << mPID << "\n";
        return false;
    }

    std::cout << "[xmux::info] Launching command: " << mCommand << std::endl;

    // Spawn the child process (CreateProcessA)
    if (!launchProcess(showNormal)) {
        std::cerr << "[xmux::error] Failed to launch process.\n";
        return false;
    }

    // Wait for the child process to create a visible window.
    // We poll up to ~30s (300 * 100ms) looking through child PIDs for a visible HWND.
    std::cout << "[xmux::info] Waiting for child window...\n";
    for (int i = 0; i < 300; ++i) {
        auto child_pids = getAllChildPIDs(mProcessInformation.dwProcessId);
        child_pids.push_back(mProcessInformation.dwProcessId);
        mChildHWND = findWindowByAnyPID(child_pids);
        if (mChildHWND) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (!mChildHWND) {
        std::cerr << "[xmux::error] Child HWND not found for PID: " << mProcessInformation.dwProcessId << "\n";
        return false;
    }

    std::cout << "[xmux::info] Found child HWND: " << mChildHWND << "\n";

    // Hook all child windows (set custom WndProc) so we can block dragging, etc.
    hookAllChildren(mChildHWND);

    // Spawn a detached thread that repeatedly patches window style for ~30s.
    // Why? Some applications aggressively restore their own styles; we fight back briefly.
    std::thread([hwnd = mChildHWND]() {
        // Patch style repeatedly for 300 iterations (100ms each = ~30s)
        for (int i = 0; i < 300; ++i) {
            LONG_PTR style = GetWindowLongPtrA(hwnd, GWL_STYLE);
            // Remove typical chrome styles and force as WS_CHILD.
            style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
            style |= WS_CHILD;

            SetWindowLongPtrA(hwnd, GWL_STYLE, style);

            // Tell the window to recalc frames without changing position/size or z-order.
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }).detach();

    // Remove some extended styles that might cause separate taskbar/edge issues.
    LONG_PTR ex_style = GetWindowLongPtrA(mChildHWND, GWL_EXSTYLE);
    ex_style &= ~(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_DLGMODALFRAME);
    SetWindowLongPtrA(mChildHWND, GWL_EXSTYLE, ex_style);

    // Make sure the child is a WS_CHILD and remove caption/thickframe/etc.
    LONG_PTR style = GetWindowLongPtrA(mChildHWND, GWL_STYLE);
    style |= WS_CHILD;
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    SetWindowLongPtrA(mChildHWND, GWL_STYLE, style);

    // Force frame recalculation
    SetWindowPos(mChildHWND, nullptr, 0, 0, 0, 0,
                SWP_NOZORDER | SWP_NOSIZE | SWP_NOMOVE | SWP_FRAMECHANGED);

    // Parent the child window into the console parent window (mParentHWND).
    SetParent(mChildHWND, mParentHWND);

    // Force redraw/state update
    SetWindowPos(
        mChildHWND,
        nullptr,
        0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED
    );

	// Ensure the parent window doesn't paint over areas occupied by child windows
	// This reduces flicker and prevents overdraw when embedding other HWNDs
    LONG_PTR parent_style = GetWindowLongPtrA(mParentHWND, GWL_STYLE);
    parent_style |= WS_CLIPCHILDREN;
    SetWindowLongPtrA(mParentHWND, GWL_STYLE, parent_style);

    // Avoid focus stealing: make sure child isn't topmost and don't activate it.
    SetWindowPos(mChildHWND, HWND_NOTOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);

    // Set true parent and ensure the HWND parent pointer is consistent.
    SetWindowLongPtrA(mChildHWND, GWLP_HWNDPARENT, (LONG_PTR)mParentHWND);
    SetParent(mChildHWND, mParentHWND);

    // Start up threads that keep everything in sync:
    mAtomicStateRunning = true;
    mLoopTickThread = std::thread(&xmux::attachTick, this);
    mMonitorThread = std::thread(&xmux::monitorThread, this);

    return true;
}

/* ----------------------------------------------------------------------------
 * launchProcess
 *
 * CreateProcessA wrapper that:
 *  - launches the child,
 *  - creates a job object and assigns the child so it will be killed when the job closes,
 *  - resumes thread and cleans up handles appropriately.
 *
 * Notes:
 *  - We pass FALSE for bInheritHandles so handles are not inherited.
 *  - We keep the child attached to terminal (no DETACHED_PROCESS flag).
 *  - Error handling: if anything fails we cleanup gJob and return false.
 * ----------------------------------------------------------------------------
 */
bool xmux::launchProcess(bool showNormal) {
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = showNormal ? SW_SHOWNORMAL : SW_HIDE;  // Try to hide any console window for the child

    // CreateProcess expects a mutable C string (char*). Copy command into vector with trailing null.
    std::vector<char> mutable_cmd(mCommand.begin(), mCommand.end());
    mutable_cmd.push_back('\0');

    if (!CreateProcessA(
            nullptr,
            mutable_cmd.data(),
            nullptr,
            nullptr,
            FALSE,
            0,  // NOTE: Don't detach; keep it tied to terminal
            nullptr,
            nullptr,
            &si,
            &mProcessInformation)) {
        std::cerr << "[xmux::error] Failed to launch the entered command inside xmux's constructor.\n";
        return false;
    }

    // Create a job object to manage child process lifetime.
    gJob = CreateJobObjectA(nullptr, nullptr);
    if (gJob == nullptr) {
        std::cerr << "[xmux::error] Failed to create Job Object\n";
        return false;
    }

    // Set job limits to ensure all processes in job are terminated when job is closed.
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

    if (!SetInformationJobObject(gJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
        std::cerr << "[xmux::error] Failed to set Job Object info\n";
        CloseHandle(gJob);
        gJob = nullptr;
        return false;
    }

    if (!AssignProcessToJobObject(gJob, mProcessInformation.hProcess)) {
        std::cerr << "[xmux::error] Failed to assign child process to Job Object\n";
        CloseHandle(gJob);
        gJob = nullptr;
        return false;
    }

    // Resume thread (CreateProcess returns suspended thread if we were creating suspended).
    ResumeThread(mProcessInformation.hThread);
    CloseHandle(mProcessInformation.hThread);
    return true;
}

/* ----------------------------------------------------------------------------
 * terminateInformationProcess
 *
 * Close and optionally wait on the child process handle.
 * Always null out handle after closing to avoid double-close.
 * ----------------------------------------------------------------------------
 */
bool xmux::terminateInformationProcess(bool wait) {
    if (mProcessInformation.hProcess) {
        if (!wait)
            WaitForSingleObject(mProcessInformation.hProcess, INFINITE);
        CloseHandle(mProcessInformation.hProcess);
        mProcessInformation.hProcess = nullptr;
    }

    return true;
}

/* ----------------------------------------------------------------------------
 * stop
 *
 * Stops monitoring threads and terminates the child process.
 * Joins threads if joinable (clean shutdown).
 * ----------------------------------------------------------------------------
 */
bool xmux::stop(bool force) {
    terminateInformationProcess(force);

    mAtomicStateRunning = false;
    if (mLoopTickThread.joinable())
        mLoopTickThread.join();

    if (mMonitorThread.joinable())
        mMonitorThread.join();

    return true;
}

/* ----------------------------------------------------------------------------
 * monitorThread
 *
 * Watches the parent process and kills the child when parent dies.
 * This avoids orphaned child instances if the console exits.
 *
 * Notes:
 *  - Uses OpenProcess(SYNCHRONIZE) to wait on parent termination.
 *  - Posts WM_CLOSE to the child and calls ExitProcess(0) to terminate process quickly.
 * ----------------------------------------------------------------------------
 */
void xmux::monitorThread() {
    DWORD parent_pid = getParentProcessId();
    HANDLE hParent = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
    if (hParent == nullptr) {
        std::cerr << "Failed to open parent process handle\n";
        return;
    }

    // Block until parent process terminates
    WaitForSingleObject(hParent, INFINITE);
    CloseHandle(hParent);

    std::cerr << "[xmux::info] Parent process terminated. Killing child processes.\n";
    terminateInformationProcess(true);

    mAtomicStateRunning = false;
    // Close the target window nicely; if it doesn't exit, job object will kill it.
    PostMessageA(mChildHWND, WM_CLOSE, 0, 0);
    ExitProcess(0);
}

/* ----------------------------------------------------------------------------
 * attachTick
 *
 * Main loop that:
 *  - polls parent/child window placements,
 *  - synchronizes the child window position/size to parent client area,
 *  - manages minimize/restore states,
 *  - handles a Win11 rounded-corner region hack to avoid ugly borders when embedded,
 *  - sets a topmost flag to keep child above parent contents if necessary.
 *
 * Important detail:
 *  - Sleeps for std::chrono::nanoseconds(1000) per loop — that's 1 microsecond,
 *    which is extremely tight (CPU-bound). Consider using milliseconds (e.g., 10ms).
 * ----------------------------------------------------------------------------
 */
void xmux::attachTick() {
    RECT pLastRect = {};
    bool pWasMinimized = false;

    // is_win11 lambda: checks Windows build number for Win11 (build >= 22000).
    bool is_win11 = [] {
        OSVERSIONINFOEXW os = {};
        os.dwOSVersionInfoSize = sizeof(os);
        GetVersionExW(reinterpret_cast<OSVERSIONINFOW*>(&os));
        return (os.dwMajorVersion == 10 && os.dwBuildNumber >= 22000);
    }();

    while (mAtomicStateRunning) {
        // Get window placement for parent and child — used to detect minimized/maximized states.
        WINDOWPLACEMENT pParentPlacement = {};
        pParentPlacement.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(mParentHWND, &pParentPlacement);

        WINDOWPLACEMENT pChildPlacement = {};
        pChildPlacement.length = sizeof(WINDOWPLACEMENT);
        GetWindowPlacement(mChildHWND, &pChildPlacement);

        bool pParentMinimized = (pParentPlacement.showCmd == SW_SHOWMINIMIZED);
        bool pChildMinimized = (pChildPlacement.showCmd == SW_SHOWMINIMIZED);

        // Update client rect and move/size child window to fill the parent client area.
        {
            RECT client_rect;
            if (GetClientRect(mParentHWND, &client_rect)) {
                gLockedRect = client_rect;
                RECT pTargetRect = client_rect;

                // Move child to 0,0 within parent and resize to match client area.
                MoveWindow(
                    mChildHWND,
                    0, 0,
                    pTargetRect.right - pTargetRect.left,
                    pTargetRect.bottom - pTargetRect.top,
                    TRUE
                );

                // Keep the child topmost relative to this parent so it doesn't get occluded.
                SetWindowPos(
                    mChildHWND,
                    HWND_TOPMOST,
                    0, 0,
                    pTargetRect.right - pTargetRect.left,
                    pTargetRect.bottom - pTargetRect.top,
                    SWP_SHOWWINDOW
                );
            }
        }

        // Minimize/restore handling: if parent minimized, hide the child; if restored, show child.
        if (pParentMinimized) {
            if (!pWasMinimized) {
                ShowWindow(mChildHWND, SW_HIDE);
                pWasMinimized = true;
            }
        } else {
            if (pWasMinimized || pChildMinimized) {
                // Restore child when parent is restored
                ShowWindow(mChildHWND, SW_RESTORE);
                pWasMinimized = false;
            }

            static bool fullscreen_applied = false;

            // Check if child is maximized (zoomed) — this used to be derived from WINDOWPLACEMENT.
            bool pChildMaximized = IsZoomed(mChildHWND);
            bool pParentMinimized = pParentPlacement.showCmd == SW_SHOWMINIMIZED;

            // If child is maximized, expand the parent to the monitor size (fullscreen).
            if (!pParentMinimized && pChildMaximized && !fullscreen_applied) {
                HMONITOR hMonitor = MonitorFromWindow(mParentHWND, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = {};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfo(hMonitor, &mi)) {
                    SetWindowPos(mParentHWND, nullptr,
                        mi.rcMonitor.left, mi.rcMonitor.top,
                        mi.rcMonitor.right - mi.rcMonitor.left,
                        mi.rcMonitor.bottom - mi.rcMonitor.top,
                        SWP_NOZORDER | SWP_NOACTIVATE);
                    fullscreen_applied = true;
                }
            } else if (!pChildMaximized && fullscreen_applied) {
                // If child exited fullscreen, restore parent window state.
                ShowWindow(mParentHWND, SW_RESTORE);
                fullscreen_applied = false;
            }

            // If the parent client rect changed, update the child size — optimize by memcmp.
            RECT client_rect;
            if (GetClientRect(mParentHWND, &client_rect)) {
                RECT pTargetRect = client_rect;

                if (memcmp(&pLastRect, &pTargetRect, sizeof(RECT)) != 0) {
                    pLastRect = pTargetRect;

                    MoveWindow(
                        mChildHWND,
                        0, 0,
                        pTargetRect.right - pTargetRect.left,
                        pTargetRect.bottom - pTargetRect.top,
                        TRUE
                    );

                    SetWindowPos(
                        mChildHWND,
                        HWND_TOPMOST,
                        0, 0,
                        pTargetRect.right - pTargetRect.left,
                        pTargetRect.bottom - pTargetRect.top,
                        SWP_SHOWWINDOW
                    );
                }

                // Win11 rounded corners hack:
                // - When not maximized, create a complex region to approximate rounded corners
                //   and avoid weird border artifacts. SetWindowRgn is used which transfers
                //   ownership of the HRGN to the system (do not delete after SetWindowRgn).
                if (is_win11 && !(pParentPlacement.showCmd == SW_MAXIMIZE)) {
                    int width = pTargetRect.right - pTargetRect.left + 1;
                    int height = pTargetRect.bottom - pTargetRect.top + 1;
                    int radius = 12; // corner radius; tweak to taste

                    // Create base region (rect) and corner rounded rects to OR-in.
                    HRGN region = CreateRectRgn(0, 0, width, height);
                    HRGN rBottomLeft = CreateRoundRectRgn(0, height - 2 * radius, 2 * radius, height, radius, radius);
                    HRGN rBottomRight = CreateRoundRectRgn(width - 2 * radius, height - 2 * radius, width, height, radius, radius);

                    // Combine the regions to approximate rounding only on the bottom corners.
                    CombineRgn(region, region, rBottomLeft, RGN_OR);
                    CombineRgn(region, region, rBottomRight, RGN_OR);

                    // Delete intermediate regions — they're no longer needed.
                    DeleteObject(rBottomLeft);
                    DeleteObject(rBottomRight);

                    // Subtract top-left and top-right so top corners remain sharp (matching parent).
                    HRGN top_left = CreateRectRgn(0, 0, radius, radius);
                    HRGN top_right = CreateRectRgn(width - radius, 0, width, radius);

                    CombineRgn(region, region, top_left, RGN_DIFF);
                    CombineRgn(region, region, top_right, RGN_DIFF);

                    DeleteObject(top_left);
                    DeleteObject(top_right);

                    // Set window region — system owns 'region' afterwards.
                    SetWindowRgn(mChildHWND, region, TRUE);
                } else {
                    // Remove any custom region when not applying Win11 hack.
                    SetWindowRgn(mChildHWND, nullptr, TRUE);
                }
            }
        }

        // NOTE: sleeping for nanoseconds(1000) == 1 microsecond is extremely tight and CPU heavy.
        // Consider raising to milliseconds (e.g., 10ms or 16ms) unless you need ultra-low latency.
        std::this_thread::sleep_for(std::chrono::nanoseconds(1000));
    }
}
