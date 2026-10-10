#ifndef ATG_ENGINE_SIM_GAME_AUDIO_ENGINE_SESSION_H
#define ATG_ENGINE_SIM_GAME_AUDIO_ENGINE_SESSION_H

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Engine;
class Simulator;
class Transmission;
class Vehicle;

namespace game_audio {

    struct SessionConfig {
        // Engine script, e.g. assets/main.mr
        std::string scriptPath;

        // Directories searched for script imports (the engine-sim "es" library).
        std::vector<std::string> searchPaths;

        // Physics steps per second. 0 uses the value set by the engine script.
        // Higher values reduce aliasing at high RPM at the cost of render time.
        double simulationFrequency = 0.0;
    };

    struct TelemetrySample {
        double time = 0.0;          // seconds since the session started
        size_t audioSample = 0;     // index into the audio buffer being recorded
        double rpm = 0.0;
        double throttle = 0.0;      // smoothed speed-control value actually applied
        double dynoTorque = 0.0;    // N*m, positive when the engine drives the dyno
    };

    // One engine instance driven entirely offline, without a window, audio
    // device or audio thread. Controls mirror the interactive application
    // (ignition, starter, throttle, dyno with RPM hold) and audio is produced
    // at 44.1 kHz from the same synthesizer, before the real-time leveler.
    //
    // Sessions are independent and may run on different threads. Creation is
    // serialized internally because the script compiler keeps global state.
    class EngineSession {
    public:
        static constexpr int AudioSampleRate = 44100;
        static constexpr double BlockDuration = 1.0 / 250.0;

        struct Info {
            std::string name;
            int cylinderCount = 0;
            int exhaustSystemCount = 0;
            double displacementLiters = 0.0;
            double redlineRpm = 0.0;
            double dynoMinRpm = 0.0;
            double dynoMaxRpm = 0.0;
            double starterRpm = 0.0;
            double simulationFrequency = 0.0;
        };

    public:
        ~EngineSession();

        EngineSession(const EngineSession &) = delete;
        EngineSession &operator=(const EngineSession &) = delete;

        static std::unique_ptr<EngineSession> create(
            const SessionConfig &config, std::string *error);

        const Info &info() const { return m_info; }

        // Target throttle (speed control) in [0, 1]. The applied value follows
        // the target with a short time constant, as in the interactive app.
        void setThrottle(double target);
        void setThrottleImmediate(double value);
        void setIgnition(bool enabled);
        void setStarter(bool enabled);

        // Dynamometer holding the crankshaft at `rpm` (absorbing torque when
        // the engine is on throttle, motoring it when the throttle is closed).
        void setDynoHold(double rpm);
        void releaseDyno();

        // Runs the simulation for `seconds`. Audio is appended to `audio` and
        // one telemetry sample per block to `telemetry`; either may be null.
        void advance(
            double seconds,
            std::vector<float> *audio,
            std::vector<TelemetrySample> *telemetry);

        // Runs until `condition` returns true (checked after every block) or
        // `timeout` seconds elapse. Returns whether the condition was met.
        bool advanceUntil(
            const std::function<bool(const EngineSession &)> &condition,
            double timeout,
            std::vector<float> *audio,
            std::vector<TelemetrySample> *telemetry);

        double rpm() const;
        double time() const { return m_time; }
        double appliedThrottle() const { return m_throttle; }

    private:
        EngineSession() = default;

        void advanceBlock(
            double dt,
            std::vector<float> *audio,
            std::vector<TelemetrySample> *telemetry);

        Info m_info;

        Engine *m_engine = nullptr;
        Vehicle *m_vehicle = nullptr;
        Transmission *m_transmission = nullptr;
        Simulator *m_simulator = nullptr;

        double m_time = 0.0;
        double m_throttle = 0.0;
        double m_targetThrottle = 0.0;

        std::vector<float> m_scratch;
    };

} /* namespace game_audio */

#endif /* ATG_ENGINE_SIM_GAME_AUDIO_ENGINE_SESSION_H */
