# xmux

**xmux** is a C++ project that lets you embed (almost) **any Windows GUI application** directly inside a terminal window, including Command Prompt, PowerShell, Windows Terminal, Alacritty, Gitbash, and more.  
Think of it as *window inception*, running graphical apps *inside* terminal windows, with proper event handling.

It's **not** MPV-exclusive, MPV was just a convenient test case for me. You can embed literally any HWND-based GUI app.

---

## Features

- Run (almost) **any Windows GUI app** inside a terminal window.
- Works in **Command Prompt**, **PowerShell**, **Gitbash**, **Alacritty** and **Windows Terminal**.
- Compatible with both **native Windows apps** and cross-platform ones (Must be compatiable to run in Windows).
- Minimal dependencies: just pure Win32 API and C++.
- Terminal and child window interaction with no drag/move glitches.

---

## Building

You'll need:

- **CMake** (with Ninja generator)
- **Clang** (recommended for building)
- **Windows 10+**
- A terminal that supports resizing & drawing (CMD/PowerShell works fine)

**Steps:**
```bash
# 1. Clone the project
git clone https://github.com/xhzrd/xmux.git
cd xmux

# 2. Create & go inside the build folder
mkdir build
cd build

# 3. Generate build files using Clang + Ninja
cmake -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ..

# 4. Build
ninja
```

---

Here’s a clean **Usage Responsibility** section you can drop into your README:

---

## Usage Responsibility

**xmux** is a technical tool intended for legitimate software development, debugging, and experimentation purposes.
It is not designed or endorsed for use in any activity that is illegal, unethical, or prohibited in your local laws or religious beliefs.

By using **xmux**, you agree that:

* You are solely responsible for how you use this software.
* The author(s) are **not** liable for any misuse or resulting consequences.
* You will not use it to display, distribute, or promote any immoral, harmful, unlawful, or content that disrespects **religious beliefs**.

This project is released as-is, without any warranty or guarantee of fitness for a particular purpose.

---

### This project is licensed under the Apache License 2.0, see [LICENSE](LICENSE) for details.