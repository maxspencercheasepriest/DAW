#include "Drummer.h"

using namespace juce;

namespace Drummer
{

enum Style
{
    straight8 = 0, driving16, halfTime, rideGroove, fourOnFloor, motorik, floorToms
};

StringArray styleNames()
{
    return { "Rock - Straight 8ths", "Rock - Driving 16ths", "Rock - Half-Time", "Rock - Ride Groove",
             "Indie - Four on the Floor", "Indie - Motorik", "Indie - Floor Toms" };
}

StringArray cymbalNames()
{
    return { "Auto", "Hi-Hat", "Ride" };
}

String noteName (int n)
{
    switch (n)
    {
        case kick: return "Kick";            case sideStick: return "Side Stick";
        case snare: return "Snare";          case clap: return "Clap";
        case floorTomLow: return "Floor Tom 2"; case closedHat: return "Hi-Hat Closed";
        case floorTomHigh: return "Floor Tom 1"; case pedalHat: return "Hi-Hat Pedal";
        case tomLow: return "Low Tom";       case openHat: return "Hi-Hat Open";
        case tomLowMid: return "Low-Mid Tom"; case tomHighMid: return "Hi-Mid Tom";
        case crash: return "Crash";          case tomHigh: return "High Tom";
        case ride: return "Ride";            case rideBell: return "Ride Bell";
        default: return MidiMessage::getMidiNoteName (n, true, true, 3);
    }
}

std::vector<int> groupsFor (int num, int den)
{
    num = jlimit (1, 32, num);
    if (den >= 8)
    {
        // compound / odd eighths: groups of 3, with 2s to make up the rest
        switch (num)
        {
            case 2:  return { 2 };
            case 3:  return { 3 };
            case 4:  return { 2, 2 };
            case 5:  return { 3, 2 };
            case 6:  return { 3, 3 };
            case 7:  return { 2, 2, 3 };
            case 8:  return { 3, 3, 2 };
            case 9:  return { 3, 3, 3 };
            case 10: return { 3, 3, 2, 2 };
            case 11: return { 3, 3, 3, 2 };
            case 12: return { 3, 3, 3, 3 };
            default: break;
        }
        std::vector<int> g;
        int left = num;
        while (left > 4) { g.push_back (3); left -= 3; }
        if (left == 4) { g.push_back (2); g.push_back (2); }
        else if (left > 0) g.push_back (left);
        return g;
    }

    // simple time: each beat is its own pulse; group in 2s (and a 3 when odd)
    std::vector<int> g;
    int left = num;
    while (left > 3) { g.push_back (2); left -= 2; }
    g.push_back (left);
    return g;
}

namespace
{
    struct Rng
    {
        uint32 s;
        explicit Rng (uint32 seed) : s (seed * 2654435761u + 12345u) { next(); }
        uint32 next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
        float unit() { return (float) (next() & 0xffffff) / (float) 0xffffff; }
        bool chance (float p) { return unit() < p; }
    };

    struct Builder
    {
        std::vector<MidiNote> notes;
        double stepSec = 0.1, swingSec = 0.0, barStart = 0.0;
        Rng humanise { 7 };

        void hit (int step, int note, int vel, double nudge = 0.0)
        {
            double t = barStart + step * stepSec + nudge;
            if ((step & 1) == 1)       t += swingSec * 0.66;   // swung 16ths
            else if ((step & 3) == 2)  t += swingSec;          // swung 8ths
            t += (humanise.unit() - 0.5) * 0.006;               // +-3 ms
            vel += (int) ((humanise.unit() - 0.5) * 10.0);
            notes.push_back ({ jmax (0.0, t), jmin (stepSec * 0.9, 0.12), note, jlimit (1, 127, vel) });
        }
    };
}

std::vector<MidiNote> generate (const DrummerSettings& s, double tempo, int num, int den)
{
    num = jlimit (1, 32, num);
    den = (den == 2 || den == 8 || den == 16) ? den : 4;

    const double quarter = 60.0 / jmax (20.0, tempo);
    const double unitSec = quarter * 4.0 / den;              // one beat unit
    const int stepsPerUnit = den >= 8 ? 2 : (den == 2 ? 8 : 4);   // always 16th-note steps
    const double stepSec = unitSec / stepsPerUnit;
    const int stepsPerBar = num * stepsPerUnit;
    const double barSec = unitSec * num;

    const auto groups = groupsFor (num, den);
    const bool compound = den >= 8;

    // Pulses ("big beats"): in simple time every beat, in compound time every group.
    std::vector<int> pulseStep;      // step index of each pulse
    std::vector<int> pulseLen;       // length of each pulse in steps
    {
        int unit = 0;
        for (int g : groups)
        {
            if (compound) { pulseStep.push_back (unit * stepsPerUnit); pulseLen.push_back (g * stepsPerUnit); }
            else for (int k = 0; k < g; ++k) { pulseStep.push_back ((unit + k) * stepsPerUnit); pulseLen.push_back (stepsPerUnit); }
            unit += g;
        }
    }
    const int pulses = (int) pulseStep.size();

    // Which pulses carry the backbeat (snare)
    std::vector<bool> isBack ((size_t) pulses, false);
    if (s.style == halfTime)
    {
        isBack[(size_t) (pulses >= 4 ? pulses / 2 : pulses - 1)] = pulses > 1;
    }
    else
    {
        for (int i = 1; i < pulses; ++i)
            isBack[(size_t) i] = (i == pulses - 1) || (i % 2 == 1);
        if (pulses == 3 && ! compound) isBack[1] = false;   // 3/4: kick, -, snare
    }

    const float cx = jlimit (0.0f, 1.0f, s.complexity);
    const float ld = jlimit (0.0f, 1.0f, s.loudness);
    const float fl = jlimit (0.0f, 1.0f, s.fills);
    const int baseVel = 48 + (int) (ld * 72.0f);

    // Time-keeping voice
    int keeper = closedHat;
    if (s.cymbal == 2 || (s.cymbal == 0 && s.style == rideGroove)) keeper = ride;
    if (s.style == floorToms && s.cymbal == 0) keeper = floorTomHigh;
    const bool sideStickSnare = ld < 0.22f;

    // Time-keeping rate in steps: 16ths, 8ths or quarters
    int rate = 2;
    if (s.style == driving16) rate = 1;
    else if (cx < 0.18f && s.style != motorik) rate = 4;
    else if (cx > 0.8f && (s.style == straight8 || s.style == halfTime)) rate = 1;
    if (compound) rate = jmin (rate, 2);

    const int fillEvery = fl < 0.05f ? 0 : (fl < 0.4f ? 8 : 4);
    const int fillSteps = jlimit (2, stepsPerBar, (int) std::round (jmap (fl, 0.0f, 1.0f, (float) stepsPerUnit, (float) stepsPerUnit * 2.5f)));

    Builder b;
    b.stepSec = stepSec;
    b.swingSec = jlimit (0.0f, 1.0f, s.swing) * stepSec * 0.5;
    b.humanise = Rng ((uint32) (s.seed * 31 + 3));

    bool crashNext = ld > 0.3f;   // crash on the region's first downbeat

    for (int bar = 0; bar < jmax (1, s.bars); ++bar)
    {
        Rng rng ((uint32) (s.seed * 7919 + bar * 104729 + s.style * 13));
        b.barStart = bar * barSec;

        const bool isLastBar = bar == s.bars - 1;
        const bool fillBar = fillEvery > 0 && (isLastBar || ((bar + 1) % fillEvery == 0));
        const int fillStart = fillBar ? stepsPerBar - fillSteps : stepsPerBar;
        // a small variation every 2nd bar
        const bool variant = (bar % 2) == 1;

        // ---- crash / downbeat ----
        if (crashNext)
        {
            b.hit (0, crash, baseVel + 12);
            crashNext = false;
        }

        // ---- kick & snare on the pulses ----
        for (int p = 0; p < pulses; ++p)
        {
            const int st = pulseStep[(size_t) p];
            if (st >= fillStart) break;

            const bool waltzGap = pulses == 3 && ! compound && p == 1 && cx < 0.5f;   // 3/4: kick, -, snare
            const bool halfTimeGap = s.style == halfTime && p != 0 && ! isBack[(size_t) p]
                                     && ! (p == pulses - 1 && rng.chance (cx * 0.6f));
            if (isBack[(size_t) p])
                b.hit (st, sideStickSnare ? sideStick : snare, baseVel + 8);
            else if (! waltzGap && ! halfTimeGap)
                b.hit (st, kick, baseVel + (p == 0 ? 10 : 4));

            if (s.style == fourOnFloor && isBack[(size_t) p])
                b.hit (st, kick, baseVel + 2);   // kick under the snare too
        }

        // ---- extra kicks, by complexity ----
        for (int p = 0; p < pulses; ++p)
        {
            const int st = pulseStep[(size_t) p], len = pulseLen[(size_t) p];
            const int half = st + len / 2;     // the "and"
            if (half >= fillStart) continue;

            const bool beforeBack = (p + 1 < pulses) && isBack[(size_t) (p + 1)];
            if (s.style == motorik)
            {
                if (! isBack[(size_t) p] && rng.chance (0.35f + cx * 0.5f)) b.hit (half, kick, baseVel - 4);
            }
            else if (s.style != fourOnFloor)
            {
                if (isBack[(size_t) p] && rng.chance (cx * 0.55f + (variant ? 0.15f : 0.0f)))
                    b.hit (half, kick, baseVel - 6);                     // "and" after the snare
                if (beforeBack && len >= 4 && rng.chance (cx * cx * 0.6f))
                    b.hit (st + len - 1, kick, baseVel - 12);            // 16th pickup into the backbeat
            }
        }

        // ---- ghost notes ----
        if (cx > 0.5f && ! sideStickSnare)
        {
            const float pGhost = (cx - 0.5f) * 0.7f;
            for (int st = 1; st < fillStart; ++st)
            {
                bool nearMain = false;
                for (int p = 0; p < pulses; ++p)
                    if (std::abs (st - pulseStep[(size_t) p]) < 2 && isBack[(size_t) p]) nearMain = true;
                if (! nearMain && (st % 2 == 1) && rng.chance (pGhost))
                    b.hit (st, snare, 24 + (int) (ld * 18.0f));
            }
        }

        // ---- time-keeping ----
        for (int st = 0; st < fillStart; st += rate)
        {
            bool onPulse = false;
            for (int p : pulseStep) onPulse = onPulse || p == st;
            const bool offbeat8 = (st % 4) == 2 || (compound && (st % 2) == 0 && ! onPulse);

            int note = keeper;
            int vel = baseVel - (onPulse ? 4 : (st % 2 == 1 ? 26 : 16));

            if (s.style == fourOnFloor && keeper == closedHat && (st % 4) == 2 && ! compound)
                { note = openHat; vel = baseVel - 6; }              // disco-indie open hats on the offbeat
            else if (keeper == closedHat && ld > 0.82f && offbeat8 && rng.chance (0.25f))
                note = openHat;                                      // loose hats when playing hard
            else if (keeper == ride && onPulse && cx > 0.6f)
                note = rideBell;

            if (s.style == floorToms && keeper == floorTomHigh)
                vel = baseVel - (onPulse ? 0 : 10);

            b.hit (st, note, vel);
        }

        // open hat at the end of a phrase
        if (! fillBar && keeper == closedHat && variant && cx > 0.3f && rng.chance (0.5f))
            b.hit (stepsPerBar - 2, openHat, baseVel - 8);

        // ---- fill ----
        if (fillBar)
        {
            const int kind = (int) (rng.next() % 3);   // 0 snare roll, 1 tom run, 2 snare + toms
            const int fillRate = cx < 0.3f ? 2 : 1;
            const int toms[] = { tomHigh, tomHighMid, tomLowMid, tomLow, floorTomHigh, floorTomLow };
            const int count = (stepsPerBar - fillStart + fillRate - 1) / fillRate;
            int i = 0;
            for (int st = fillStart; st < stepsPerBar; st += fillRate, ++i)
            {
                const float prog = count > 1 ? (float) i / (float) (count - 1) : 1.0f;
                const int vel = baseVel - 14 + (int) (prog * 26.0f);
                int note = snare;
                if (kind == 1 || (kind == 2 && prog > 0.4f))
                    note = toms[jlimit (0, 5, (int) (prog * 5.99f))];
                b.hit (st, note, vel);
                if (i == 0 || (cx > 0.6f && (st % 4) == 0)) b.hit (st, kick, vel - 6);
            }
            crashNext = ld > 0.25f;   // land the fill on a crash
        }
    }

    // Keep everything inside the region
    const double total = barSec * jmax (1, s.bars);
    std::vector<MidiNote> out;
    out.reserve (b.notes.size());
    for (auto& n : b.notes)
        if (n.time < total - 0.001)
            out.push_back (n);
    std::sort (out.begin(), out.end(), [] (const MidiNote& a, const MidiNote& c) { return a.time < c.time; });
    return out;
}

} // namespace Drummer
