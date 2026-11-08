cmake_minimum_required(VERSION 3.21)

project(go-for-it-qt
    VERSION 2.0.0
    DESCRIPTION "Go for It Qt — A stylish to-do list with built-in productivity timer (Qt 6)"
    HOMEPAGE_URL "https://github.com/bladeroot/go-for-it-qt"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

add_compile_definitions(GOFI_VERSION="${PROJECT_VERSION}")

include(GNUInstallDirs)
include(CTest)

find_package(Qt6 6.5 REQUIRED COMPONENTS
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
)

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
endif()

if(COMMAND qt_standard_project_setup)
    qt_standard_project_setup()
endif()
if(COMMAND qt_policy)
    qt_policy(SET QTP0001 NEW)
    if(Qt6_VERSION VERSION_GREATER_EQUAL "6.8.0")
        qt_policy(SET QTP0004 NEW)
    endif()
endif()

set(GOFI_CORE_SOURCES
    src/core/Schedule.h
    src/core/Schedule.cpp
    src/core/DatabaseManager.h
    src/core/DatabaseManager.cpp
    src/core/IssueTrackerService.h
    src/core/IssueTrackerService.cpp
    src/core/SettingsManager.h
    src/core/SettingsManager.cpp
    src/core/UkTranslator.h
    src/core/UkTranslator.cpp
    data/translations.qrc
    src/core/ShortcutManager.h
    src/core/ShortcutManager.cpp
    src/core/ActivityLog.h
    src/core/ActivityLog.cpp
    src/core/AnalyticsService.h
    src/core/AnalyticsService.cpp
    src/core/AmbientSoundService.h
    src/core/AmbientSoundService.cpp
    src/core/SystemHookService.h
    src/core/SystemHookService.cpp
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
    src/tui/TuiApp.h
    src/tui/TuiApp.cpp
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
        qml/components/MiniPlayerWindow.qml
        qml/components/DashboardView.qml
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

qt_add_executable(go-for-it-tui
    src/main_tui.cpp
)
target_link_libraries(go-for-it-tui PRIVATE
    gofi_core
)

# Copy runtime data into build tree so uninstalled builds find sounds/icons relative to binary
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/data DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

if(BUILD_TESTING)
    add_executable(tst_todotxt
        tests/tst_todotxt.cpp
    )
    target_link_libraries(tst_todotxt PRIVATE
        gofi_core
        Qt6::Test
    )
    add_test(NAME tst_todotxt COMMAND tst_todotxt)
endif()

# ==============================================================================
# System Installation Targets (Arch PKGBUILD, Debian/Ubuntu .deb, Fedora .rpm)
# ==============================================================================

install(TARGETS go-for-it-qt go-for-it-tui
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT Runtime
)

install(FILES data/go-for-it-qt.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
    COMPONENT Runtime
)

install(FILES data/go-for-it-qt.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo
    COMPONENT Runtime
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data/sounds/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/go-for-it-qt/sounds
    COMPONENT Runtime
    FILES_MATCHING PATTERN "*.ogg"
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data/translations/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/go-for-it-qt/translations
    COMPONENT Runtime
    FILES_MATCHING PATTERN "*.json"
)

foreach(ICON_SIZE 16x16 24x24 32x32 48x48 64x64 128x128)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data/icons/${ICON_SIZE}/apps/go-for-it.svg")
        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/data/icons/${ICON_SIZE}/apps/go-for-it.svg"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${ICON_SIZE}/apps"
            RENAME go-for-it-qt.svg
            COMPONENT Runtime
        )
    endif()
endforeach()

foreach(SYM_SIZE 16x16 24x24)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data/icons/${SYM_SIZE}/apps/go-for-it-symbolic.svg")
        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/data/icons/${SYM_SIZE}/apps/go-for-it-symbolic.svg"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${SYM_SIZE}/apps"
            RENAME go-for-it-qt-symbolic.svg
            COMPONENT Runtime
        )
    endif()
endforeach()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data/icons/48x48/apps/go-for-it.svg")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/data/icons/48x48/apps/go-for-it.svg"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
        RENAME go-for-it-qt.svg
        COMPONENT Runtime
    )
endif()

install(FILES LICENSE
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/${PROJECT_NAME}"
    COMPONENT Runtime
)

install(FILES README.md AUTHORS
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
    COMPONENT Runtime
)

install(FILES data/go-for-it-qt.1 data/go-for-it-tui.1
    DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
    COMPONENT Runtime
)

# ==============================================================================
# CPack Configuration (for `cpack -G DEB` / `cpack -G TGZ`)
# ==============================================================================
set(CPACK_PACKAGE_NAME "go-for-it-qt")
set(CPACK_PACKAGE_VENDOR "Yurii Myronchuk (@bladeroot)")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "Yurii Myronchuk <bladeroot@gmail.com>")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
set(CPACK_STRIP_FILES TRUE)

# Debian / Ubuntu (.deb) specific settings
set(CPACK_DEBIAN_PACKAGE_NAME "go-for-it-qt")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Yurii Myronchuk <bladeroot@gmail.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${PROJECT_HOMEPAGE_URL}")
option(CPACK_DEBIAN_PACKAGE_SHLIBDEPS "Enable automatic dependency detection with dpkg-shlibdeps" ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6sql6-sqlite, qml6-module-qtquick, qml6-module-qtquick-controls, qml6-module-qtquick-layouts, qml6-module-qtquick-window, qml6-module-qtquick-dialogs")

include(CPack)
