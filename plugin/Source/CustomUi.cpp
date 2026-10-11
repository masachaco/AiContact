#include "CustomUi.h"

#include <cmath>
#include <map>

#if AICONTACT_WEBVIEW
 #include <juce_gui_extra/juce_gui_extra.h>
 #include "AIContactWebData.h"
#endif

using json = nlohmann::json;
using aicontact::InstrumentModule;
using aicontact::ParamInfo;

namespace {

juce::String formatParam(const ParamInfo& p, double v) {
    if (p.steps > 1 && (int)p.labels.size() == p.steps) {
        const int i = (int)std::lround((v - p.min) / (p.max - p.min) * (p.steps - 1));
        return juce::String(p.labels[(size_t)juce::jlimit(0, p.steps - 1, i)]);
    }
    const double a = std::fabs(v);
    if (p.unit == "Hz" && a >= 1000.0) return juce::String(v / 1000.0, a >= 10000.0 ? 1 : 2) + " kHz";
    if (p.unit == "s" && a < 1.0) return juce::String(v * 1000.0, a < 0.01 ? 1 : 0) + " ms";
    const juce::String s(v, a >= 100.0 ? 0 : a >= 10.0 ? 1 : 2);
    return p.unit.empty() ? s : s + " " + juce::String(p.unit);
}

bool looksLikeSvg(const uint8_t* d, size_t n) {
    const std::string head((const char*)d, std::min<size_t>(n, 512));
    return head.find("<svg") != std::string::npos || head.find("<?xml") != std::string::npos;
}

// "#rgb", "#rrggbb" or "#rrggbbaa"
juce::Colour parseColour(const json& j, const char* key, juce::Colour fallback) {
    if (!j.contains(key) || !j[key].is_string()) return fallback;
    std::string s = j[key].get<std::string>();
    if (s.empty() || s[0] != '#') return fallback;
    s = s.substr(1);
    if (s.size() == 3) s = {s[0], s[0], s[1], s[1], s[2], s[2]};
    if (s.size() == 6) s += "ff";
    if (s.size() != 8) return fallback;
    const auto v = (uint32_t)std::stoul(s, nullptr, 16);
    return juce::Colour((uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v);
}

// ============================================================================
// Native skin (aci.ui/ui.json)

struct Asset {
    juce::Image image;                    // raster images (needed for filmstrips)
    std::unique_ptr<juce::Drawable> svg;  // vector images
    bool valid() const { return image.isValid() || svg != nullptr; }
    // The SVG's viewBox (not the bounds of what it draws), so margins and rotation centres are kept.
    juce::Rectangle<float> bounds() const {
        if (!svg) return image.getBounds().toFloat();
        if (auto* c = dynamic_cast<juce::DrawableComposite*>(svg.get()))
            if (!c->getContentArea().isEmpty()) return c->getContentArea();
        return svg->getDrawableBounds();
    }
};

class SkinView;

// One entry of "elements". Static ones (image, label) ignore the mouse.
class Element : public juce::Component, public juce::SettableTooltipClient {
public:
    Element(SkinView& owner, const json& el, int paramIndex);
    const int param;  // -1 for static elements
    virtual void updateValue() { repaint(); }

protected:
    float value() const;
    float normalized() const;
    const ParamInfo& info() const;
    void setNormalized(float n);
    void setValue(float v);
    const Asset* asset(const json& el, const char* key) const;
    juce::Font font(float size, bool bold) const;

    SkinView& owner_;
    juce::Colour colour_;
};

class SkinView : public CustomUi {
public:
    SkinView(std::shared_ptr<const InstrumentModule> module, const json& skin, std::function<void(int, float)> send)
        : module_(std::move(module)), send_(std::move(send)) {
        const auto& params = module_->meta().params;
        values_.resize(params.size());
        for (size_t i = 0; i < params.size(); ++i) values_[i] = params[i].def;

        const auto& size = skin.at("size");
        design_ = {0, 0, size.at(0).get<int>(), size.at(1).get<int>()};
        text_ = parseColour(skin, "color", juce::Colour(0xffe6e6e6));
        accent_ = parseColour(skin, "accent", juce::Colour(0xff4fc3f7));
        background_ = parseColour(skin, "background", juce::Colour(0xff1f232b));
        if (skin.contains("background") && skin["background"].is_string() &&
            skin["background"].get<std::string>().rfind('#', 0) != 0)
            backgroundImage_ = skin["background"].get<std::string>();
        if (skin.contains("font") && skin["font"].is_string())
            if (const auto* f = module_->ui().find(skin["font"].get<std::string>()))
                typeface_ = juce::Typeface::createSystemTypefaceFor(module_->uiData(*f), f->size);

        addAndMakeVisible(content_);
        content_.setBounds(design_);
        content_.paintBackground = [this](juce::Graphics& g) {
            g.fillAll(background_);
            if (const Asset* a = asset(backgroundImage_)) draw(g, *a, design_.toFloat());
        };
        for (const auto& el : skin.at("elements")) addElement(el);
    }

    // ---- CustomUi ----
    void setValues(const std::vector<float>& values) override {
        for (size_t i = 0; i < values.size() && i < values_.size(); ++i) {
            if (juce::exactlyEqual(values[i], values_[i])) continue;
            bool dragging = false;
            for (auto& e : elements_)
                if (e->param == (int)i && e->isMouseButtonDown()) dragging = true;
            if (dragging) continue;  // don't fight the user
            values_[i] = values[i];
            refreshParam((int)i);
        }
    }
    juce::Rectangle<int> designBounds() const override { return design_; }
    bool scalesToFit() const override { return true; }
    juce::String kind() const override { return "skin"; }

    void resized() override {
        const float s = juce::jmin(2.0f, juce::jmin((float)getWidth() / (float)design_.getWidth(),
                                                    (float)getHeight() / (float)design_.getHeight()));
        const float ox = ((float)getWidth() - design_.getWidth() * s) / 2.0f;
        const float oy = ((float)getHeight() - design_.getHeight() * s) / 2.0f;
        content_.setTransform(juce::AffineTransform::scale(s).translated(ox, oy));
    }

    // ---- for elements ----
    const InstrumentModule& module() const { return *module_; }
    float value(int index) const { return values_[(size_t)index]; }
    void set(int index, float v) {
        v = module_->meta().params[(size_t)index].clamp(v);
        if (juce::exactlyEqual(v, values_[(size_t)index])) return;
        values_[(size_t)index] = v;
        send_(index, v);
        refreshParam(index);
    }
    juce::Colour text() const { return text_; }
    juce::Colour accent() const { return accent_; }
    juce::Typeface::Ptr typeface() const { return typeface_; }

    const Asset* asset(const std::string& path) {
        if (path.empty()) return nullptr;
        auto it = assets_.find(path);
        if (it == assets_.end()) {
            Asset a;
            if (const auto* f = module_->ui().find(path)) {
                const uint8_t* d = module_->uiData(*f);
                if (looksLikeSvg(d, f->size)) a.svg = juce::Drawable::createFromImageData(d, f->size);
                else a.image = juce::ImageFileFormat::loadFrom(d, f->size);
            }
            it = assets_.emplace(path, std::move(a)).first;
        }
        return it->second.valid() ? &it->second : nullptr;
    }

    static void draw(juce::Graphics& g, const Asset& a, juce::Rectangle<float> r, float angle = 0.0f) {
        const auto rot = juce::AffineTransform::rotation(angle, r.getCentreX(), r.getCentreY());
        if (a.svg) {
            const auto fit = juce::RectanglePlacement(juce::RectanglePlacement::stretchToFit)
                                 .getTransformToFit(a.bounds(), r);
            a.svg->draw(g, 1.0f, fit.followedBy(rot));
        } else {
            const auto fit = juce::AffineTransform::scale(r.getWidth() / (float)a.image.getWidth(),
                                                          r.getHeight() / (float)a.image.getHeight())
                                 .translated(r.getX(), r.getY());
            g.drawImageTransformed(a.image, fit.followedBy(rot));
        }
    }

private:
    struct Content : juce::Component {
        std::function<void(juce::Graphics&)> paintBackground;
        void paint(juce::Graphics& g) override { paintBackground(g); }
    };

    void refreshParam(int index) {
        for (auto& e : elements_)
            if (e->param == index) e->updateValue();
    }

    void addElement(const json& el);

    std::shared_ptr<const InstrumentModule> module_;
    std::function<void(int, float)> send_;
    std::vector<float> values_;
    juce::Rectangle<int> design_;
    juce::Colour text_, accent_, background_;
    std::string backgroundImage_;
    juce::Typeface::Ptr typeface_;
    std::map<std::string, Asset> assets_;
    Content content_;
    std::vector<std::unique_ptr<Element>> elements_;
};

Element::Element(SkinView& owner, const json& el, int paramIndex) : param(paramIndex), owner_(owner) {
    colour_ = parseColour(el, "color", param >= 0 ? owner.accent() : owner.text());
    setBounds(juce::Rectangle<float>(el.at("x").get<float>(), el.at("y").get<float>(), el.at("w").get<float>(),
                                     el.at("h").get<float>()).toNearestInt());
    if (param < 0) setInterceptsMouseClicks(false, false);
}

float Element::value() const { return owner_.value(param); }
const ParamInfo& Element::info() const { return owner_.module().meta().params[(size_t)param]; }
float Element::normalized() const { return info().toNormalized(value()); }
void Element::setNormalized(float n) { owner_.set(param, info().fromNormalized(n)); }
void Element::setValue(float v) { owner_.set(param, v); }
const Asset* Element::asset(const json& el, const char* key) const {
    return el.contains(key) && el[key].is_string() ? owner_.asset(el[key].get<std::string>()) : nullptr;
}
juce::Font Element::font(float size, bool bold) const {
    juce::FontOptions o = owner_.typeface() ? juce::FontOptions(owner_.typeface()) : juce::FontOptions();
    o = o.withHeight(size);
    if (bold && !owner_.typeface()) o = o.withStyle("Bold");
    return juce::Font(o);
}

juce::Justification justification(const json& el) {
    const std::string a = el.value("align", std::string("center"));
    if (a == "left") return juce::Justification::centredLeft;
    if (a == "right") return juce::Justification::centredRight;
    return juce::Justification::centred;
}

class ImageElement : public Element {
public:
    ImageElement(SkinView& o, const json& el) : Element(o, el, -1), image_(asset(el, "src")) {}
    void paint(juce::Graphics& g) override {
        if (image_) SkinView::draw(g, *image_, getLocalBounds().toFloat());
    }

private:
    const Asset* image_;
};

// "label" (fixed text) and "value" (formatted param value).
class TextElement : public Element {
public:
    TextElement(SkinView& o, const json& el, int paramIndex)
        : Element(o, el, paramIndex), text_(el.value("text", std::string())),
          font_(font(el.value("size", 13.0f), el.value("bold", false))), just_(justification(el)) {
        if (paramIndex >= 0) {
            colour_ = parseColour(el, "color", o.text());
            setInterceptsMouseClicks(false, false);
        }
    }
    void paint(juce::Graphics& g) override {
        g.setColour(colour_);
        g.setFont(font_);
        const juce::String s = param >= 0 ? formatParam(info(), value()) : juce::String::fromUTF8(text_.c_str());
        g.drawFittedText(s, getLocalBounds(), just_, 2, 0.8f);
    }

private:
    std::string text_;
    juce::Font font_;
    juce::Justification just_;
};

// Shared mouse behaviour of continuous controls: drag, shift = fine, double click = default, wheel.
class DragElement : public Element {
public:
    using Element::Element;
    void mouseDown(const juce::MouseEvent&) override { startN_ = normalized(); }
    void mouseDoubleClick(const juce::MouseEvent&) override { setValue(info().def); }
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override {
        setNormalized(normalized() + (w.deltaY + w.deltaX) * (e.mods.isShiftDown() ? 0.05f : 0.25f));
    }
    void updateValue() override {
        setTooltip(juce::String(info().name) + ": " + formatParam(info(), value()));
        repaint();
    }

protected:
    float startN_ = 0;
};

class KnobElement : public DragElement {
public:
    KnobElement(SkinView& o, const json& el, int p) : DragElement(o, el, p), image_(asset(el, "src")) {
        frames_ = juce::jmax(1, el.value("frames", 1));
        if (el.contains("angle") && el["angle"].is_array() && el["angle"].size() == 2) {
            start_ = juce::degreesToRadians(el["angle"][0].get<float>());
            end_ = juce::degreesToRadians(el["angle"][1].get<float>());
        }
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
        updateValue();
    }

    void mouseDrag(const juce::MouseEvent& e) override {
        const float range = e.mods.isShiftDown() ? 800.0f : 200.0f;
        setNormalized(startN_ + (float)(e.getDistanceFromDragStartX() - e.getDistanceFromDragStartY()) / range);
    }

    void paint(juce::Graphics& g) override {
        g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
        const auto r = getLocalBounds().toFloat();
        const float n = normalized();
        if (image_ && image_->image.isValid() && frames_ > 1) {
            const auto& img = image_->image;
            const bool vertical = img.getHeight() >= img.getWidth();
            const int frame = juce::jlimit(0, frames_ - 1, (int)std::lround(n * (frames_ - 1)));
            const int fw = vertical ? img.getWidth() : img.getWidth() / frames_;
            const int fh = vertical ? img.getHeight() / frames_ : img.getHeight();
            g.drawImage(img, (int)r.getX(), (int)r.getY(), (int)r.getWidth(), (int)r.getHeight(),
                        vertical ? 0 : frame * fw, vertical ? frame * fh : 0, fw, fh);
            return;
        }
        const float angle = start_ + n * (end_ - start_);
        if (image_) {
            SkinView::draw(g, *image_, r, angle);
            return;
        }
        // Vector knob in the skin's colours.
        const float radius = juce::jmin(r.getWidth(), r.getHeight()) / 2.0f - 3.0f;
        const auto c = r.getCentre();
        juce::Path track, arc;
        track.addCentredArc(c.x, c.y, radius, radius, 0.0f, start_, end_, true);
        arc.addCentredArc(c.x, c.y, radius, radius, 0.0f, start_, angle, true);
        const float thick = juce::jmax(2.0f, radius * 0.14f);
        juce::PathStrokeType stroke(thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        g.setColour(juce::Colours::black.withAlpha(0.45f));
        g.strokePath(track, stroke);
        g.setColour(colour_);
        g.strokePath(arc, stroke);
        const float cap = radius * 0.72f;
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff5a5f69), c.x - cap * 0.4f, c.y - cap * 0.6f,
                                               juce::Colour(0xff1c1e23), c.x + cap * 0.5f, c.y + cap, true));
        g.fillEllipse(c.x - cap, c.y - cap, cap * 2.0f, cap * 2.0f);
        juce::Path pointer;
        pointer.addRoundedRectangle(-thick * 0.4f, -cap * 0.9f, thick * 0.8f, cap * 0.5f, thick * 0.4f);
        g.setColour(owner_.text());
        g.fillPath(pointer, juce::AffineTransform::rotation(angle).translated(c.x, c.y));
    }

private:
    const Asset* image_;
    int frames_ = 1;
    float start_ = juce::degreesToRadians(-135.0f), end_ = juce::degreesToRadians(135.0f);
};

class SliderElement : public DragElement {
public:
    SliderElement(SkinView& o, const json& el, int p)
        : DragElement(o, el, p), thumb_(asset(el, "src")), track_(asset(el, "track")) {
        vertical_ = getHeight() > getWidth();
        setMouseCursor(vertical_ ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::LeftRightResizeCursor);
        updateValue();
    }

    void mouseDown(const juce::MouseEvent& e) override {
        // Clicking the track jumps there; dragging the thumb moves it relatively.
        if (!thumbBounds().contains(e.position)) setNormalized(positionToNormalized(e.position));
        DragElement::mouseDown(e);
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        const float travel = juce::jmax(1.0f, travelLength()) * (e.mods.isShiftDown() ? 4.0f : 1.0f);
        const float d = vertical_ ? -(float)e.getDistanceFromDragStartY() : (float)e.getDistanceFromDragStartX();
        setNormalized(startN_ + d / travel);
    }

    void paint(juce::Graphics& g) override {
        g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
        const auto r = getLocalBounds().toFloat();
        if (track_) {
            SkinView::draw(g, *track_, r);
        } else {
            const float t = juce::jmax(3.0f, (vertical_ ? r.getWidth() : r.getHeight()) * 0.18f);
            auto groove = vertical_ ? r.withSizeKeepingCentre(t, r.getHeight() - thumbLength())
                                    : r.withSizeKeepingCentre(r.getWidth() - thumbLength(), t);
            g.setColour(juce::Colours::black.withAlpha(0.45f));
            g.fillRoundedRectangle(groove, t / 2.0f);
            const auto tb = thumbBounds();
            auto fill = vertical_ ? groove.withTop(tb.getCentreY()) : groove.withRight(tb.getCentreX());
            g.setColour(colour_);
            g.fillRoundedRectangle(fill, t / 2.0f);
        }
        const auto tb = thumbBounds();
        if (thumb_) {
            SkinView::draw(g, *thumb_, tb);
        } else {
            g.setColour(juce::Colour(0xffd8dbe0));
            g.fillRoundedRectangle(tb.reduced(1.0f), 3.0f);
            g.setColour(colour_);
            if (vertical_) g.fillRect(tb.withSizeKeepingCentre(tb.getWidth() - 6.0f, 2.0f));
            else g.fillRect(tb.withSizeKeepingCentre(2.0f, tb.getHeight() - 6.0f));
        }
    }

private:
    float thumbLength() const {
        const float cross = vertical_ ? (float)getWidth() : (float)getHeight();
        if (thumb_) {
            const auto b = thumb_->bounds();
            if (b.getWidth() > 0 && b.getHeight() > 0)
                return vertical_ ? cross * b.getHeight() / b.getWidth() : cross * b.getWidth() / b.getHeight();
        }
        return cross * 0.6f;
    }
    float travelLength() const { return (vertical_ ? (float)getHeight() : (float)getWidth()) - thumbLength(); }
    juce::Rectangle<float> thumbBounds() const {
        const float len = thumbLength(), pos = normalized() * travelLength();
        if (vertical_) return {0.0f, (float)getHeight() - len - pos, (float)getWidth(), len};
        return {pos, 0.0f, len, (float)getHeight()};
    }
    float positionToNormalized(juce::Point<float> p) const {
        const float len = thumbLength();
        return vertical_ ? ((float)getHeight() - len / 2.0f - p.y) / travelLength()
                         : (p.x - len / 2.0f) / travelLength();
    }

    const Asset* thumb_;
    const Asset* track_;
    bool vertical_ = false;
};

class ToggleElement : public Element {
public:
    ToggleElement(SkinView& o, const json& el, int p)
        : Element(o, el, p), image_(asset(el, "src")), text_(el.value("text", std::string())),
          font_(font(el.value("size", 12.0f), true)) {
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        updateValue();
    }
    void updateValue() override {
        setTooltip(juce::String(info().name) + ": " + formatParam(info(), value()));
        repaint();
    }
    void mouseUp(const juce::MouseEvent& e) override {
        if (!e.mouseWasDraggedSinceMouseDown() && contains(e.getPosition())) setValue(on() ? info().min : info().max);
    }
    void paint(juce::Graphics& g) override {
        const auto r = getLocalBounds().toFloat();
        if (image_ && image_->image.isValid()) {
            const auto& img = image_->image;
            const bool vertical = img.getHeight() >= img.getWidth();
            const int fw = vertical ? img.getWidth() : img.getWidth() / 2, fh = vertical ? img.getHeight() / 2 : img.getHeight();
            g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
            g.drawImage(img, 0, 0, getWidth(), getHeight(), vertical ? 0 : (on() ? fw : 0), vertical ? (on() ? fh : 0) : 0, fw, fh);
            return;
        }
        if (text_.empty()) {  // switch
            const auto pill = r.withSizeKeepingCentre(juce::jmin(r.getWidth(), r.getHeight() * 2.0f),
                                                      juce::jmin(r.getHeight(), r.getWidth() / 2.0f));
            g.setColour(on() ? colour_ : juce::Colours::black.withAlpha(0.45f));
            g.fillRoundedRectangle(pill, pill.getHeight() / 2.0f);
            const float d = pill.getHeight() - 4.0f;
            g.setColour(juce::Colour(0xffe8eaee));
            g.fillEllipse(on() ? pill.getRight() - d - 2.0f : pill.getX() + 2.0f, pill.getY() + 2.0f, d, d);
        } else {  // lit button
            g.setColour(on() ? colour_ : juce::Colours::black.withAlpha(0.35f));
            g.fillRoundedRectangle(r.reduced(1.0f), 4.0f);
            g.setColour(on() ? juce::Colours::black.withAlpha(0.8f) : owner_.text());
            g.setFont(font_);
            g.drawFittedText(juce::String::fromUTF8(text_.c_str()), getLocalBounds(), juce::Justification::centred, 1);
        }
    }

private:
    bool on() const { return value() > (info().min + info().max) / 2.0f; }
    const Asset* image_;
    std::string text_;
    juce::Font font_;
};

int stepIndex(const ParamInfo& p, float v) {
    return juce::jlimit(0, p.steps - 1, (int)std::lround((v - p.min) / (p.max - p.min) * (p.steps - 1)));
}
float stepValue(const ParamInfo& p, int i) { return p.min + (p.max - p.min) * (float)i / (float)(p.steps - 1); }
juce::String stepLabel(const ParamInfo& p, int i) {
    return (int)p.labels.size() == p.steps ? juce::String(p.labels[(size_t)i]) : juce::String(stepValue(p, i), 2);
}

class MenuElement : public Element {
public:
    MenuElement(SkinView& o, const json& el, int p) : Element(o, el, p) {
        const auto& pi = info();
        for (int i = 0; i < pi.steps; ++i) combo_.addItem(stepLabel(pi, i), i + 1);
        combo_.setColour(juce::ComboBox::backgroundColourId, parseColour(el, "background", juce::Colour(0xb0000000)));
        combo_.setColour(juce::ComboBox::textColourId, parseColour(el, "color", o.text()));
        combo_.setColour(juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
        combo_.setColour(juce::ComboBox::arrowColourId, o.accent());
        combo_.onChange = [this] { setValue(stepValue(info(), combo_.getSelectedId() - 1)); };
        addAndMakeVisible(combo_);
        updateValue();
    }
    void updateValue() override { combo_.setSelectedId(stepIndex(info(), value()) + 1, juce::dontSendNotification); }
    void resized() override { combo_.setBounds(getLocalBounds()); }

private:
    juce::ComboBox combo_;
};

class ButtonsElement : public Element {
public:
    ButtonsElement(SkinView& o, const json& el, int p)
        : Element(o, el, p), font_(font(el.value("size", 12.0f), true)) {
        vertical_ = getHeight() > getWidth() * 2;
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }
    void mouseDown(const juce::MouseEvent& e) override {
        const int n = info().steps;
        const float pos = vertical_ ? e.position.y / (float)getHeight() : e.position.x / (float)getWidth();
        setValue(stepValue(info(), juce::jlimit(0, n - 1, (int)(pos * (float)n))));
    }
    void paint(juce::Graphics& g) override {
        const auto& pi = info();
        const int sel = stepIndex(pi, value());
        auto r = getLocalBounds().toFloat();
        g.setColour(juce::Colours::black.withAlpha(0.4f));
        g.fillRoundedRectangle(r, 4.0f);
        g.setFont(font_);
        for (int i = 0; i < pi.steps; ++i) {
            const float f0 = (float)i / (float)pi.steps, f1 = (float)(i + 1) / (float)pi.steps;
            auto cell = vertical_ ? juce::Rectangle<float>(r.getX(), r.getY() + f0 * r.getHeight(), r.getWidth(), (f1 - f0) * r.getHeight())
                                  : juce::Rectangle<float>(r.getX() + f0 * r.getWidth(), r.getY(), (f1 - f0) * r.getWidth(), r.getHeight());
            if (i == sel) {
                g.setColour(colour_);
                g.fillRoundedRectangle(cell.reduced(2.0f), 3.0f);
            }
            g.setColour(i == sel ? juce::Colours::black.withAlpha(0.85f) : owner_.text());
            g.drawFittedText(stepLabel(pi, i), cell.toNearestInt().reduced(2), juce::Justification::centred, 1);
        }
    }

private:
    juce::Font font_;
    bool vertical_ = false;
};

void SkinView::addElement(const json& el) {
    const std::string type = el.at("type").get<std::string>();
    const int p = el.contains("param") ? aicontact::uiParamIndex(module_->meta(), el["param"]) : -1;
    const bool needsParam = type != "image" && type != "label";
    if (needsParam && p < 0) throw std::runtime_error("element '" + type + "' refers to an unknown param");
    std::unique_ptr<Element> e;
    if (type == "image") e = std::make_unique<ImageElement>(*this, el);
    else if (type == "label") e = std::make_unique<TextElement>(*this, el, -1);
    else if (type == "value") e = std::make_unique<TextElement>(*this, el, p);
    else if (type == "knob") e = std::make_unique<KnobElement>(*this, el, p);
    else if (type == "slider") e = std::make_unique<SliderElement>(*this, el, p);
    else if (type == "toggle") e = std::make_unique<ToggleElement>(*this, el, p);
    else if ((type == "menu" || type == "buttons") && module_->meta().params[(size_t)p].steps >= 2)
        e = type == "menu" ? std::unique_ptr<Element>(std::make_unique<MenuElement>(*this, el, p))
                           : std::unique_ptr<Element>(std::make_unique<ButtonsElement>(*this, el, p));
    else throw std::runtime_error("unsupported element '" + type + "'");
    content_.addAndMakeVisible(*e);
    elements_.push_back(std::move(e));
}

// ============================================================================
// HTML page in a web view (aci.ui/index.html)

#if AICONTACT_WEBVIEW

juce::var toVar(const json& j) { return juce::JSON::parse(juce::String::fromUTF8(j.dump().c_str())); }

juce::WebBrowserComponent::Options baseOptions() {
    auto o = juce::WebBrowserComponent::Options{};
   #if JUCE_WINDOWS
    o = o.withBackend(juce::WebBrowserComponent::Options::Backend::webview2)
             .withWinWebView2Options(juce::WebBrowserComponent::Options::WinWebView2{}
                                         .withUserDataFolder(juce::File::getSpecialLocation(juce::File::tempDirectory)
                                                                 .getChildFile("AIContactWebView2"))
                                         .withBackgroundColour(juce::Colour(0xff1f232b))
                                         .withStatusBarDisabled());
   #endif
    return o;
}

class WebView : public CustomUi {
public:
    WebView(std::shared_ptr<const InstrumentModule> module, std::string slotId, Rpc rpc, LogFn log)
        : module_(std::move(module)), slotId_(std::move(slotId)), rpc_(std::move(rpc)), log_(std::move(log)) {
        const auto& meta = module_->meta();
        int w = 720, h = 360;
        aicontact::uiPreferredSize(module_->bytes(), module_->ui(), w, h);
        design_ = {0, 0, w, h};

        // Current values go into the initial state so the first frame is already right.
        values_.assign(meta.params.size(), std::nanf(""));
        const json info = rpc_("slot.info", {{"slot_id", slotId_}}).value("result", json::object());
        for (const auto& p : info.value("params", json::array()))
            if (p.value("index", -1) >= 0 && (size_t)p["index"].get<int>() < values_.size())
                values_[(size_t)p["index"].get<int>()] = p.value("value", 0.0f);
        json params = json::array();
        for (size_t i = 0; i < meta.params.size(); ++i) {
            const auto& p = meta.params[i];
            json pj = {{"index", i}, {"id", p.id}, {"name", p.name}, {"min", p.min}, {"max", p.max},
                       {"default", p.def}, {"value", std::isnan(values_[i]) ? p.def : values_[i]},
                       {"unit", p.unit}, {"scale", p.scale}, {"steps", p.steps}, {"labels", p.labels}};
            params.push_back(pj);
        }
        const json state = {{"slot", slotId_},
                            {"params", params},
                            {"instrument", {{"id", meta.id}, {"name", meta.name}, {"version", meta.version},
                                            {"author", meta.author}, {"description", meta.description}}}};

        // The state goes in as a script with every non-ASCII character escaped: JUCE 8's Linux web view
        // sizes its IPC messages in characters instead of bytes and truncates anything non-ASCII
        // (param names, labels). The bridge itself is kept ASCII-only for the same reason.
        const std::string init = "window.__ACI_INIT__ = " + state.dump(-1, ' ', true) + ";";
        const auto options = baseOptions()
                                 .withNativeIntegrationEnabled()
                                 .withKeepPageLoadedWhenBrowserIsHidden()
                                 .withUserScript(juce::String(init.c_str()))
                                 .withUserScript(juce::String::fromUTF8(AIContactWebData::acibridge_js,
                                                                        AIContactWebData::acibridge_jsSize))
                                 .withEventListener("aci", [this](const juce::var& msg) { onMessage(msg); })
                                 .withResourceProvider([this](const juce::String& path) { return provide(path); });
        browser_ = std::make_unique<Browser>(options);
        showingWatcher_ = std::make_unique<ShowingWatcher>(*this, [this] { updateShowing(); });
        browser_->goToURL(juce::WebBrowserComponent::getResourceProviderRoot());
    }
    ~WebView() override { showingWatcher_.reset(); }

    void setValues(const std::vector<float>& values) override {
        json changed = json::object();
        for (size_t i = 0; i < values.size() && i < values_.size(); ++i)
            if (!juce::exactlyEqual(values[i], values_[i])) {  // NaN (unknown yet) never matches
                values_[i] = values[i];
                changed[std::to_string(i)] = values[i];
            }
        if (!changed.empty()) emit(changed);
    }
    juce::Rectangle<int> designBounds() const override { return design_; }
    bool scalesToFit() const override { return false; }
    juce::String kind() const override { return "html"; }
    void resized() override { browser_->setBounds(getLocalBounds()); }

private:
    // The web view is a native window: hiding an ancestor (the "Default UI" toggle, closing the panel)
    // does not hide it, so it follows whether this component is actually on screen.
    void updateShowing() {
        const bool show = isShowing();
       #if JUCE_LINUX || JUCE_BSD
        // JUCE's Linux web view (XEmbed) ignores visibility; only leaving the window unmaps it.
        if (show && browser_->getParentComponent() == nullptr) {
            addAndMakeVisible(*browser_);
            browser_->setBounds(getLocalBounds());
        } else if (!show && browser_->getParentComponent() == this) {
            removeChildComponent(browser_.get());
        }
       #else
        if (browser_->getParentComponent() == nullptr) addChildComponent(*browser_);
        browser_->setVisible(show);
       #endif
    }

    // Only the bundle may be shown: navigating anywhere else (links, redirects) is refused.
    struct Browser : juce::WebBrowserComponent {
        using juce::WebBrowserComponent::WebBrowserComponent;
        bool pageAboutToLoad(const juce::String& url) override {
            return url.startsWith(getResourceProviderRoot()) || url.startsWith("about:") || url.startsWith("data:") ||
                   url.startsWith("blob:");
        }
        void newWindowAttemptingToLoad(const juce::String&) override {}
    };

    struct ShowingWatcher : juce::ComponentMovementWatcher {
        ShowingWatcher(juce::Component& c, std::function<void()> f) : juce::ComponentMovementWatcher(&c), fn(std::move(f)) { fn(); }
        using juce::ComponentMovementWatcher::componentMovedOrResized;
        using juce::ComponentMovementWatcher::componentVisibilityChanged;
        void componentMovedOrResized(bool, bool) override {}
        void componentPeerChanged() override { fn(); }
        void componentVisibilityChanged() override { fn(); }
        std::function<void()> fn;
    };

    void emit(const json& values) {
        if (browser_ && browser_->isVisible())
            browser_->emitEventIfBrowserIsVisible("aci", toVar({{"type", "values"}, {"values", values}}));
    }

    void onMessage(const juce::var& msg) {
        const juce::String type = msg.getProperty("type", {}).toString();
        const auto& params = module_->meta().params;
        if (type == "hello") {  // page (re)loaded: send every value
            json all = json::object();
            for (size_t i = 0; i < values_.size(); ++i)
                if (!std::isnan(values_[i])) all[std::to_string(i)] = values_[i];
            emit(all);
        } else if (type == "set") {
            const int index = (int)msg.getProperty("index", -1);
            if (index < 0 || (size_t)index >= params.size()) return;
            const float v = params[(size_t)index].clamp((float)(double)msg.getProperty("value", 0.0));
            values_[(size_t)index] = v;  // the page already shows it
            rpc_("slot.set_params", {{"slot_id", slotId_}, {"params", {{std::to_string(params[(size_t)index].id), v}}}});
        } else if (type == "note") {
            const int key = (int)msg.getProperty("key", 60);
            const int velocity = (int)msg.getProperty("velocity", 100);
            const double ms = (double)msg.getProperty("ms", 500.0);
            rpc_("slot.note", {{"slot_id", slotId_}, {"key", juce::jlimit(0, 127, key)},
                               {"velocity", juce::jlimit(1, 127, velocity)}, {"duration_ms", juce::jlimit(1.0, 60000.0, ms)}});
        } else if (type == "log") {
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (now - logWindow_ > 1000.0) { logWindow_ = now; logCount_ = 0; }
            if (++logCount_ > 20) return;  // a page logging every frame must not flood the rack log
            const bool error = msg.getProperty("level", {}).toString() == "error";
            log_(error ? "error" : "info", "ui: " + unescape(msg.getProperty("message", {}).toString().substring(0, 500)));
        }
    }

    // The bridge sends non-ASCII text as \uXXXX (see aci-bridge.js).
    static std::string unescape(const juce::String& s) {
        juce::String out;
        for (int i = 0; i < s.length(); ++i) {
            if (s[i] == '\\' && i + 5 < s.length() && s[i + 1] == 'u' &&
                s.substring(i + 2, i + 6).containsOnly("0123456789abcdefABCDEF")) {
                out += juce::String::charToString((juce::juce_wchar)s.substring(i + 2, i + 6).getHexValue32());
                i += 5;
            } else {
                out += juce::String::charToString(s[i]);
            }
        }
        return out.toStdString();
    }

    std::optional<juce::WebBrowserComponent::Resource> provide(const juce::String& rawPath) {
        juce::String path = rawPath.upToFirstOccurrenceOf("?", false, false).upToFirstOccurrenceOf("#", false, false);
        path = juce::URL::removeEscapeChars(path).trimCharactersAtStart("/");
        if (path.isEmpty()) path = "index.html";
        const auto* f = module_->ui().find(path.toStdString());
        if (!f) return std::nullopt;
        const auto* d = reinterpret_cast<const std::byte*>(module_->uiData(*f));
        std::vector<std::byte> data(d, d + f->size);
        const std::string mime = aicontact::uiMimeType(f->path);
        if (mime == "text/html") data = withPolicy(data);
        return juce::WebBrowserComponent::Resource{std::move(data), juce::String(mime)};
    }

    // No network: everything a page may load comes from its own bundle.
    static std::vector<std::byte> withPolicy(const std::vector<std::byte>& html) {
        static const std::string csp =
            "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'self' 'unsafe-inline' "
            "'unsafe-eval' data: blob:; connect-src 'self' data: blob:\">";
        std::string s(reinterpret_cast<const char*>(html.data()), html.size());
        std::string lower = s;
        for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
        size_t at = lower.find("<head");
        at = at == std::string::npos ? 0 : lower.find('>', at) + 1;
        s.insert(at, csp);
        const auto* b = reinterpret_cast<const std::byte*>(s.data());
        return std::vector<std::byte>(b, b + s.size());
    }

    std::shared_ptr<const InstrumentModule> module_;
    std::string slotId_;
    Rpc rpc_;
    LogFn log_;
    juce::Rectangle<int> design_;
    std::vector<float> values_;
    std::unique_ptr<Browser> browser_;
    std::unique_ptr<ShowingWatcher> showingWatcher_;
    double logWindow_ = 0;
    int logCount_ = 0;
};

#endif  // AICONTACT_WEBVIEW

}  // namespace

bool CustomUi::webViewAvailable() {
#if !AICONTACT_WEBVIEW
    return false;
#elif JUCE_LINUX || JUCE_BSD
    // JUCE loads WebKitGTK at runtime; without it a web view stays blank.
    static const bool ok = [] {
        auto opens = [](std::initializer_list<const char*> names) {
            for (const char* n : names) {
                juce::DynamicLibrary lib;
                if (!lib.open(n)) return false;
            }
            return true;
        };
        return opens({"libgtk-3.so"}) &&
               (opens({"libwebkit2gtk-4.1.so", "libjavascriptcoregtk-4.1.so", "libsoup-3.0.so"}) ||
                opens({"libwebkit2gtk-4.0.so", "libjavascriptcoregtk-4.0.so", "libsoup-2.4.so"}));
    }();
    return ok;
#else
    static const bool ok = juce::WebBrowserComponent::areOptionsSupported(baseOptions());
    return ok;
#endif
}

std::unique_ptr<CustomUi> CustomUi::create(std::shared_ptr<const InstrumentModule> module, const std::string& slotId,
                                           Rpc rpc, LogFn log, juce::String& whyNot) {
    whyNot = {};
    if (!module || module->ui().empty()) return nullptr;
    const auto& ui = module->ui();
    if (ui.hasHtml()) {
        if (webViewAvailable()) {
#if AICONTACT_WEBVIEW
            return std::make_unique<WebView>(module, slotId, rpc, log);
#endif
        }
#if !AICONTACT_WEBVIEW
        whyNot = "HTML UI: this build has no web view";
#elif JUCE_LINUX || JUCE_BSD
        whyNot = "HTML UI needs WebKitGTK (libwebkit2gtk-4.1)";
#elif JUCE_WINDOWS
        whyNot = "HTML UI needs the Microsoft Edge WebView2 Runtime";
#else
        whyNot = "HTML UI: no web view available";
#endif
    }
    if (const auto* f = ui.find("ui.json")) {
        try {
            const uint8_t* d = module->uiData(*f);
            const json skin = json::parse(d, d + f->size);
            auto send = [rpc, slotId, module](int index, float v) {
                rpc("slot.set_params",
                    {{"slot_id", slotId}, {"params", {{std::to_string(module->meta().params[(size_t)index].id), v}}}});
            };
            whyNot = {};
            return std::make_unique<SkinView>(module, skin, send);
        } catch (const std::exception& e) {
            whyNot = juce::String("custom UI (ui.json): ") + e.what();
        }
    }
    return nullptr;
}
