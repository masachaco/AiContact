#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <vector>

#include "ParamPanel.h"
#include "PluginProcessor.h"

class AIContactEditor : public juce::AudioProcessorEditor,
                      public juce::FileDragAndDropTarget,
                      private juce::Timer {
public:
    explicit AIContactEditor(AIContactProcessor&);
    ~AIContactEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    void fileDragEnter(const juce::StringArray&, int, int) override { dragging_ = true; repaint(); }
    void fileDragExit(const juce::StringArray&) override { dragging_ = false; repaint(); }

    class SlotRow;

private:
    void timerCallback() override;
    void refresh(bool rebuild);
    void deployFile(const juce::File& file, const std::string& slotId = {});
    void locateSlot(const std::string& slotId);  // pick the file for a slot whose instrument could not be loaded
    void selectSlot(const std::string& id);  // toggles the parameter panel
    void showTokenMenu();
    nlohmann::json rpc(const std::string& method, const nlohmann::json& params = nlohmann::json::object());

    AIContactProcessor& processor_;
    juce::Label title_, nameLabel_, statusLabel_;
    juce::TextButton loadButton_{"Load .wasm..."}, panicButton_{"Panic"}, tokenButton_{"API Token"};
    juce::Viewport viewport_;
    juce::Component rowsHolder_;
    std::vector<std::unique_ptr<SlotRow>> rows_;
    juce::TextEditor logView_;
    std::unique_ptr<ParamPanel> panel_;
    juce::TooltipWindow tooltips_{this, 600};
    std::unique_ptr<juce::FileChooser> chooser_;
    uint64_t lastLogSeq_ = 0;
    bool dragging_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AIContactEditor)
};
