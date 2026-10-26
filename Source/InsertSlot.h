#pragma once

#include "AppContext.h"

//==============================================================================
// A plugin slot. slot == -1 is a track's Instrument slot; 0.. are Audio FX inserts.
// Empty: click to choose a plugin. Filled: click to open it, right-click for options.
class InsertSlot : public juce::Component, public juce::SettableTooltipClient
{
public:
    InsertSlot (AppContext& c, TrackPtr t, int s) : ctx (c), track (std::move (t)), slot (s)
    {
        Theme::noFocus (*this);
        refresh();
    }

    bool isInstrumentSlot() const { return slot < 0; }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        auto* p = track->pluginAt (slot);
        const bool byp = ! isInstrumentSlot() && track->bypassed[(size_t) slot].load();

        if (p == nullptr)
        {
            g.setColour (isMouseOver() ? Theme::panelLight : Theme::panelDark);
            g.fillRoundedRectangle (r, 2.0f);
            g.setColour (juce::Colour (0xff3a3a40));
            g.drawRoundedRectangle (r, 2.0f, 1.0f);
            if (isInstrumentSlot())
            {
                g.setColour (Theme::textDim);
                g.setFont (Theme::font (11.0f));
                g.drawFittedText ("Choose instrument...", getLocalBounds().reduced (4, 0), juce::Justification::centred, 1, 0.8f);
            }
            return;
        }

        const auto fill = isInstrumentSlot() ? juce::Colour (0xff3f7a4a) : juce::Colour (0xff3b5f86);
        g.setColour (byp ? juce::Colour (0xff3a3a40) : fill.brighter (isMouseOver() ? 0.1f : 0.0f));
        g.fillRoundedRectangle (r, 2.0f);
        g.setColour (byp ? Theme::textDim : juce::Colours::white);
        g.setFont (Theme::font (11.0f, true));
        g.drawFittedText (p->getName(), getLocalBounds().reduced (3, 0), juce::Justification::centred, 1, 0.8f);
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { repaint(); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! getLocalBounds().contains (e.getPosition()))
            return;

        auto* p = track->pluginAt (slot);
        if (p == nullptr || e.mods.isPopupMenu())
            showMenu (p != nullptr);
        else
            ctx.openPlugin (track, slot);
    }

    void refresh()
    {
        auto* p = track->pluginAt (slot);
        if (isInstrumentSlot())
            setTooltip (p == nullptr ? "Click to choose an instrument plugin"
                                     : p->getName() + " - click to open, right-click to change");
        else
            setTooltip (p == nullptr ? "Click to add an effect plugin"
                                     : p->getName() + " - click to open, right-click for options");
        repaint();
    }

private:
    void showMenu (bool filled)
    {
        juce::PopupMenu m;
        auto types = isInstrumentSlot() ? ctx.engine.getInstrumentPlugins() : ctx.engine.getEffectPlugins();
        const auto* current = track->pluginAt (slot);

        if (filled)
        {
            m.addItem (1, "Open");
            if (! isInstrumentSlot())
                m.addItem (2, "Bypass", true, track->bypassed[(size_t) slot].load());
            m.addItem (3, "Remove");
            m.addSeparator();
        }

        juce::PopupMenu pluginMenu;
        juce::KnownPluginList::addToMenu (pluginMenu, types, juce::KnownPluginList::sortByManufacturer,
                                          current != nullptr ? current->getPluginDescription().createIdentifierString() : juce::String());
        if (types.isEmpty())
            m.addItem (4, isInstrumentSlot() ? "No instruments found - open Plugin Manager..."
                                             : "No plugins found - open Plugin Manager...");
        else if (filled)
            m.addSubMenu ("Replace With", pluginMenu);
        else
            for (juce::PopupMenu::MenuItemIterator it (pluginMenu); it.next();)
                m.addItem (it.getItem());

        m.addSeparator();
        m.addItem (5, "Plugin Manager...");

        juce::Component::SafePointer<InsertSlot> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [safe, types] (int result)
        {
            if (safe == nullptr || result == 0)
                return;
            auto& self = *safe;

            switch (result)
            {
                case 1: self.ctx.openPlugin (self.track, self.slot); return;
                case 2:
                    self.track->bypassed[(size_t) self.slot] = ! self.track->bypassed[(size_t) self.slot].load();
                    self.ctx.engine.dirty = true;
                    self.repaint();
                    return;
                case 3: self.ctx.removePlugin (self.track, self.slot); return;
                case 4:
                case 5: self.ctx.runCommand (cmdPluginManager); return;
                default: break;
            }

            const int idx = juce::KnownPluginList::getIndexChosenByMenu (types, result);
            if (idx >= 0)
                self.ctx.loadPlugin (self.track, self.slot, types.getReference (idx));
        });
    }

    AppContext& ctx;
    TrackPtr track;
    int slot;
};
