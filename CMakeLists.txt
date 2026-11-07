cmake_minimum_required(VERSION 3.25)
# saucer's dependencies create their namespaced aliases twice (once themselves, once through
# PackageProject). A vcpkg toolchain wraps add_library() under the policies in effect here,
# so keep duplicate aliases legal for them.
cmake_policy(SET CMP0107 OLD)
project(ncm-classic VERSION 1.2.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
include(GNUInstallDirs)
include(FetchContent)

# Disable C++ Exceptions (Qt 6.9 and later depends on RTTI)
if(MSVC)
    string(REGEX REPLACE "/EH[a-z]+" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHs-c-")
    add_definitions(-D_HAS_EXCEPTIONS=0)
else()
    string(REGEX REPLACE "-fexceptions" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions")
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network Core5Compat)
find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(PkgConfig QUIET)

# saucer: System webview (WebView2 / WebKitGTK / WKWebView)
set(saucer_exceptions OFF CACHE BOOL "" FORCE)
set(saucer_static ON CACHE BOOL "" FORCE)
FetchContent_Declare(saucer
    GIT_REPOSITORY https://github.com/saucer/saucer
    GIT_TAG v8.0.5
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
    SYSTEM
)
FetchContent_MakeAvailable(saucer)

# qtkeychain: Keeps the session encrypted in the system keychain
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TRANSLATIONS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(qtkeychain
    GIT_REPOSITORY https://github.com/frankosterfeld/qtkeychain
    GIT_TAG bb50c8ba87d8210116eb6874eb36dfb1c0ae9dce
    EXCLUDE_FROM_ALL
    SYSTEM
)
FetchContent_MakeAvailable(qtkeychain)
target_compile_definitions(qt6keychain PUBLIC QT6KEYCHAIN_STATIC_DEFINE)

add_executable(${PROJECT_NAME}
    src/app/main.cpp
    src/bridge/Bridge.cpp
    src/platform/DesktopIntegration.cpp
    src/platform/SingleInstance.cpp
    src/platform/SystemCommands.cpp
    src/services/crypto/Crypto.cpp
    src/services/library/CueParser.cpp
    src/services/library/MusicLibrary.cpp
    src/services/library/MusicTags.cpp
    src/services/media/AudioService.cpp
    src/services/media/MediaService.cpp
    src/services/media/StreamCache.cpp
    src/services/media/VlcRuntime.cpp
    src/services/network/MultipartServices.cpp
    src/services/network/CookieVault.cpp
    src/services/network/NativeNetwork.cpp
    src/services/persistence/DatabaseExecutor.cpp
    src/services/persistence/PersistenceService.cpp
    src/services/persistence/Profile.cpp
    src/services/persistence/SettingsStore.cpp
    src/services/persistence/SqlDatabase.cpp
    src/services/transfers/TransferService.cpp
    src/ui/AppCommands.cpp
    src/ui/DesktopLyrics.cpp
    src/ui/DialogCommands.cpp
    src/ui/Lyrics.cpp
    src/ui/LyricsPainter.cpp
    src/ui/MenuStyle.cpp
    src/ui/MiniPlayer.cpp
    src/ui/MiniPlaylist.cpp
    src/ui/PageMenu.cpp
    src/ui/PlayerWindows.cpp
    src/ui/PopupMenus.cpp
    src/ui/SessionImportDialog.cpp
    src/ui/Skin.cpp
    src/ui/SpriteButton.cpp
    src/ui/TrayIcon.cpp
    src/ui/VolumeWindow.cpp
    src/ui/WindowCommands.cpp
    src/web/ResourceHandler.cpp
    src/web/WebHost.cpp
    src/web/WebWindow.cpp
)
target_include_directories(${PROJECT_NAME} PRIVATE src)
target_link_libraries(${PROJECT_NAME} PRIVATE saucer::private)
if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
    set(app_icon "${CMAKE_CURRENT_SOURCE_DIR}/public/webapp/style/res/ico/cloud_music.ico")
    set(app_resource "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.rc")
    configure_file("contrib/${PROJECT_NAME}.rc.in" "${app_resource}" @ONLY)
    set_property(SOURCE "${app_resource}" PROPERTY OBJECT_DEPENDS "${app_icon}")
    target_sources(${PROJECT_NAME} PRIVATE
        "${app_resource}"
        src/platform/DesktopIntegration_win.cpp
        src/platform/GlobalHotkeys_win.cpp
        src/web/NativeView_win.cpp
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Qt6 6.8 REQUIRED COMPONENTS DBus)
    target_sources(${PROJECT_NAME} PRIVATE
        src/platform/DesktopIntegration_linux.cpp
        src/platform/GlobalHotkeys_linux.cpp
        src/platform/Mpris.cpp
        src/web/NativeView_generic.cpp
    )
    target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::DBus)
else()
    target_sources(${PROJECT_NAME} PRIVATE
        src/platform/DesktopIntegration_generic.cpp
        src/platform/GlobalHotkeys_generic.cpp
        src/web/NativeView_generic.cpp
    )
endif()

find_package(TagLib 2 CONFIG QUIET)
if(TARGET TagLib::TagLib)
    set(taglib TagLib::TagLib)
elseif(TARGET TagLib::tag)
    set(taglib TagLib::tag)
elseif(TARGET tag)
    set(taglib tag)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(TAGLIB REQUIRED IMPORTED_TARGET taglib>=2.0)
    set(taglib PkgConfig::TAGLIB)
endif()

find_package(VLC CONFIG QUIET)
if(TARGET VLC::vlc)
    set(vlc VLC::vlc)
elseif(TARGET VLC::VLC)
    set(vlc VLC::VLC)
else()
    if(PkgConfig_FOUND)
        pkg_check_modules(VLC QUIET IMPORTED_TARGET libvlc)
    endif()
    if(TARGET PkgConfig::VLC)
        set(vlc PkgConfig::VLC)
    else()
        find_path(NCM_VLC_INCLUDE_DIR vlc/vlc.h HINTS ENV VLC_ROOT PATH_SUFFIXES include sdk/include)
        # libvlc first: the Windows SDK's vlc.lib is a GNU ld INPUT() stub MSVC cannot link.
        find_library(NCM_VLC_LIBRARY NAMES libvlc vlc HINTS ENV VLC_ROOT PATH_SUFFIXES lib sdk/lib)
        if(NOT NCM_VLC_INCLUDE_DIR OR NOT NCM_VLC_LIBRARY)
            message(FATAL_ERROR "libVLC 3 headers and library required. Set VLC_ROOT to the VLC SDK directory.")
        endif()
        target_include_directories(${PROJECT_NAME} PRIVATE "${NCM_VLC_INCLUDE_DIR}")
        set(vlc "${NCM_VLC_LIBRARY}")
    endif()
endif()

if(TARGET SQLite3::SQLite3)
    set(sqlite SQLite3::SQLite3)
else()
    set(sqlite SQLite::SQLite3)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Core5Compat
    saucer::saucer qt6keychain
    ${sqlite} ${taglib} ${vlc} OpenSSL::Crypto ZLIB::ZLIB
)

set(schema_dir "${CMAKE_CURRENT_SOURCE_DIR}/src/services/persistence")
qt_add_resources(${PROJECT_NAME} persistence_schema
    PREFIX /ncm/persistence
    BASE "${schema_dir}"
    FILES "${schema_dir}/online-schema.sql" "${schema_dir}/offline-schema.sql"
)

set(NCM_DISPLAY_NAME "NetEase Cloud Music (Classic)")
target_compile_definitions(${PROJECT_NAME} PRIVATE
    NCM_APP_ID="${PROJECT_NAME}"
    NCM_DISPLAY_NAME="${NCM_DISPLAY_NAME}"
)

set(public_dir "${CMAKE_CURRENT_SOURCE_DIR}/public")
file(GLOB_RECURSE asset_files CONFIGURE_DEPENDS "${public_dir}/*")
list(FILTER asset_files EXCLUDE REGEX "/i18n/[^/]+\\.ts$")

# Use the checked-in catalogs when Qt Linguist is unavailable.
find_package(Qt6LinguistTools QUIET)
if(COMMAND qt_add_translations)
    list(REMOVE_ITEM asset_files
        "${public_dir}/i18n/cloud-music.qm" "${public_dir}/i18n/cloud-music_zh_CN.qm"
    )
    qt_add_translations(${PROJECT_NAME}
        TS_FILES public/i18n/cloud-music.ts public/i18n/cloud-music_zh_CN.ts
        RESOURCE_PREFIX /ncm/assets/i18n
    )
endif()

block()
    # BIG_RESOURCES builds a separate object, which needs its own PIC setting.
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    qt_add_resources(${PROJECT_NAME} app_assets BIG_RESOURCES
        PREFIX /ncm/assets
        BASE "${public_dir}"
        FILES ${asset_files}
    )
endblock()

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    configure_file("contrib/${PROJECT_NAME}.desktop.in" "${PROJECT_NAME}.desktop" @ONLY)
    qt_add_resources(${PROJECT_NAME} desktop_entry
        PREFIX /ncm
        BASE "${CMAKE_CURRENT_BINARY_DIR}"
        FILES "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.desktop"
    )
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
    )
    install(FILES "${public_dir}/misc/netease-cloud-music.svg"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
        RENAME "${PROJECT_NAME}.svg"
    )
endif()
