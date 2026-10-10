// engine-sim-export: renders a game-ready engine sound set (seamless RPM
// loops, transients and sweeps) from an engine-sim script, without opening
// the interactive application. See docs/game-audio-export.md.

#include "../../include/game_audio/exporter.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

    const char *Usage = R"(Usage: engine-sim-export [options]

Renders an engine sound set for games from an engine-sim script.

Input
  --script <file.mr>       Engine script (default: <repo>/assets/main.mr)
  --es-dir <dir>           engine-sim script library (default: auto-detected)
  --sim-frequency <hz>     Physics rate (default: the engine's own setting)

Output
  --out <dir>              Output root (default: ./engine-audio-export)
  --formats <list>         Comma separated: ogg,mp3,wav (default: all)
  --clips <list>           Comma separated groups (default: all):
                             idle, onload, offload, limiter, startup,
                             shutdown, revs, sweeps

RPM loops
  --rpm-min <rpm>          Lowest loop RPM (default: engine dyno minimum)
  --rpm-max <rpm>          Highest loop RPM (default: redline)
  --rpm-step <rpm>         Spacing between loops (default: 500)
  --onload-throttle <0-1>  Throttle for on-load loops/sweep (default: 1.0)
  --offload-throttle <0-1> Throttle for off-load loops/sweep (default: 0.0)
  --loop-length <s>        Target loop length (default: 4.0)
  --crossfade <ms>         Loop seam crossfade (default: 80)
  --settle <s>             Stabilization time before recording (default: 2.0)
  --sweep-length <s>       Duration of RPM sweeps (default: 12)

Level and encoding
  --normalize <mode>       loudness: every clip at the same LUFS (default)
                           set:      one gain for all, raw levels kept
                           file:     peak-normalize each clip
  --loudness <LUFS>        Loudness target for 'loudness' (default: -18)
  --peak-db <dBFS>         Sample peak ceiling (default: -1.0)
  --ogg-quality <-1..10>   Vorbis VBR quality (default: 6)
  --mp3-vbr <0..9>         LAME VBR quality, 0 = best (default: 2)
  --mp3-cbr <kbps>         Use constant bitrate MP3 instead of VBR
  --wav-bits <16|24>       WAV bit depth (default: 24)

Misc
  --jobs <n>               Render threads (default: all cores)
  --seed <n>               Noise seed for reproducible renders (default: 1)
  -h, --help               Show this help
)";

    std::vector<std::string> splitList(const std::string &s) {
        std::vector<std::string> out;
        std::stringstream ss(s);
        std::string item;
        while (std::getline(ss, item, ',')) {
            const size_t b = item.find_first_not_of(" \t");
            const size_t e = item.find_last_not_of(" \t");
            if (b != std::string::npos) out.push_back(item.substr(b, e - b + 1));
        }
        return out;
    }

    bool parseDouble(const std::string &s, double *out) {
        try {
            size_t used = 0;
            const double v = std::stod(s, &used);
            if (used != s.size()) return false;
            *out = v;
            return true;
        }
        catch (const std::exception &) {
            return false;
        }
    }

    bool parseInt(const std::string &s, long long *out) {
        try {
            size_t used = 0;
            const long long v = std::stoll(s, &used);
            if (used != s.size()) return false;
            *out = v;
            return true;
        }
        catch (const std::exception &) {
            return false;
        }
    }

    fs::path executableDirectory(const char *argv0) {
#ifdef _WIN32
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;) {
            const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (n == 0) break;
            if (n < buffer.size()) {
                buffer.resize(n);
                return fs::path(buffer).parent_path();
            }
            buffer.resize(buffer.size() * 2);
        }
#endif
        std::error_code ec;
        const fs::path p = fs::absolute(argv0, ec);
        return ec ? fs::current_path() : p.parent_path();
    }

    // Walks up from `start` looking for the engine-sim script library.
    bool findRepositoryRoot(const fs::path &start, fs::path *root) {
        std::error_code ec;
        for (fs::path p = fs::absolute(start, ec); !p.empty(); p = p.parent_path()) {
            if (fs::exists(p / "es" / "engine_sim.mr", ec)) {
                *root = p;
                return true;
            }
            if (p == p.parent_path()) break;
        }
        return false;
    }

    std::string asSearchPath(const fs::path &dir) {
        std::string s = fs::absolute(dir).lexically_normal().generic_string();
        if (s.empty() || s.back() != '/') s += '/';
        return s;
    }

    enum class ParseResult { Ok, Help, Error };

    ParseResult parseArguments(
        int argc,
        char **argv,
        game_audio::ExportOptions *o,
        std::string *esDir,
        std::string *error)
    {
        bool cbr = false;

        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "-h" || arg == "--help") return ParseResult::Help;

            if (arg.rfind("--", 0) != 0) {
                *error = "unexpected argument '" + arg + "'";
                return ParseResult::Error;
            }

            if (i + 1 >= argc) {
                *error = "option '" + arg + "' needs a value";
                return ParseResult::Error;
            }
            const std::string value = argv[++i];

            double d = 0.0;
            long long n = 0;
            const auto number = [&](double *target) {
                if (!parseDouble(value, &d)) {
                    *error = "option '" + arg + "' expects a number, got '" + value + "'";
                    return false;
                }
                *target = d;
                return true;
            };
            const auto integer = [&](long long *target) {
                if (!parseInt(value, &n)) {
                    *error = "option '" + arg + "' expects an integer, got '" + value + "'";
                    return false;
                }
                *target = n;
                return true;
            };

            bool ok = true;
            if (arg == "--script") o->session.scriptPath = value;
            else if (arg == "--es-dir") *esDir = value;
            else if (arg == "--out") o->outputDirectory = value;
            else if (arg == "--sim-frequency") ok = number(&o->session.simulationFrequency);
            else if (arg == "--rpm-min") ok = number(&o->rpmMin);
            else if (arg == "--rpm-max") ok = number(&o->rpmMax);
            else if (arg == "--rpm-step") ok = number(&o->rpmStep);
            else if (arg == "--onload-throttle") ok = number(&o->onLoadThrottle);
            else if (arg == "--offload-throttle") ok = number(&o->offLoadThrottle);
            else if (arg == "--loop-length") ok = number(&o->loopSeconds);
            else if (arg == "--settle") ok = number(&o->settleSeconds);
            else if (arg == "--sweep-length") ok = number(&o->sweepSeconds);
            else if (arg == "--peak-db") ok = number(&o->peakDb);
            else if (arg == "--loudness") ok = number(&o->loudnessLufs);
            else if (arg == "--crossfade") {
                double ms = 0;
                ok = number(&ms);
                o->crossfadeSeconds = ms / 1000.0;
            }
            else if (arg == "--ogg-quality") {
                double q = 0;
                ok = number(&q);
                o->ogg.quality = static_cast<float>(q);
            }
            else if (arg == "--mp3-vbr") {
                ok = integer(&n);
                o->mp3.vbrQuality = static_cast<int>(n);
            }
            else if (arg == "--mp3-cbr") {
                ok = integer(&n);
                o->mp3.cbrKbps = static_cast<int>(n);
                cbr = true;
            }
            else if (arg == "--wav-bits") {
                ok = integer(&n);
                o->wavBits = static_cast<int>(n);
            }
            else if (arg == "--jobs") {
                ok = integer(&n);
                o->jobs = static_cast<int>(n);
            }
            else if (arg == "--seed") {
                ok = integer(&n);
                o->seed = static_cast<uint32_t>(n);
            }
            else if (arg == "--normalize") {
                if (value == "loudness") o->normalization = game_audio::Normalization::Loudness;
                else if (value == "set") o->normalization = game_audio::Normalization::Set;
                else if (value == "file") o->normalization = game_audio::Normalization::PerFile;
                else {
                    *error = "--normalize expects 'loudness', 'set' or 'file'";
                    return ParseResult::Error;
                }
            }
            else if (arg == "--formats") {
                o->writeOgg = o->writeMp3 = o->writeWav = false;
                for (const std::string &f : splitList(value)) {
                    if (f == "ogg") o->writeOgg = true;
                    else if (f == "mp3") o->writeMp3 = true;
                    else if (f == "wav") o->writeWav = true;
                    else {
                        *error = "unknown format '" + f + "' (expected ogg, mp3 or wav)";
                        return ParseResult::Error;
                    }
                }
            }
            else if (arg == "--clips") {
                o->clipGroups.clear();
                for (const std::string &g : splitList(value)) o->clipGroups.insert(g);
            }
            else {
                *error = "unknown option '" + arg + "'";
                return ParseResult::Error;
            }

            if (!ok) return ParseResult::Error;
        }

        o->mp3.vbr = !cbr;
        return ParseResult::Ok;
    }

} /* namespace */

int main(int argc, char **argv) {
    game_audio::ExportOptions options;
    std::string esDir;
    std::string error;

    switch (parseArguments(argc, argv, &options, &esDir, &error)) {
    case ParseResult::Help:
        std::cout << Usage;
        return 0;
    case ParseResult::Error:
        std::cerr << "error: " << error << "\n\n" << Usage;
        return 2;
    case ParseResult::Ok:
        break;
    }

    const fs::path exeDir = executableDirectory(argv[0]);
    std::error_code ec;

    // Locate the repository (for the default script and the "es" library):
    // next to the script first, then relative to the executable, then the
    // working directory.
    fs::path root;
    bool haveRoot = false;
    if (!options.session.scriptPath.empty()) {
        haveRoot = findRepositoryRoot(fs::path(options.session.scriptPath).parent_path(), &root);
    }
    if (!haveRoot) haveRoot = findRepositoryRoot(exeDir, &root);
    if (!haveRoot) haveRoot = findRepositoryRoot(fs::current_path(ec), &root);

    if (options.session.scriptPath.empty()) {
        if (!haveRoot) {
            std::cerr << "error: cannot locate engine-sim (assets/main.mr); pass --script\n";
            return 2;
        }
        options.session.scriptPath = (root / "assets" / "main.mr").string();
    }

    if (!fs::exists(options.session.scriptPath, ec)) {
        std::cerr << "error: script '" << options.session.scriptPath << "' does not exist\n";
        return 2;
    }
    options.session.scriptPath =
        fs::absolute(options.session.scriptPath, ec).lexically_normal().generic_string();

    if (!esDir.empty()) {
        if (!fs::exists(fs::path(esDir) / "engine_sim.mr", ec)) {
            std::cerr << "error: '" << esDir << "' does not contain engine_sim.mr\n";
            return 2;
        }
        options.session.searchPaths.push_back(asSearchPath(esDir));
    }
    else if (haveRoot) {
        options.session.searchPaths.push_back(asSearchPath(root / "es"));
    }
    else {
        std::cerr << "error: cannot locate the engine-sim script library; pass --es-dir\n";
        return 2;
    }

    return game_audio::runExport(options, std::cout);
}
