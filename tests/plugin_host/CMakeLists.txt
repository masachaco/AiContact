# Test host that loads the built AI Contact.vst3 like a DAW would.
# Configure with -DAICONTACT_BUILD_PLUGIN_TEST=ON.
juce_add_console_app(aicontact_plugin_test PRODUCT_NAME "aicontact_plugin_test")
target_sources(aicontact_plugin_test PRIVATE main.cpp)
target_compile_definitions(aicontact_plugin_test PRIVATE
    JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(aicontact_plugin_test PRIVATE juce::juce_audio_processors juce::juce_audio_formats juce::juce_gui_basics
    PUBLIC juce::juce_recommended_config_flags)
