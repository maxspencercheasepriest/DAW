#include "BuiltIn.h"
#include "Theme.h"
#include <juce_dsp/juce_dsp.h>
#include <BinaryData.h>
#include <mutex>

using namespace juce;
using APVTS = AudioProcessorValueTreeState;

namespace BuiltIn
{

//==============================================================================
// Descriptions
//==============================================================================
static PluginDescription describe (const String& id)
{
    PluginDescription d;
    d.pluginFormatName = "Built-in";
    d.manufacturerName = "Tracklayer";
    d.fileOrIdentifier = id;
    d.version = "1.0";
    d.uniqueId = d.deprecatedUid = id.hashCode();
    d.isInstrument = id != "tracklayer:britamp";
    d.numInputChannels = d.isInstrument ? 0 : 2;
    d.numOutputChannels = 2;

    if (id == "tracklayer:synth")    { d.name = "Tracklayer Synth"; d.category = "Synth"; }
    if (id == "tracklayer:keys")     { d.name = "Tracklayer Keys";  d.category = "Piano"; }
    if (id == "tracklayer:britamp")  { d.name = "Tracklayer Brit Amp"; d.category = "Amp"; }
    d.descriptiveName = d.name;
    return d;
}

Array<PluginDescription> getTypes()
{
    return { describe ("tracklayer:keys"), describe ("tracklayer:synth"), describe ("tracklayer:britamp") };
}

//==============================================================================
// Shared base: parameters live in an AudioProcessorValueTreeState, presets are programs.
//==============================================================================
class Base : public AudioPluginInstance
{
public:
    struct Preset { String name; std::vector<std::pair<String, float>> values; };

    Base (const String& id, const BusesProperties& buses, APVTS::ParameterLayout layout)
        : AudioPluginInstance (buses), identifier (id), state (*this, nullptr, "STATE", std::move (layout)) {}

    void fillInPluginDescription (PluginDescription& d) const override { d = describe (identifier); }
    const String getName() const override { return describe (identifier).name; }
    bool acceptsMidi() const override  { return describe (identifier).isInstrument; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return jmax (1, (int) presets.size()); }
    int getCurrentProgram() override { return currentPreset; }
    const String getProgramName (int i) override { return isPositiveAndBelow (i, (int) presets.size()) ? presets[(size_t) i].name : String(); }
    void changeProgramName (int, const String&) override {}
    void setCurrentProgram (int i) override
    {
        if (! isPositiveAndBelow (i, (int) presets.size())) return;
        currentPreset = i;
        for (auto& [pid, v] : presets[(size_t) i].values)
            if (auto* p = state.getParameter (pid))
                p->setValueNotifyingHost (p->convertTo0to1 (v));
    }

    void getStateInformation (MemoryBlock& m) override
    {
        auto s = state.copyState();
        s.setProperty ("preset", currentPreset, nullptr);
        if (auto xml = s.createXml()) copyXmlToBinary (*xml, m);
    }

    void setStateInformation (const void* data, int size) override
    {
        if (auto xml = getXmlFromBinary (data, size))
        {
            auto s = ValueTree::fromXml (*xml);
            if (s.hasType (state.state.getType()))
            {
                currentPreset = s.getProperty ("preset", 0);
                state.replaceState (s);
            }
        }
    }

    bool hasEditor() const override { return true; }
    AudioProcessorEditor* createEditor() override;

    float v (const char* pid) const { return state.getRawParameterValue (pid)->load(); }

    virtual Colour accent() const = 0;
    virtual String tagline() const = 0;

    String identifier;
    APVTS state;
    std::vector<Preset> presets;
    int currentPreset = 0;
};

//==============================================================================
// A simple knob panel that works for every built-in.
//==============================================================================
class KnobEditor : public AudioProcessorEditor
{
public:
    explicit KnobEditor (Base& p) : AudioProcessorEditor (p), proc (p)
    {
        if (p.presets.size() > 1)
        {
            for (int i = 0; i < (int) p.presets.size(); ++i)
                presetBox.addItem (p.presets[(size_t) i].name, i + 1);
            presetBox.setSelectedId (p.getCurrentProgram() + 1, dontSendNotification);
            presetBox.onChange = [this] { proc.setCurrentProgram (presetBox.getSelectedId() - 1); };
            addAndMakeVisible (presetBox);
        }

        for (auto* param : p.getParameters())
        {
            auto* ranged = dynamic_cast<RangedAudioParameter*> (param);
            if (ranged == nullptr) continue;

            auto c = std::make_unique<Control>();
            c->label.setText (ranged->getName (20), dontSendNotification);
            c->label.setJustificationType (Justification::centred);
            c->label.setFont (Theme::font (11.5f, true));
            c->label.setColour (Label::textColourId, Theme::text);
            addAndMakeVisible (c->label);

            if (auto* choice = dynamic_cast<AudioParameterChoice*> (ranged))
            {
                c->box = std::make_unique<ComboBox>();
                c->box->addItemList (choice->choices, 1);
                addAndMakeVisible (*c->box);
                c->boxAttach = std::make_unique<APVTS::ComboBoxAttachment> (p.state, ranged->paramID, *c->box);
            }
            else if (dynamic_cast<AudioParameterBool*> (ranged) != nullptr)
            {
                c->toggle = std::make_unique<TextButton> (ranged->getName (12));
                c->toggle->setClickingTogglesState (true);
                c->toggle->setColour (TextButton::buttonOnColourId, p.accent());
                c->toggle->setColour (TextButton::textColourOnId, Colour (0xff111111));
                addAndMakeVisible (*c->toggle);
                c->toggleAttach = std::make_unique<APVTS::ButtonAttachment> (p.state, ranged->paramID, *c->toggle);
                c->label.setText ({}, dontSendNotification);
            }
            else
            {
                c->knob = std::make_unique<Slider> (Slider::RotaryHorizontalVerticalDrag, Slider::TextBoxBelow);
                c->knob->setTextBoxStyle (Slider::TextBoxBelow, false, 70, 16);
                c->knob->setColour (Slider::rotarySliderFillColourId, p.accent());
                c->knob->setColour (Slider::textBoxOutlineColourId, Colours::transparentBlack);
                c->knob->setColour (Slider::textBoxTextColourId, Theme::textDim);
                addAndMakeVisible (*c->knob);
                c->knobAttach = std::make_unique<APVTS::SliderAttachment> (p.state, ranged->paramID, *c->knob);
            }
            controls.push_back (std::move (c));
        }

        const int perRow = jmin (6, (int) controls.size());
        const int rows = ((int) controls.size() + perRow - 1) / perRow;
        setSize (jmax (420, perRow * cellW + 24), 64 + rows * cellH + 12);
    }

    void paint (Graphics& g) override
    {
        g.fillAll (Colour (0xff1d1d21));
        auto top = getLocalBounds().removeFromTop (52);
        g.setColour (Colour (0xff141417));
        g.fillRect (top);
        g.setColour (proc.accent());
        g.fillRect (top.removeFromBottom (2));
        g.setFont (Theme::font (19.0f, true));
        g.drawText (proc.getName().toUpperCase(), 16, 6, 400, 24, Justification::centredLeft);
        g.setColour (Theme::textDim);
        g.setFont (Theme::font (11.5f));
        g.drawText (proc.tagline(), 16, 28, 460, 16, Justification::centredLeft);
    }

    void resized() override
    {
        presetBox.setBounds (getWidth() - 196, 14, 180, 24);
        const int perRow = jmin (6, (int) controls.size());
        for (int i = 0; i < (int) controls.size(); ++i)
        {
            auto cell = Rectangle<int> (12 + (i % perRow) * cellW, 64 + (i / perRow) * cellH, cellW, cellH).reduced (4);
            auto& c = *controls[(size_t) i];
            c.label.setBounds (cell.removeFromTop (18));
            if (c.knob)   c.knob->setBounds (cell);
            if (c.box)    c.box->setBounds (cell.withSizeKeepingCentre (cellW - 12, 24));
            if (c.toggle) c.toggle->setBounds (cell.withSizeKeepingCentre (cellW - 20, 28));
        }
    }

private:
    struct Control
    {
        Label label;
        std::unique_ptr<Slider> knob;
        std::unique_ptr<ComboBox> box;
        std::unique_ptr<TextButton> toggle;
        std::unique_ptr<APVTS::SliderAttachment> knobAttach;
        std::unique_ptr<APVTS::ComboBoxAttachment> boxAttach;
        std::unique_ptr<APVTS::ButtonAttachment> toggleAttach;
    };

    static constexpr int cellW = 108, cellH = 112;
    Base& proc;
    ComboBox presetBox;
    std::vector<std::unique_ptr<Control>> controls;
};

AudioProcessorEditor* Base::createEditor() { return new KnobEditor (*this); }

//==============================================================================
// Helpers
//==============================================================================
static NormalisableRange<float> skewed (float lo, float hi, float centre)
{
    NormalisableRange<float> r (lo, hi);
    r.setSkewForCentre (centre);
    return r;
}

static std::unique_ptr<AudioParameterFloat> knob (const String& id, const String& name, NormalisableRange<float> r,
                                                  float def, const String& suffix = {})
{
    const float span = r.end - r.start;
    auto toText = [span, suffix] (float value, int)
    {
        const int places = span > 200.0f ? 0 : span > 20.0f ? 1 : span > 2.5f ? 1 : 2;
        auto s = String (value, places);
        if (span > 200.0f && value >= 1000.0f && suffix == " Hz")
            s = String (value / 1000.0f, 1) + "k";
        return s + suffix;
    };
    return std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, r, def,
                                                  AudioParameterFloatAttributes().withLabel (suffix)
                                                                                 .withStringFromValueFunction (toText));
}

struct AnySound : public SynthesiserSound
{
    bool appliesToNote (int) override    { return true; }
    bool appliesToChannel (int) override { return true; }
};

// Space (reverb) + output volume shared by the instruments.
struct OutputStage
{
    Reverb reverb;
    void prepare (double sr) { reverb.setSampleRate (sr); reverb.reset(); }
    void process (AudioBuffer<float>& b, int n, float space, float volumeDb)
    {
        if (space > 0.001f)
        {
            Reverb::Parameters rp;
            rp.roomSize = 0.55f + 0.35f * space;
            rp.damping = 0.45f;
            rp.wetLevel = 0.32f * space;
            rp.dryLevel = 1.0f - 0.25f * space;
            rp.width = 1.0f;
            reverb.setParameters (rp);
            reverb.processStereo (b.getWritePointer (0), b.getWritePointer (1), n);
        }
        b.applyGain (0, n, Decibels::decibelsToGain (volumeDb));
    }
};

//==============================================================================
// TRACKLAYER SYNTH: two-oscillator subtractive poly synth
//==============================================================================
class SynthPlugin : public Base
{
public:
    SynthPlugin() : Base ("tracklayer:synth", BusesProperties().withOutput ("Out", AudioChannelSet::stereo()), layout())
    {
        presets = {
            { "Init Saw",     {} },
            { "Warm Pad",     { {"wave1",0},{"wave2",0},{"oct2",1},{"detune",9},{"mix",0.5f},{"cutoff",1400},{"reso",0.15f},{"envamt",0.25f},{"fdecay",1.5f},{"attack",0.9f},{"decay",1.0f},{"sustain",0.85f},{"release",1.6f},{"width",0.8f},{"space",0.55f},{"volume",-8} } },
            { "Synth Bass",   { {"wave1",1},{"wave2",0},{"oct2",0},{"detune",4},{"mix",0.35f},{"cutoff",420},{"reso",0.35f},{"envamt",0.65f},{"fdecay",0.25f},{"attack",0.002f},{"decay",0.3f},{"sustain",0.7f},{"release",0.12f},{"width",0.0f},{"space",0.0f},{"volume",-4} } },
            { "Bright Lead",  { {"wave1",0},{"wave2",1},{"oct2",2},{"detune",6},{"mix",0.4f},{"cutoff",3800},{"reso",0.3f},{"envamt",0.3f},{"fdecay",0.4f},{"attack",0.01f},{"decay",0.3f},{"sustain",0.8f},{"release",0.25f},{"width",0.4f},{"space",0.25f},{"volume",-9} } },
            { "Soft Strings", { {"wave1",0},{"wave2",0},{"oct2",1},{"detune",14},{"mix",0.5f},{"cutoff",2600},{"reso",0.05f},{"envamt",0.1f},{"fdecay",2.0f},{"attack",0.45f},{"decay",1.0f},{"sustain",0.9f},{"release",1.0f},{"width",1.0f},{"space",0.6f},{"volume",-9} } },
            { "Brass Stab",   { {"wave1",0},{"wave2",0},{"oct2",1},{"detune",5},{"mix",0.5f},{"cutoff",900},{"reso",0.2f},{"envamt",0.7f},{"fdecay",0.35f},{"attack",0.03f},{"decay",0.4f},{"sustain",0.6f},{"release",0.25f},{"width",0.3f},{"space",0.3f},{"volume",-8} } },
            { "Pluck",        { {"wave1",1},{"wave2",0},{"oct2",2},{"detune",3},{"mix",0.3f},{"cutoff",500},{"reso",0.25f},{"envamt",0.8f},{"fdecay",0.18f},{"attack",0.001f},{"decay",0.45f},{"sustain",0.0f},{"release",0.35f},{"width",0.5f},{"space",0.4f},{"volume",-6} } },
        };
        for (int i = 0; i < 12; ++i) synth.addVoice (new Voice (*this));
        synth.addSound (new AnySound());
    }

    static APVTS::ParameterLayout layout()
    {
        const StringArray waves { "Saw", "Square", "Triangle", "Sine" };
        APVTS::ParameterLayout l;
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { "wave1", 1 }, "Osc 1", waves, 0));
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { "wave2", 1 }, "Osc 2", waves, 0));
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { "oct2", 1 }, "Osc 2 Octave", StringArray { "-1", "0", "+1" }, 1));
        l.add (knob ("detune",  "Detune",     { 0.0f, 50.0f }, 7.0f, " ct"));
        l.add (knob ("mix",     "Osc Mix",    { 0.0f, 1.0f }, 0.5f));
        l.add (knob ("cutoff",  "Cutoff",     skewed (40.0f, 18000.0f, 1200.0f), 2400.0f, " Hz"));
        l.add (knob ("reso",    "Resonance",  { 0.0f, 1.0f }, 0.2f));
        l.add (knob ("envamt",  "Filter Env", { 0.0f, 1.0f }, 0.4f));
        l.add (knob ("fdecay",  "Filter Decay", skewed (0.02f, 4.0f, 0.5f), 0.5f, " s"));
        l.add (knob ("attack",  "Attack",     skewed (0.001f, 4.0f, 0.3f), 0.005f, " s"));
        l.add (knob ("decay",   "Decay",      skewed (0.01f, 4.0f, 0.5f), 0.4f, " s"));
        l.add (knob ("sustain", "Sustain",    { 0.0f, 1.0f }, 0.75f));
        l.add (knob ("release", "Release",    skewed (0.01f, 5.0f, 0.6f), 0.3f, " s"));
        l.add (knob ("width",   "Width",      { 0.0f, 1.0f }, 0.3f));
        l.add (knob ("space",   "Space",      { 0.0f, 1.0f }, 0.2f));
        l.add (knob ("volume",  "Volume",     { -36.0f, 6.0f }, -8.0f, " dB"));
        return l;
    }

    Colour accent() const override { return Colour (0xff4ea1ff); }
    String tagline() const override { return "Two-oscillator analogue-style synth"; }

    void prepareToPlay (double sr, int bs) override
    {
        synth.setCurrentPlaybackSampleRate (sr);
        for (int i = 0; i < synth.getNumVoices(); ++i)
            if (auto* vc = dynamic_cast<Voice*> (synth.getVoice (i))) vc->prepare (sr);
        chorus.prepare ({ sr, (uint32) jmax (1, bs), 2 });
        chorus.setRate (0.6f); chorus.setDepth (0.25f); chorus.setCentreDelay (9.0f); chorus.setFeedback (0.0f);
        out.prepare (sr);
    }

    void releaseResources() override {}

    void processBlock (AudioBuffer<float>& b, MidiBuffer& midi) override
    {
        ScopedNoDenormals nd;
        b.clear();
        synth.renderNextBlock (b, midi, 0, b.getNumSamples());
        if (b.getNumChannels() < 2) return;

        const float w = v ("width");
        if (w > 0.001f)
        {
            chorus.setMix (0.5f * w);
            dsp::AudioBlock<float> blk (b);
            chorus.process (dsp::ProcessContextReplacing<float> (blk));
        }
        out.process (b, b.getNumSamples(), v ("space"), v ("volume"));
    }

private:
    struct Voice : public SynthesiserVoice
    {
        explicit Voice (SynthPlugin& o) : owner (o) {}
        bool canPlaySound (SynthesiserSound*) override { return true; }

        void prepare (double sr)
        {
            rate = sr;
            filter.prepare ({ sr, 512, 1 });
            filter.setType (dsp::StateVariableTPTFilterType::lowpass);
            amp.setSampleRate (sr);
            fenv.setSampleRate (sr);
        }

        void startNote (int note, float velocity, SynthesiserSound*, int) override
        {
            freq = MidiMessage::getMidiNoteInHertz (note);
            vel = velocity;
            if (! isVoiceActive() || ! amp.isActive()) { p1 = 0.0; p2 = Random::getSystemRandom().nextDouble(); filter.reset(); }
            amp.setParameters ({ owner.v ("attack"), owner.v ("decay"), owner.v ("sustain"), owner.v ("release") });
            fenv.setParameters ({ jmin (owner.v ("attack"), 0.05f), owner.v ("fdecay"), 0.0f, owner.v ("release") });
            amp.noteOn();
            fenv.noteOn();
        }

        void stopNote (float, bool allowTailOff) override
        {
            amp.noteOff(); fenv.noteOff();
            if (! allowTailOff) { amp.reset(); clearCurrentNote(); }
        }

        void pitchWheelMoved (int value) override { bend = std::pow (2.0, (value - 8192) / 8192.0 * 2.0 / 12.0); }
        void controllerMoved (int, int) override {}

        static float blep (double t, double dt)
        {
            if (t < dt)       { t /= dt; return (float) (t + t - t * t - 1.0); }
            if (t > 1.0 - dt) { t = (t - 1.0) / dt; return (float) (t * t + t + t + 1.0); }
            return 0.0f;
        }

        static float osc (int wave, double& ph, double dt)
        {
            float s = 0.0f;
            switch (wave)
            {
                case 0:  s = (float) (2.0 * ph - 1.0) - blep (ph, dt); break;
                case 1:  s = (ph < 0.5 ? 1.0f : -1.0f) + blep (ph, dt) - blep (std::fmod (ph + 0.5, 1.0), dt); break;
                case 2:  s = (float) (ph < 0.5 ? 4.0 * ph - 1.0 : 3.0 - 4.0 * ph); break;
                default: s = (float) std::sin (MathConstants<double>::twoPi * ph); break;
            }
            ph += dt;
            if (ph >= 1.0) ph -= 1.0;
            return s;
        }

        void renderNextBlock (AudioBuffer<float>& out, int start, int n) override
        {
            if (! isVoiceActive()) return;

            const int w1 = (int) owner.v ("wave1"), w2 = (int) owner.v ("wave2");
            const double oct = std::pow (2.0, (int) owner.v ("oct2") - 1);
            const double det = std::pow (2.0, owner.v ("detune") / 1200.0);
            const float mix = owner.v ("mix");
            const float cutoff = owner.v ("cutoff"), envAmt = owner.v ("envamt");
            filter.setResonance (0.7f + owner.v ("reso") * 8.0f);
            const float gain = 0.22f * (0.35f + 0.65f * vel);
            const double dt1 = freq * bend / rate, dt2 = freq * bend * oct * det / rate;

            for (int i = 0; i < n; ++i)
            {
                if ((i & 15) == 0)
                {
                    const float f = cutoff * std::pow (2.0f, fenv.getNextSample() * envAmt * 6.0f * (0.5f + 0.5f * vel));
                    filter.setCutoffFrequency (jlimit (30.0f, (float) (rate * 0.45), f));
                }
                else fenv.getNextSample();

                float s = osc (w1, p1, jmin (dt1, 0.45)) * (1.0f - mix) + osc (w2, p2, jmin (dt2, 0.45)) * mix;
                s = filter.processSample (0, s) * amp.getNextSample() * gain;
                out.addSample (0, start + i, s);
                if (out.getNumChannels() > 1) out.addSample (1, start + i, s);
            }

            if (! amp.isActive()) clearCurrentNote();
        }

        SynthPlugin& owner;
        double rate = 44100.0, freq = 440.0, p1 = 0.0, p2 = 0.0, bend = 1.0;
        float vel = 1.0f;
        ADSR amp, fenv;
        dsp::StateVariableTPTFilter<float> filter;
    };

    Synthesiser synth;
    dsp::Chorus<float> chorus;
    OutputStage out;
};

//==============================================================================
// TRACKLAYER KEYS: sampled grand piano (Salamander) + FM electric piano
//==============================================================================
struct PianoBank
{
    struct Zone
    {
        int root = 60, layer = 1;
        double rate = 48000.0;
        int frames = 0;
        std::vector<int16> data;   // interleaved stereo
    };

    std::vector<Zone> zones;
    std::array<std::array<int, 3>, 128> lookup {};   // note x layer -> zone index

    static PianoBank& get()
    {
        static PianoBank bank;
        static std::once_flag once;
        std::call_once (once, [] { bank.load(); });
        return bank;
    }

    void load()
    {
        ZipFile zip (new MemoryInputStream (BinaryData::piano_zip, (size_t) BinaryData::piano_zipSize, false), true);
        OggVorbisAudioFormat ogg;

        for (int i = 0; i < zip.getNumEntries(); ++i)
        {
            auto name = zip.getEntry (i)->filename;   // e.g. "060_m.ogg"
            std::unique_ptr<InputStream> in (zip.createStreamForEntry (i));
            if (in == nullptr) continue;
            std::unique_ptr<AudioFormatReader> reader (ogg.createReaderFor (in.release(), true));
            if (reader == nullptr) continue;

            Zone z;
            z.root = name.substring (0, 3).getIntValue();
            z.layer = name.containsChar ('p') ? 0 : name.containsChar ('f') ? 2 : 1;
            z.rate = reader->sampleRate;
            z.frames = (int) reader->lengthInSamples;
            AudioBuffer<float> buf (2, z.frames);
            reader->read (&buf, 0, z.frames, 0, true, true);
            z.data.resize ((size_t) z.frames * 2);
            for (int s = 0; s < z.frames; ++s)
                for (int c = 0; c < 2; ++c)
                    z.data[(size_t) (s * 2 + c)] = (int16) jlimit (-32767, 32767, roundToInt (buf.getSample (jmin (c, buf.getNumChannels() - 1), s) * 32767.0f));
            zones.push_back (std::move (z));
        }

        for (int note = 0; note < 128; ++note)
            for (int layer = 0; layer < 3; ++layer)
            {
                int best = -1, bestDist = 1000;
                for (int zi = 0; zi < (int) zones.size(); ++zi)
                    if (zones[(size_t) zi].layer == layer && std::abs (zones[(size_t) zi].root - note) < bestDist)
                        { best = zi; bestDist = std::abs (zones[(size_t) zi].root - note); }
                lookup[(size_t) note][(size_t) layer] = best;
            }
    }
};

class KeysPlugin : public Base
{
public:
    KeysPlugin() : Base ("tracklayer:keys", BusesProperties().withOutput ("Out", AudioChannelSet::stereo()), layout())
    {
        presets = {
            { "Grand Piano",        { {"instrument",0},{"tone",0.0f},{"release",0.25f},{"tremolo",0.0f},{"space",0.3f},{"volume",-4} } },
            { "Bright Piano",       { {"instrument",0},{"tone",0.6f},{"release",0.2f},{"tremolo",0.0f},{"space",0.2f},{"volume",-4} } },
            { "Felt Piano",         { {"instrument",0},{"tone",-0.7f},{"release",0.6f},{"tremolo",0.0f},{"space",0.55f},{"volume",-2} } },
            { "Electric Piano",     { {"instrument",1},{"tone",0.0f},{"release",0.3f},{"tremolo",0.0f},{"space",0.25f},{"volume",-4} } },
            { "Tremolo EP",         { {"instrument",1},{"tone",-0.2f},{"release",0.35f},{"tremolo",0.55f},{"space",0.35f},{"volume",-4} } },
        };
        PianoBank::get();   // decode the samples once (shared by every instance)
        for (int i = 0; i < 24; ++i) synth.addVoice (new Voice (*this));
        synth.addSound (new AnySound());
    }

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (std::make_unique<AudioParameterChoice> (ParameterID { "instrument", 1 }, "Instrument", StringArray { "Grand Piano", "Electric Piano" }, 0));
        l.add (knob ("tone",    "Tone",    { -1.0f, 1.0f }, 0.0f));
        l.add (knob ("release", "Release", skewed (0.05f, 2.0f, 0.3f), 0.25f, " s"));
        l.add (knob ("tremolo", "Tremolo", { 0.0f, 1.0f }, 0.0f));
        l.add (knob ("space",   "Space",   { 0.0f, 1.0f }, 0.3f));
        l.add (knob ("volume",  "Volume",  { -36.0f, 6.0f }, -4.0f, " dB"));
        return l;
    }

    Colour accent() const override { return Colour (0xffe8c547); }
    String tagline() const override { return "Grand piano (Salamander Grand Piano, CC-BY Alexander Holm) and electric piano"; }

    void prepareToPlay (double sr, int bs) override
    {
        rate = sr;
        synth.setCurrentPlaybackSampleRate (sr);
        toneFilter.prepare ({ sr, (uint32) jmax (1, bs), 2 });
        toneFilter.setType (dsp::StateVariableTPTFilterType::lowpass);
        toneFilter.setResonance (0.6f);
        out.prepare (sr);
    }

    void releaseResources() override {}

    void processBlock (AudioBuffer<float>& b, MidiBuffer& midi) override
    {
        ScopedNoDenormals nd;
        b.clear();
        const int n = b.getNumSamples();
        synth.renderNextBlock (b, midi, 0, n);
        if (b.getNumChannels() < 2) return;

        // Tone: darker (low-pass) to the left, brighter (treble lift) to the right
        const float tone = v ("tone");
        if (tone < -0.01f)
        {
            toneFilter.setCutoffFrequency (jmap (tone, -1.0f, 0.0f, 900.0f, 16000.0f));
            dsp::AudioBlock<float> blk (b);
            toneFilter.process (dsp::ProcessContextReplacing<float> (blk));
        }
        else if (tone > 0.01f)
        {
            for (int c = 0; c < 2; ++c)
            {
                auto* d = b.getWritePointer (c);
                for (int i = 0; i < n; ++i)
                {
                    const float hp = d[i] - lastIn[c] + 0.6f * lastHp[c];   // crude treble emphasis
                    lastIn[c] = d[i]; lastHp[c] = hp;
                    d[i] += hp * tone * 0.8f;
                }
            }
        }

        // Tremolo: stereo auto-pan like a suitcase EP
        const float trem = v ("tremolo");
        if (trem > 0.001f)
        {
            const double inc = MathConstants<double>::twoPi * 4.8 / rate;
            for (int i = 0; i < n; ++i)
            {
                const float m = (float) std::sin (tremPhase) * trem;
                tremPhase += inc;
                b.setSample (0, i, b.getSample (0, i) * (1.0f - jmax (0.0f, m)));
                b.setSample (1, i, b.getSample (1, i) * (1.0f - jmax (0.0f, -m)));
            }
            tremPhase = std::fmod (tremPhase, MathConstants<double>::twoPi);
        }

        out.process (b, n, v ("space"), v ("volume"));
    }

private:
    struct Voice : public SynthesiserVoice
    {
        explicit Voice (KeysPlugin& o) : owner (o) {}
        bool canPlaySound (SynthesiserSound*) override { return true; }

        void startNote (int note, float velocity, SynthesiserSound*, int) override
        {
            const double sr = getSampleRate();
            ep = owner.v ("instrument") > 0.5f;
            vel = velocity;
            releasing = false;
            relGain = 1.0f;
            const float relTime = owner.v ("release") * (note > 88 ? 3.0f : 1.0f);   // top keys have no dampers
            relMul = (float) std::exp (-1.0 / (jmax (0.01f, relTime) * 0.35 * sr));

            if (! ep)
            {
                auto& bank = PianoBank::get();
                const int layer = velocity < 0.4f ? 0 : velocity < 0.75f ? 1 : 2;
                zone = bank.lookup[(size_t) note][(size_t) layer];
                if (zone < 0) { clearCurrentNote(); return; }
                const auto& z = bank.zones[(size_t) zone];
                pos = 0.0;
                inc = std::pow (2.0, (note - z.root) / 12.0) * z.rate / sr;
                gain = 0.55f * (0.55f + 0.45f * velocity);
            }
            else
            {
                const double f = MidiMessage::getMidiNoteInHertz (note);
                cPh = mPh = bPh = 0.0;
                cInc = f / sr; mInc = f / sr; bInc = f * 14.0 / sr;
                t = 0.0; dt = 1.0 / sr;
                decay = 0.9 + 3.0 * std::pow (0.5, (note - 36) / 24.0);
                gain = 0.17f * (0.45f + 0.55f * velocity);
            }
        }

        void stopNote (float, bool allowTailOff) override
        {
            if (allowTailOff) releasing = true;
            else clearCurrentNote();
        }

        void pitchWheelMoved (int) override {}
        void controllerMoved (int, int) override {}

        void renderNextBlock (AudioBuffer<float>& out, int start, int n) override
        {
            if (! isVoiceActive()) return;
            const bool stereo = out.getNumChannels() > 1;

            for (int i = 0; i < n; ++i)
            {
                float l = 0.0f, r = 0.0f;

                if (! ep)
                {
                    const auto& z = PianoBank::get().zones[(size_t) zone];
                    const int idx = (int) pos;
                    if (idx + 1 >= z.frames) { clearCurrentNote(); return; }
                    const float frac = (float) (pos - idx);
                    const auto* d = z.data.data() + idx * 2;
                    l = (d[0] + (d[2] - d[0]) * frac) * (1.0f / 32767.0f);
                    r = (d[1] + (d[3] - d[1]) * frac) * (1.0f / 32767.0f);
                    pos += inc;
                }
                else
                {
                    const double env = std::exp (-t / decay);
                    const double modEnv = std::exp (-t / 0.45) * (1.2 + 2.8 * vel);
                    const double bell = std::exp (-t / 0.05) * vel * 0.9;
                    const double s = std::sin (MathConstants<double>::twoPi * cPh
                                               + modEnv * std::sin (MathConstants<double>::twoPi * mPh)
                                               + bell * std::sin (MathConstants<double>::twoPi * bPh));
                    l = r = (float) (s * env);
                    cPh += cInc; mPh += mInc; bPh += bInc;
                    if (cPh >= 1.0) cPh -= 1.0;
                    if (mPh >= 1.0) mPh -= 1.0;
                    if (bPh >= 1.0) bPh -= 1.0;
                    t += dt;
                    if (env < 0.0005) { clearCurrentNote(); return; }
                }

                if (releasing)
                {
                    relGain *= relMul;
                    if (relGain < 0.0005f) { clearCurrentNote(); return; }
                }

                const float g = gain * relGain;
                out.addSample (0, start + i, l * g);
                if (stereo) out.addSample (1, start + i, r * g);
            }
        }

        KeysPlugin& owner;
        bool ep = false, releasing = false;
        int zone = -1;
        double pos = 0.0, inc = 1.0;
        double cPh = 0, mPh = 0, bPh = 0, cInc = 0, mInc = 0, bInc = 0, t = 0, dt = 0, decay = 1.0;
        float vel = 1.0f, gain = 1.0f, relGain = 1.0f, relMul = 0.999f;
    };

    Synthesiser synth;
    dsp::StateVariableTPTFilter<float> toneFilter;
    OutputStage out;
    double rate = 44100.0, tremPhase = 0.0;
    float lastIn[2] {}, lastHp[2] {};
};

//==============================================================================
// TRACKLAYER BRIT AMP: a mid-forward British-style valve amp with a 4x12 cab
//==============================================================================
class BritAmp : public Base
{
public:
    BritAmp() : Base ("tracklayer:britamp", BusesProperties().withInput ("In", AudioChannelSet::stereo())
                                                             .withOutput ("Out", AudioChannelSet::stereo()), layout())
    {
        presets = {
            { "Classic Crunch",  { {"gain",5.5f},{"bass",5},{"middle",7},{"treble",6},{"presence",5},{"master",5},{"bright",1},{"cab",1},{"gate",0} } },
            { "Clean Edge",      { {"gain",2.5f},{"bass",5},{"middle",6},{"treble",6},{"presence",4},{"master",6},{"bright",1},{"cab",1},{"gate",0} } },
            { "Hot Lead",        { {"gain",8.5f},{"bass",4.5f},{"middle",8},{"treble",6.5f},{"presence",6},{"master",4.5f},{"bright",0},{"cab",1},{"gate",3} } },
            { "Rhythm Chug",     { {"gain",7.5f},{"bass",6},{"middle",6},{"treble",6},{"presence",5.5f},{"master",4.5f},{"bright",0},{"cab",1},{"gate",4} } },
            { "Into Your Own IR",{ {"gain",5.5f},{"bass",5},{"middle",7},{"treble",6},{"presence",5},{"master",5},{"bright",1},{"cab",0},{"gate",0} } },
        };
    }

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (knob ("gain",     "Gain",     { 0.0f, 10.0f }, 5.5f));
        l.add (knob ("bass",     "Bass",     { 0.0f, 10.0f }, 5.0f));
        l.add (knob ("middle",   "Middle",   { 0.0f, 10.0f }, 7.0f));
        l.add (knob ("treble",   "Treble",   { 0.0f, 10.0f }, 6.0f));
        l.add (knob ("presence", "Presence", { 0.0f, 10.0f }, 5.0f));
        l.add (knob ("master",   "Master",   { 0.0f, 10.0f }, 5.0f));
        l.add (knob ("gate",     "Gate",     { 0.0f, 10.0f }, 0.0f));
        l.add (std::make_unique<AudioParameterBool> (ParameterID { "bright", 1 }, "Bright", true));
        l.add (std::make_unique<AudioParameterBool> (ParameterID { "cab", 1 }, "Cabinet", true));
        return l;
    }

    Colour accent() const override { return Colour (0xffd9b45a); }
    String tagline() const override { return "Mid-forward British valve amp with a 4x12 cabinet. Turn Cabinet off to use your own IR."; }

    void prepareToPlay (double sr, int bs) override
    {
        rate = sr;
        mono.setSize (1, jmax (1, bs));
        oversampler.initProcessing ((size_t) jmax (1, bs));
        oversampler.reset();
        setLatencySamples ((int) oversampler.getLatencyInSamples());
        for (auto* f : allFilters()) f->reset();
        hpA = hpB = hpPrevA = hpPrevB = 0.0f;
        gateEnv = 0.0f; gateGain = 1.0f;
        lastKey = -1.0f;
        lastRate = 0.0;
    }

    void releaseResources() override {}

    void processBlock (AudioBuffer<float>& b, MidiBuffer&) override
    {
        ScopedNoDenormals nd;
        const int n = b.getNumSamples();
        if (mono.getNumSamples() < n) return;   // host broke its promise about block size
        updateFilters();

        // Sum to mono (a guitar is a mono source)
        auto* m = mono.getWritePointer (0);
        const int ins = getTotalNumInputChannels();
        for (int i = 0; i < n; ++i)
            m[i] = ins > 1 ? 0.5f * (b.getSample (0, i) + b.getSample (1, i)) : b.getSample (0, i);

        // Noise gate before the gain
        const float gate = v ("gate");
        if (gate > 0.05f)
        {
            const float thresh = Decibels::decibelsToGain (-78.0f + gate * 4.2f);
            const float atk = 1.0f - std::exp (-1.0f / (0.001f * (float) rate));
            const float rel = 1.0f - std::exp (-1.0f / (0.08f * (float) rate));
            for (int i = 0; i < n; ++i)
            {
                gateEnv += (std::abs (m[i]) - gateEnv) * (std::abs (m[i]) > gateEnv ? atk : rel * 0.2f);
                const float target = gateEnv > thresh ? 1.0f : 0.0f;
                gateGain += (target - gateGain) * (target > gateGain ? atk : rel);
                m[i] *= gateGain;
            }
        }

        // Pre-gain voicing: tight low end, mid push, optional bright cap
        for (int i = 0; i < n; ++i)
        {
            float x = preHp.processSample (m[i]);
            x = midPush.processSample (x);
            if (v ("bright") > 0.5f) x = bright.processSample (x);
            m[i] = x;
        }

        // Two cascaded valve stages at 4x oversampling
        const float g = v ("gain") / 10.0f;
        const float g1 = Decibels::decibelsToGain (4.0f + 26.0f * g);
        const float g2 = Decibels::decibelsToGain (2.0f + 22.0f * g * g);
        {
            dsp::AudioBlock<float> blk (mono.getArrayOfWritePointers(), 1, (size_t) n);
            auto up = oversampler.processSamplesUp (blk);
            auto* u = up.getChannelPointer (0);
            const int un = (int) up.getNumSamples();
            const float hpCoef = 1.0f - (MathConstants<float>::twoPi * 30.0f / (float) (rate * 4.0));
            for (int i = 0; i < un; ++i)
            {
                float x = std::tanh (u[i] * g1 + 0.25f) - 0.2449f;       // stage 1 (asymmetric)
                const float a = x - hpPrevA + hpCoef * hpA; hpPrevA = x; hpA = a;   // coupling cap
                x = std::tanh (a * g2 * 1.6f - 0.12f) + 0.1194f;          // stage 2
                const float c = x - hpPrevB + hpCoef * hpB; hpPrevB = x; hpB = c;
                u[i] = c;
            }
            oversampler.processSamplesDown (blk);
        }

        // Tone stack, power amp, presence, cabinet, master
        const bool cab = v ("cab") > 0.5f;
        const float master = Decibels::decibelsToGain (-26.0f + v ("master") * 3.2f);
        for (int i = 0; i < n; ++i)
        {
            float x = bassF.processSample (m[i]);
            x = midF.processSample (x);
            x = trebleF.processSample (x);
            x = std::tanh (x * 1.4f) * 0.75f;              // gentle power-amp squash
            x = presenceF.processSample (x);
            if (cab)
            {
                x = cabHp.processSample (x);
                x = cabLowRes.processSample (x);
                x = cabUpper.processSample (x);
                x = cabLp1.processSample (x);
                x = cabLp2.processSample (x);
            }
            m[i] = x * master;
        }

        for (int c = 0; c < b.getNumChannels(); ++c)
            b.copyFrom (c, 0, mono, 0, 0, n);
    }

private:
    using Filt = dsp::IIR::Filter<float>;
    using Coefs = dsp::IIR::Coefficients<float>;

    std::vector<Filt*> allFilters()
    {
        return { &preHp, &midPush, &bright, &bassF, &midF, &trebleF, &presenceF, &cabHp, &cabLowRes, &cabUpper, &cabLp1, &cabLp2 };
    }

    void updateFilters()
    {
        const float key = v ("bass") * 1000.0f + v ("middle") * 100.0f + v ("treble") * 10.0f + v ("presence") + (float) rate;
        if (approximatelyEqual (key, lastKey)) return;
        const bool first = ! approximatelyEqual (rate, lastRate);
        lastKey = key;
        lastRate = rate;
        const auto sr = rate;
        auto db = [] (float knobValue, float range) { return Decibels::decibelsToGain ((knobValue - 5.0f) / 5.0f * range); };

        if (first)
        {
            *preHp.coefficients   = *Coefs::makeHighPass (sr, 110.0f, 0.6f);
            *midPush.coefficients = *Coefs::makePeakFilter (sr, 850.0f, 0.7f, Decibels::decibelsToGain (5.0f));
            *bright.coefficients  = *Coefs::makeHighShelf (sr, 2800.0f, 0.6f, Decibels::decibelsToGain (5.0f));
            *cabHp.coefficients     = *Coefs::makeHighPass (sr, 75.0f, 0.75f);
            *cabLowRes.coefficients = *Coefs::makePeakFilter (sr, 115.0f, 1.2f, Decibels::decibelsToGain (3.5f));
            *cabUpper.coefficients  = *Coefs::makePeakFilter (sr, 2300.0f, 1.3f, Decibels::decibelsToGain (3.0f));
            *cabLp1.coefficients    = *Coefs::makeLowPass (sr, 5200.0f, 0.7f);
            *cabLp2.coefficients    = *Coefs::makeLowPass (sr, 6500.0f, 0.6f);
        }
        *bassF.coefficients     = *Coefs::makeLowShelf (sr, 140.0f, 0.7f, db (v ("bass"), 12.0f));
        *midF.coefficients      = *Coefs::makePeakFilter (sr, 700.0f, 0.8f, db (v ("middle"), 10.0f) * Decibels::decibelsToGain (1.5f));
        *trebleF.coefficients   = *Coefs::makeHighShelf (sr, 2600.0f, 0.7f, db (v ("treble"), 12.0f));
        *presenceF.coefficients = *Coefs::makeHighShelf (sr, 4200.0f, 0.7f, db (v ("presence"), 8.0f));
    }

    double rate = 44100.0;
    AudioBuffer<float> mono;
    dsp::Oversampling<float> oversampler { 1, 2, dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true };
    Filt preHp { Coefs::makeAllPass (44100.0, 1000.0f) }, midPush { Coefs::makeAllPass (44100.0, 1000.0f) },
         bright { Coefs::makeAllPass (44100.0, 1000.0f) }, bassF { Coefs::makeAllPass (44100.0, 1000.0f) },
         midF { Coefs::makeAllPass (44100.0, 1000.0f) }, trebleF { Coefs::makeAllPass (44100.0, 1000.0f) },
         presenceF { Coefs::makeAllPass (44100.0, 1000.0f) }, cabHp { Coefs::makeAllPass (44100.0, 1000.0f) },
         cabLowRes { Coefs::makeAllPass (44100.0, 1000.0f) }, cabUpper { Coefs::makeAllPass (44100.0, 1000.0f) },
         cabLp1 { Coefs::makeAllPass (44100.0, 1000.0f) }, cabLp2 { Coefs::makeAllPass (44100.0, 1000.0f) };
    float hpA = 0, hpB = 0, hpPrevA = 0, hpPrevB = 0, gateEnv = 0, gateGain = 1, lastKey = -1;
    double lastRate = 0.0;
};

//==============================================================================
// Format
//==============================================================================
static std::unique_ptr<AudioPluginInstance> create (const String& id)
{
    if (id == "tracklayer:synth")   return std::make_unique<SynthPlugin>();
    if (id == "tracklayer:keys")    return std::make_unique<KeysPlugin>();
    if (id == "tracklayer:britamp") return std::make_unique<BritAmp>();
    return {};
}

void Format::findAllTypesForFile (OwnedArray<PluginDescription>& results, const String& id)
{
    for (auto& d : getTypes())
        if (d.fileOrIdentifier == id)
            results.add (new PluginDescription (d));
}

String Format::getNameOfPluginFromIdentifier (const String& id)
{
    for (auto& d : getTypes())
        if (d.fileOrIdentifier == id)
            return d.name;
    return id;
}

void Format::createPluginInstance (const PluginDescription& d, double sr, int bs, PluginCreationCallback callback)
{
    auto p = create (d.fileOrIdentifier);
    if (p == nullptr)
    {
        callback (nullptr, "Unknown built-in plugin: " + d.name);
        return;
    }
    p->setRateAndBufferSizeDetails (sr, bs);
    callback (std::move (p), {});
}

} // namespace BuiltIn
