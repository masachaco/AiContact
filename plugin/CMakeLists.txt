FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# HTML instrument UIs (ACI spec §10) need a platform web view: WKWebView on macOS, WebView2 on Windows
# (the Microsoft.Web.WebView2 NuGet package at build time, statically linked), WebKitGTK on Linux.
# Without it the plugin still builds and shows skins / the generated controls instead.
option(AICONTACT_WEBVIEW "Support HTML instrument UIs (web view)" ON)
set(aicontact_webview OFF)
if(AICONTACT_WEBVIEW)
    if(APPLE)
        set(aicontact_webview ON)
    elseif(WIN32)
        # Same lookup as JUCE's FindWebView2.cmake, which would otherwise fail the configure step.
        if(JUCE_WEBVIEW2_PACKAGE_LOCATION)
            set(_wv2_dir "${JUCE_WEBVIEW2_PACKAGE_LOCATION}")
        else()
            set(_wv2_dir "$ENV{USERPROFILE}/AppData/Local/PackageManagement/NuGet/Packages")
        endif()
        file(GLOB _wv2 "${_wv2_dir}/*Microsoft.Web.WebView2*")
        if(_wv2)
            set(aicontact_webview ON)
        else()
            message(WARNING "Microsoft.Web.WebView2 NuGet package not found in ${_wv2_dir}: building without HTML "
                            "instrument UIs. Install it (see .github/workflows/build.yml) or set "
                            "JUCE_WEBVIEW2_PACKAGE_LOCATION.")
        endif()
    else()
        find_package(PkgConfig)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(AICONTACT_WEBKIT QUIET webkit2gtk-4.1)
            if(NOT AICONTACT_WEBKIT_FOUND)
                pkg_check_modules(AICONTACT_WEBKIT QUIET webkit2gtk-4.0)
            endif()
            pkg_check_modules(AICONTACT_GTK QUIET gtk+-3.0)
        endif()
        if(AICONTACT_WEBKIT_FOUND AND AICONTACT_GTK_FOUND)
            set(aicontact_webview ON)
        else()
            message(WARNING "webkit2gtk-4.1 / gtk+-3.0 development packages not found: building without HTML "
                            "instrument UIs (apt install libwebkit2gtk-4.1-dev libgtk-3-dev).")
        endif()
    endif()
endif()
message(STATUS "AI Contact: HTML instrument UIs ${aicontact_webview}")

set(AICONTACT_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND AICONTACT_FORMATS AU)
endif()

juce_add_plugin(AIContact
    COMPANY_NAME "AI Contact"
    PRODUCT_NAME "AI Contact"
    VERSION ${PROJECT_VERSION}
    PLUGIN_MANUFACTURER_CODE Aict
    PLUGIN_CODE Aicn
    FORMATS ${AICONTACT_FORMATS}
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Instrument Synth
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    NEEDS_WEB_BROWSER ${aicontact_webview}
    NEEDS_WEBVIEW2 ${aicontact_webview})

target_sources(AIContact PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/ParamPanel.cpp
    Source/CustomUi.cpp
    Source/ApiToken.cpp)

target_compile_definitions(AIContact PUBLIC
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

if(aicontact_webview)
    # The `window.aci` bridge injected into HTML UIs (the skill ships the same file for previews).
    juce_add_binary_data(AIContactWebData
        HEADER_NAME AIContactWebData.h
        NAMESPACE AIContactWebData
        SOURCES ${CMAKE_SOURCE_DIR}/sdk/web/aci-bridge.js)
    target_link_libraries(AIContact PRIVATE AIContactWebData)
    target_compile_definitions(AIContact PUBLIC JUCE_WEB_BROWSER=1 AICONTACT_WEBVIEW=1)
    if(WIN32)
        target_compile_definitions(AIContact PUBLIC JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1)
    endif()
else()
    target_compile_definitions(AIContact PUBLIC JUCE_WEB_BROWSER=0 AICONTACT_WEBVIEW=0)
endif()

target_link_libraries(AIContact
    PRIVATE
        aicontact_core
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
