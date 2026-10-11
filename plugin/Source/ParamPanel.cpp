#include "ParamPanel.h"

#include <cmath>

using json = nlohmann::json;

namespace {
const juce::Colour kPanel(0xff1f232b), kAccent(0xff4fc3f7), kText(0xffe6e6e6), kDim(0xff8a909c), kKnobBg(0xff2c313c);
constexpr int kControlW = 92, kControlH = 112, kHeaderH = 46;

class KnobLook : public juce::LookAndFeel_V4 {
public:
    void drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float start, float end,
                          juce::Slider&) override {
        auto bounds = juce::Rectangle<float>((float)x, (float)y, (float)w, (float)h).reduced(6.0f);
        const float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) / 2.0f;
        const auto c = bounds.getCentre();
        const float angle = start + pos * (end - start);
        juce::Path track, value;
        track.addCentredArc(c.x, c.y, radius, radius, 0.0f, start, end, true);
        value.addCentredArc(c.x, c.y, radius, radius, 0.0f, start, angle, true);
        g.setColour(kKnobBg);
        g.strokePath(track, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(kAccent);
        g.strokePath(value, juce::PathStrokeType(4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(kKnobBg.brighter(0.15f));
        g.fillEllipse(c.x - radius * 0.68f, c.y - radius * 0.68f, radius * 1.36f, radius * 1.36f);
        juce::Path pointer;
        pointer.addRoundedRectangle(-1.5f, -radius * 0.64f, 3.0f, radius * 0.36f, 1.5f);
        g.setColour(kText);
        g.fillPath(pointer, juce::AffineTransform::rotation(angle).translated(c.x, c.y));
    }
};

KnobLook& knobLook() {
    static KnobLook look;
    return look;
}

juce::String formatValue(double v, const std::string& unit) {
    juce::String s;
    const double a = std::fabs(v);
    if (unit == "Hz" && a >= 1000.0) return juce::String(v / 1000.0, a >= 10000.0 ? 1 : 2) + " kHz";
    if (unit == "s" && a < 1.0) return juce::String(v * 1000.0, a < 0.01 ? 1 : 0) + " ms";
    s = juce::String(v, a >= 100.0 ? 0 : a >= 10.0 ? 1 : 2);
    return unit.empty() ? s : s + " " + juce::String(unit);
}
}  // namespace

// One parameter: name + knob (or combo box for labelled steps).
class ParamPanel::Control : public juce::Component {
public:
    Control(ParamPanel& owner, const json& p) : owner_(owner) {
        id_ = p["id"].get<int>();
        name_.setText(p["name"].get<std::string>(), juce::dontSendNotification);
        name_.setJustificationType(juce::Justification::centred);
        name_.setColour(juce::Label::textColourId, kText);
        name_.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        addAndMakeVisible(name_);

        const double lo = p["min"].get<double>(), hi = p["max"].get<double>(), def = p["default"].get<double>();
        const int steps = p.value("steps", 0);
        const auto labels = p.value("labels", std::vector<std::string>());
        unit_ = p.value("unit", std::string());

        if (steps > 1 && (int)labels.size() == steps) {
            combo_ = std::make_unique<juce::ComboBox>();
            for (int i = 0; i < steps; ++i) combo_->addItem(labels[(size_t)i], i + 1);
            combo_->onChange = [this, lo, hi, steps] {
                const double v = lo + (hi - lo) * (combo_->getSelectedId() - 1) / (steps - 1);
                send(v);
            };
            addAndMakeVisible(*combo_);
            lo_ = lo; hi_ = hi; steps_ = steps;
        } else {
            slider_ = std::make_unique<juce::Slider>(juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
            slider_->setLookAndFeel(&knobLook());
            slider_->setRange(lo, hi, steps > 1 ? (hi - lo) / (steps - 1) : 0.0);
            if (p.value("scale", std::string()) == "log" && lo > 0.0)
                slider_->setSkewFactorFromMidPoint(std::sqrt(lo * hi));
            slider_->setDoubleClickReturnValue(true, def);  // double click = default
            slider_->setPopupDisplayEnabled(false, false, nullptr);
            slider_->onValueChange = [this] {
                value_.setText(formatValue(slider_->getValue(), unit_), juce::dontSendNotification);
                if (!updating_) send(slider_->getValue());
            };
            slider_->setTooltip(juce::String(p["name"].get<std::string>()) + "  (" + formatValue(lo, unit_) + " .. " +
                                formatValue(hi, unit_) + ", double-click resets)");
            addAndMakeVisible(*slider_);
            value_.setJustificationType(juce::Justification::centred);
            value_.setColour(juce::Label::textColourId, kDim);
            value_.setFont(juce::FontOptions(12.0f));
            addAndMakeVisible(value_);
        }
        setValue(p["value"].get<double>());
    }

    ~Control() override {
        if (slider_) slider_->setLookAndFeel(nullptr);
    }

    void setValue(double v) {
        if (slider_) {
            if (slider_->isMouseButtonDown()) return;  // don't fight the user
            updating_ = true;
            slider_->setValue(v, juce::sendNotificationSync);
            updating_ = false;
            value_.setText(formatValue(v, unit_), juce::dontSendNotification);
        } else if (combo_) {
            const int idx = (int)std::lround((v - lo_) / (hi_ - lo_) * (steps_ - 1));
            combo_->setSelectedId(idx + 1, juce::dontSendNotification);
        }
    }

    int id() const { return id_; }

    void resized() override {
        auto r = getLocalBounds().reduced(4);
        name_.setBounds(r.removeFromTop(18));
        if (slider_) {
            value_.setBounds(r.removeFromBottom(18));
            slider_->setBounds(r);
        } else if (combo_) {
            combo_->setBounds(r.withSizeKeepingCentre(r.getWidth(), 26));
        }
    }

private:
    void send(double v) {
        owner_.rpc_("slot.set_params", {{"slot_id", owner_.slotId_}, {"params", {{std::to_string(id_), v}}}});
    }

    ParamPanel& owner_;
    int id_ = 0;
    juce::Label name_, value_;
    std::unique_ptr<juce::Slider> slider_;
    std::unique_ptr<juce::ComboBox> combo_;
    std::string unit_;
    double lo_ = 0, hi_ = 1;
    int steps_ = 0;
    bool updating_ = false;
};

ParamPanel::ParamPanel(Rpc rpc, ModuleFn module, CustomUi::LogFn log)
    : rpc_(std::move(rpc)), module_(std::move(module)), log_(std::move(log)) {
    close_.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible(close_);
    viewButton_.setTooltip("Switch between the instrument's own GUI and the generated controls");
    viewButton_.onClick = [this] {
        preferDefault_ = !preferDefault_;
        updateView();
        if (auto* parent = getParentComponent()) parent->resized();  // height may change
    };
    addChildComponent(viewButton_);
}

ParamPanel::~ParamPanel() = default;

void ParamPanel::setSlot(const std::string& slotId) {
    if (slotId == slotId_) return;
    slotId_ = slotId;
    builtHash_.clear();
    controls_.clear();
    custom_.reset();
    preferDefault_ = false;
    refresh();
}

void ParamPanel::refresh() {
    if (slotId_.empty()) return;
    json r = rpc_("slot.info", {{"slot_id", slotId_}});
    if (!r.contains("result")) {
        slotId_.clear();
        if (onClose) onClose();
        return;
    }
    const json& info = r["result"];
    const std::string hash = info.contains("instrument") ? info["instrument"].value("hash", std::string()) : "";
    if (hash != builtHash_) {
        builtHash_ = hash;
        rebuild(info);  // new or hot-reloaded instrument: params may have changed
    }
    std::vector<float> values;
    for (const auto& p : info.value("params", json::array())) {
        values.push_back(p["value"].get<float>());
        for (auto& c : controls_)
            if (c->id() == p["id"].get<int>()) c->setValue(p["value"].get<double>());
    }
    if (custom_) custom_->setValues(values);
}

void ParamPanel::rebuild(const json& info) {
    controls_.clear();
    const auto inst = info.value("instrument", json::object());
    title_ = juce::String(info.value("name", std::string())) + "  —  " + juce::String(info.value("id", std::string()));
    subtitle_ = juce::String(inst.value("name", std::string())) + " " + juce::String(inst.value("version", std::string()));
    if (!inst.value("author", std::string()).empty()) subtitle_ << "  by " << juce::String(inst.value("author", std::string()));
    if (!inst.value("description", std::string()).empty()) subtitle_ << "  ·  " << juce::String(inst.value("description", std::string()));
    for (const auto& p : info.value("params", json::array())) {
        controls_.push_back(std::make_unique<Control>(*this, p));
        addChildComponent(*controls_.back());
    }
    custom_.reset();
    custom_ = CustomUi::create(module_(slotId_), slotId_, rpc_, log_, customNote_);
    if (custom_) addChildComponent(*custom_);
    updateView();
}

void ParamPanel::updateView() {
    const bool custom = showingCustom();
    if (custom_) custom_->setVisible(custom);
    for (auto& c : controls_) c->setVisible(!custom);
    viewButton_.setVisible(custom_ != nullptr);
    viewButton_.setButtonText(custom ? "Default UI" : "Custom UI");
    resized();
    repaint();
}

int ParamPanel::preferredHeight(int width) const {
    if (showingCustom()) {
        const auto design = custom_->designBounds();
        const double scale = custom_->scalesToFit() ? juce::jmin(1.0, (width - 20) / (double)design.getWidth()) : 1.0;
        return kHeaderH + (int)std::ceil(design.getHeight() * scale) + 10;
    }
    const int perRow = juce::jmax(1, (width - 20) / kControlW);
    const int rows = controls_.empty() ? 1 : ((int)controls_.size() + perRow - 1) / perRow;
    return kHeaderH + rows * kControlH + 10;
}

void ParamPanel::paint(juce::Graphics& g) {
    g.setColour(kPanel);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);
    g.setColour(kAccent.withAlpha(0.6f));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 8.0f, 1.0f);
    auto header = getLocalBounds().reduced(12, 6).removeFromTop(kHeaderH - 8);
    g.setColour(kText);
    g.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    g.drawText(title_, header.removeFromTop(20), juce::Justification::left);
    g.setColour(kDim);
    g.setFont(juce::FontOptions(12.0f));
    juce::String sub = subtitle_;
    if (customNote_.isNotEmpty()) sub << "  [" << customNote_ << "]";
    g.drawText(sub, header.withTrimmedRight(custom_ ? 160 : 70), juce::Justification::left, true);
    if (controls_.empty() && !showingCustom()) {
        g.drawText("This instrument has no parameters.", getLocalBounds().withTrimmedTop(kHeaderH),
                   juce::Justification::centred);
    }
}

void ParamPanel::resized() {
    close_.setBounds(getWidth() - 70, 10, 60, 24);
    viewButton_.setBounds(getWidth() - 160, 10, 84, 24);
    if (showingCustom()) {
        auto area = getLocalBounds().withTrimmedTop(kHeaderH).reduced(10, 0).withTrimmedBottom(10);
        if (!custom_->scalesToFit()) {  // a web page gets its design size (or what fits), centred
            const auto design = custom_->designBounds();
            area = area.withSizeKeepingCentre(juce::jmin(area.getWidth(), design.getWidth()), area.getHeight())
                       .withHeight(juce::jmin(area.getHeight(), design.getHeight()));
        }
        custom_->setBounds(area);
        return;
    }
    const int perRow = juce::jmax(1, (getWidth() - 20) / kControlW);
    for (size_t i = 0; i < controls_.size(); ++i) {
        const int row = (int)i / perRow, col = (int)i % perRow;
        controls_[i]->setBounds(10 + col * kControlW, kHeaderH + row * kControlH, kControlW, kControlH);
    }
}
