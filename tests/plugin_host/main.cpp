// aicontact_plugin_test <AI Contact.vst3> <seconds> [--load state.bin] [--save state.bin] [--show-editor]
// Loads the plugin through JUCE's VST3 hosting, plays a C-major arpeggio on
// MIDI channels 1 and 2 in real time and prints the output level per second.
#include <juce_audio_processors/juce_audio_processors.h>

#include <iostream>
#include <thread>

int main(int argc, char** argv) {
    if (argc < 3) { std::cerr << "usage: aicontact_plugin_test <vst3> <seconds> [--load f] [--save f] [--show-editor]\n"; return 2; }
    juce::ScopedJuceInitialiser_GUI init;
    juce::String loadPath, savePath;
    bool showEditor = false;
    for (int i = 3; i < argc; ++i)
        if (juce::String(argv[i]) == "--show-editor") showEditor = true;
    for (int i = 3; i + 1 < argc; ++i) {
        if (juce::String(argv[i]) == "--load") loadPath = argv[i + 1];
        if (juce::String(argv[i]) == "--save") savePath = argv[i + 1];
    }

    juce::AudioPluginFormatManager formats;
    formats.addFormat(new juce::VST3PluginFormat());
    juce::OwnedArray<juce::PluginDescription> found;
    juce::VST3PluginFormat vst3;
    vst3.findAllTypesForFile(found, argv[1]);
    if (found.isEmpty()) { std::cerr << "no plugin found in " << argv[1] << "\n"; return 1; }
    std::cout << "found: " << found[0]->name << " synth=" << found[0]->isInstrument << "\n";

    juce::String error;
    const double sr = 48000.0;
    const int block = 512;
    auto plugin = formats.createPluginInstance(*found[0], sr, block, error);
    if (!plugin) { std::cerr << "load failed: " << error << "\n"; return 1; }
    plugin->setPlayConfigDetails(0, 2, sr, block);
    plugin->prepareToPlay(sr, block);
    std::cout << "params: " << plugin->getParameters().size() << ", first: "
              << plugin->getParameters()[0]->getName(64) << "\n";

    if (loadPath.isNotEmpty()) {
        juce::MemoryBlock mb;
        juce::File(loadPath).loadFileAsData(mb);
        plugin->setStateInformation(mb.getData(), (int)mb.getSize());
        std::cout << "state loaded (" << mb.getSize() << " bytes)\n";
    }

    // The editor lives in the plugin binary, so it is shown in a real window
    // (capture the screen externally, e.g. with xwd) rather than snapshotted here.
    std::unique_ptr<juce::DocumentWindow> window;
    if (showEditor) {
        window = std::make_unique<juce::DocumentWindow>("AI Contact", juce::Colours::black, 0);
        window->setUsingNativeTitleBar(false);
        window->setContentOwned(plugin->createEditorIfNeeded(), true);
        window->setTopLeftPosition(0, 0);
        window->setVisible(true);
    }

    juce::AudioBuffer<float> buffer(2, block);
    juce::MidiBuffer midi;
    const int total = (int)(std::atof(argv[2]) * sr);
    const int notes[] = {60, 64, 67, 72};
    double sum = 0;
    int counted = 0, step = 0;
    auto next = std::chrono::steady_clock::now();
    for (int pos = 0; pos < total; pos += block) {
        midi.clear();
        const int stepLen = (int)(0.25 * sr);
        if (pos / stepLen != step || pos == 0) {
            step = pos / stepLen;
            const int k = notes[step % 4];
            const int offset = (step * stepLen) - pos;
            const int o = juce::jlimit(0, block - 1, offset);
            midi.addEvent(juce::MidiMessage::noteOn(1, k, (juce::uint8)100), o);
            midi.addEvent(juce::MidiMessage::noteOn(2, k - 24, (juce::uint8)100), o);
            midi.addEvent(juce::MidiMessage::noteOff(1, notes[(step + 3) % 4]), o);
            midi.addEvent(juce::MidiMessage::noteOff(2, notes[(step + 3) % 4] - 24), o);
        }
        buffer.clear();
        plugin->processBlock(buffer, midi);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < block; ++i) sum += buffer.getSample(ch, i) * buffer.getSample(ch, i);
        counted += block;
        if (counted >= (int)sr) {
            std::cout << "t=" << (pos + block) / sr << "s rms=" << juce::Decibels::gainToDecibels(std::sqrt(sum / (2 * counted)), -140.0) << " dB\n" << std::flush;
            sum = 0;
            counted = 0;
        }
        next += std::chrono::microseconds((int64_t)(1e6 * block / sr));
        std::this_thread::sleep_until(next);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);  // lets the plugin's timer run
    }
    if (window) {
        plugin->editorBeingDeleted(plugin->getActiveEditor());
        window.reset();
    }
    if (savePath.isNotEmpty()) {
        juce::MemoryBlock mb;
        plugin->getStateInformation(mb);
        juce::File(savePath).replaceWithData(mb.getData(), mb.getSize());
        std::cout << "state saved (" << mb.getSize() << " bytes)\n";
    }
    std::cout << "first param now: " << plugin->getParameters()[0]->getName(64) << " = "
              << plugin->getParameters()[0]->getValue() << "\n";
    plugin->releaseResources();
    plugin.reset();
    return 0;
}
