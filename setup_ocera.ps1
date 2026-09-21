# setup_ocera.ps1
# Run this from inside your OCERA repo folder:
#   .\setup_ocera.ps1
# It creates the full skeleton: CMakeLists.txt, src/main.cpp, .gitignore,
# README.md, per-module placeholder READMEs, and .vscode/extensions.json.

Write-Host "Creating OCERA skeleton in $(Get-Location) ..."

# --- folders ---
$folders = @(
    "src\storage", "src\txn", "src\sql", "src\common",
    "tests", "docs", ".vscode"
)
foreach ($f in $folders) {
    New-Item -ItemType Directory -Path $f -Force | Out-Null
}

# --- CMakeLists.txt ---
@'
cmake_minimum_required(VERSION 3.15)
project(OCERA CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
endif()

find_package(Threads REQUIRED)

add_executable(ocera src/main.cpp)
target_link_libraries(ocera PRIVATE Threads::Threads)
'@ | Set-Content -Path "CMakeLists.txt" -Encoding UTF8

# --- src/main.cpp ---
@'
#include <iostream>
#include <thread>
#include <mutex>
#include <vector>

int shared_counter = 0;
std::mutex counter_mutex;

void increment_many_times(int times) {
    for (int i = 0; i < times; i++) {
        std::lock_guard<std::mutex> lock(counter_mutex);
        shared_counter++;
    }
}

int main() {
    std::cout << "OCERA environment check starting...\n";

    const int num_threads = 4;
    const int increments_per_thread = 50000;
    std::vector<std::thread> threads;

    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(increment_many_times, increments_per_thread);
    }

    for (auto& t : threads) {
        t.join();
    }

    std::cout << "Final counter value: " << shared_counter << "\n";
    std::cout << "Expected value:      " << (num_threads * increments_per_thread) << "\n";

    if (shared_counter == num_threads * increments_per_thread) {
        std::cout << "Environment OK - threads + mutex working correctly.\n";
    } else {
        std::cout << "Mismatch! Something is wrong with the build/threading setup.\n";
    }

    return 0;
}
'@ | Set-Content -Path "src\main.cpp" -Encoding UTF8

# --- .gitignore ---
@'
build/
out/
cmake-build-*/
CMakeFiles/
CMakeCache.txt
*.exe
*.obj
*.o
*.dll
*.exp
*.lib
*.pdb

.vs/
.vscode/*
!.vscode/extensions.json
*.code-workspace

*.db
*.wal
*.log
/data/

Thumbs.db
.DS_Store
'@ | Set-Content -Path ".gitignore" -Encoding UTF8

# --- .vscode/extensions.json ---
@'
{
    "recommendations": [
        "ms-vscode.cpptools",
        "ms-vscode.cmake-tools",
        "twxs.cmake"
    ]
}
'@ | Set-Content -Path ".vscode\extensions.json" -Encoding UTF8

# --- README.md ---
@'
# OCERA

A concurrent, crash-proof embedded database engine, built from scratch to
demonstrate core Operating Systems and DBMS concepts.

## Building

```
mkdir build
cd build
cmake -G "MinGW Makefiles" ..
cmake --build .
./ocera.exe
```

If everything is set up correctly, ocera.exe runs a threading sanity
check and prints "Environment OK - threads + mutex working correctly."
'@ | Set-Content -Path "README.md" -Encoding UTF8

# --- module placeholder READMEs ---
"# storage/`n`nOwner: Team Lead`nScope (Month 1-2): page format, disk I/O, buffer pool (LRU)." | Set-Content -Path "src\storage\README.md" -Encoding UTF8
"# txn/`n`nOwner: Member 2`nScope (Month 3): transaction manager, 2PL lock manager, deadlock detection." | Set-Content -Path "src\txn\README.md" -Encoding UTF8
"# sql/`n`nOwner: Member 3`nScope (Month 2-4): SQL parser, B+Tree index, WAL, crash recovery." | Set-Content -Path "src\sql\README.md" -Encoding UTF8
"# common/`n`nShared code used by more than one module (page struct, constants, error types)." | Set-Content -Path "src\common\README.md" -Encoding UTF8
"# tests`n`nTest scripts and crash-simulation scripts will live here (Month 3-4)." | Set-Content -Path "tests\README.md" -Encoding UTF8
"# docs`n`nArchitecture diagrams and design notes go here." | Set-Content -Path "docs\README.md" -Encoding UTF8

Write-Host "Done. Files created:"
Get-ChildItem -Recurse -File | Where-Object { $_.FullName -notmatch "\\build\\" } | ForEach-Object { Write-Host " - $($_.FullName.Substring((Get-Location).Path.Length + 1))" }
