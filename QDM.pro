# Qt 5.8 / MSVC 2013 x64. Keep Qt's C++ ABI inside the v120 application.
QT += core gui widgets network
CONFIG += c++11 no_batch
# Windows 默认双配置会额外创建 build/debug/bin；每次只生成所选配置。
CONFIG -= debug_and_release debug_and_release_target
TEMPLATE = app
TARGET = QDM
VERSION = 0.2.1
DESTDIR = $$OUT_PWD/bin
INCLUDEPATH += $$PWD/src $$PWD/third_party/curl/curl-8.23.0_9-win64-mingw/include $$PWD/third_party/sqlite/sqlite-amalgamation-3540100
SOURCES += src/main.cpp src/task.cpp src/repository.cpp src/downloadengine.cpp src/mainwindow.cpp third_party/sqlite/sqlite-amalgamation-3540100/sqlite3.c
HEADERS += src/task.h src/repository.h src/downloadengine.h src/mainwindow.h
SOURCES += src/browserbridge.cpp src/browserprotocol.cpp
HEADERS += src/browserbridge.h src/browserprotocol.h
DEFINES += SQLITE_THREADSAFE=1 SQLITE_DQS=0 SQLITE_OMIT_LOAD_EXTENSION
win32 {
    LIBS += $$OUT_PWD/curl-import.lib
    LIBS += -lshell32 -lcrypt32 -ladvapi32
}
