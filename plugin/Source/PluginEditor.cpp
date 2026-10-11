#include "PluginEditor.h"

#include "ApiToken.h"
#include "aicontact/Auth.h"

using json = nlohmann::json;

namespace {
const juce::Colour kBg(0xff16181d), kPanel(0xff1f232b), kAccent(0xff4fc3f7), kText(0xffe6e6e6),
    kDim(0xff8a909c), kRed(0xffef5350), kGreen(0xff66bb6a), kYellow(0xffffca28);
}

// One rack slot: name, channel, key range, mute/solo/remove, meter.
class AIContactEditor::SlotRow : public juce::Component {
public:
    SlotRow(AIContactEditor& owner, const json& info) : owner_(owner), id_(info["id"].get<std::string>()) {
        addAndMakeVisible(channel_);
        channel_.addItem("Omni", 1);
        for (int c = 1; c <= 16; ++c) channel_.addItem("Ch " + juce::String(c), c + 1);
        channel_.onChange = [this] {
            owner_.rpc("slot.update", {{"slot_id", id_}, {"channel", channel_.getSelectedId() - 1}});
        };
        for (auto* b : {&mute_, &solo_, &remove_}) addAndMakeVisible(*b);
        mute_.setClickingTogglesState(true);
        solo_.setClickingTogglesState(true);
        mute_.setColour(juce::TextButton::buttonOnColourId, kYellow.darker(0.3f));
        solo_.setColour(juce::TextButton::buttonOnColourId, kGreen.darker(0.3f));
        mute_.onClick = [this] { owner_.rpc("slot.update", {{"slot_id", id_}, {"mute", mute_.getToggleState()}}); };
        solo_.onClick = [this] { owner_.rpc("slot.update", {{"slot_id", id_}, {"solo", solo_.getToggleState()}}); };
        remove_.onClick = [this] { owner_.rpc("slot.remove", {{"slot_id", id_}}); };
        remove_.setTooltip("Remove from rack");
        locate_.onClick = [this] { owner_.locateSlot(id_); };
        locate_.setTooltip("The instrument file is missing, changed or broken: choose the file to use "
                           "(choosing a changed file accepts its new content)");
        addChildComponent(locate_);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        update(info);
    }

    void update(const json& info) {
        info_ = info;
        channel_.setSelectedId(info["channel"].get<int>() + 1, juce::dontSendNotification);
        mute_.setToggleState(info["mute"].get<bool>(), juce::dontSendNotification);
        solo_.setToggleState(info["solo"].get<bool>(), juce::dontSendNotification);
        const bool broken = info.value("status", std::string()) == "error";
        if (locate_.isVisible() != broken) {
            locate_.setVisible(broken);
            resized();
        }
        repaint();
    }

    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().reduced(2);
        g.setColour(kPanel);
        g.fillRoundedRectangle(r.toFloat(), 6.0f);
        if (selected_) {
            g.setColour(kAccent);
            g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 6.0f, 1.5f);
        }

        const std::string status = info_.value("status", "ok");
        const juce::Colour statusColour = status == "ok" ? kGreen : kRed;
        g.setColour(statusColour);
        g.fillEllipse(10.0f, r.getCentreY() - 4.0f, 8.0f, 8.0f);

        auto text = r.withTrimmedLeft(26).withWidth(textWidth_);
        g.setColour(kText);
        g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
        g.drawText(juce::String(info_["index"].get<int>() + 1) + ". " + juce::String(info_["name"].get<std::string>()),
                   text.removeFromTop(text.getHeight() / 2), juce::Justification::bottomLeft);
        g.setColour(status == "ok" ? kDim : kRed);
        g.setFont(juce::FontOptions(12.0f));
        juce::String sub = juce::String(id_);
        if (info_.contains("instrument")) {
            const auto version = info_["instrument"].value("version", std::string());
            if (!version.empty()) sub << "  v" << juce::String(version);
        }
        sub << "  keys " << info_["key_low"].get<int>() << "-" << info_["key_high"].get<int>();
        sub << "  cpu " << juce::String(info_["cpu"].get<double>() * 100.0, 1) << "%";
        if (info_.contains("error")) sub = juce::String(info_["error"].get<std::string>());
        g.drawText(sub, text, juce::Justification::topLeft, true);

        // peak meter
        const double peakDb = info_["peak_db"].get<double>();
        const float level = (float)juce::jlimit(0.0, 1.0, (peakDb + 60.0) / 60.0);
        auto meter = r.removeFromRight(8).reduced(1, 6);
        g.setColour(kBg);
        g.fillRect(meter);
        g.setColour(peakDb > -1.0 ? kRed : kAccent);
        g.fillRect(meter.withTop(meter.getBottom() - (int)(meter.getHeight() * level)));
    }

    void resized() override {
        auto r = getLocalBounds().reduced(8, 10);
        r.removeFromRight(10);
        remove_.setBounds(r.removeFromRight(28));
        r.removeFromRight(4);
        solo_.setBounds(r.removeFromRight(28));
        r.removeFromRight(4);
        mute_.setBounds(r.removeFromRight(28));
        r.removeFromRight(8);
        channel_.setBounds(r.removeFromRight(80));
        if (locate_.isVisible()) {
            r.removeFromRight(6);
            locate_.setBounds(r.removeFromRight(70));
        }
        textWidth_ = juce::jmax(50, r.getWidth() - 30);
    }

    const std::string& id() const { return id_; }
    void setSelected(bool selected) {
        if (selected_ != selected) { selected_ = selected; repaint(); }
    }
    void mouseUp(const juce::MouseEvent& e) override {
        if (!e.mouseWasDraggedSinceMouseDown()) owner_.selectSlot(id_);
    }

private:
    bool selected_ = false;
    AIContactEditor& owner_;
    std::string id_;
    json info_;
    juce::ComboBox channel_;
    juce::TextButton mute_{"M"}, solo_{"S"}, remove_{"X"}, locate_{"Locate..."};
    int textWidth_ = 200;
};

// ----------------------------------------------------------------------------

AIContactEditor::AIContactEditor(AIContactProcessor& p) : juce::AudioProcessorEditor(&p), processor_(p) {
    title_.setText("AI Contact", juce::dontSendNotification);
    title_.setFont(juce::FontOptions(22.0f, juce::Font::bold));
    title_.setColour(juce::Label::textColourId, kAccent);
    addAndMakeVisible(title_);

    nameLabel_.setEditable(true);
    nameLabel_.setColour(juce::Label::textColourId, kText);
    nameLabel_.setTooltip("Rack name (used by the API/CLI to pick a rack). Click to rename.");
    nameLabel_.onTextChange = [this] { rpc("rack.set_name", {{"name", nameLabel_.getText().toStdString()}}); };
    addAndMakeVisible(nameLabel_);

    statusLabel_.setColour(juce::Label::textColourId, kDim);
    statusLabel_.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(statusLabel_);

    loadButton_.onClick = [this] {
        chooser_ = std::make_unique<juce::FileChooser>("Load ACI instrument", juce::File(), "*.wasm");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this](const juce::FileChooser& fc) {
                                  if (fc.getResult().existsAsFile()) deployFile(fc.getResult());
                              });
    };
    panicButton_.onClick = [this] { rpc("rack.panic"); };
    tokenButton_.onClick = [this] { showTokenMenu(); };
    tokenButton_.setTooltip("Copy or regenerate the token that API clients (the Claude skill, curl) must send");
    addAndMakeVisible(tokenButton_);
    addAndMakeVisible(loadButton_);
    addAndMakeVisible(panicButton_);

    viewport_.setViewedComponent(&rowsHolder_, false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport_);

    logView_.setMultiLine(true);
    logView_.setReadOnly(true);
    logView_.setScrollbarsShown(true);
    logView_.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
    logView_.setColour(juce::TextEditor::backgroundColourId, kBg.darker(0.3f));
    logView_.setColour(juce::TextEditor::textColourId, kDim);
    logView_.setColour(juce::TextEditor::outlineColourId, kPanel);
    addAndMakeVisible(logView_);

    panel_ = std::make_unique<ParamPanel>(
        [this](const std::string& m, const json& p) { return rpc(m, p); },
        [this](const std::string& slotId) { return processor_.rack().slotModule(slotId); },
        [this](const std::string& level, const std::string& message) {
            processor_.rack().log().log(level, panel_ ? panel_->slotId() : std::string(), message);
        });
    panel_->onClose = [this] { selectSlot(panel_->slotId()); };
    addChildComponent(*panel_);

    setResizable(true, true);
    setResizeLimits(560, 400, 1600, 1400);
    setSize(760, 640);
    refresh(true);
    startTimerHz(12);
}

AIContactEditor::~AIContactEditor() { stopTimer(); }

json AIContactEditor::rpc(const std::string& method, const json& params) {
    json r = processor_.rack().handleRpc({{"method", method}, {"params", params}});
    if (r.contains("error")) processor_.rack().log().log("error", "", method + ": " + r["error"].get<std::string>());
    return r;
}

void AIContactEditor::paint(juce::Graphics& g) {
    g.fillAll(kBg);
    if (rows_.empty()) {
        g.setColour(kDim);
        g.setFont(juce::FontOptions(14.0f));
        g.drawFittedText("No instruments yet.\nDrop an ACI .wasm file here, or deploy one through the API (Claude skill \"aicontact\").",
                         viewport_.getBounds(), juce::Justification::centred, 3);
    }
    if (dragging_) {
        g.setColour(kAccent.withAlpha(0.25f));
        g.fillRect(getLocalBounds());
        g.setColour(kAccent);
        g.drawRect(getLocalBounds(), 3);
    }
}

void AIContactEditor::resized() {
    auto r = getLocalBounds().reduced(10);
    auto top = r.removeFromTop(32);
    title_.setBounds(top.removeFromLeft(110));
    panicButton_.setBounds(top.removeFromRight(60).reduced(2));
    loadButton_.setBounds(top.removeFromRight(110).reduced(2));
    tokenButton_.setBounds(top.removeFromRight(90).reduced(2));
    nameLabel_.setBounds(top.removeFromLeft(150));
    statusLabel_.setBounds(top);
    r.removeFromTop(6);
    logView_.setBounds(r.removeFromBottom(juce::jmax(70, r.getHeight() / 6)));
    r.removeFromBottom(6);
    if (panel_ && panel_->isVisible()) {
        const int h = juce::jmin(panel_->preferredHeight(r.getWidth()), r.getHeight() - 120);
        panel_->setBounds(r.removeFromBottom(h));
        r.removeFromBottom(6);
    }
    viewport_.setBounds(r);
    const int rowH = 56;
    rowsHolder_.setBounds(0, 0, viewport_.getMaximumVisibleWidth(), (int)rows_.size() * rowH);
    for (size_t i = 0; i < rows_.size(); ++i)
        rows_[i]->setBounds(0, (int)i * rowH, rowsHolder_.getWidth(), rowH);
}

void AIContactEditor::refresh(bool rebuild) {
    json info = rpc("rack.info");
    if (!info.contains("result")) return;
    const json& rack = info["result"];

    if (!nameLabel_.isBeingEdited()) nameLabel_.setText(rack["name"].get<std::string>(), juce::dontSendNotification);
    juce::String status;
    status << "API :" << (processor_.controlPort() ? juce::String(processor_.controlPort()) : juce::String("off"));
    if (processor_.isGateway()) status << " +" << processor_.gatewayPort();
    status << juce::String::fromUTF8(" \xC2\xB7 ") << (int)rack["sample_rate"].get<double>() << " Hz"
           << juce::String::fromUTF8(" \xC2\xB7 ") << (rack["audio_running"].get<bool>() ? "audio on" : "audio idle");
    statusLabel_.setTooltip("Control API on 127.0.0.1 (token required, see API Token). Clients reach every open "
                            "rack through the rack directory on port " + juce::String(processor_.gatewayPort()));
    statusLabel_.setText(status, juce::dontSendNotification);

    const json& slots = rack["slots"];
    bool sameLayout = !rebuild && slots.size() == rows_.size();
    for (size_t i = 0; sameLayout && i < rows_.size(); ++i)
        sameLayout = rows_[i]->id() == slots[i]["id"].get<std::string>();
    if (sameLayout) {
        for (size_t i = 0; i < rows_.size(); ++i) rows_[i]->update(slots[i]);
    } else {
        rows_.clear();
        for (const auto& s : slots) {
            rows_.push_back(std::make_unique<SlotRow>(*this, s));
            rowsHolder_.addAndMakeVisible(*rows_.back());
        }
        resized();
        repaint();
    }
    for (auto& row : rows_) row->setSelected(panel_->isVisible() && row->id() == panel_->slotId());
    if (panel_->isVisible()) {
        const int before = panel_->preferredHeight(panel_->getWidth());
        panel_->refresh();  // follows API changes, automation and hot reloads
        if (panel_->slotId().empty()) panel_->setVisible(false);
        if (!panel_->isVisible() || panel_->preferredHeight(panel_->getWidth()) != before) resized();
    }

    for (const auto& l : processor_.rack().log().since(lastLogSeq_)) {
        lastLogSeq_ = l.seq;
        juce::String line;
        line << "[" << l.level << "] " << (l.slot.empty() ? juce::String() : juce::String(l.slot) + ": ") << l.message << "\n";
        logView_.moveCaretToEnd();
        logView_.insertTextAtCaret(line);
    }
}

void AIContactEditor::timerCallback() { refresh(false); }

void AIContactEditor::showTokenMenu() {
    std::string token;
    try {
        token = ApiToken::get();
    } catch (const std::exception& e) {
        processor_.rack().log().log("error", "", std::string("API token: ") + e.what());
        return;
    }
    juce::PopupMenu menu;
    menu.addSectionHeader("API token  " + juce::String::fromUTF8(aicontact::maskToken(token).c_str()));
    menu.addItem(1, "Copy token");
    menu.addItem(2, "Regenerate token...");
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&tokenButton_), [this, token](int choice) {
        if (choice == 1) {
            juce::SystemClipboard::copyTextToClipboard(token);
            processor_.rack().log().log("info", "", "API token copied to the clipboard; give it to the client "
                                                    "(e.g. AICONTACT_TOKEN for the Claude skill)");
        } else if (choice == 2) {
            auto options = juce::MessageBoxOptions()
                               .withIconType(juce::MessageBoxIconType::WarningIcon)
                               .withTitle("Regenerate API token?")
                               .withMessage("Every client using the current token (the Claude skill, scripts, other "
                                            "tools) will be rejected until it is given the new one.")
                               .withButton("Regenerate")
                               .withButton("Cancel")
                               .withAssociatedComponent(this);
            juce::AlertWindow::showAsync(options, [this](int result) {
                if (result != 1) return;
                try {
                    const std::string fresh = ApiToken::rotate();
                    processor_.rack().log().log("info", "", "API token regenerated (" + aicontact::maskToken(fresh) +
                                                                "); the old token no longer works");
                } catch (const std::exception& e) {
                    processor_.rack().log().log("error", "", std::string("could not regenerate token: ") + e.what());
                }
            });
        }
    });
}

void AIContactEditor::selectSlot(const std::string& id) {
    const bool close = panel_->isVisible() && panel_->slotId() == id;
    panel_->setVisible(!close);
    panel_->setSlot(close ? std::string() : id);
    for (auto& row : rows_) row->setSelected(!close && row->id() == id);
    resized();
    repaint();
}

bool AIContactEditor::isInterestedInFileDrag(const juce::StringArray& files) {
    for (const auto& f : files)
        if (f.endsWithIgnoreCase(".wasm")) return true;
    return false;
}

void AIContactEditor::filesDropped(const juce::StringArray& files, int, int) {
    dragging_ = false;
    for (const auto& f : files)
        if (f.endsWithIgnoreCase(".wasm")) deployFile(juce::File(f));
    repaint();
}

void AIContactEditor::deployFile(const juce::File& file, const std::string& slotId) {
    // The slot refers to the file: the project stores its path and reads it again when opened.
    json p = {{"path", file.getFullPathName().toStdString()}};
    if (!slotId.empty()) p["slot_id"] = slotId;
    rpc("slot.deploy", p);
    refresh(true);
}

void AIContactEditor::locateSlot(const std::string& slotId) {
    chooser_ = std::make_unique<juce::FileChooser>("Locate the instrument for " + juce::String(slotId), juce::File(), "*.wasm");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this, slotId](const juce::FileChooser& fc) {
                              if (fc.getResult().existsAsFile()) deployFile(fc.getResult(), slotId);
                          });
}
