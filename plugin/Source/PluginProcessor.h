#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <vector>

#include "ApiToken.h"
#include "aicontact/ControlServer.h"
#include "aicontact/Rack.h"

// AI Contact plugin: a container that hosts any number of ACI instruments
// (WebAssembly), routed by MIDI channel, deployable and hot-reloadable at
// runtime through the local control server (used by the aicontact CLI / Claude skill).
class AIContactProcessor : public juce::AudioProcessor, private juce::Timer {
public:
    // Host-automatable parameters: the first 8 params of the first 16 slots.
    static constexpr int kMacroSlots = 16;
    static constexpr int kMacrosPerSlot = 8;
    static constexpr int kNumMacros = kMacroSlots * kMacrosPerSlot;

    AIContactProcessor();
    ~AIContactProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "AI Contact"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    aicontact::Rack& rack() { return rack_; }
    int controlPort() const { return server_.port(); }
    int gatewayPort() const { return server_.gatewayPort(); }
    bool isGateway() const { return server_.isGateway(); }

private:
    void timerCallback() override;

    aicontact::Rack rack_;
    aicontact::ControlServer server_{rack_, [] { return ApiToken::get(); }};

    std::vector<juce::RangedAudioParameter*> macros_;
    std::array<float, kNumMacros> lastHostValue_{};   // audio thread
    std::array<float, kNumMacros> lastRackValue_{};   // message thread
    std::vector<aicontact::MidiEvent> events_;
    std::vector<aicontact::MacroChange> macroChanges_;
    std::vector<float> scratch_;
    uint64_t lastChangeCounter_ = ~0ull;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AIContactProcessor)
};
