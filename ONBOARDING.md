# Onboarding: Get OCERA running on your machine

You have two options. **Docker is recommended** - it gives everyone the exact
same compiler/tools regardless of OS, and avoids a couple of gotchas we hit
setting this up natively on Windows (see bottom of this doc).

---

## Option A: Docker (recommended)

### 1. Install prerequisites
- **Docker Desktop**: https://www.docker.com/products/docker-desktop
  - On Windows: also run `wsl --install` in an admin PowerShell first if you
    don't already have WSL2, then restart if prompted.
- **VS Code**: https://code.visualstudio.com
- In VS Code, install the **"Dev Containers"** extension (search it in the
  Extensions panel, publisher: Microsoft).

### 2. Clone the repo
```bash
git clone <REPO_URL>
```
Open the resulting `OCERA` folder in VS Code.

### 3. Reopen in container
A popup should appear: *"Folder contains a Dev Container configuration.
Reopen in Container?"* - click it.
If it doesn't appear: `Ctrl+Shift+P` -> "Dev Containers: Reopen in Container".

First time takes 1-3 minutes (builds the image). You'll know it worked when
the bottom-left corner of VS Code says **"Dev Container: OCERA Dev
Environment"**.

### 4. Build and verify
Open a terminal inside VS Code (now a Linux shell) and run:
```bash
mkdir -p build && cd build
cmake -G "Unix Makefiles" ..
cmake --build .
./ocera
```
You should see: `Environment OK - threads + mutex working correctly.`

If you're stuck at "Docker returned an error" / container fails to start:
see the **Known gotchas** section below before asking for help - it's
probably one of these two.

---

## Option B: Native install (no Docker)

Only do this if you have a specific reason to avoid Docker. Steps differ
by OS:

**Windows:** Install MSYS2 (msys2.org), then in the "MSYS2 UCRT64" terminal:
```bash
pacman -Syu
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-gdb make
```
Add `C:\msys64\ucrt64\bin` to your Windows PATH (System Properties -> Environment
Variables), open a **fresh** terminal, then:
```bash
mkdir build && cd build
cmake -G "MinGW Makefiles" ..
cmake --build .
./ocera.exe
```

**Mac:** `brew install cmake gdb` (Xcode Command Line Tools give you g++/clang).

**Linux:** `sudo apt install build-essential cmake gdb` (or your distro's equivalent).

Then on Mac/Linux:
```bash
mkdir build && cd build
cmake ..
cmake --build .
./ocera
```

---

## Known gotchas (from setting this up the first time)

1. **Windows PATH picks up an old/wrong compiler.** If `g++ --version` shows
   something ancient like "MinGW.org GCC 6.3.0" instead of a recent MSYS2
   build, you likely have a leftover `C:\MinGW\bin` (or similar) earlier in
   your PATH than `C:\msys64\ucrt64\bin`. Check with `Get-Command g++ -All`
   in PowerShell (not `where g++` - that's aliased to something else in
   PowerShell and gives a false empty result). Fix: remove the old entry
   from **both** User and System PATH in Environment Variables, then open a
   fresh terminal.

2. **Docker Dev Container fails with a mount/exit-code-1 error mentioning
   `wslg\runtime-dir\wayland-0`.** This is a stale socket file, not a real
   problem with our setup. Go to
   `\\wsl.localhost\Ubuntu\mnt\wslg\runtime-dir\` in File Explorer (distro
   name may differ), delete `wayland-0` and `wayland-0.lock`, restart VS
   Code, and retry "Reopen in Container".

3. **"Docker returned an error: make sure the Docker daemon is running."**
   Docker Desktop needs to actually be open and fully started (whale icon
   static, not animating) before VS Code can use it - it doesn't auto-start
   on its own after install on every system.
