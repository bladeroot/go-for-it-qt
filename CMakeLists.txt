cmake_minimum_required(VERSION 3.21)

project(go-for-it-qt
    VERSION 1.9.5
    DESCRIPTION "Go for It Qt — A stylish to-do list with built-in productivity timer (Qt 6)"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Qml
    Quick
    QuickControls2
    Multimedia
    DBus
    Sql
    Network
    Test
)

qt_standard_project_setup(REQUIRES 6.5)
if(COMMAND qt_policy)
    qt_policy(SET QTP0004 OLD)
endif()

add_compile_definitions(
    GOFI_SOURCE_DATADIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

set(GOFI_CORE_SOURCES
    src/core/Schedule.h
    src/core/Schedule.cpp
    src/core/DatabaseManager.h
    src/core/DatabaseManager.cpp
    src/core/IssueTrackerService.h
    src/core/IssueTrackerService.cpp
    src/core/SettingsManager.h
    src/core/SettingsManager.cpp
    src/core/ShortcutManager.h
    src/core/ShortcutManager.cpp
    src/core/ActivityLog.h
    src/core/ActivityLog.cpp
    src/core/TaskTimer.h
    src/core/TaskTimer.cpp
    src/core/NotificationService.h
    src/core/NotificationService.cpp
    src/core/DesktopIntegration.h
    src/core/DesktopIntegration.cpp
    src/core/ListManagerModel.h
    src/core/ListManagerModel.cpp
    src/core/AppController.h
    src/core/AppController.cpp
    src/todotxt/TxtTask.h
    src/todotxt/TxtTask.cpp
    src/todotxt/TaskStoreModel.h
    src/todotxt/TaskStoreModel.cpp
    src/todotxt/TaskFilterProxyModel.h
    src/todotxt/TaskFilterProxyModel.cpp
    src/todotxt/TaskManager.h
    src/todotxt/TaskManager.cpp
)

add_library(gofi_core STATIC ${GOFI_CORE_SOURCES})
target_include_directories(gofi_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/todotxt
)
target_link_libraries(gofi_core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Multimedia
    Qt6::DBus
    Qt6::Sql
    Qt6::Network
)

qt_add_executable(go-for-it-qt
    src/main.cpp
)

qt_add_qml_module(go-for-it-qt
    URI GoForIt
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/pages/SelectionPage.qml
        qml/pages/TaskListPage.qml
        qml/components/TaskListView.qml
        qml/components/TaskRowDelegate.qml
        qml/components/TimerView.qml
        qml/components/TimerScheduleWidget.qml
        qml/dialogs/SettingsDialog.qml
        qml/dialogs/TxtListEditDialog.qml
        qml/dialogs/TaskDetailsDialog.qml
        qml/dialogs/FileConflictDialog.qml
        qml/dialogs/ContributeDialog.qml
        qml/dialogs/AboutDialog.qml
)

target_link_libraries(go-for-it-qt PRIVATE
    gofi_core
)

enable_testing()

add_executable(tst_todotxt
    tests/tst_todotxt.cpp
)
target_link_libraries(tst_todotxt PRIVATE
    gofi_core
    Qt6::Test
)
add_test(NAME tst_todotxt COMMAND tst_todotxt)

include(GNUInstallDirs)

install(TARGETS go-for-it-qt
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES data/go-for-it-qt.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data/sounds/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/go-for-it-qt/sounds
    FILES_MATCHING PATTERN "*.ogg"
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data/icons/48x48/apps/go-for-it.svg")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/data/icons/48x48/apps/go-for-it.svg"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
        RENAME go-for-it-qt.svg
    )
endif()
