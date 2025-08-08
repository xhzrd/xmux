// xmux.hpp
// 
// Declares the xmux class — a Windows-specific process manager designed to
// launch, embed, and monitor a child process within a parent window.
// 
// Responsibilities:
//  - Spawn and terminate a target process (with optional window embedding).
//  - Locate parent/child HWNDs by title or process ID.
//  - Hook and forward WndProc calls to preserve original behavior.
//  - Maintain process lifecycle state using atomics and threads.
//  - Use a Job object to ensure all child processes are auto-killed when xmux dies.
// 
// Notes:
//  - This header is self-contained (inline statics used for shared state).
//  - Thread safety is handled via std::atomic and careful window procedure hooks.
//  - Only supports Win32 API; not portable to non-Windows platforms.
// 

#pragma once

#include <atomic>
#include <thread>
#include <windows.h>

class xmux {
	public:
		explicit xmux(int parentPID, const std::string command);
		xmux();
		~xmux();

		bool launch(bool showNormal = false);
		bool terminateInformationProcess(bool wait = true);
		bool stop(bool force = false);

		static HWND findWindowByTitle(const std::string& title) {
			HWND hwnd = nullptr;
			do {
				hwnd = FindWindowExA(nullptr, hwnd, nullptr, nullptr);
				if (hwnd) {
					char wnd_title[256];
					GetWindowTextA(hwnd, wnd_title, sizeof(wnd_title));
					if (std::string(wnd_title).find(title) != std::string::npos) {
						return hwnd;
					}
				}
			} while (hwnd != nullptr);
			return nullptr;
		}

		bool isStateRunning() const {
			return mAtomicStateRunning.load();
		}

	private:
		int mPID = -1;
		std::string mCommand = "echo";

		bool launchProcess(bool showNormal = false);
		void attachTick();
		void monitorThread();

		HWND mChildHWND = nullptr;
		HWND mParentHWND = nullptr;

		PROCESS_INFORMATION mProcessInformation = {};

		// Shared
		std::atomic<bool> mAtomicStateRunning = false;
		std::thread mLoopTickThread;
		std::thread mMonitorThread;

		/* ---- Globals ---- */

		// Job object handle used to auto-kill the child process when job closed.
		// Using a job object makes sure children die when the manager dies (optional but useful).
		HANDLE gJob = nullptr;

		// Keep original WndProcs so we can forward messages back to the original window proc.
		// Map key is HWND (child window), value is WNDPROC (original function pointer).
		inline static std::unordered_map<HWND, WNDPROC> gOriginalProcs;
		inline static HWND gFoundHWND;

		// A client rect cached for the locked region — used by attachTick to size/move child window.
		RECT gLockedRect = { 0, 0, 0, 0 };

		void hookAllChildren(HWND hwnd);

		static LRESULT CALLBACK LockedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
		DWORD getParentProcessId();

		HWND findWindowByPID(DWORD pid);
		HWND findWindowByPIDRecursive(DWORD pid);
		HWND findWindowByPIDFullScan(DWORD pid);
		HWND findWindowByAnyPID(const std::vector<DWORD>& pids);

		static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam);
		static BOOL CALLBACK EnumThreadWindowsProc(HWND hwnd, LPARAM lParam);

		std::vector<DWORD> getThreadsInProcess(DWORD pid);
		std::vector<DWORD> getAllChildPIDs(DWORD parent_pid);
};
