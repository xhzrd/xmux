// * READ BEFORE USING
// * READ BEFORE USING
// * READ BEFORE USING

// This is a main.cpp file just as an example on how to use xmux.hpp
// It is not a main part of the library/package or whatever
// you want to call it :)

#include "xmux.hpp"

#include <filesystem>
#include <windows.h>
#include <thread>
#include <chrono>
#include <iostream>
#include <string>

std::string getTerminalTitleExecutable() {
    char title[1024];
    DWORD len = GetConsoleTitleA(title, sizeof(title));
    if (len == 0) return "unknown";

    std::string fullTitle(title, len);

    // Just extract the filename part like "bash.exe" or "cmd.exe"
    std::filesystem::path path(fullTitle);
    return path.filename().string();
}

int main() {
	HWND pConsoleHWND = xmux::findWindowByTitle(getTerminalTitleExecutable());
	if (!pConsoleHWND) {
        std::cerr << "[xmux-demo] Failed to get console window.\n";
        return 1;
    }

    DWORD consolePID = 0;
    if (!GetWindowThreadProcessId(pConsoleHWND, &consolePID) || consolePID == 0) {
        std::cerr << "[xmux-demo] Failed to get console PID from HWND.\n";
        return -1;
    }

    std::cout << "[xmux-demo] Console HWND: " << pConsoleHWND << ", PID: " << consolePID << "\n";

    // Use a simple, stable program like notepad
    // std::string childCommand = "mspaint.exe";
	// std::string childCommand = R"("mpv" "bunny.mp4" --no-border --ontop)";
	std::string childCommand = "notepad.exe";
    xmux mux(consolePID, childCommand);

	// * Some apps doesn't like to be hidden on start
	// * so for this example, we will set the showNormal to true because
	// * we want the application to be seen on start so windows doesn't freak out 
	// * about the HWND
    if (!mux.launch(true)) {
        std::cerr << "[xmux-demo] Failed to launch/embed the process.\n";
        return 1;
    }

    std::cout << "[xmux-demo] Successfully embedded notepad into the terminal.\n";

    while (mux.isStateRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        // Do whatever background work here.
    }

    std::cout << "[xmux-demo] Embedded process exited.\n";
    return 0;
}
