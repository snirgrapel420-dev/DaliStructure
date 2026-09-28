#include "PluginEditor.h"

namespace dali_ui
{
juce::Colour sectionColour (dali::SectionType t)
{
    using T = dali::SectionType;
    switch (t)
    {
        case T::Intro:      return juce::Colour (0xff64748b);
        case T::Groove:     return juce::Colour (0xff0ea5e9);
        case T::Build:      return juce::Colour (0xfff59e0b);
        case T::PreDrop:    return juce::Colour (0xfff97316);
        case T::Drop:       return juce::Colour (0xffa855f7);
        case T::Main:       return juce::Colour (0xff7c3aed);
        case T::Breakdown:  return juce::Colour (0xff14b8a6);
        case T::Break:      return juce::Colour (0xff10b981);
        case T::Outro:      return juce::Colour (0xff475569);
        case T::Transition:
        case T::Section:    return juce::Colour (0xff4b5563);
    }
    return juce::Colour (0xff4b5563);
}

juce::Colour eventColour (dali::EventCategory c)
{
    using C = dali::EventCategory;
    switch (c)
    {
        case C::Drums:     return juce::Colour (0xfff43f5e);
        case C::Bass:      return juce::Colour (0xff3b82f6);
        case C::Musical:   return juce::Colour (0xff22c55e);
        case C::Vocal:     return juce::Colour (0xffeab308);
        case C::FX:        return juce::Colour (0xfff97316);
        case C::Structure: return juce::Colour (0xffa855f7);
        case C::User:      return juce::Colours::white;
    }
    return juce::Colours::white;
}

juce::Font font (float height, bool bold)
{
    return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
}

juce::String formatTime (double s)
{
    s = juce::jmax (0.0, s);
    const int m = (int) (s / 60.0);
    return juce::String (m) + ":" + juce::String (s - m * 60.0, 1).paddedLeft ('0', 4);
}

static juce::String str (const std::string& s) { return juce::String::fromUTF8 (s.c_str(), (int) s.size()); }

// ============================================================================ TimelineView
TimelineView::TimelineView (DaliStructureProcessor& p) : proc (p) {}

void TimelineView::setDocument (std::shared_ptr<const dali::StructureDocument> d)
{
    const bool newTrack = d == nullptr || doc == nullptr || d->reference.contentHash != doc->reference.contentHash;
    doc = std::move (d);
    if (newTrack)
    {
        selected = -1;
        viewStart = 0.0;
        viewLength = totalDuration();
    }
    repaint();
}

void TimelineView::setShowMinorEvents (bool b) { showMinor = b; repaint(); }

double TimelineView::totalDuration() const
{
    if (doc == nullptr) return 1.0;
    return juce::jmax (1.0, doc->reference.durationSec);
}

juce::Rectangle<int> TimelineView::sectionArea() const
{
    auto r = getLocalBounds().withTrimmedTop (24);
    return r.removeFromTop ((int) (r.getHeight() * 0.62f));
}

juce::Rectangle<int> TimelineView::eventArea() const
{
    auto r = getLocalBounds().withTrimmedTop (24);
    r.removeFromTop ((int) (r.getHeight() * 0.62f) + 4);
    return r;
}

double TimelineView::timeToX (double t) const { return (t - viewStart) / juce::jmax (1e-6, viewLength) * getWidth(); }
double TimelineView::xToTime (double x) const { return viewStart + x / juce::jmax (1, getWidth()) * viewLength; }

bool TimelineView::visibleEvent (const dali::Event& e) const
{
    return showMinor || e.importance != dali::Importance::Minor || e.origin == dali::Origin::User;
}

void TimelineView::paint (juce::Graphics& g)
{
    g.fillAll (Colours::panel);
    if (doc == nullptr || doc->tempo.empty())
        return;
    const auto& tm = doc->tempo;
    const int w = getWidth();

    // ---------------- ruler: bar numbers, density adapts to zoom
    const auto ruler = rulerArea();
    g.setColour (Colours::panelHi);
    g.fillRect (ruler);
    const double barSec = 60.0 / juce::jmax (1.0, doc->reference.bpm) * tm.beatsPerBar;
    const double pxPerBar = barSec / viewLength * w;
    int step = 1;
    for (int s : { 1, 2, 4, 8, 16, 32, 64 }) { step = s; if (pxPerBar * s >= 34.0) break; }
    g.setFont (font (11.0f));
    for (int bar = 1; bar <= tm.barCount(); bar += step)
    {
        const float x = (float) timeToX (tm.timeAtBar (bar));
        if (x < -40 || x > w + 40) continue;
        g.setColour (Colours::border);
        g.drawVerticalLine ((int) x, (float) ruler.getBottom() - 6.0f, (float) getHeight());
        g.setColour (Colours::textDim);
        g.drawText (juce::String (bar), (int) x + 3, ruler.getY(), 40, ruler.getHeight(), juce::Justification::centredLeft);
    }

    // ---------------- sections
    const auto sa = sectionArea();
    for (int i = 0; i < (int) doc->sections.size(); ++i)
    {
        const auto& s = doc->sections[(size_t) i];
        const float x0 = (float) timeToX (s.startTime), x1 = (float) timeToX (s.endTime);
        if (x1 < 0 || x0 > w) continue;
        auto r = juce::Rectangle<float> (x0 + 1.0f, (float) sa.getY(), juce::jmax (1.0f, x1 - x0 - 2.0f), (float) sa.getHeight());
        const auto c = sectionColour (s.type);
        const bool unsure = s.type == dali::SectionType::Transition || s.type == dali::SectionType::Section;
        g.setColour (c.withAlpha (i == selected ? 0.55f : 0.30f));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (i == selected ? Colours::text : c.withAlpha (0.9f));
        if (unsure)
        {
            const float dashes[] = { 5.0f, 4.0f };
            juce::Path p; p.addRoundedRectangle (r, 4.0f);
            juce::Path dashed; juce::PathStrokeType (1.2f).createDashedStroke (dashed, p, dashes, 2);
            g.fillPath (dashed);
        }
        else
            g.drawRoundedRectangle (r, 4.0f, i == selected ? 2.0f : 1.0f);

        if (r.getWidth() > 46.0f)
        {
            auto tr = r.reduced (6.0f, 5.0f).toNearestInt();
            g.setColour (Colours::text);
            g.setFont (font (13.0f, true));
            g.drawText (str (s.label), tr.removeFromTop (16), juce::Justification::topLeft, true);
            g.setFont (font (11.0f));
            g.setColour (Colours::textDim);
            g.drawText (juce::String (s.startBar) + "-" + juce::String (s.endBar) + "  " + juce::String (juce::roundToInt (s.confidence * 100)) + "%",
                        tr.removeFromTop (14), juce::Justification::topLeft, true);
        }
    }

    // ---------------- energy curve (relative 0-100) over the section band
    if (! doc->energy.empty())
    {
        juce::Path line, fill;
        const float top = (float) sa.getY() + 36.0f, bottom = (float) sa.getBottom() - 4.0f;
        bool started = false;
        float firstX = 0, lastX = 0;
        for (const auto& p : doc->energy)
        {
            const float x = (float) timeToX (tm.timeAtBar (p.bar));
            if (x < -10 || x > w + 10) continue;
            const float y = bottom - (float) (p.value / 100.0) * (bottom - top);
            if (! started) { line.startNewSubPath (x, y); fill.startNewSubPath (x, bottom); fill.lineTo (x, y); firstX = x; started = true; }
            else { line.lineTo (x, y); fill.lineTo (x, y); }
            lastX = x;
        }
        if (started)
        {
            fill.lineTo (lastX, bottom); fill.lineTo (firstX, bottom); fill.closeSubPath();
            g.setColour (Colours::accent.withAlpha (0.12f));
            g.fillPath (fill);
            g.setColour (Colours::text.withAlpha (0.75f));
            g.strokePath (line, juce::PathStrokeType (1.4f));
        }
    }

    // ---------------- events
    const auto ea = eventArea();
    g.setFont (font (11.0f));
    float lastLabelEnd = -1e9f;
    int lane = 0;
    for (const auto& e : doc->events)
    {
        if (! visibleEvent (e)) continue;
        const float x = (float) timeToX (e.time);
        if (x < -2 || x > w + 2) continue;
        const auto c = eventColour (dali::categoryOf (e.type));
        const float alpha = e.importance == dali::Importance::Major ? 1.0f : (e.importance == dali::Importance::Medium ? 0.75f : 0.45f);
        if (e.lengthBars > 0)
        {
            const float x1 = (float) timeToX (tm.timeAtBar (e.bar + e.lengthBars));
            g.setColour (c.withAlpha (0.18f));
            g.fillRect (juce::Rectangle<float> (x, (float) ea.getY(), x1 - x, 6.0f));
        }
        g.setColour (c.withAlpha (alpha));
        g.fillRect (juce::Rectangle<float> (x - 1.0f, (float) ea.getY(), e.importance == dali::Importance::Major ? 2.5f : 1.5f, (float) ea.getHeight() * 0.35f));
        const auto label = str (e.label);
        const float lw = (float) juce::GlyphArrangement::getStringWidthInt (font (11.0f), label) + 8.0f;
        lane = (x < lastLabelEnd) ? (lane + 1) % 3 : 0;
        const float ly = (float) ea.getY() + (float) ea.getHeight() * 0.38f + (float) lane * 15.0f;
        if (ly + 14 <= (float) ea.getBottom())
        {
            g.drawText (label, juce::Rectangle<float> (x + 2.0f, ly, lw, 14.0f), juce::Justification::centredLeft, false);
            lastLabelEnd = juce::jmax (lastLabelEnd, x + lw);
        }
    }

    // ---------------- playhead
    if (proc.hasAudio())
    {
        const float x = (float) timeToX (proc.getPositionSeconds());
        g.setColour (Colours::accent);
        g.fillRect (juce::Rectangle<float> (x - 1.0f, 0.0f, 2.0f, (float) getHeight()));
    }
}

int TimelineView::sectionAt (juce::Point<int> p) const
{
    if (doc == nullptr || ! sectionArea().contains (p)) return -1;
    const double t = xToTime (p.x);
    for (int i = 0; i < (int) doc->sections.size(); ++i)
        if (t >= doc->sections[(size_t) i].startTime && t < doc->sections[(size_t) i].endTime) return i;
    return -1;
}

const dali::Event* TimelineView::eventAt (juce::Point<int> p) const
{
    if (doc == nullptr || ! eventArea().contains (p)) return nullptr;
    const dali::Event* best = nullptr;
    double bestD = 6.0;
    for (const auto& e : doc->events)
    {
        if (! visibleEvent (e)) continue;
        const double d = std::abs (timeToX (e.time) - p.x);
        if (d < bestD) { bestD = d; best = &e; }
    }
    return best;
}

void TimelineView::mouseDown (const juce::MouseEvent& e)
{
    if (doc == nullptr) return;
    const int s = sectionAt (e.getPosition());
    if (s >= 0)
    {
        selected = s;
        if (onSectionSelected) onSectionSelected (s);
    }
    proc.seekSeconds (xToTime (e.position.x));
    repaint();
}

void TimelineView::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int s = sectionAt (e.getPosition());
    if (s >= 0 && doc != nullptr)
    {
        proc.seekSeconds (doc->sections[(size_t) s].startTime);
        proc.setPlaying (true);
    }
}

void TimelineView::mouseMove (const juce::MouseEvent& e)
{
    juce::String tip;
    if (const auto* ev = eventAt (e.getPosition()))
        tip = str (ev->label) + "  |  bar " + juce::String (juce::roundToInt (ev->bar)) + "  |  " + dali::toString (ev->importance)
            + "  |  confidence " + juce::String (juce::roundToInt (ev->confidence * 100)) + "%";
    else if (const int s = sectionAt (e.getPosition()); s >= 0)
    {
        const auto& sec = doc->sections[(size_t) s];
        tip = str (sec.label) + "  |  bars " + juce::String (sec.startBar) + "-" + juce::String (sec.endBar)
            + "  |  energy " + juce::String (juce::roundToInt (sec.energy));
    }
    if (tip != getTooltip()) setTooltip (tip);
}

void TimelineView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (doc == nullptr) return;
    const double total = totalDuration();
    if (std::abs (w.deltaX) > std::abs (w.deltaY) || e.mods.isShiftDown())
    {
        const double d = (std::abs (w.deltaX) > 0 ? -w.deltaX : -w.deltaY) * viewLength * 0.5;
        viewStart = juce::jlimit (0.0, juce::jmax (0.0, total - viewLength), viewStart + d);
    }
    else
    {
        const double anchor = xToTime (e.position.x);
        const double factor = std::pow (0.85, w.deltaY * 4.0);
        viewLength = juce::jlimit (4.0, total, viewLength * factor);
        viewStart = juce::jlimit (0.0, juce::jmax (0.0, total - viewLength), anchor - (e.position.x / juce::jmax (1, getWidth())) * viewLength);
    }
    repaint();
}

// ============================================================================ DetailsPanel
void DetailsPanel::setContent (std::shared_ptr<const dali::StructureDocument> d, int s, bool cached)
{
    doc = std::move (d); section = s; fromCache = cached;
    repaint();
}

void DetailsPanel::paint (juce::Graphics& g)
{
    g.setColour (Colours::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
    if (doc == nullptr) return;
    auto r = getLocalBounds().reduced (14, 12);
    auto line = [&] (const juce::String& key, const juce::String& value)
    {
        auto row = r.removeFromTop (20);
        g.setFont (font (12.0f));
        g.setColour (Colours::textDim);
        g.drawText (key, row.removeFromLeft (92), juce::Justification::centredLeft);
        g.setColour (Colours::text);
        g.drawText (value, row, juce::Justification::centredLeft, true);
    };

    if (section < 0 || section >= (int) doc->sections.size())
    {
        g.setFont (font (15.0f, true));
        g.setColour (Colours::text);
        g.drawText ("Track Overview", r.removeFromTop (24), juce::Justification::centredLeft);
        r.removeFromTop (4);
        line ("File", str (doc->reference.fileName));
        line ("BPM", juce::String (doc->reference.bpm, 2) + (doc->tempo.stable ? "" : "  (variable)"));
        line ("Key", doc->reference.key.confidence >= 0.35 ? str (doc->reference.key.name) : juce::String ("-"));
        line ("Bars", juce::String (doc->reference.barCount));
        line ("Length", formatTime (doc->reference.durationSec));
        line ("Sections", juce::String ((int) doc->sections.size()));
        line ("Grid", "beat " + juce::String (juce::roundToInt (doc->tempo.beatConfidence * 100)) + "%, bars "
                          + juce::String (juce::roundToInt (doc->tempo.downbeatConfidence * 100)) + "%");
        line ("Source", juce::String (doc->diagnostics.usedStems ? "stems" : "full mix") + (fromCache ? ", from cache" : ""));
        r.removeFromTop (8);
        g.setFont (font (11.5f));
        for (const auto& w : doc->diagnostics.warnings)
        {
            g.setColour (juce::Colour (0xfff59e0b));
            g.drawFittedText (str (w), r.removeFromTop (32), juce::Justification::topLeft, 2);
        }
        g.setColour (Colours::textDim);
        g.drawFittedText ("Click a section for details. Double-click to play from it. Mouse wheel: zoom, Shift+wheel: scroll. Space: play/pause.",
                          r.removeFromBottom (36), juce::Justification::bottomLeft, 3);
        return;
    }

    const auto& s = doc->sections[(size_t) section];
    g.setColour (sectionColour (s.type));
    g.fillRoundedRectangle (r.removeFromTop (4).toFloat(), 2.0f);
    r.removeFromTop (8);
    g.setFont (font (18.0f, true));
    g.setColour (Colours::text);
    g.drawText (str (s.label), r.removeFromTop (26), juce::Justification::centredLeft);
    r.removeFromTop (4);
    line ("Bars", juce::String (s.startBar) + " - " + juce::String (s.endBar) + "   (" + juce::String (s.durationBars()) + " bars)");
    line ("Time", formatTime (s.startTime) + " - " + formatTime (s.endTime));
    line ("Energy", juce::String (juce::roundToInt (s.energy)) + " / 100");
    line ("Confidence", juce::String (juce::roundToInt (s.confidence * 100)) + "%"
                        + (s.type == dali::SectionType::Transition || s.type == dali::SectionType::Section ? "  (type uncertain)" : ""));
    juce::StringArray els;
    for (const auto& e : s.elements) els.add (str (e));
    auto row = r.removeFromTop (40);
    g.setColour (Colours::textDim);
    g.setFont (font (12.0f));
    g.drawText ("Elements", row.removeFromLeft (92).removeFromTop (20), juce::Justification::centredLeft);
    g.setColour (Colours::text);
    g.drawFittedText (els.isEmpty() ? juce::String ("-") : els.joinIntoString (", "), row, juce::Justification::topLeft, 2);

    r.removeFromTop (6);
    g.setColour (Colours::textDim);
    g.drawText ("Events in section", r.removeFromTop (20), juce::Justification::centredLeft);
    g.setFont (font (11.5f));
    for (const auto& e : doc->events)
    {
        if (e.bar < s.startBar || e.bar >= s.endBar + 1 || e.importance == dali::Importance::Minor) continue;
        if (r.getHeight() < 16) break;
        auto er = r.removeFromTop (17);
        g.setColour (eventColour (dali::categoryOf (e.type)));
        g.fillEllipse (er.removeFromLeft (10).toFloat().withSizeKeepingCentre (6.0f, 6.0f));
        g.setColour (Colours::text);
        g.drawText ("bar " + juce::String (juce::roundToInt (e.bar)) + "   " + str (e.label), er.withTrimmedLeft (4), juce::Justification::centredLeft, true);
    }
}
} // namespace dali_ui

// ============================================================================ Editor
using namespace dali_ui;

DaliStructureEditor::DaliStructureEditor (DaliStructureProcessor& p)
    : AudioProcessorEditor (p), proc (p), timeline (p)
{
    lnf.setColourScheme ({ Colours::background, Colours::panel, Colours::panelHi, Colours::border, Colours::text,
                           Colours::accent, Colours::text, Colours::accent.withAlpha (0.35f), Colours::text });
    lnf.setColour (juce::TextButton::buttonColourId, Colours::panelHi);
    lnf.setColour (juce::TextButton::buttonOnColourId, Colours::accent);
    lnf.setColour (juce::Slider::thumbColourId, Colours::accent);
    lnf.setColour (juce::Slider::trackColourId, Colours::accent.withAlpha (0.6f));
    lnf.setColour (juce::TooltipWindow::backgroundColourId, Colours::panelHi);
    lnf.setColour (juce::TooltipWindow::textColourId, Colours::text);
    setLookAndFeel (&lnf);

    for (auto* c : std::initializer_list<juce::Component*> { &loadButton, &playButton, &cancelButton, &reanalyzeButton,
                                                             &manualBpmButton, &minorToggle, &bpmField, &volume, &timeline, &details })
        addAndMakeVisible (c);

    loadButton.onClick = [this] { chooseFile(); };
    playButton.onClick = [this] { proc.setPlaying (! proc.isPlaying()); };
    cancelButton.onClick = [this] { proc.cancelAnalysis(); };
    reanalyzeButton.onClick = [this] { proc.reanalyze(); };
    reanalyzeButton.setTooltip ("Analyze the reference again, ignoring the cache");
    manualBpmButton.onClick = [this]
    {
        const double bpm = bpmField.getText().getDoubleValue();
        if (bpm >= 40.0 && bpm <= 250.0) proc.reanalyze (bpm);
    };
    bpmField.setInputRestrictions (6, "0123456789.");
    bpmField.setTextToShowWhenEmpty ("BPM", Colours::textDim);
    bpmField.setJustification (juce::Justification::centred);
    minorToggle.onClick = [this] { timeline.setShowMinorEvents (minorToggle.getToggleState()); };
    volume.setTooltip ("Reference playback volume");
    volumeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.parameters, "volume", volume);
    timeline.onSectionSelected = [this] (int s) { details.setContent (shownDoc, s, proc.loadedFromCache()); };

    setWantsKeyboardFocus (true);
    setResizable (true, true);
    setResizeLimits (900, 520, 2600, 1500);
    setSize (1180, 640);

    proc.addChangeListener (this);
    refresh();
    startTimerHz (30);
}

DaliStructureEditor::~DaliStructureEditor()
{
    proc.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

juce::Rectangle<int> DaliStructureEditor::contentArea() const
{
    return getLocalBounds().reduced (16).withTrimmedTop (64);
}

void DaliStructureEditor::resized()
{
    auto header = getLocalBounds().reduced (16).removeFromTop (48);
    header.removeFromLeft (260);   // title
    loadButton.setBounds (header.removeFromLeft (130).reduced (0, 8));
    header.removeFromLeft (8);
    reanalyzeButton.setBounds (header.removeFromLeft (100).reduced (0, 8));
    volume.setBounds (header.removeFromRight (120).reduced (0, 12));
    header.removeFromRight (8);
    playButton.setBounds (header.removeFromRight (80).reduced (0, 8));

    auto content = contentArea();
    auto bottom = content.removeFromBottom (26);
    minorToggle.setBounds (bottom.removeFromLeft (170));
    details.setBounds (content.removeFromRight (300));
    content.removeFromRight (12);
    timeline.setBounds (content);

    // error-state controls (placed in the centre of the timeline area)
    auto c = timeline.getBounds().withSizeKeepingCentre (420, 34).translated (0, 40);
    bpmField.setBounds (c.removeFromLeft (90));
    c.removeFromLeft (8);
    manualBpmButton.setBounds (c.removeFromLeft (190));
    cancelButton.setBounds (timeline.getBounds().withSizeKeepingCentre (100, 30).translated (0, 44));
}

void DaliStructureEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);
    auto header = getLocalBounds().reduced (16).removeFromTop (48);
    g.setColour (Colours::text);
    g.setFont (font (22.0f, true));
    g.drawText ("DaliStructure", header.removeFromLeft (170), juce::Justification::centredLeft);
    g.setColour (Colours::accent);
    g.setFont (font (12.0f));
    g.drawText ("by Dali Audio", header.removeFromLeft (90).withTrimmedTop (6), juce::Justification::centredLeft);

    // track info chips
    if (auto doc = shownDoc)
    {
        auto info = getLocalBounds().reduced (16).removeFromTop (48);
        info.removeFromLeft (520);
        info.removeFromRight (224);
        juce::StringArray chips;
        chips.add (str (doc->reference.fileName));
        chips.add (juce::String (doc->reference.bpm, 1) + " BPM");
        chips.add (doc->reference.key.confidence >= 0.35 ? str (doc->reference.key.name) : juce::String ("Key -"));
        chips.add (juce::String (doc->reference.barCount) + " bars");
        chips.add (formatTime (proc.getPositionSeconds()) + " / " + formatTime (doc->reference.durationSec));
        g.setFont (font (12.0f));
        for (int i = 1; i < chips.size(); ++i)
        {
            const int cw = juce::GlyphArrangement::getStringWidthInt (font (12.0f), chips[i]) + 18;
            auto chip = info.removeFromRight (cw).reduced (2, 12);
            if (chip.getX() < info.getX()) break;
            g.setColour (Colours::panelHi);
            g.fillRoundedRectangle (chip.toFloat(), 10.0f);
            g.setColour (Colours::text);
            g.drawText (chips[i], chip, juce::Justification::centred);
            info.removeFromRight (4);
        }
        g.setColour (Colours::textDim);
        g.drawText (chips[0], info.reduced (6, 0), juce::Justification::centredRight, true);
    }

    // status overlays inside the timeline area
    const auto tl = timeline.getBounds();
    const auto st = proc.getStatus();
    if (st == DaliStructureProcessor::Status::Loading || st == DaliStructureProcessor::Status::Analyzing)
    {
        g.setColour (Colours::panel);
        g.fillRect (tl);
        auto bar = tl.withSizeKeepingCentre (juce::jmin (460, tl.getWidth() - 40), 8);
        g.setColour (Colours::panelHi);
        g.fillRoundedRectangle (bar.toFloat(), 4.0f);
        g.setColour (Colours::accent);
        g.fillRoundedRectangle (bar.toFloat().withWidth ((float) bar.getWidth() * proc.getProgress()), 4.0f);
        g.setColour (Colours::text);
        g.setFont (font (14.0f));
        g.drawText (proc.getStage() + "  " + juce::String (juce::roundToInt (proc.getProgress() * 100)) + "%",
                    bar.translated (0, -34).withHeight (24), juce::Justification::centred);
    }
    else if (st == DaliStructureProcessor::Status::Empty || (st == DaliStructureProcessor::Status::Error && shownDoc == nullptr))
    {
        g.setColour (dragOver ? Colours::accent.withAlpha (0.12f) : Colours::panel);
        g.fillRect (tl);
        const float dashes[] = { 8.0f, 6.0f };
        juce::Path p; p.addRoundedRectangle (tl.toFloat().reduced (10.0f), 8.0f);
        juce::Path dashed; juce::PathStrokeType (1.5f).createDashedStroke (dashed, p, dashes, 2);
        g.setColour (dragOver ? Colours::accent : Colours::border);
        g.fillPath (dashed);
        g.setFont (font (16.0f, true));
        g.setColour (Colours::text);
        if (st == DaliStructureProcessor::Status::Empty)
        {
            g.drawText ("Drop a reference track here", tl.translated (0, -14), juce::Justification::centred);
            g.setFont (font (12.5f));
            g.setColour (Colours::textDim);
            g.drawText ("WAV, AIFF, MP3, FLAC  -  or click Load Reference", tl.translated (0, 14), juce::Justification::centred);
        }
        else
        {
            g.setColour (Colours::error);
            g.drawFittedText (proc.getErrorMessage(), tl.withSizeKeepingCentre (tl.getWidth() - 60, 60).translated (0, -30),
                              juce::Justification::centred, 3);
        }
    }
    else if (st == DaliStructureProcessor::Status::Error || proc.getErrorMessage().isNotEmpty())
    {
        g.setColour (Colours::error);
        g.setFont (font (12.0f));
        g.drawText (proc.getErrorMessage(), tl.getX(), tl.getBottom() + 4, tl.getWidth(), 20, juce::Justification::centredRight, true);
    }
}

void DaliStructureEditor::refresh()
{
    const auto st = proc.getStatus();
    auto doc = proc.getDocument();
    if (doc != shownDoc)
    {
        shownDoc = doc;
        timeline.setDocument (doc);
        details.setContent (doc, timeline.getSelectedSection(), proc.loadedFromCache());
    }
    const bool busy = st == DaliStructureProcessor::Status::Loading || st == DaliStructureProcessor::Status::Analyzing;
    timeline.setVisible (! busy && shownDoc != nullptr);
    cancelButton.setVisible (busy);
    const bool manual = st == DaliStructureProcessor::Status::Error && proc.errorAllowsManualBpm();
    bpmField.setVisible (manual);
    manualBpmButton.setVisible (manual);
    reanalyzeButton.setEnabled (! busy && proc.getReferenceFile().existsAsFile());
    playButton.setEnabled (proc.hasAudio());
    repaint();
}

void DaliStructureEditor::changeListenerCallback (juce::ChangeBroadcaster*) { refresh(); }

// JUCE 8 draws with Direct2D on Windows by default. Inside some hosts / with some
// GPU drivers this produces a blank or black plugin window. The software renderer
// works everywhere and is more than fast enough for this UI.
void DaliStructureEditor::ensureReliableRenderer()
{
   #if JUCE_WINDOWS
    if (rendererChecked) return;
    if (auto* peer = getPeer())
    {
        if (peer->getCurrentRenderingEngine() != 0)
            peer->setCurrentRenderingEngine (0);
        rendererChecked = true;
        repaint();
    }
   #endif
}

void DaliStructureEditor::parentHierarchyChanged()
{
    rendererChecked = false;
    ensureReliableRenderer();
}

void DaliStructureEditor::timerCallback()
{
    ensureReliableRenderer();
    playButton.setButtonText (proc.isPlaying() ? "Pause" : "Play");
    const auto st = proc.getStatus();
    const bool busy = st == DaliStructureProcessor::Status::Loading || st == DaliStructureProcessor::Status::Analyzing;
    if (busy != cancelButton.isVisible()) refresh();
    if (busy) repaint (timeline.getBounds());
    if (proc.isPlaying()) { timeline.repaint(); repaint (getLocalBounds().removeFromTop (72)); }
    playButton.setEnabled (proc.hasAudio());
}

bool DaliStructureEditor::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey && proc.hasAudio())
    {
        proc.setPlaying (! proc.isPlaying());
        return true;
    }
    return false;
}

void DaliStructureEditor::chooseFile()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose a reference track", juce::File(), "*.wav;*.wave;*.aif;*.aiff;*.mp3;*.flac");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f.existsAsFile()) proc.loadReference (f);
                          });
}

bool DaliStructureEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1 && DaliStructureProcessor::isSupportedFile (juce::File (files[0]));
}

void DaliStructureEditor::filesDropped (const juce::StringArray& files, int, int)
{
    dragOver = false;
    if (files.size() == 1) proc.loadReference (juce::File (files[0]));
    repaint();
}
