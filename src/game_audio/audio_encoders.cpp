#include "../../include/game_audio/audio_encoders.h"

#include <lame/lame.h>
#include <vorbis/vorbisenc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

namespace {

    struct FileCloser {
        void operator()(std::FILE *f) const {
            if (f != nullptr) std::fclose(f);
        }
    };

    using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

    FilePtr openForWriting(const std::string &path) {
        std::FILE *f = nullptr;
#ifdef _MSC_VER
        if (fopen_s(&f, path.c_str(), "wb+") != 0) f = nullptr;
#else
        f = std::fopen(path.c_str(), "wb+");
#endif
        return FilePtr(f);
    }

    bool writeAll(std::FILE *f, const void *data, size_t bytes) {
        return bytes == 0 || std::fwrite(data, 1, bytes, f) == bytes;
    }

    float sanitize(float s) {
        if (!std::isfinite(s)) return 0.0f;
        return std::max(-1.0f, std::min(1.0f, s));
    }

    // RAII owner for the libvorbis/libogg encoder state.
    class VorbisEncoder {
    public:
        VorbisEncoder() {
            vorbis_info_init(&m_info);
            vorbis_comment_init(&m_comment);
        }

        ~VorbisEncoder() {
            if (m_streamInitialized) ogg_stream_clear(&m_stream);
            if (m_blockInitialized) vorbis_block_clear(&m_block);
            if (m_dspInitialized) vorbis_dsp_clear(&m_dsp);
            vorbis_comment_clear(&m_comment);
            vorbis_info_clear(&m_info);
        }

        VorbisEncoder(const VorbisEncoder &) = delete;
        VorbisEncoder &operator=(const VorbisEncoder &) = delete;

        bool initialize(int sampleRate, float quality, int serial, std::string *error) {
            const float q = std::max(-1.0f, std::min(10.0f, quality)) / 10.0f;
            if (vorbis_encode_init_vbr(&m_info, 1, sampleRate, q) != 0) {
                *error = "libvorbis rejected the encoder settings";
                return false;
            }

            if (vorbis_analysis_init(&m_dsp, &m_info) != 0) {
                *error = "vorbis_analysis_init failed";
                return false;
            }
            m_dspInitialized = true;

            if (vorbis_block_init(&m_dsp, &m_block) != 0) {
                *error = "vorbis_block_init failed";
                return false;
            }
            m_blockInitialized = true;

            if (ogg_stream_init(&m_stream, serial) != 0) {
                *error = "ogg_stream_init failed";
                return false;
            }
            m_streamInitialized = true;

            return true;
        }

        vorbis_info m_info;
        vorbis_comment m_comment;
        vorbis_dsp_state m_dsp;
        vorbis_block m_block;
        ogg_stream_state m_stream;

    private:
        bool m_dspInitialized = false;
        bool m_blockInitialized = false;
        bool m_streamInitialized = false;
    };

    bool writePage(std::FILE *f, const ogg_page &page) {
        return writeAll(f, page.header, static_cast<size_t>(page.header_len))
            && writeAll(f, page.body, static_cast<size_t>(page.body_len));
    }

    // Drains finished analysis blocks into Ogg pages.
    bool drainVorbis(VorbisEncoder &enc, std::FILE *f) {
        ogg_packet packet;
        ogg_page page;

        while (vorbis_analysis_blockout(&enc.m_dsp, &enc.m_block) == 1) {
            if (vorbis_analysis(&enc.m_block, nullptr) != 0) return false;
            if (vorbis_bitrate_addblock(&enc.m_block) != 0) return false;

            while (vorbis_bitrate_flushpacket(&enc.m_dsp, &packet) == 1) {
                ogg_stream_packetin(&enc.m_stream, &packet);

                while (ogg_stream_pageout(&enc.m_stream, &page) != 0) {
                    if (!writePage(f, page)) return false;
                }
            }
        }

        return true;
    }

    struct LameCloser {
        void operator()(lame_global_flags *gf) const {
            if (gf != nullptr) lame_close(gf);
        }
    };

} /* namespace */

bool game_audio::encodeOggVorbis(
    const std::string &path,
    const std::vector<float> &samples,
    int sampleRate,
    const OggVorbisOptions &options,
    std::string *error)
{
    VorbisEncoder enc;
    if (!enc.initialize(sampleRate, options.quality, options.serialNumber, error)) {
        return false;
    }

    vorbis_comment_add_tag(&enc.m_comment, "ENCODER", "engine-sim-export");
    for (const auto &tag : options.comments) {
        vorbis_comment_add_tag(&enc.m_comment, tag.first.c_str(), tag.second.c_str());
    }

    FilePtr file = openForWriting(path);
    if (!file) {
        *error = "cannot create '" + path + "'";
        return false;
    }

    ogg_packet header, headerComment, headerCode;
    vorbis_analysis_headerout(&enc.m_dsp, &enc.m_comment, &header, &headerComment, &headerCode);
    ogg_stream_packetin(&enc.m_stream, &header);
    ogg_stream_packetin(&enc.m_stream, &headerComment);
    ogg_stream_packetin(&enc.m_stream, &headerCode);

    // The audio must start on a fresh page.
    ogg_page page;
    while (ogg_stream_flush(&enc.m_stream, &page) != 0) {
        if (!writePage(file.get(), page)) {
            *error = "failed writing '" + path + "'";
            return false;
        }
    }

    constexpr size_t ChunkSize = 4096;
    for (size_t offset = 0; offset < samples.size(); offset += ChunkSize) {
        const size_t n = std::min(ChunkSize, samples.size() - offset);
        float **buffer = vorbis_analysis_buffer(&enc.m_dsp, static_cast<int>(n));
        for (size_t i = 0; i < n; ++i) {
            buffer[0][i] = sanitize(samples[offset + i]);
        }

        vorbis_analysis_wrote(&enc.m_dsp, static_cast<int>(n));
        if (!drainVorbis(enc, file.get())) {
            *error = "Vorbis encoding failed for '" + path + "'";
            return false;
        }
    }

    // End of stream
    vorbis_analysis_wrote(&enc.m_dsp, 0);
    if (!drainVorbis(enc, file.get())) {
        *error = "Vorbis encoding failed for '" + path + "'";
        return false;
    }

    while (ogg_stream_flush(&enc.m_stream, &page) != 0) {
        if (!writePage(file.get(), page)) {
            *error = "failed writing '" + path + "'";
            return false;
        }
    }

    if (std::fflush(file.get()) != 0) {
        *error = "failed writing '" + path + "'";
        return false;
    }

    return true;
}

bool game_audio::encodeMp3(
    const std::string &path,
    const std::vector<float> &samples,
    int sampleRate,
    const Mp3Options &options,
    std::string *error)
{
    std::unique_ptr<lame_global_flags, LameCloser> gf(lame_init());
    if (!gf) {
        *error = "lame_init failed";
        return false;
    }

    lame_set_in_samplerate(gf.get(), sampleRate);
    lame_set_out_samplerate(gf.get(), sampleRate);
    lame_set_num_channels(gf.get(), 1);
    lame_set_mode(gf.get(), MONO);
    lame_set_quality(gf.get(), std::max(0, std::min(9, options.algorithmQuality)));
    lame_set_bWriteVbrTag(gf.get(), 1);

    if (options.vbr) {
        lame_set_VBR(gf.get(), vbr_default);
        lame_set_VBR_q(gf.get(), std::max(0, std::min(9, options.vbrQuality)));
    }
    else {
        lame_set_VBR(gf.get(), vbr_off);
        lame_set_brate(gf.get(), options.cbrKbps);
    }

    if (lame_init_params(gf.get()) < 0) {
        *error = "LAME rejected the encoder settings";
        return false;
    }

    FilePtr file = openForWriting(path);
    if (!file) {
        *error = "cannot create '" + path + "'";
        return false;
    }

    constexpr int ChunkSize = 8192;
    // Worst case output size documented by LAME: 1.25 * n + 7200
    std::vector<unsigned char> mp3Buffer(static_cast<size_t>(ChunkSize * 5 / 4 + 7200));
    std::vector<float> chunk(ChunkSize);

    for (size_t offset = 0; offset < samples.size(); offset += ChunkSize) {
        const int n = static_cast<int>(std::min<size_t>(ChunkSize, samples.size() - offset));
        for (int i = 0; i < n; ++i) {
            chunk[i] = sanitize(samples[offset + i]);
        }

        const int bytes = lame_encode_buffer_ieee_float(
            gf.get(),
            chunk.data(),
            chunk.data(),
            n,
            mp3Buffer.data(),
            static_cast<int>(mp3Buffer.size()));
        if (bytes < 0) {
            *error = "LAME encoding failed for '" + path + "' (code " + std::to_string(bytes) + ")";
            return false;
        }

        if (!writeAll(file.get(), mp3Buffer.data(), static_cast<size_t>(bytes))) {
            *error = "failed writing '" + path + "'";
            return false;
        }
    }

    const int flushed = lame_encode_flush(
        gf.get(), mp3Buffer.data(), static_cast<int>(mp3Buffer.size()));
    if (flushed < 0) {
        *error = "LAME flush failed for '" + path + "'";
        return false;
    }

    if (!writeAll(file.get(), mp3Buffer.data(), static_cast<size_t>(flushed))) {
        *error = "failed writing '" + path + "'";
        return false;
    }

    // Rewrites the reserved first frame with the final Xing/LAME info tag
    // (frame count, seek table, encoder delay and padding).
    lame_mp3_tags_fid(gf.get(), file.get());

    if (std::fflush(file.get()) != 0) {
        *error = "failed writing '" + path + "'";
        return false;
    }

    return true;
}
