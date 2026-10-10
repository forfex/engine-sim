#ifndef ATG_ENGINE_SIM_GAME_AUDIO_JSON_WRITER_H
#define ATG_ENGINE_SIM_GAME_AUDIO_JSON_WRITER_H

#include <string>
#include <vector>

namespace game_audio {

    // Minimal streaming JSON writer with pretty printing. Callers are
    // responsible for well-formed nesting; misuse is caught by assertions.
    class JsonWriter {
    public:
        JsonWriter &beginObject();
        JsonWriter &endObject();
        JsonWriter &beginArray();
        JsonWriter &endArray();

        // Writes the key of the next member (inside an object only).
        JsonWriter &key(const std::string &name);

        JsonWriter &value(const std::string &v);
        JsonWriter &value(const char *v);
        JsonWriter &value(double v);
        JsonWriter &value(int v);
        JsonWriter &value(long long v);
        JsonWriter &value(size_t v);
        JsonWriter &value(bool v);
        JsonWriter &null();

        const std::string &str() const { return m_out; }

        static std::string escape(const std::string &s);

    private:
        struct Scope {
            bool isObject;
            bool empty;
        };

        void beforeValue();
        void newline();

        std::string m_out;
        std::vector<Scope> m_stack;
        bool m_afterKey = false;
    };

} /* namespace game_audio */

#endif /* ATG_ENGINE_SIM_GAME_AUDIO_JSON_WRITER_H */
