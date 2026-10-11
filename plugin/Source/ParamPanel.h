#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "CustomUi.h"

// Per-instrument control panel. Shows the instrument's own GUI when it ships one
// (CustomUi), otherwise one generated from its ACI metadata: a knob per
// continuous param (log scale and units honoured), a combo box for stepped
// params with labels. A header button switches between the two.
class ParamPanel : public juce::Component {
public:
    using Rpc = CustomUi::Rpc;
    using ModuleFn = std::function<std::shared_ptr<const aicontact::InstrumentModule>(const std::string&)>;

    ParamPanel(Rpc rpc, ModuleFn module, CustomUi::LogFn log);
    ~ParamPanel() override;

    // Shows `slotId` (empty = hide). Call refresh() periodically to follow
    // value changes made elsewhere (API, automation, hot reload).
    void setSlot(const std::string& slotId);
    const std::string& slotId() const { return slotId_; }
    void refresh();
    int preferredHeight(int width) const;

    std::function<void()> onClose;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    class Control;
    void rebuild(const nlohmann::json& info);
    bool showingCustom() const { return custom_ != nullptr && !preferDefault_; }
    void updateView();

    Rpc rpc_;
    ModuleFn module_;
    CustomUi::LogFn log_;
    std::string slotId_, builtHash_;
    juce::String title_, subtitle_, customNote_;
    juce::TextButton close_{"Close"}, viewButton_;
    std::vector<std::unique_ptr<Control>> controls_;
    std::unique_ptr<CustomUi> custom_;
    bool preferDefault_ = false;  // user picked the generated knobs over the instrument's GUI
};
