cmake_minimum_required(VERSION 3.15)
project(ksc C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MINGW)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -LC:/msys64/ucrt64/lib")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -s")
endif()

add_library(sqlite3 STATIC sqlite3/sqlite3.c)
target_include_directories(sqlite3 PUBLIC sqlite3)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_DEPRECATED
)

set(SOURCES
    src/main.c
    src/keyhook.c
    src/database.c
    src/keylogdb.c
    src/cloudsync.c
    src/http_win.c
    src/json_micro.c
    src/ssh_sync.c
    src/telegram.c
    src/gui.c
    src/tray.c
    src/startup.c
    src/ksc.rc
)

add_executable(ksc WIN32 ${SOURCES})

target_include_directories(ksc PRIVATE src "${CMAKE_SOURCE_DIR}/libssh2/include")

target_compile_definitions(ksc PRIVATE _CRT_SECURE_NO_WARNINGS)

target_link_libraries(ksc PRIVATE
    sqlite3
    "${CMAKE_SOURCE_DIR}/libssh2/libssh2.a"
    ssl
    crypto
    bcrypt
    crypt32
    ws2_32
    comctl32
    shell32
    advapi32
    uxtheme
    dwmapi
    comdlg32
    winhttp
)
