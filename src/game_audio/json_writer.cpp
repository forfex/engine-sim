#include "../../include/game_audio/json_writer.h"

#include <cassert>
#include <cmath>
#include <cstdio>

game_audio::JsonWriter &game_audio::JsonWriter::beginObject() {
    beforeValue();
    m_out += '{';
    m_stack.push_back({ true, true });
    return *this;
}

game_audio::JsonWriter &game_audio::JsonWriter::endObject() {
    assert(!m_stack.empty() && m_stack.back().isObject && !m_afterKey);
    const bool empty = m_stack.back().empty;
    m_stack.pop_back();
    if (!empty) newline();
    m_out += '}';
    return *this;
}

game_audio::JsonWriter &game_audio::JsonWriter::beginArray() {
    beforeValue();
    m_out += '[';
    m_stack.push_back({ false, true });
    return *this;
}

game_audio::JsonWriter &game_audio::JsonWriter::endArray() {
    assert(!m_stack.empty() && !m_stack.back().isObject);
    const bool empty = m_stack.back().empty;
    m_stack.pop_back();
    if (!empty) newline();
    m_out += ']';
    return *this;
}

game_audio::JsonWriter &game_audio::JsonWriter::key(const std::string &name) {
    assert(!m_stack.empty() && m_stack.back().isObject && !m_afterKey);
    if (!m_stack.back().empty) m_out += ',';
    m_stack.back().empty = false;
    newline();
    m_out += '"' + escape(name) + "\": ";
    m_afterKey = true;
    return *this;
}

game_audio::JsonWriter &game_audio::JsonWriter::value(const std::string &v) {
    beforeValue();
    m_out += '"' + escape(v) + '"';
    return *this;
}

game_audio::JsonWriter &game_audio::JsonWriter::value(const char *v) {
    return value(std::string(v));
}

game_audio::JsonWriter &game_audio::JsonWriter::value(double v) {
    beforeValue();
    if (!std::isfinite(v)) {
        // JSON has no representation for NaN or infinity.
        m_out += "null";
        return *this;
    }

    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.10g", v);
    m_out += buffer;
    return *this;
}

game_audio::JsonWriter &game_audio::JsonWriter::value(int v) {
    return value(static_cast<long long>(v));
}

game_audio::JsonWriter &game_audio::JsonWriter::value(long long v) {
    beforeValue();
    m_out += std::to_string(v);
    return *this;
}

game_audio::JsonWriter &game_audio::JsonWriter::value(size_t v) {
    beforeValue();
    m_out += std::to_string(v);
    return *this;
}

game_audio::JsonWriter &game_audio::JsonWriter::value(bool v) {
    beforeValue();
    m_out += v ? "true" : "false";
    return *this;
}

game_audio::JsonWriter &game_audio::JsonWriter::null() {
    beforeValue();
    m_out += "null";
    return *this;
}

std::string game_audio::JsonWriter::escape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 2);

    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c));
                out += buffer;
            }
            else {
                // UTF-8 bytes pass through unchanged.
                out += c;
            }
        }
    }

    return out;
}

void game_audio::JsonWriter::beforeValue() {
    if (m_afterKey) {
        m_afterKey = false;
        return;
    }

    if (m_stack.empty()) {
        assert(m_out.empty() && "a JSON document has a single root value");
        return;
    }

    assert(!m_stack.back().isObject && "object members need a key");
    if (!m_stack.back().empty) m_out += ',';
    m_stack.back().empty = false;
    newline();
}

void game_audio::JsonWriter::newline() {
    m_out += '\n';
    m_out.append(m_stack.size() * 2, ' ');
}
