#include "PluginProcessor.h"

#include "PluginEditor.h"

namespace {

// A float parameter whose display name follows whatever instrument currently
// occupies its slot ("supersaw: Cutoff"), falling back to "Slot 3 P2".
class MacroParameter : public juce::AudioParameterFloat {
public:
    MacroParameter(aicontact::Rack& rack, int slot, int param)
        : juce::AudioParameterFloat(juce::ParameterID("s" + juce::String(slot + 1) + "p" + juce::String(param + 1), 1),
                                    "Slot " + juce::String(slot + 1) + " P" + juce::String(param + 1),
                                    juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f),
          rack_(rack), slot_(slot), param_(param) {}

    juce::String getName(int maximumStringLength) const override {
        const auto current = rack_.paramLabel(slot_, param_);
        const juce::String text = current.empty() ? juce::AudioParameterFloat::getName(1024) : juce::String(current);
        return text.substring(0, maximumStringLength);
    }

private:
    aicontact::Rack& rack_;
    int slot_, param_;
};

}  // namespace

AIContactProcessor::AIContactProcessor()
    : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
    for (int s = 0; s < kMacroSlots; ++s)
        for (int p = 0; p < kMacrosPerSlot; ++p) {
            auto* param = new MacroParameter(rack_, s, p);
            macros_.push_back(param);
            addParameter(param);
        }
    lastHostValue_.fill(0.0f);
    lastRackValue_.fill(-1.0f);
    events_.reserve(4096);
    macroChanges_.reserve(kNumMacros);

    server_.start();
    startTimerHz(15);
}

AIContactProcessor::~AIContactProcessor() {
    stopTimer();
    server_.stop();
}

bool AIContactProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void AIContactProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    scratch_.assign((size_t)std::max(samplesPerBlock, 4096), 0.0f);
    rack_.prepare(sampleRate, samplesPerBlock);
}

void AIContactProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    const int frames = buffer.getNumSamples();

    events_.clear();
    for (const auto meta : midi) {
        const auto msg = meta.getMessage();
        if (msg.getRawDataSize() > 3 || msg.isSysEx() || events_.size() == events_.capacity()) continue;
        const auto* d = msg.getRawData();
        if (d[0] < 0x80 || d[0] >= 0xF0) continue;
        events_.push_back({meta.samplePosition, d[0], msg.getRawDataSize() > 1 ? d[1] : (uint8_t)0,
                           msg.getRawDataSize() > 2 ? d[2] : (uint8_t)0});
    }

    macroChanges_.clear();
    for (int i = 0; i < kNumMacros; ++i) {
        const float v = macros_[(size_t)i]->getValue();
        if (v != lastHostValue_[(size_t)i]) {
            lastHostValue_[(size_t)i] = v;
            macroChanges_.push_back({i / kMacrosPerSlot, i % kMacrosPerSlot, v});
        }
    }

    float* left = buffer.getWritePointer(0);
    float* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
    if (right) {
        rack_.process(left, right, frames, events_.data(), (int)events_.size(), macroChanges_.data(),
                      (int)macroChanges_.size());
    } else {
        // Mono output: render stereo into scratch and sum.
        if (scratch_.size() < (size_t)frames) scratch_.resize((size_t)frames);  // only if the host lied about block size
        float* scratch = scratch_.data();
        rack_.process(left, scratch, frames, events_.data(), (int)events_.size(), macroChanges_.data(),
                      (int)macroChanges_.size());
        for (int i = 0; i < frames; ++i) left[i] = 0.5f * (left[i] + scratch[i]);
    }
    for (int ch = 2; ch < buffer.getNumChannels(); ++ch) buffer.clear(ch, 0, frames);
}

void AIContactProcessor::timerCallback() {
    rack_.tick();

    // Values changed through the API / the editor -> reflect them in the host's parameters.
    for (int i = 0; i < kNumMacros; ++i) {
        const float v = rack_.paramNormalized(i / kMacrosPerSlot, i % kMacrosPerSlot);
        if (v < 0.0f || v == lastRackValue_[(size_t)i]) continue;
        lastRackValue_[(size_t)i] = v;
        auto* p = macros_[(size_t)i];
        if (std::abs(p->getValue() - v) > 1e-5f) p->setValueNotifyingHost(v);
    }

    const uint64_t counter = rack_.changeCounter();
    if (counter != lastChangeCounter_) {
        lastChangeCounter_ = counter;
        lastRackValue_.fill(-1.0f);  // slot layout may have changed: resync everything next tick
        updateHostDisplay(juce::AudioProcessorListener::ChangeDetails().withParameterInfoChanged(true));
    }
}

juce::AudioProcessorEditor* AIContactProcessor::createEditor() { return new AIContactEditor(*this); }

void AIContactProcessor::getStateInformation(juce::MemoryBlock& destData) {
    const std::string state = rack_.saveState();
    destData.replaceAll(state.data(), state.size());
}

void AIContactProcessor::setStateInformation(const void* data, int sizeInBytes) {
    try {
        rack_.loadState(std::string(static_cast<const char*>(data), (size_t)sizeInBytes));
    } catch (const std::exception& e) {
        rack_.log().log("error", "", std::string("could not restore plugin state: ") + e.what());
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new AIContactProcessor(); }
