#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "aicontact/Instrument.h"

// An instrument's own GUI (ACI spec §10), shown by ParamPanel instead of the
// generated knobs: a native "skin" drawn from aci.ui/ui.json, or an HTML page
// in a web view (aci.ui/index.html).
class CustomUi : public juce::Component {
public:
    using Rpc = std::function<nlohmann::json(const std::string&, const nlohmann::json&)>;
    using LogFn = std::function<void(const std::string& level, const std::string& message)>;

    // Picks the best UI the module ships that this build can show. Returns null
    // (and the reason in `whyNot`, empty if the module has no UI) otherwise.
    static std::unique_ptr<CustomUi> create(std::shared_ptr<const aicontact::InstrumentModule> module,
                                            const std::string& slotId, Rpc rpc, LogFn log,
                                            juce::String& whyNot);
    static bool webViewAvailable();

    // Current plain param values by index (polled from the rack), to follow
    // changes made elsewhere (API, automation, the other view).
    virtual void setValues(const std::vector<float>& values) = 0;
    // Size the page/skin was designed for.
    virtual juce::Rectangle<int> designBounds() const = 0;
    virtual bool scalesToFit() const = 0;  // skins scale, web pages lay themselves out
    virtual juce::String kind() const = 0;
};
