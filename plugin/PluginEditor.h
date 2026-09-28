#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"

namespace dali_ui
{
namespace Colours
{
    const juce::Colour background { 0xff0e0e12 };
    const juce::Colour panel      { 0xff16161d };
    const juce::Colour panelHi    { 0xff1e1e28 };
    const juce::Colour border     { 0xff2a2a36 };
    const juce::Colour text       { 0xffe8e8f0 };
    const juce::Colour textDim    { 0xff8a8a99 };
    const juce::Colour accent     { 0xffa855f7 };
    const juce::Colour error      { 0xffef4444 };
}
juce::Colour sectionColour (dali::SectionType);
juce::Colour eventColour (dali::EventCategory);
juce::Font font (float height, bool bold = false);
juce::String formatTime (double seconds);

// Single-timeline view: ruler, sections, energy curve, events, playhead.
class TimelineView : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit TimelineView (DaliStructureProcessor&);
    void setDocument (std::shared_ptr<const dali::StructureDocument>);
    void setShowMinorEvents (bool);
    int getSelectedSection() const noexcept { return selected; }
    std::function<void (int)> onSectionSelected;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    double timeToX (double t) const;
    double xToTime (double x) const;
    double totalDuration() const;
    juce::Rectangle<int> rulerArea() const    { return getLocalBounds().removeFromTop (22); }
    juce::Rectangle<int> sectionArea() const;
    juce::Rectangle<int> eventArea() const;
    int sectionAt (juce::Point<int>) const;
    const dali::Event* eventAt (juce::Point<int>) const;
    bool visibleEvent (const dali::Event&) const;

    DaliStructureProcessor& proc;
    std::shared_ptr<const dali::StructureDocument> doc;
    double viewStart = 0.0, viewLength = 0.0;
    int selected = -1;
    bool showMinor = false;
};

class DetailsPanel : public juce::Component
{
public:
    void setContent (std::shared_ptr<const dali::StructureDocument> d, int sectionIndex, bool cached);
    void paint (juce::Graphics&) override;
private:
    std::shared_ptr<const dali::StructureDocument> doc;
    int section = -1;
    bool fromCache = false;
};
} // namespace dali_ui

class DaliStructureEditor : public juce::AudioProcessorEditor,
                            public juce::FileDragAndDropTarget,
                            private juce::ChangeListener,
                            private juce::Timer
{
public:
    explicit DaliStructureEditor (DaliStructureProcessor&);
    ~DaliStructureEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dragOver = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { dragOver = false; repaint(); }
    void filesDropped (const juce::StringArray&, int, int) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void refresh();
    void chooseFile();
    juce::Rectangle<int> contentArea() const;

    DaliStructureProcessor& proc;
    juce::LookAndFeel_V4 lnf;
    juce::TooltipWindow tooltips { this, 300 };

    juce::TextButton loadButton { "Load Reference" }, playButton { "Play" }, cancelButton { "Cancel" },
                     reanalyzeButton { "Re-analyze" }, manualBpmButton { "Analyze with this BPM" };
    juce::ToggleButton minorToggle { "Show minor events" };
    juce::TextEditor bpmField;
    juce::Slider volume { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> volumeAttachment;
    std::unique_ptr<juce::FileChooser> chooser;

    dali_ui::TimelineView timeline;
    dali_ui::DetailsPanel details;
    std::shared_ptr<const dali::StructureDocument> shownDoc;
    bool dragOver = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DaliStructureEditor)
};
