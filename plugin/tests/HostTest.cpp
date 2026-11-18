// SPDX-License-Identifier: AGPL-3.0-only
// Vintage Chonk VST3 host test: loads the built plugin the way a DAW does (JUCE's VST3 hosting), plays
// MIDI into it at 44.1 / 48 / 96 kHz with block sizes 32 / 64 / 441 / 512 / 1024 and checks:
//   * sound: non-silent, finite, no overload;
//   * block boundaries: every block size gives the same output (bit for bit), and at 44.1 kHz the
//     plugin equals the engine rendered offline (dsp/, events at the nearest 128-frame block, 64 later);
//   * resampler: the kernel's response (pass band, image rejection), the reported latency, and the
//     resampled output against an exact offline resample of the offline engine render;
//   * state save / restore round trip, host programs, parameter automation moves the sound, host tempo;
//   * EngineRunner directly: no heap allocation while processing;
//   * the editor (WebView2 panel) opens, the panel talks to the plugin both ways, and a screenshot;
//   * the VST3 parameter list a DAW shows: only ours (+ program / bypass) without kIsHidden (JUCE patch);
//   * mod wheel, pitch bend and sustain reach the engine through the VST3 path (IMidiMapping);
//   * user presets of the pre-rename folder are migrated once.
//   VintageChonkHostTest "<path>\Vintage Chonk.vst3" <out dir> [--no-screenshot]
#define NOMINMAX
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <set>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include "EngineRunner.h"
#include "PresetStore.h"
#include "Params.h"
#include "factory_presets.h"

// ------------------------------------------------------------ allocation counter (this thread) ---
static thread_local bool countAllocs = false;
static std::atomic<long> allocs { 0 };
void* operator new (size_t n)
{
    if (countAllocs) allocs++;
    if (void* p = std::malloc (n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[] (size_t n) { return operator new (n); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, size_t) noexcept { std::free (p); }
void operator delete[] (void* p, size_t) noexcept { std::free (p); }

static int passes = 0, fails = 0;
static void check (bool ok, const juce::String& what)
{
    if (ok) passes++;
    else fails++;
    std::printf ("%s %s\n", ok ? "  ok  " : "  FAIL", what.toRawUTF8());
    std::fflush (stdout);
}
static void info (const juce::String& s) { std::printf ("        %s\n", s.toRawUTF8()); std::fflush (stdout); }

// ------------------------------------------------------------------------------- MIDI script ---
struct Ev { double t; uint8_t b[3]; };
static std::vector<Ev> script()
{
    return {
        { 0.050, { 0x90, 48, 100 } }, { 0.100, { 0x90, 55, 90 } }, { 0.150, { 0x90, 60, 110 } },
        { 0.300, { 0xB0, 1, 80 } },   { 0.500, { 0xE0, 0x20, 0x5D } }, { 0.600, { 0xD0, 90, 0 } },
        { 0.800, { 0xB0, 64, 127 } }, { 1.000, { 0x80, 48, 0 } }, { 1.000, { 0x80, 55, 0 } },
        { 1.010, { 0x80, 60, 0 } },   { 1.200, { 0xE0, 0x00, 0x40 } }, { 1.400, { 0xB0, 64, 0 } },
        { 1.500, { 0x90, 67, 120 } }, { 1.733, { 0x90, 70, 64 } }, { 2.000, { 0x80, 67, 0 } },
        { 2.050, { 0x90, 72, 100 } }, { 2.300, { 0x80, 72, 0 } },  { 2.310, { 0x80, 70, 0 } },
    };
}
static int evSize (const Ev& e) { return (e.b[0] & 0xF0) == 0xD0 || (e.b[0] & 0xF0) == 0xC0 ? 2 : 3; }

struct TempoHead final : juce::AudioPlayHead
{
    double bpm = 120.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setBpm (bpm);
        p.setIsPlaying (true);
        return p;
    }
};

static juce::AudioBuffer<float> renderPlugin (juce::AudioPluginInstance& p, double fs, int bs, double seconds,
                                              const std::vector<Ev>& evs, std::function<void (int64_t)> perBlock = {})
{
    p.setRateAndBufferSizeDetails (fs, bs);
    p.prepareToPlay (fs, bs);
    const int total = (int) std::lround (seconds * fs);
    juce::AudioBuffer<float> out (2, total), buf (2, bs);
    juce::MidiBuffer midi;
    for (int pos = 0; pos < total; pos += bs)
    {
        const int n = std::min (bs, total - pos);
        if (perBlock) perBlock (pos);
        midi.clear();
        for (const auto& e : evs)
        {
            const int64_t m = std::llround (e.t * fs);
            if (m >= pos && m < pos + n) midi.addEvent (e.b, evSize (e), (int) (m - pos));
        }
        buf.setSize (2, n, false, false, true);
        buf.clear();
        p.processBlock (buf, midi);
        for (int c = 0; c < 2; c++) out.copyFrom (c, pos, buf, c, 0, n);
    }
    p.releaseResources();
    return out;
}

struct Stats { double rms = 0, peak = 0; bool finite = true; };
static Stats stats (const juce::AudioBuffer<float>& b, int from = 0, int to = -1)
{
    Stats s;
    double sum = 0;
    int64_t cnt = 0;
    if (to < 0) to = b.getNumSamples();
    for (int c = 0; c < b.getNumChannels(); c++)
        for (int i = from; i < to; i++)
        {
            const float v = b.getSample (c, i);
            if (! std::isfinite (v)) { s.finite = false; continue; }
            sum += (double) v * v;
            s.peak = std::max (s.peak, (double) std::fabs (v));
            cnt++;
        }
    s.rms = cnt ? std::sqrt (sum / (double) cnt) : 0;
    return s;
}
static double db (double x) { return 20.0 * std::log10 (std::max (x, 1e-12)); }
static double maxDiff (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    double d = 0;
    const int n = std::min (a.getNumSamples(), b.getNumSamples());
    for (int c = 0; c < 2; c++)
        for (int i = 0; i < n; i++) d = std::max (d, (double) std::fabs (a.getSample (c, i) - b.getSample (c, i)));
    return d;
}
// brightness: RMS of the first difference over RMS
static double brightness (const juce::AudioBuffer<float>& b, int from, int to)
{
    double e = 0, d = 0;
    for (int i = from + 1; i < to; i++)
    {
        const double x = b.getSample (0, i), px = b.getSample (0, i - 1);
        e += x * x;
        d += (x - px) * (x - px);
    }
    return e > 0 ? std::sqrt (d / e) : 0;
}

// the engine alone at 44.1 kHz, events applied at the 128-frame block nearest to their time
static juce::AudioBuffer<float> renderReference (const mfe_patch_t& patch, int frames, const std::vector<Ev>& evs)
{
    mfm1::initTablesOnce();
    auto* e = static_cast<mfe_engine_t*> (std::calloc (1, mfe_engine_size()));
    mfe_engine_init (e);
    mfe_patch_t tmp = patch;
    if (tmp.max_voices == 8) tmp.max_voices = MFE_MAX_VOICES;
    mfe_engine_set_patch (e, &tmp);
    mfe_set_tempo (e, 120u << 8);
    juce::AudioBuffer<float> out (2, frames);
    out.clear();
    size_t next = 0;
    int32_t blk[MFE_BLOCK * 2];
    for (int b = 0; b < frames; b += MFE_BLOCK)
    {
        while (next < evs.size() && std::llround (evs[next].t * 44100.0) < b + MFE_BLOCK / 2)
        {
            mfe_midi (e, evs[next].b, (size_t) evSize (evs[next]));
            next++;
        }
        mfe_render (e, blk);
        for (int i = 0; i < MFE_BLOCK && b + i < frames; i++)
        {
            out.setSample (0, b + i, (float) blk[2 * i] * (1.0f / 32768.0f));
            out.setSample (1, b + i, (float) blk[2 * i + 1] * (1.0f / 32768.0f));
        }
    }
    std::free (e);
    return out;
}

static juce::Image captureClient (HWND h)
{
    RECT r;
    GetClientRect (h, &r);
    const int w = r.right - r.left, hh = r.bottom - r.top;
    juce::Image img (juce::Image::RGB, std::max (1, w), std::max (1, hh), true);
    if (w <= 0 || hh <= 0) return img;
    HDC wdc = GetDC (h);
    HDC mdc = CreateCompatibleDC (wdc);
    HBITMAP bmp = CreateCompatibleBitmap (wdc, w, hh);
    auto old = SelectObject (mdc, bmp);
    PrintWindow (h, mdc, PW_CLIENTONLY | 2 /* PW_RENDERFULLCONTENT */);
    BITMAPINFO bi {};
    bi.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -hh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> px ((size_t) w * (size_t) hh * 4);
    GetDIBits (mdc, bmp, 0, (UINT) hh, px.data(), &bi, DIB_RGB_COLORS);
    {
        juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < hh; y++)
            for (int x = 0; x < w; x++)
            {
                const uint8_t* p = &px[((size_t) y * (size_t) w + (size_t) x) * 4];
                bd.setPixelColour (x, y, juce::Colour (p[2], p[1], p[0]));
            }
    }
    SelectObject (mdc, old);
    DeleteObject (bmp);
    DeleteDC (mdc);
    ReleaseDC (h, wdc);
    return img;
}

// the plugin's own state record inside the VST3 host's wrapper (VST3PluginState / IComponent, base64)
static std::unique_ptr<juce::XmlElement> pluginState (juce::AudioPluginInstance& p)
{
    juce::MemoryBlock mb;
    p.getStateInformation (mb);
    auto host = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
    auto* comp = host != nullptr ? host->getChildByName ("IComponent") : nullptr;
    if (comp == nullptr)
    {
        info ("host state without IComponent: " + (host != nullptr ? host->toString().substring (0, 300) : juce::String ("not XML, ") + juce::String ((int) mb.getSize()) + " bytes"));
        return {};
    }
    juce::MemoryBlock raw;
    raw.fromBase64Encoding (comp->getAllSubText());
    const std::string bytes (static_cast<const char*> (raw.getData()), raw.getSize());   // binary header first
    const auto a0 = bytes.find ("<VINTAGECHONK"), b0 = a0 == std::string::npos ? a0 : bytes.find ("/>", a0);
    const auto text = juce::String::fromUTF8 (bytes.c_str(), (int) bytes.size());
    const int a = a0 == std::string::npos ? -1 : (int) a0, b = b0 == std::string::npos ? -1 : (int) b0;
    if (a < 0 || b < 0)
    {
        info ("no plugin record in the host state (" + juce::String ((int) raw.getSize()) + " bytes): " + text.substring (0, 120).removeCharacters ("\r\n"));
        return {};
    }
    return juce::parseXML (juce::String::fromUTF8 (bytes.c_str() + a, b + 2 - a));
}
// give the plugin one of its own state records, wrapped the way JUCE's VST3 host stores it
static void setPluginState (juce::AudioPluginInstance& p, const juce::MemoryBlock& own)
{
    juce::XmlElement host ("VST3PluginState");
    host.createNewChildElement ("IComponent")->addTextElement (own.toBase64Encoding());
    juce::MemoryBlock mb;
    juce::AudioProcessor::copyXmlToBinary (host, mb);
    p.setStateInformation (mb.getData(), (int) mb.getSize());
}
static juce::String stateUi (juce::AudioPluginInstance& p)
{
    auto x = pluginState (p);
    return x != nullptr ? x->getStringAttribute ("ui") : juce::String();
}

// a hosted VST3 parameter's ID is the VST3 number; find ours by the name the plugin gives each id
static juce::AudioProcessorParameter* paramById (juce::AudioPluginInstance& p, const juce::String& id)
{
    juce::String name;
    for (const auto& d : mfm1::paramDefs) if (id == d.id) name = d.name;
    for (auto* prm : p.getParameters())
        if (prm->getName (64) == name) return prm;
    return nullptr;
}

static bool pumpUntil (std::function<bool()> done, int ms)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
    while (juce::Time::getMillisecondCounter() < end)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        if (done && done()) return true;
    }
    return ! done ? true : done();
}

static void savePng (const juce::Image& img, const juce::File& f)
{
    f.deleteFile();
    juce::FileOutputStream os (f);
    juce::PNGImageFormat png;
    png.writeImageToStream (img, os);
}

// The parameter list a DAW builds: the plugin's own edit controller (a second instance from the bundle's
// factory, connected to the hosted component like a host does) and each parameter's VST3 flags.
struct Vst3ParamList { int total = -1, visible = 0, hiddenCc = 0; juce::StringArray visibleNames; };
static Vst3ParamList vst3ParamList (juce::AudioPluginInstance& p, const juce::File& bundle)
{
    using namespace Steinberg;
    Vst3ParamList r;
    struct { Vst::IComponent* comp = nullptr; } v;
    if (auto* c = p.getVST3Client()) v.comp = c->getIComponentPtr();
    const auto dll = bundle.getChildFile ("Contents").getChildFile ("x86_64-win").getChildFile (bundle.getFileName());
    HMODULE mod = LoadLibraryW (dll.getFullPathName().toWideCharPointer());
    if (v.comp == nullptr || mod == nullptr) return r;
    using GetFactory = IPluginFactory* (PLUGIN_API*)();
    auto gf = reinterpret_cast<GetFactory> (reinterpret_cast<void*> (GetProcAddress (mod, "GetPluginFactory")));
    IPluginFactory* factory = gf != nullptr ? gf() : nullptr;
    TUID cid {};
    Vst::IEditController* ec = nullptr;
    if (factory != nullptr && v.comp->getControllerClassId (cid) == kResultOk
        && factory->createInstance (cid, Vst::IEditController::iid, reinterpret_cast<void**> (&ec)) == kResultOk && ec != nullptr)
    {
        ec->initialize (nullptr);
        Vst::IConnectionPoint *ecp = nullptr, *ccp = nullptr;
        ec->queryInterface (Vst::IConnectionPoint::iid, reinterpret_cast<void**> (&ecp));
        v.comp->queryInterface (Vst::IConnectionPoint::iid, reinterpret_cast<void**> (&ccp));
        if (ecp != nullptr && ccp != nullptr) ecp->connect (ccp);
        r.total = ec->getParameterCount();
        for (int32 i = 0; i < r.total; i++)
        {
            Vst::ParameterInfo pi {};
            if (ec->getParameterInfo (i, pi) != kResultOk) continue;
            const juce::String title (juce::CharPointer_UTF16 (reinterpret_cast<const juce::CharPointer_UTF16::CharType*> (pi.title)));
            if ((pi.flags & Vst::ParameterInfo::kIsHidden) == 0) { r.visible++; r.visibleNames.add (title); }
            else if (title.startsWith ("MIDI CC ")) r.hiddenCc++;
        }
        if (ecp != nullptr && ccp != nullptr) ecp->disconnect (ccp);
        if (ccp != nullptr) ccp->release();
        if (ecp != nullptr) ecp->release();
        ec->terminate();
        ec->release();
    }
    if (factory != nullptr) factory->release();
    FreeLibrary (mod);
    return r;
}

// zero crossings of the left channel (a pitch measure for one held note)
static int zeroCrossings (const juce::AudioBuffer<float>& b, int from, int to)
{
    int n = 0;
    for (int i = from + 1; i < to; i++)
        if ((b.getSample (0, i - 1) < 0.0f) != (b.getSample (0, i) < 0.0f)) n++;
    return n;
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::String path = argc > 1 ? juce::String (argv[1]) : juce::String (MFM1_VST3_PATH);
    const juce::File outDir (argc > 2 ? juce::String (argv[2]) : juce::File::getCurrentWorkingDirectory().getFullPathName());
    bool shots = true;
    for (int i = 1; i < argc; i++) if (juce::String (argv[i]) == "--no-screenshot") shots = false;
    outDir.createDirectory();
    std::printf ("Vintage Chonk host test\n  plugin: %s\n", path.toRawUTF8());

    // ---------------------------------------------------------------- 1. EngineRunner directly ---
    std::printf ("[engine runner]\n");
    mfe_patch_t f0;
    {
        check (mfe_preset_size() == mfm1_factory::size, "C preset size " + juce::String ((int) mfe_preset_size()) + " == the panel's " + juce::String ((int) mfm1_factory::size));
        check (mfe_preset_read (&f0, mfm1_factory::images[0], mfm1_factory::size) == 0, "factory image 0 reads in C");
        int rt = 0;
        for (int i = 0; i < mfm1_factory::count; i++)
        {
            mfe_patch_t q;
            uint8_t back[1024];
            if (mfe_preset_read (&q, mfm1_factory::images[i], mfm1_factory::size) == 0
                && mfe_preset_write (&q, back, sizeof back) == mfm1_factory::size
                && std::memcmp (back, mfm1_factory::images[i], mfm1_factory::size) == 0) rt++;
        }
        check (rt == mfm1_factory::count, juce::String (rt) + " of 16 factory images round-trip byte for byte through the C reader/writer");

        for (double fs : { 44100.0, 48000.0, 96000.0 })
        {
            mfm1::EngineRunner r;
            r.prepare (fs, 512);
            r.postPatch (f0);
            std::vector<float> l (1024), rr (1024);
            r.process (l.data(), rr.data(), 512, nullptr, 0);   // first engine block takes the patch
            mfm1::MidiEvent evs[3] = { { 10, { 0x90, 60, 100 }, 3 }, { 200, { 0x90, 64, 100 }, 3 }, { 300, { 0xB0, 1, 64 }, 3 } };
            uint8_t on[3] = { 0x90, 67, 90 };
            allocs = 0;
            countAllocs = true;
            for (int k = 0; k < 400; k++)
            {
                if (k == 50) r.postPatch (f0);          // patch hand-over
                if (k == 60) r.postMidi (on, 3);        // a panel note
                if (k == 70) r.setTempo (97.0);
                r.process (l.data(), rr.data(), (k % 7) * 61 + 17, k == 3 ? evs : nullptr, k == 3 ? 3 : 0);
            }
            countAllocs = false;
            check (allocs.load() == 0, juce::String ("no heap allocation in process(), postPatch, postMidi at ") + juce::String (fs / 1000.0, 1) + " kHz (" + juce::String ((int) allocs.load()) + " allocations)");
            if (fs != 44100.0)
            {
                // response of the continuous kernel the phase table samples (all phases interleaved =
                // the impulse response at 512x the engine rate): pass band, and the rejection of the
                // images of 0-20 kHz content that upsampling leaves above 24.1 kHz
                const auto& t = r.kernel();
                const int taps = r.kernelTaps(), H = r.kernelHalfWidth(), P = mfm1::EngineRunner::kernelPhases();
                auto resp = [&] (double f)
                {
                    std::complex<double> s = 0;
                    for (int ph = 0; ph < P; ph++)
                        for (int k = 0; k < taps; k++)
                        {
                            const double tt = (double) (k - H + 1) - (double) ph / P;
                            s += (double) t[(size_t) ph * (size_t) taps + (size_t) k] * std::polar (1.0, -2.0 * juce::MathConstants<double>::pi * f / 44100.0 * tt);
                        }
                    return db (std::abs (s) / P);
                };
                double passWorst = 0, stopWorst = -300;
                for (double f : { 100.0, 1000.0, 5000.0, 10000.0, 15000.0, 18000.0 }) passWorst = std::min (passWorst, resp (f));
                for (double f = 24100.0; f <= 44100.0; f += 400.0) stopWorst = std::max (stopWorst, resp (f));
                info ("resampler " + juce::String (fs / 1000.0, 1) + " kHz: " + juce::String (taps) + " taps, at 20 kHz " + juce::String (resp (20000.0), 2)
                      + " dB, 0-18 kHz worst " + juce::String (passWorst, 3) + " dB, images 24.1-44.1 kHz <= " + juce::String (stopWorst, 1) + " dB");
                check (passWorst > -0.1, "resampler pass band 0-18 kHz within 0.1 dB");
                check (stopWorst < -70.0, "resampler image rejection >= 70 dB from 24.1 kHz");
            }
            info ("latency at " + juce::String (fs / 1000.0, 1) + " kHz: " + juce::String (r.latencySamples()) + " samples = "
                  + juce::String (r.latencySamples() * 1000.0 / fs, 2) + " ms");
        }
    }

    // ------------------------------------------------------------------------- 2. load the VST3 ---
    std::printf ("[vst3]\n");
    juce::AudioPluginFormatManager fm;
    juce::addDefaultFormatsToManager (fm);
    juce::VST3PluginFormat* vst3 = nullptr;
    for (auto* f : fm.getFormats()) if (auto* v = dynamic_cast<juce::VST3PluginFormat*> (f)) vst3 = v;
    check (vst3 != nullptr, "VST3 hosting available");
    if (vst3 == nullptr) return 1;
    juce::OwnedArray<juce::PluginDescription> types;
    vst3->findAllTypesForFile (types, path);
    check (types.size() == 1, "one plugin in the bundle (" + juce::String (types.size()) + ")");
    if (types.isEmpty()) return 1;
    const auto desc = *types[0];
    info (desc.name + " by " + desc.manufacturerName + ", " + desc.category + ", version " + desc.version + ", id " + desc.createIdentifierString());
    check (desc.name == "Vintage Chonk" && desc.manufacturerName == "Coley", "name Vintage Chonk, company Coley");
    check (desc.version == "1.0.0", "version 1.0.0 (" + desc.version + ")");
    check (desc.isInstrument, "registered as an instrument");
    check (desc.numInputChannels == 0 && desc.numOutputChannels == 2, "no audio in, stereo out (" + juce::String (desc.numInputChannels) + " in / " + juce::String (desc.numOutputChannels) + " out)");

    auto make = [&]
    {
        juce::String err;
        auto p = fm.createPluginInstance (desc, 44100.0, 512, err);
        if (p == nullptr) info ("create failed: " + err);
        return p;
    };
    auto plug = make();
    check (plug != nullptr, "instantiate");
    if (plug == nullptr) return 1;
    check (plug->acceptsMidi() && ! plug->producesMidi(), "MIDI in, no MIDI out");
    check (plug->getNumPrograms() == 16, "16 host programs (" + juce::String (plug->getNumPrograms()) + ")");
    check (plug->getProgramName (0) == "Walnut Bass" && plug->getProgramName (9) == "Parlour Keys", "program names from the factory bank");
    int named = 0;
    for (int i = 0; i < mfm1::numParams; i++)
        if (auto* prm = paramById (*plug, mfm1::paramDefs[i].id); prm != nullptr && prm->getName (64) == mfm1::paramDefs[i].name) named++;
    check (named == mfm1::numParams, juce::String (named) + " of " + juce::String (mfm1::numParams) + " host parameters found by id and name");
    if (auto* prm = paramById (*plug, "f1_cutoff"))
        info ("f1_cutoff shows " + prm->getCurrentValueAsText() + ", master " + paramById (*plug, "master")->getCurrentValueAsText()
              + ", glide " + paramById (*plug, "glide")->getCurrentValueAsText());
    {
        auto layout = plug->getBusesLayout();
        layout.getChannelSet (false, 0) = juce::AudioChannelSet::mono();
        info (juce::String ("mono output layout ") + (plug->checkBusesLayoutSupported (layout) ? "supported" : "not offered by the host wrapper"));
    }

    // -------------------------------------------- 3. render at 3 rates x 5 block sizes ---
    std::printf ("[render: 3 s of chords, bend, mod wheel, aftertouch, sustain]\n");
    const auto evs = script();
    const double seconds = 3.0;
    for (double fs : { 44100.0, 48000.0, 96000.0 })
    {
        juce::AudioBuffer<float> refOut;
        for (int bs : { 1024, 32, 64, 441, 512 })
        {
            const auto t0 = std::chrono::steady_clock::now();
            auto out = renderPlugin (*plug, fs, bs, seconds, evs);
            const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
            const auto s = stats (out);
            const juce::String tag = juce::String (fs / 1000.0, 1) + " kHz / " + juce::String (bs);
            check (s.finite && s.rms > 0.003 && s.peak < 2.0, tag + ": finite, sounding, no overload (rms " + juce::String (db (s.rms), 1)
                                                         + " dBFS, peak " + juce::String (db (s.peak), 1) + " dBFS, " + juce::String (seconds / secs, 0) + "x real time)");
            if (bs == 1024)
            {
                refOut = out;
                const int want = (int) std::lround ((64 + (fs == 44100.0 ? 0 : 32)) * fs / 44100.0);
                check (plug->getLatencySamples() == want, tag + ": latency reported to the host " + juce::String (plug->getLatencySamples()) + " samples ("
                                                            + juce::String (plug->getLatencySamples() * 1000.0 / fs, 2) + " ms)");
            }
            else
            {
                const double d = maxDiff (out, refOut);
                check (d < 1e-6, tag + ": identical to block size 1024 (max diff " + juce::String (d, 9) + ")");
            }
        }
        if (fs == 44100.0)
        {
            const int frames = (int) std::lround (seconds * fs);
            auto ref = renderReference (f0, frames, evs);
            double d = 0;
            for (int c = 0; c < 2; c++)
                for (int i = 64; i < frames; i++) d = std::max (d, (double) std::fabs (refOut.getSample (c, i) - ref.getSample (c, i - 64)));
            check (d < 1e-7, "44.1 kHz: plugin == offline engine render, 64 samples later (max diff " + juce::String (d, 9) + ")");
            juce::WavAudioFormat wav;
            auto f = outDir.getChildFile ("render_44k1.wav");
            f.deleteFile();
            std::unique_ptr<juce::OutputStream> os (f.createOutputStream().release());
            if (os != nullptr)
                if (std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), 44100.0, 2, 24, {}, 0)); w != nullptr)
                {
                    os.release();
                    w->writeFromAudioSampleBuffer (refOut, 0, refOut.getNumSamples());
                }
        }
        else
        {
            // resampled output vs the offline engine render resampled exactly (double precision, the same
            // windowed sinc computed per sample instead of from the phase table)
            const int frames = (int) std::lround (seconds * 44100.0) + 256;
            auto ref = renderReference (f0, frames, evs);
            const int H = 32;
            const double c = 0.92, beta = 9.0;
            auto i0 = [] (double x) { double s = 1, t = 1, q = x * x / 4; for (int k = 1; k < 64; k++) { t *= q / ((double) k * k); s += t; } return s; };
            const double i0b = i0 (beta);
            double sig = 0, err = 0;
            const int L = 64 + H;
            for (int m = (int) fs / 10; m < refOut.getNumSamples() - 64; m += 3)
            {
                const double x = (double) m * 44100.0 / fs - L;
                const int64_t idx = (int64_t) std::floor (x);
                const double frac = x - (double) idx;
                for (int ch = 0; ch < 2; ch++)
                {
                    double y = 0, wsum = 0;
                    for (int k = 0; k < 2 * H; k++)
                    {
                        const double t = (double) (k - H + 1) - frac, u = t / H;
                        const double w = std::fabs (u) < 1 ? i0 (beta * std::sqrt (1 - u * u)) / i0b : 0;
                        const double a = juce::MathConstants<double>::pi * c * t;
                        const double h = c * (std::fabs (a) < 1e-9 ? 1.0 : std::sin (a) / a) * w;
                        const int64_t j = idx - H + 1 + k;
                        y += h * (j >= 0 && j < frames ? ref.getSample (ch, (int) j) : 0.0);
                        wsum += h;
                    }
                    y /= wsum;
                    const double e = refOut.getSample (ch, m) - y;
                    sig += y * y;
                    err += e * e;
                }
            }
            const double snr = 10.0 * std::log10 (sig / std::max (err, 1e-30));
            check (snr > 90.0, juce::String (fs / 1000.0, 1) + " kHz: plugin vs exact offline resample of the offline engine render: " + juce::String (snr, 1) + " dB SNR");
        }
    }

    // --------------------------------------------------------------- 4. state, programs, ... ---
    std::printf ("[state, programs, automation, tempo]\n");
    {
        auto a = make();
        a->setCurrentProgram (5);
        paramById (*a, "f1_cutoff")->setValueNotifyingHost (0.31f);
        paramById (*a, "macro3")->setValueNotifyingHost (0.77f);
        juce::MemoryBlock mb;
        a->getStateInformation (mb);
        auto b = make();
        b->setStateInformation (mb.getData(), (int) mb.getSize());
        check (b->getCurrentProgram() == 5, "restored program number (" + juce::String (b->getCurrentProgram()) + ")");
        const float ca = paramById (*a, "f1_cutoff")->getValue(), cb = paramById (*b, "f1_cutoff")->getValue();
        const float ma = paramById (*a, "macro3")->getValue(), mbv = paramById (*b, "macro3")->getValue();
        check (std::fabs (ca - cb) < 1e-4f && std::fabs (ma - mbv) < 1e-4f, "restored parameter values (cutoff " + juce::String (cb, 4) + ", macro 3 " + juce::String (mbv, 4) + ")");
        auto oa = renderPlugin (*a, 48000.0, 512, 2.5, evs), ob = renderPlugin (*b, 48000.0, 512, 2.5, evs);
        check (stats (oa).rms > 0.003 && maxDiff (oa, ob) < 1e-6, "restored instance sounds identical (max diff " + juce::String (maxDiff (oa, ob), 9) + ")");
        juce::MemoryBlock mb2;
        b->getStateInformation (mb2);
        check (mb2 == mb, "state saved again is byte-identical (" + juce::String ((int) mb.getSize()) + " bytes)");
        info ("ui state: " + stateUi (*b));
    }
    {
        auto p = make();
        int sounding = 0;
        juce::StringArray quiet;
        for (int i = 0; i < p->getNumPrograms(); i++)
        {
            p->setCurrentProgram (i);
            const std::vector<Ev> one = { { 0.02, { 0x90, 57, 110 } }, { 0.7, { 0x80, 57, 0 } } };
            const auto s = stats (renderPlugin (*p, 48000.0, 256, 1.2, one));
            if (s.finite && s.rms > 0.0005 && s.peak < 2.0) sounding++;
            else quiet.add (p->getProgramName (i) + " " + juce::String (db (s.rms), 1) + " dBFS");
        }
        check (sounding == p->getNumPrograms(), juce::String (sounding) + " of 16 programs play (finite, sounding, no overload) " + quiet.joinIntoString (", "));
    }
    {
        auto p = make();
        const std::vector<Ev> chord = { { 0.02, { 0x90, 45, 110 } }, { 0.02, { 0x90, 57, 110 } }, { 1.9, { 0x80, 45, 0 } }, { 1.9, { 0x80, 57, 0 } } };
        auto* cut = paramById (*p, "f1_cutoff");
        cut->setValueNotifyingHost (0.25f);
        auto lo = renderPlugin (*p, 48000.0, 512, 1.5, chord);
        cut->setValueNotifyingHost (0.85f);
        auto hi = renderPlugin (*p, 48000.0, 512, 1.5, chord);
        const double bl = brightness (lo, 4800, 60000), bh = brightness (hi, 4800, 60000);
        check (bh > 2.0 * bl, "f1_cutoff automation changes the sound (brightness " + juce::String (bl, 4) + " -> " + juce::String (bh, 4) + ")");
        auto moving = renderPlugin (*p, 48000.0, 480, 2.0, chord,
                                    [cut] (int64_t pos) { cut->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, 0.2f + 0.7f * (float) pos / 96000.0f)); });
        const double b1 = brightness (moving, 4800, 30000), b2 = brightness (moving, 66000, 90000);
        check (stats (moving).finite && b2 > 1.5 * b1, "a cutoff sweep automated during the chord opens the filter (" + juce::String (b1, 4) + " -> " + juce::String (b2, 4) + ")");
        auto* vol = paramById (*p, "master");
        vol->setValueNotifyingHost (0.25f);
        const double q = stats (renderPlugin (*p, 48000.0, 512, 1.5, chord)).rms;
        vol->setValueNotifyingHost (0.5f);
        const double l = stats (renderPlugin (*p, 48000.0, 512, 1.5, chord)).rms;
        check (l > 3.0 * q, "master volume automation (" + juce::String (db (q), 1) + " -> " + juce::String (db (l), 1) + " dBFS)");
        auto* m1 = paramById (*p, "macro1");
        check (m1 != nullptr && m1->getCurrentValueAsText().endsWith ("%"), "macro 1 shows " + (m1 ? m1->getCurrentValueAsText() : juce::String()));
    }
    {
        // host tempo -> a BBD echo synced to 1/2 beat; short dry note: the echo lands at 0.05 + 30/BPM s
        mfe_patch_t t = f0;
        mfe_fx_defaults (&t.fx);                    // only the echo
        t.fx.bbd_mix = 30000;
        t.fx.bbd_feedback = 0;
        t.fx.fx_sync = (uint8_t) ((5 + 1) << 4);    // mfe_sync_beats(5) = 1/2 beat
        t.env[3].release = 2000;
        t.env[3].sust_time = 0;
        uint8_t img[1024];
        const size_t n = mfe_preset_write (&t, img, sizeof img);
        juce::XmlElement x ("VINTAGECHONK");
        x.setAttribute ("version", 1);
        x.setAttribute ("mfp", juce::Base64::toBase64 (img, n));
        x.setAttribute ("ui", "{}");
        x.setAttribute ("program", 0);
        juce::MemoryBlock mb;
        juce::AudioProcessor::copyXmlToBinary (x, mb);
        double found[2] = { 0, 0 };
        const double bpms[2] = { 60.0, 150.0 };
        for (int k = 0; k < 2; k++)
        {
            auto p = make();
            setPluginState (*p, mb);
            TempoHead head;
            head.bpm = bpms[k];
            p->setPlayHead (&head);
            const std::vector<Ev> blip = { { 0.05, { 0x90, 60, 120 } }, { 0.09, { 0x80, 60, 0 } } };
            auto out = renderPlugin (*p, 48000.0, 512, 1.2, blip);
            p->setPlayHead (nullptr);
            juce::String env;
            for (int w = 0; w < (int) (1.1 * 48000); w += 2400) env << juce::String (db (stats (out, w, w + 2400).rms), 0) << " ";
            info (juce::String (bpms[k], 0) + " BPM, dBFS per 50 ms: " + env);
            double best = 0;
            for (int w = (int) (0.17 * 48000); w < (int) (1.1 * 48000); w += 240)
            {
                const double e = stats (out, w, w + 480).rms;
                if (e > best) { best = e; found[k] = (w + 240) / 48000.0; }
            }
        }
        // the loudest 10 ms of the echo sits a little after its start (attack); the spacing is exact
        check (std::fabs ((found[0] - found[1]) - 0.30) < 0.02 && std::fabs (found[0] - 0.55) < 0.06 && std::fabs (found[1] - 0.25) < 0.06,
               "host tempo reaches the synced echo: 60 BPM echo at " + juce::String (found[0], 3) + " s (0.55 + attack), 150 BPM at "
                   + juce::String (found[1], 3) + " s (0.25 + attack)");
    }

    // --------------------------------------- 4b. the parameter list a DAW shows; MIDI via the VST3 path ---
    std::printf ("[vst3 parameter list, MIDI controllers]\n");
    {
        const auto pl = vst3ParamList (*plug, juce::File (path));
        info ("edit controller: " + juce::String (pl.total) + " parameters, " + juce::String (pl.visible) + " without kIsHidden ("
              + pl.visibleNames.joinIntoString (", ") + "), " + juce::String (pl.hiddenCc) + " hidden MIDI CC parameters");
        int ours = 0;
        juce::StringArray extra;
        for (const auto& n : pl.visibleNames)
        {
            bool mine = false;
            for (const auto& d : mfm1::paramDefs) mine = mine || n == juce::String (d.name);
            if (mine) ours++;
            else extra.add (n);
        }
        check (ours == mfm1::numParams && pl.visible <= mfm1::numParams + 2,
               juce::String (pl.visible) + " visible VST3 parameters: our " + juce::String (ours) + " + [" + extra.joinIntoString (", ") + "]");
        check (pl.hiddenCc == 16 * 130, juce::String (pl.hiddenCc) + " MIDI CC emulation parameters, all flagged kIsHidden (16 x 130)");
        info ("JUCE's hosted view of the plugin lists " + juce::String (plug->getParameters().size()) + " parameters (host side, flags not applied)");
    }
    {
        // one held note through a patch with mod wheel -> F1 cutoff; amp envelope with a short release
        mfe_patch_t t = f0;
        t.mod[MFE_NMOD - 1].src = (uint8_t) MFE_S_MODWHEEL;
        t.mod[MFE_NMOD - 1].dst = (uint8_t) MFE_D_F1_CUTOFF;
        t.mod[MFE_NMOD - 1].amount = 24000;
        t.env[3].release = 2000;
        uint8_t img[1024];
        const size_t n = mfe_preset_write (&t, img, sizeof img);
        juce::XmlElement x ("VINTAGECHONK");
        x.setAttribute ("version", 1);
        x.setAttribute ("mfp", juce::Base64::toBase64 (img, n));
        x.setAttribute ("ui", "{}");
        x.setAttribute ("program", 0);
        juce::MemoryBlock mb;
        juce::AudioProcessor::copyXmlToBinary (x, mb);
        auto p = make();
        setPluginState (*p, mb);
        const double fs = 48000.0;
        auto run = [&] (std::vector<Ev> e) { return renderPlugin (*p, fs, 512, 1.6, e); };
        const auto plain = run ({ { 0.02, { 0x90, 45, 110 } }, { 0.9, { 0x80, 45, 0 } } });
        const auto wheel = run ({ { 0.01, { 0xB0, 1, 127 } }, { 0.02, { 0x90, 45, 110 } }, { 0.9, { 0x80, 45, 0 } }, { 1.5, { 0xB0, 1, 0 } } });
        const double b0 = brightness (plain, 9600, 40000), b1 = brightness (wheel, 9600, 40000);
        check (b1 > 1.3 * b0, "mod wheel (CC 1) reaches the engine through the VST3 path: brightness " + juce::String (b0, 4) + " -> " + juce::String (b1, 4));
        const auto bent = run ({ { 0.01, { 0xE0, 0x7F, 0x7F } }, { 0.02, { 0x90, 45, 110 } }, { 0.9, { 0x80, 45, 0 } }, { 1.5, { 0xE0, 0x00, 0x40 } } });
        const int z0 = zeroCrossings (plain, 9600, 40000), z1 = zeroCrossings (bent, 9600, 40000);
        check (z1 > z0 + z0 / 20, "pitch bend reaches the engine through the VST3 path: zero crossings " + juce::String (z0) + " -> " + juce::String (z1));
        const auto held = run ({ { 0.01, { 0xB0, 64, 127 } }, { 0.02, { 0x90, 45, 110 } }, { 0.4, { 0x80, 45, 0 } }, { 1.5, { 0xB0, 64, 0 } } });
        const auto dry = run ({ { 0.02, { 0x90, 45, 110 } }, { 0.4, { 0x80, 45, 0 } } });
        const double sh = stats (held, 48000, 62400).rms, sd = stats (dry, 48000, 62400).rms;
        check (sh > 0.003 && sh > 10.0 * sd, "sustain (CC 64) holds the note after its note-off through the VST3 path: "
                                             + juce::String (db (sd), 1) + " -> " + juce::String (db (sh), 1) + " dBFS at 1.0-1.3 s");
    }
    {
        // user presets: the pre-rename folder is copied once into the new one, never overwriting
        auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("vc_preset_migration_test");
        tmp.deleteRecursively();
        auto from = tmp.getChildFile ("old"), to = tmp.getChildFile ("new");
        from.createDirectory();
        to.createDirectory();
        from.getChildFile ("A.mfp").replaceWithText ("old a");
        from.getChildFile ("B.mfp").replaceWithText ("old b");
        to.getChildFile ("B.mfp").replaceWithText ("new b");
        const int c1 = mfm1::PresetStore::migrate (from, to);
        to.getChildFile ("A.mfp").deleteFile();
        const int c2 = mfm1::PresetStore::migrate (from, to);
        check (c1 == 1 && to.getChildFile ("B.mfp").loadFileAsString() == "new b" && c2 == 0 && ! to.getChildFile ("A.mfp").exists(),
               "old preset folder migrated once (copied " + juce::String (c1) + ", existing kept, deleted stays deleted)");
        tmp.deleteRecursively();
    }

    {
        // a DAW project saved by a pre-release build (its state tag has the old name) still loads
        uint8_t img[1024];
        const size_t n = mfe_preset_write (&f0, img, sizeof img);
        juce::XmlElement x ("MOOSIVEFM1");   // legacy name: the pre-release state tag
        x.setAttribute ("version", 1);
        x.setAttribute ("mfp", juce::Base64::toBase64 (img, n));
        x.setAttribute ("ui", "{}");
        x.setAttribute ("program", 7);
        juce::MemoryBlock mb;
        juce::AudioProcessor::copyXmlToBinary (x, mb);
        auto p = make();
        setPluginState (*p, mb);
        check (p->getCurrentProgram() == 7, "a pre-release project state still loads (program " + juce::String (p->getCurrentProgram()) + ")");
    }

    // ------------------------------------------------------------------------------ 5. editor ---
    if (shots)
    {
        std::printf ("[editor: WebView2 panel]\n");
        auto p = make();
        std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditorIfNeeded());
        check (ed != nullptr, "editor created");
        if (ed != nullptr)
        {
            info ("editor size " + juce::String (ed->getWidth()) + " x " + juce::String (ed->getHeight()) + (ed->isResizable() ? ", resizable" : ", fixed size"));
            check (ed->getWidth() == 1440 && ed->getHeight() == 900, "default size 1440 x 900");
            juce::DocumentWindow win ("Vintage Chonk (host test)", juce::Colours::black, 0, true);
            win.setUsingNativeTitleBar (true);
            win.setContentNonOwned (ed.get(), true);
            win.setTopLeftPosition (40, 40);
            win.setVisible (true);
            const auto t0 = juce::Time::getMillisecondCounter();
            // the panel reports its preset over the bridge as JSON.stringify writes it (no spaces);
            // the plugin's own initial record has spaces: the change proves page load + JS -> native
            const bool up = pumpUntil ([&] { return stateUi (*p).contains ("\"kind\":\"F\""); }, 25000);
            check (up, "panel loaded and reported its state to the plugin (" + juce::String ((int) (juce::Time::getMillisecondCounter() - t0)) + " ms): " + stateUi (*p));
            p->setCurrentProgram (3);
            const bool moved = pumpUntil ([&] { return stateUi (*p).contains ("\"name\":\"Velvet Pad\""); }, 6000);
            check (moved, "host program change reached the panel and came back: " + stateUi (*p));
            auto* cut = paramById (*p, "f1_cutoff");
            cut->setValueNotifyingHost (0.42f);
            pumpUntil ({}, 1200);
            check (std::fabs (cut->getValue() - 0.42f) < 1e-3f, "host automation shown on the panel without an echo back (cutoff still " + juce::String (cut->getValue(), 4) + ")");
            pumpUntil ({}, 1500);
            auto* peer = win.getPeer();
            if (peer != nullptr)
            {
                auto img = captureClient ((HWND) peer->getNativeHandle());
                const auto f = outDir.getChildFile ("editor_" + juce::String (img.getWidth()) + "x" + juce::String (img.getHeight()) + ".png");
                savePng (img, f);
                // a rendered panel has many colours; a blank or failed WebView is one or two
                std::set<juce::uint32> cols;
                for (int y = 0; y < img.getHeight(); y += 7)
                    for (int x2 = 0; x2 < img.getWidth(); x2 += 7) cols.insert (img.getPixelAt (x2, y).getARGB() & 0xfcfcfcu);
                check (cols.size() > 200, "screenshot shows a drawn panel (" + juce::String ((int) cols.size()) + " colours): " + f.getFullPathName());
            }
            ed->setSize (960, 600);
            pumpUntil ({}, 1500);
            check (std::abs (ed->getWidth() * 1200 - ed->getHeight() * 1920) < 2000, "resize keeps 16:10 (" + juce::String (ed->getWidth()) + " x " + juce::String (ed->getHeight()) + ")");
            if (peer != nullptr)
            {
                auto img = captureClient ((HWND) peer->getNativeHandle());
                savePng (img, outDir.getChildFile ("editor_" + juce::String (img.getWidth()) + "x" + juce::String (img.getHeight()) + ".png"));
            }
            win.setVisible (false);
            win.clearContentComponent();
            ed.reset();
            pumpUntil ({}, 300);
        }
    }

    std::printf ("\n%d passed, %d failed\n", passes, fails);
    return fails == 0 ? 0 : 1;
}
