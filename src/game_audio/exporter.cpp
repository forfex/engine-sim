#include "../../include/game_audio/exporter.h"

#include "../../include/game_audio/json_writer.h"
#include "../../include/game_audio/loudness.h"
#include "../../include/game_audio/wav_file.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace {

    using game_audio::EngineSession;
    using game_audio::TelemetrySample;

    constexpr double SampleRate = EngineSession::AudioSampleRate;
    constexpr const char *GeneratorName = "engine-sim-export";
    constexpr const char *ManifestVersion = "1.0";

    enum class ClipKind { Loop, OneShot, Sweep };

    const char *kindName(ClipKind kind) {
        switch (kind) {
        case ClipKind::Loop: return "loop";
        case ClipKind::OneShot: return "oneshot";
        case ClipKind::Sweep: return "sweep";
        }
        return "unknown";
    }

    struct ClipSpec {
        std::string name;
        std::string group;
        ClipKind kind = ClipKind::Loop;

        // "on", "off", "idle", "limiter" or "transient"
        std::string load;
        double throttle = 0.0;

        // Target RPM for steady loops; sweep start/end for sweeps.
        double rpm = 0.0;
        double rpmEnd = 0.0;
    };

    struct RpmStats {
        double mean = 0.0;
        double min = 0.0;
        double max = 0.0;
    };

    struct RenderedClip {
        ClipSpec spec;
        bool ok = false;
        std::string error;

        std::vector<float> audio;
        std::vector<TelemetrySample> telemetry;
        RpmStats rpm;

        game_audio::LoopResult loop;
        double renderSeconds = 0.0;
    };

    RpmStats rpmStats(const std::vector<TelemetrySample> &telemetry) {
        RpmStats stats;
        if (telemetry.empty()) return stats;

        stats.min = std::numeric_limits<double>::infinity();
        stats.max = 0.0;
        double sum = 0.0;
        for (const TelemetrySample &t : telemetry) {
            sum += t.rpm;
            stats.min = std::min(stats.min, t.rpm);
            stats.max = std::max(stats.max, t.rpm);
        }
        stats.mean = sum / telemetry.size();

        return stats;
    }

    // FNV-1a; used to derive per-clip seeds and stream serials so output does
    // not depend on clip order or thread scheduling.
    uint32_t hashString(const std::string &s, uint32_t seed) {
        uint32_t h = 2166136261u ^ seed;
        for (const unsigned char c : s) {
            h ^= c;
            h *= 16777619u;
        }
        return h;
    }

    std::string rpmLabel(double rpm) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%05ld", std::lround(rpm));
        return buffer;
    }

    size_t secondsToSamples(double seconds) {
        return static_cast<size_t>(std::llround(std::max(0.0, seconds) * SampleRate));
    }

    // ---------------------------------------------------------------------
    // Engine control helpers

    // Linearly moves the dyno hold speed from `from` to `to`.
    void rampDyno(
        EngineSession &session,
        double from,
        double to,
        double seconds,
        std::vector<float> *audio,
        std::vector<TelemetrySample> *telemetry)
    {
        const double start = session.time();
        const double end = start + seconds;
        while (session.time() < end - 0.5 * EngineSession::BlockDuration) {
            const double t = (session.time() - start) / seconds;
            session.setDynoHold(from + (to - from) * std::min(1.0, t));
            session.advance(EngineSession::BlockDuration, audio, telemetry);
        }
        session.setDynoHold(to);
    }

    // Cranks the engine until it runs on its own. Audio is recorded when
    // `audio` is non-null so the same routine produces the startup clip.
    //
    // The starter can only drive the crank up to its rated speed, so running
    // clearly above that speed for a moment means combustion has taken over.
    // (Idle speed is often below twice the starter speed, so a fixed multiple
    // would not work.)
    bool crankAndStart(
        EngineSession &session,
        std::vector<float> *audio,
        std::vector<TelemetrySample> *telemetry,
        std::string *error)
    {
        const double starterRpm = session.info().starterRpm;
        const double threshold = 1.15 * starterRpm;
        constexpr double SustainSeconds = 0.2;

        session.setIgnition(true);
        session.setThrottleImmediate(0.0);
        session.setStarter(true);

        double aboveSince = -1.0;
        const bool caught = session.advanceUntil(
            [&](const EngineSession &s) {
                if (s.rpm() <= threshold) {
                    aboveSince = -1.0;
                    return false;
                }
                if (aboveSince < 0) aboveSince = s.time();
                return s.time() - aboveSince >= SustainSeconds;
            },
            8.0, audio, telemetry);
        session.setStarter(false);

        if (!caught) {
            *error = "engine did not start within 8 s of cranking (reached "
                + std::to_string(std::lround(session.rpm())) + " rpm, starter speed "
                + std::to_string(std::lround(starterRpm)) + " rpm)";
            return false;
        }

        // Confirm it keeps running without the starter; a non-firing engine
        // spins down well below cranking speed within a second.
        session.advance(1.0, audio, telemetry);
        if (session.rpm() < 0.75 * starterRpm) {
            *error = "engine stalled after the starter was released ("
                + std::to_string(std::lround(session.rpm())) + " rpm)";
            return false;
        }

        return true;
    }

    // ---------------------------------------------------------------------
    // Clip renderers

    game_audio::LoopParameters loopParameters(
        const game_audio::ExportOptions &options,
        double periodSamples,
        size_t searchRadius)
    {
        game_audio::LoopParameters p;
        p.periodSamples = periodSamples;
        p.targetLength = secondsToSamples(options.loopSeconds);
        p.crossfadeLength = secondsToSamples(options.crossfadeSeconds);
        p.searchRadius = searchRadius;
        p.startOffset = 0;
        p.startSearchLength = secondsToSamples(0.5);
        return p;
    }

    // One four-stroke engine cycle (720 degrees of crank rotation).
    double cyclePeriodSamples(double rpm) {
        return SampleRate * 120.0 / rpm;
    }

    // Records steady-state audio and cuts a pitch-synchronous seamless loop.
    bool recordSteadyLoop(
        EngineSession &session,
        const game_audio::ExportOptions &options,
        bool periodic,
        RenderedClip *clip)
    {
        const size_t searchRadius = secondsToSamples(0.3);

        // Estimate the amount of audio needed from the target RPM, then top
        // up once the actual RPM is known.
        const double estimatedRpm = std::max(clip->spec.rpm, 300.0);
        game_audio::LoopParameters params = loopParameters(
            options, periodic ? cyclePeriodSamples(estimatedRpm) : 0.0, searchRadius);

        std::vector<float> source;
        std::vector<TelemetrySample> telemetry;
        const size_t margin = secondsToSamples(0.1);
        session.advance(
            (game_audio::requiredSourceLength(params) + margin) / SampleRate,
            &source, &telemetry);

        for (int attempt = 0; attempt < 8; ++attempt) {
            clip->rpm = rpmStats(telemetry);
            if (periodic) {
                if (clip->rpm.mean < 60.0) {
                    clip->error = "engine is not turning (mean "
                        + std::to_string(clip->rpm.mean) + " rpm)";
                    return false;
                }
                params.periodSamples = cyclePeriodSamples(clip->rpm.mean);

                // Blend over at least one full engine cycle so the seam mixes
                // complete firing patterns, which matters at low RPM where
                // one cycle outlasts the default crossfade.
                params.crossfadeLength = std::max(
                    secondsToSamples(options.crossfadeSeconds),
                    static_cast<size_t>(std::ceil(params.periodSamples)));
            }

            const size_t required = game_audio::requiredSourceLength(params);
            if (source.size() >= required) break;

            session.advance((required - source.size() + margin) / SampleRate, &source, &telemetry);
        }

        if (!game_audio::buildSeamlessLoop(source, params, &clip->loop, &clip->error)) {
            return false;
        }

        clip->audio = clip->loop.samples;
        clip->telemetry = std::move(telemetry);
        game_audio::removeMean(clip->audio);
        return true;
    }

    void finishOneShot(RenderedClip *clip, double fadeInSeconds, double fadeOutSeconds) {
        game_audio::dcBlock(clip->audio, SampleRate, 5.0);
        if (fadeInSeconds > 0) game_audio::applyFadeIn(clip->audio, secondsToSamples(fadeInSeconds));
        if (fadeOutSeconds > 0) game_audio::applyFadeOut(clip->audio, secondsToSamples(fadeOutSeconds));
        clip->rpm = rpmStats(clip->telemetry);
    }

    bool renderLoadLoop(EngineSession &session, const game_audio::ExportOptions &options, RenderedClip *clip) {
        session.setIgnition(true);
        session.setThrottleImmediate(clip->spec.throttle);

        // Spin up gradually: a step change in the hold speed would slam the
        // crank with the dyno's full torque.
        const double spinUp = std::max(0.5, clip->spec.rpm / 4000.0);
        rampDyno(session, 0.0, clip->spec.rpm, spinUp, nullptr, nullptr);
        session.advance(options.settleSeconds, nullptr, nullptr);

        return recordSteadyLoop(session, options, true, clip);
    }

    bool renderIdle(EngineSession &session, const game_audio::ExportOptions &options, RenderedClip *clip) {
        if (!crankAndStart(session, nullptr, nullptr, &clip->error)) return false;
        session.advance(options.settleSeconds + 1.0, nullptr, nullptr);

        clip->spec.rpm = session.rpm();
        return recordSteadyLoop(session, options, true, clip);
    }

    bool renderLimiter(EngineSession &session, const game_audio::ExportOptions &options, RenderedClip *clip) {
        if (!crankAndStart(session, nullptr, nullptr, &clip->error)) return false;

        session.setThrottle(clip->spec.throttle);
        session.advance(4.0, nullptr, nullptr);

        // The limiter cuts spark in bursts, so the signal is not periodic in
        // the engine cycle; use a broad correlation search instead.
        clip->spec.rpm = session.rpm();
        return recordSteadyLoop(session, options, false, clip);
    }

    bool renderStartup(EngineSession &session, const game_audio::ExportOptions &, RenderedClip *clip) {
        // Short pre-roll so the starter engagement is not at sample 0.
        session.advance(0.15, &clip->audio, &clip->telemetry);
        if (!crankAndStart(session, &clip->audio, &clip->telemetry, &clip->error)) return false;
        session.advance(2.0, &clip->audio, &clip->telemetry);

        finishOneShot(clip, 0.0, 0.25);
        return true;
    }

    bool renderShutdown(EngineSession &session, const game_audio::ExportOptions &options, RenderedClip *clip) {
        if (!crankAndStart(session, nullptr, nullptr, &clip->error)) return false;
        session.advance(options.settleSeconds, nullptr, nullptr);

        session.advance(0.5, &clip->audio, &clip->telemetry);
        session.setIgnition(false);
        session.advanceUntil(
            [](const EngineSession &s) { return s.rpm() < 20.0; },
            8.0, &clip->audio, &clip->telemetry);
        session.advance(0.4, &clip->audio, &clip->telemetry);

        finishOneShot(clip, 0.01, 0.25);
        return true;
    }

    bool renderRevBlip(EngineSession &session, const game_audio::ExportOptions &options, RenderedClip *clip) {
        if (!crankAndStart(session, nullptr, nullptr, &clip->error)) return false;
        session.advance(options.settleSeconds, nullptr, nullptr);

        const double idleRpm = session.rpm();
        const double peakRpm = 0.85 * session.info().redlineRpm;

        session.advance(0.5, &clip->audio, &clip->telemetry);
        session.setThrottle(1.0);
        session.advanceUntil(
            [peakRpm](const EngineSession &s) { return s.rpm() >= peakRpm; },
            3.0, &clip->audio, &clip->telemetry);
        session.setThrottle(0.0);
        session.advanceUntil(
            [idleRpm](const EngineSession &s) { return s.rpm() <= idleRpm * 1.1; },
            6.0, &clip->audio, &clip->telemetry);
        session.advance(0.75, &clip->audio, &clip->telemetry);

        finishOneShot(clip, 0.01, 0.25);
        return true;
    }

    bool renderSweep(EngineSession &session, const game_audio::ExportOptions &options, RenderedClip *clip) {
        const double from = clip->spec.rpm;
        const double to = clip->spec.rpmEnd;

        session.setIgnition(true);
        session.setThrottleImmediate(clip->spec.throttle);
        rampDyno(session, 0.0, from, std::max(0.5, from / 4000.0), nullptr, nullptr);
        session.advance(1.5, nullptr, nullptr);

        session.advance(0.25, &clip->audio, &clip->telemetry);
        rampDyno(session, from, to, options.sweepSeconds, &clip->audio, &clip->telemetry);
        session.advance(0.5, &clip->audio, &clip->telemetry);

        finishOneShot(clip, 0.02, 0.02);
        return true;
    }

    // ---------------------------------------------------------------------
    // Plan

    std::vector<ClipSpec> buildPlan(
        const game_audio::ExportOptions &options,
        const EngineSession::Info &info,
        double *rpmMinOut,
        double *rpmMaxOut)
    {
        const double step = options.rpmStep;
        const double rpmMin = (options.rpmMin > 0)
            ? options.rpmMin
            : std::ceil(std::max(info.dynoMinRpm, 500.0) / step) * step;
        const double rpmMax = (options.rpmMax > 0)
            ? options.rpmMax
            : std::floor(info.redlineRpm / step) * step;

        *rpmMinOut = rpmMin;
        *rpmMaxOut = rpmMax;

        const auto wants = [&options](const char *group) {
            return options.clipGroups.count(group) > 0;
        };

        std::vector<ClipSpec> plan;

        if (wants(game_audio::groups::Idle)) {
            plan.push_back({ "idle", game_audio::groups::Idle, ClipKind::Loop, "idle", 0.0, 0.0, 0.0 });
        }

        const std::vector<double> grid = game_audio::rpmGrid(rpmMin, rpmMax, step);
        if (wants(game_audio::groups::OnLoad)) {
            for (const double rpm : grid) {
                plan.push_back({ "onload_" + rpmLabel(rpm) + "rpm", game_audio::groups::OnLoad,
                    ClipKind::Loop, "on", options.onLoadThrottle, rpm, rpm });
            }
        }

        if (wants(game_audio::groups::OffLoad)) {
            for (const double rpm : grid) {
                plan.push_back({ "offload_" + rpmLabel(rpm) + "rpm", game_audio::groups::OffLoad,
                    ClipKind::Loop, "off", options.offLoadThrottle, rpm, rpm });
            }
        }

        if (wants(game_audio::groups::Limiter)) {
            plan.push_back({ "limiter", game_audio::groups::Limiter, ClipKind::Loop, "limiter", 1.0, 0.0, 0.0 });
        }

        if (wants(game_audio::groups::Startup)) {
            plan.push_back({ "startup", game_audio::groups::Startup, ClipKind::OneShot, "transient", 0.0, 0.0, 0.0 });
        }

        if (wants(game_audio::groups::Shutdown)) {
            plan.push_back({ "shutdown", game_audio::groups::Shutdown, ClipKind::OneShot, "transient", 0.0, 0.0, 0.0 });
        }

        if (wants(game_audio::groups::RevBlip)) {
            plan.push_back({ "rev_blip", game_audio::groups::RevBlip, ClipKind::OneShot, "transient", 1.0, 0.0, 0.0 });
        }

        if (wants(game_audio::groups::Sweeps)) {
            plan.push_back({ "sweep_onload_up", game_audio::groups::Sweeps, ClipKind::Sweep,
                "on", options.onLoadThrottle, rpmMin, rpmMax });
            plan.push_back({ "sweep_offload_down", game_audio::groups::Sweeps, ClipKind::Sweep,
                "off", options.offLoadThrottle, rpmMax, rpmMin });
        }

        return plan;
    }

    void renderClip(const game_audio::ExportOptions &options, RenderedClip *clip) {
        const auto t0 = std::chrono::steady_clock::now();

        // engine-sim draws its noise from rand(). The MSVC runtime keeps rand()
        // state per thread, so seeding here makes every clip reproducible
        // regardless of which worker renders it.
        std::srand(hashString(clip->spec.name, options.seed));

        std::string error;
        std::unique_ptr<EngineSession> session = EngineSession::create(options.session, &error);
        if (!session) {
            clip->error = error;
            return;
        }

        const std::string &group = clip->spec.group;
        bool ok = false;
        if (group == game_audio::groups::OnLoad || group == game_audio::groups::OffLoad) {
            ok = renderLoadLoop(*session, options, clip);
        }
        else if (group == game_audio::groups::Idle) ok = renderIdle(*session, options, clip);
        else if (group == game_audio::groups::Limiter) ok = renderLimiter(*session, options, clip);
        else if (group == game_audio::groups::Startup) ok = renderStartup(*session, options, clip);
        else if (group == game_audio::groups::Shutdown) ok = renderShutdown(*session, options, clip);
        else if (group == game_audio::groups::RevBlip) ok = renderRevBlip(*session, options, clip);
        else if (group == game_audio::groups::Sweeps) ok = renderSweep(*session, options, clip);
        else clip->error = "unknown clip group '" + group + "'";

        if (ok) {
            for (const float s : clip->audio) {
                if (!std::isfinite(s)) {
                    ok = false;
                    clip->error = "simulation produced non-finite audio (try a higher --sim-frequency)";
                    break;
                }
            }
        }

        if (ok && game_audio::peakAbs(clip->audio) <= 0.0f) {
            ok = false;
            clip->error = "clip is silent";
        }

        clip->ok = ok;
        clip->renderSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
    }

    // ---------------------------------------------------------------------
    // Output

    bool writeTelemetryCsv(
        const fs::path &path,
        const std::vector<TelemetrySample> &telemetry,
        std::string *error)
    {
        std::ofstream file(path, std::ios::out | std::ios::trunc);
        if (!file) {
            *error = "cannot create '" + path.string() + "'";
            return false;
        }

        file << "time_s,sample,rpm,throttle,dyno_torque_nm\n";
        if (!telemetry.empty()) {
            const double t0 = telemetry.front().time;
            char line[160];
            for (const TelemetrySample &t : telemetry) {
                std::snprintf(line, sizeof(line), "%.5f,%zu,%.2f,%.4f,%.3f\n",
                    t.time - t0, t.audioSample, t.rpm, t.throttle, t.dynoTorque);
                file << line;
            }
        }

        if (!file) {
            *error = "failed writing '" + path.string() + "'";
            return false;
        }

        return true;
    }

    std::string relativePath(const fs::path &p, const fs::path &root) {
        return p.lexically_relative(root).generic_string();
    }

    struct ClipFiles {
        std::string ogg, mp3, wav, telemetry;
    };

    struct ClipLevel {
        double gainDb = 0.0;

        // Raw loudness relative to the loudest clip of the set (<= 0 dB).
        double naturalLevelDb = 0.0;

        // The peak ceiling, not the loudness target, determined the gain.
        bool peakLimited = false;
    };

    const char *normalizationName(game_audio::Normalization n) {
        switch (n) {
        case game_audio::Normalization::Loudness: return "loudness";
        case game_audio::Normalization::Set: return "set";
        case game_audio::Normalization::PerFile: return "file";
        }
        return "unknown";
    }

    void writeManifest(
        const fs::path &path,
        const game_audio::ExportOptions &options,
        const EngineSession::Info &info,
        double rpmMin,
        double rpmMax,
        double setGainDb,
        const std::vector<RenderedClip> &clips,
        const std::vector<ClipFiles> &files,
        const std::vector<ClipLevel> &levels,
        std::string *error)
    {
        game_audio::JsonWriter json;
        json.beginObject();
        json.key("generator").value(GeneratorName);
        json.key("manifestVersion").value(ManifestVersion);

        json.key("engine").beginObject();
        json.key("name").value(info.name);
        json.key("cylinders").value(info.cylinderCount);
        json.key("displacementLiters").value(info.displacementLiters);
        json.key("redlineRpm").value(info.redlineRpm);
        json.key("simulationFrequencyHz").value(info.simulationFrequency);
        for (size_t i = 0; i < clips.size(); ++i) {
            if (clips[i].ok && clips[i].spec.group == game_audio::groups::Idle) {
                json.key("idleRpm").value(clips[i].rpm.mean);
            }
            if (clips[i].ok && clips[i].spec.group == game_audio::groups::Limiter) {
                json.key("limiterRpm").value(clips[i].rpm.mean);
            }
        }
        json.endObject();

        json.key("audio").beginObject();
        json.key("sampleRate").value(static_cast<int>(SampleRate));
        json.key("channels").value(1);
        json.key("normalization").value(normalizationName(options.normalization));
        if (options.normalization == game_audio::Normalization::Loudness) {
            json.key("targetLoudnessLufs").value(options.loudnessLufs);
        }
        json.key("peakCeilingDbfs").value(options.peakDb);
        if (options.normalization == game_audio::Normalization::Set) {
            json.key("setGainDb").value(setGainDb);
        }
        json.endObject();

        json.key("rpmGrid").beginObject();
        json.key("min").value(rpmMin);
        json.key("max").value(rpmMax);
        json.key("step").value(options.rpmStep);
        json.endObject();

        // Blend layers sorted by RPM, ready for an RPM-driven crossfade.
        json.key("layers").beginObject();
        for (const char *load : { "on", "off" }) {
            json.key(std::string(load) + "Load").beginArray();
            for (size_t i = 0; i < clips.size(); ++i) {
                const RenderedClip &c = clips[i];
                if (!c.ok || c.spec.kind != ClipKind::Loop || c.spec.load != load) continue;
                json.beginObject();
                json.key("clip").value(c.spec.name);
                json.key("rpm").value(c.rpm.mean);
                json.key("naturalLevelDb").value(levels[i].naturalLevelDb);
                json.endObject();
            }
            json.endArray();
        }
        json.endObject();

        json.key("clips").beginArray();
        for (size_t i = 0; i < clips.size(); ++i) {
            const RenderedClip &c = clips[i];
            if (!c.ok) continue;

            json.beginObject();
            json.key("name").value(c.spec.name);
            json.key("kind").value(kindName(c.spec.kind));
            json.key("group").value(c.spec.group);
            json.key("load").value(c.spec.load);
            json.key("throttle").value(c.spec.throttle);

            if (c.spec.kind == ClipKind::Sweep) {
                json.key("rpmStart").value(c.spec.rpm);
                json.key("rpmEnd").value(c.spec.rpmEnd);
            }
            else if (c.spec.group == game_audio::groups::OnLoad
                || c.spec.group == game_audio::groups::OffLoad)
            {
                json.key("targetRpm").value(c.spec.rpm);
            }

            // For loops, `rpm.mean` is the pitch reference: play the clip at
            // rate currentRpm / rpm.mean.
            json.key("rpm").beginObject();
            json.key("mean").value(c.rpm.mean);
            json.key("min").value(c.rpm.min);
            json.key("max").value(c.rpm.max);
            json.endObject();

            json.key("samples").value(c.audio.size());
            json.key("durationSeconds").value(c.audio.size() / SampleRate);

            if (c.spec.kind == ClipKind::Loop) {
                json.key("loop").beginObject();
                json.key("start").value(static_cast<size_t>(0));
                json.key("length").value(c.audio.size());
                json.key("engineCycles").value(c.loop.cycles);
                json.key("crossfadeSamples").value(c.loop.crossfadeLength);
                json.key("seamCorrelation").value(c.loop.seamCorrelation);
                json.endObject();
            }

            json.key("gainDb").value(levels[i].gainDb);
            json.key("peakLimited").value(levels[i].peakLimited);
            json.key("naturalLevelDb").value(levels[i].naturalLevelDb);
            json.key("loudnessLufs").value(game_audio::integratedLoudness(c.audio, SampleRate));
            json.key("peakDbfs").value(game_audio::linearToDb(game_audio::peakAbs(c.audio)));
            json.key("rmsDbfs").value(game_audio::linearToDb(game_audio::rms(c.audio)));

            json.key("files").beginObject();
            if (!files[i].ogg.empty()) json.key("ogg").value(files[i].ogg);
            if (!files[i].mp3.empty()) json.key("mp3").value(files[i].mp3);
            if (!files[i].wav.empty()) json.key("wav").value(files[i].wav);
            if (!files[i].telemetry.empty()) json.key("telemetry").value(files[i].telemetry);
            json.endObject();

            json.endObject();
        }
        json.endArray();
        json.endObject();

        std::ofstream file(path, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!file) {
            *error = "cannot create '" + path.string() + "'";
            return;
        }

        file << json.str() << "\n";
        if (!file) *error = "failed writing '" + path.string() + "'";
    }

} /* namespace */

std::set<std::string> game_audio::allClipGroups() {
    return {
        groups::Idle, groups::OnLoad, groups::OffLoad, groups::Limiter,
        groups::Startup, groups::Shutdown, groups::RevBlip, groups::Sweeps
    };
}

std::string game_audio::validateOptions(const ExportOptions &o) {
    if (o.session.scriptPath.empty()) return "no engine script given";
    if (!o.writeOgg && !o.writeMp3 && !o.writeWav) return "no output format selected";
    if (o.clipGroups.empty()) return "no clips selected";
    for (const std::string &g : o.clipGroups) {
        if (allClipGroups().count(g) == 0) return "unknown clip group '" + g + "'";
    }
    if (!(o.rpmStep >= 50.0)) return "--rpm-step must be at least 50";
    if (o.rpmMin < 0 || o.rpmMax < 0) return "RPM limits must be positive";
    if (o.rpmMin > 0 && o.rpmMax > 0 && o.rpmMin >= o.rpmMax) return "--rpm-min must be below --rpm-max";
    if (o.onLoadThrottle < 0 || o.onLoadThrottle > 1) return "--onload-throttle must be within [0, 1]";
    if (o.offLoadThrottle < 0 || o.offLoadThrottle > 1) return "--offload-throttle must be within [0, 1]";
    if (o.loopSeconds < 0.5 || o.loopSeconds > 30) return "--loop-length must be within [0.5, 30] seconds";
    if (o.crossfadeSeconds < 0 || o.crossfadeSeconds > o.loopSeconds / 4) {
        return "--crossfade must be within [0, loop length / 4]";
    }
    if (o.settleSeconds < 0.25 || o.settleSeconds > 30) return "--settle must be within [0.25, 30] seconds";
    if (o.sweepSeconds < 1 || o.sweepSeconds > 120) return "--sweep-length must be within [1, 120] seconds";
    if (o.peakDb > 0 || o.peakDb < -40) return "--peak-db must be within [-40, 0]";
    if (o.loudnessLufs > -5 || o.loudnessLufs < -40) return "--loudness must be within [-40, -5] LUFS";
    if (o.ogg.quality < -1 || o.ogg.quality > 10) return "--ogg-quality must be within [-1, 10]";
    if (o.mp3.vbrQuality < 0 || o.mp3.vbrQuality > 9) return "--mp3-vbr must be within [0, 9]";
    if (!o.mp3.vbr && (o.mp3.cbrKbps < 32 || o.mp3.cbrKbps > 320)) return "--mp3-cbr must be within [32, 320] kbps";
    if (o.wavBits != 16 && o.wavBits != 24) return "--wav-bits must be 16 or 24";
    if (o.jobs < 0) return "--jobs must not be negative";
    if (o.session.simulationFrequency != 0 && o.session.simulationFrequency < 1000) {
        return "--sim-frequency must be at least 1000 Hz";
    }
    return {};
}

std::vector<double> game_audio::rpmGrid(double rpmMin, double rpmMax, double step) {
    std::vector<double> grid;
    if (!(step > 0) || rpmMax < rpmMin) return grid;

    const int count = static_cast<int>(std::floor((rpmMax - rpmMin) / step + 1e-9));
    for (int i = 0; i <= count; ++i) {
        grid.push_back(rpmMin + i * step);
    }

    // Always cover the top of the range; skip it when it is too close to the
    // last grid point to add useful information.
    if (rpmMax - grid.back() > 0.25 * step) grid.push_back(rpmMax);

    return grid;
}

std::string game_audio::slugify(const std::string &text) {
    std::string out;
    bool pendingSeparator = false;
    for (const char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) {
            if (pendingSeparator && !out.empty()) out += '_';
            out += static_cast<char>(std::tolower(u));
            pendingSeparator = false;
        }
        else {
            pendingSeparator = true;
        }
    }

    return out.empty() ? "engine" : out;
}

int game_audio::runExport(const ExportOptions &options, std::ostream &log) {
    const std::string invalid = validateOptions(options);
    if (!invalid.empty()) {
        log << "error: " << invalid << "\n";
        return 2;
    }

    // A probe session validates the script and provides engine data for the plan.
    std::string error;
    EngineSession::Info info;
    {
        std::unique_ptr<EngineSession> probe = EngineSession::create(options.session, &error);
        if (!probe) {
            log << "error: " << error << "\n";
            return 1;
        }
        info = probe->info();
    }

    double rpmMin = 0, rpmMax = 0;
    const std::vector<ClipSpec> plan = buildPlan(options, info, &rpmMin, &rpmMax);
    if (plan.empty()) {
        log << "error: nothing to render\n";
        return 2;
    }

    if (rpmMin >= rpmMax
        && (options.clipGroups.count(groups::OnLoad) || options.clipGroups.count(groups::OffLoad)
            || options.clipGroups.count(groups::Sweeps)))
    {
        log << "error: empty RPM range (" << rpmMin << " to " << rpmMax << " rpm)\n";
        return 2;
    }

    const fs::path root = fs::path(options.outputDirectory) / slugify(info.name);

    log << "Engine:     " << info.name << " (" << info.cylinderCount << " cyl, "
        << std::lround(info.displacementLiters * 100) / 100.0 << " L, redline "
        << std::lround(info.redlineRpm) << " rpm)\n";
    log << "Simulation: " << std::lround(info.simulationFrequency) << " Hz\n";
    log << "RPM grid:   " << std::lround(rpmMin) << " - " << std::lround(rpmMax)
        << " rpm, step " << std::lround(options.rpmStep) << "\n";
    log << "Output:     " << root.string() << "\n";

    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    const int jobs = static_cast<int>(std::min<size_t>(
        plan.size(), options.jobs > 0 ? static_cast<size_t>(options.jobs) : hardware));
    log << "Rendering " << plan.size() << " clips on " << jobs << " thread(s)...\n";

    std::vector<RenderedClip> clips(plan.size());
    for (size_t i = 0; i < plan.size(); ++i) clips[i].spec = plan[i];

    std::mutex logMutex;
    std::atomic<size_t> next{ 0 };
    std::atomic<size_t> finished{ 0 };
    const auto worker = [&]() {
        for (size_t i = next++; i < clips.size(); i = next++) {
            renderClip(options, &clips[i]);

            std::lock_guard<std::mutex> lock(logMutex);
            const size_t done = ++finished;
            char prefix[32];
            std::snprintf(prefix, sizeof(prefix), "  [%2zu/%2zu] ", done, clips.size());
            log << prefix << clips[i].spec.name;
            if (clips[i].ok) {
                char details[128];
                std::snprintf(details, sizeof(details), "  %.2f s audio, %.0f rpm, %.1f s",
                    clips[i].audio.size() / SampleRate, clips[i].rpm.mean, clips[i].renderSeconds);
                log << details;
                if (clips[i].spec.kind == ClipKind::Loop) {
                    std::snprintf(details, sizeof(details), ", seam %.3f", clips[i].loop.seamCorrelation);
                    log << details;
                }
                log << "\n";
            }
            else {
                log << "  FAILED: " << clips[i].error << "\n";
            }
            log.flush();
        }
    };

    std::vector<std::thread> threads;
    for (int i = 1; i < jobs; ++i) threads.emplace_back(worker);
    worker();
    for (std::thread &t : threads) t.join();

    // Normalization
    std::vector<ClipLevel> levels(clips.size());
    double setGainDb = 0.0;
    {
        const double ceiling = dbToLinear(options.peakDb);

        std::vector<double> rawLoudness(clips.size(), -std::numeric_limits<double>::infinity());
        double loudest = -std::numeric_limits<double>::infinity();
        float setPeak = 0.0f;
        for (size_t i = 0; i < clips.size(); ++i) {
            if (!clips[i].ok) continue;
            rawLoudness[i] = integratedLoudness(clips[i].audio, SampleRate);
            loudest = std::max(loudest, rawLoudness[i]);
            setPeak = std::max(setPeak, peakAbs(clips[i].audio));
        }

        if (setPeak > 0.0f) setGainDb = linearToDb(ceiling / setPeak);

        for (size_t i = 0; i < clips.size(); ++i) {
            RenderedClip &c = clips[i];
            if (!c.ok) continue;

            const double peakGain = ceiling / peakAbs(c.audio);
            double gain = peakGain;
            if (options.normalization == Normalization::Set) {
                gain = ceiling / setPeak;
            }
            else if (options.normalization == Normalization::Loudness && std::isfinite(rawLoudness[i])) {
                gain = dbToLinear(options.loudnessLufs - rawLoudness[i]);
                if (gain > peakGain) {
                    gain = peakGain;
                    levels[i].peakLimited = true;
                }
            }

            applyGain(c.audio, gain);
            levels[i].gainDb = linearToDb(gain);
            levels[i].naturalLevelDb = std::isfinite(rawLoudness[i])
                ? rawLoudness[i] - loudest
                : -std::numeric_limits<double>::infinity();
        }
    }

    // Encoding
    std::error_code ec;
    const fs::path oggDir = root / "ogg", mp3Dir = root / "mp3", wavDir = root / "wav";
    const fs::path telemetryDir = root / "telemetry";
    for (const fs::path &dir : { root, telemetryDir }) {
        fs::create_directories(dir, ec);
        if (ec) {
            log << "error: cannot create '" << dir.string() << "': " << ec.message() << "\n";
            return 1;
        }
    }
    if (options.writeOgg) fs::create_directories(oggDir, ec);
    if (!ec && options.writeMp3) fs::create_directories(mp3Dir, ec);
    if (!ec && options.writeWav) fs::create_directories(wavDir, ec);
    if (ec) {
        log << "error: cannot create output directories: " << ec.message() << "\n";
        return 1;
    }

    log << "Encoding...\n";
    std::vector<ClipFiles> files(clips.size());
    next = 0;

    const auto encoder = [&]() {
        for (size_t i = next++; i < clips.size(); i = next++) {
            RenderedClip &c = clips[i];
            if (!c.ok) continue;

            const bool isLoop = c.spec.kind == ClipKind::Loop;
            const uint32_t clipHash = hashString(c.spec.name, options.seed);
            std::string err;
            bool ok = true;

            if (ok && options.writeOgg) {
                const fs::path p = oggDir / (c.spec.name + ".ogg");
                OggVorbisOptions ogg = options.ogg;
                ogg.serialNumber = static_cast<int>(clipHash & 0x7FFFFFFF);
                ogg.comments.push_back({ "TITLE", c.spec.name });
                ogg.comments.push_back({ "ENGINE", info.name });
                ogg.comments.push_back({ "ENGINE_LOAD", c.spec.load });
                ogg.comments.push_back({ "ENGINE_RPM", std::to_string(std::lround(c.rpm.mean)) });
                if (isLoop) {
                    ogg.comments.push_back({ "LOOPSTART", "0" });
                    ogg.comments.push_back({ "LOOPLENGTH", std::to_string(c.audio.size()) });
                }
                ok = encodeOggVorbis(p.string(), c.audio, static_cast<int>(SampleRate), ogg, &err);
                if (ok) files[i].ogg = relativePath(p, root);
            }

            if (ok && options.writeMp3) {
                const fs::path p = mp3Dir / (c.spec.name + ".mp3");
                ok = encodeMp3(p.string(), c.audio, static_cast<int>(SampleRate), options.mp3, &err);
                if (ok) files[i].mp3 = relativePath(p, root);
            }

            if (ok && options.writeWav) {
                const fs::path p = wavDir / (c.spec.name + ".wav");
                WavWriteOptions wav;
                wav.bitsPerSample = options.wavBits;
                wav.hasLoop = isLoop;
                wav.loopStart = 0;
                wav.loopLength = static_cast<uint32_t>(c.audio.size());
                wav.ditherSeed = clipHash | 1u;
                ok = writeWavMono(p.string(), c.audio, static_cast<int>(SampleRate), wav, &err);
                if (ok) files[i].wav = relativePath(p, root);
            }

            if (ok && c.spec.kind != ClipKind::Loop) {
                const fs::path p = telemetryDir / (c.spec.name + ".csv");
                ok = writeTelemetryCsv(p, c.telemetry, &err);
                if (ok) files[i].telemetry = relativePath(p, root);
            }

            if (!ok) {
                std::lock_guard<std::mutex> lock(logMutex);
                log << "  " << c.spec.name << "  FAILED: " << err << "\n";
                c.ok = false;
            }
        }
    };

    threads.clear();
    for (int i = 1; i < jobs; ++i) threads.emplace_back(encoder);
    encoder();
    for (std::thread &t : threads) t.join();

    // Remove the telemetry folder if nothing was written to it.
    if (fs::is_empty(telemetryDir, ec)) fs::remove(telemetryDir, ec);

    std::string manifestError;
    writeManifest(root / "manifest.json", options, info, rpmMin, rpmMax, setGainDb,
        clips, files, levels, &manifestError);
    if (!manifestError.empty()) {
        log << "error: " << manifestError << "\n";
        return 1;
    }

    const size_t written = static_cast<size_t>(std::count_if(
        clips.begin(), clips.end(), [](const RenderedClip &c) { return c.ok; }));
    const size_t failures = clips.size() - written;

    log << "Done: " << written << " clip(s) written to " << root.string();
    if (options.normalization == Normalization::Set) {
        char gain[64];
        std::snprintf(gain, sizeof(gain), " (set gain %+.1f dB)", setGainDb);
        log << gain;
    }
    log << "\n";

    if (failures > 0) {
        log << failures << " clip(s) failed\n";
        return 1;
    }

    return 0;
}
