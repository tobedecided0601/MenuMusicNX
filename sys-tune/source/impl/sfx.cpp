#include "sfx.hpp"

#include "sdmc/sdmc.hpp"
#include "pm/pm.hpp"

#include <algorithm>

#ifdef WANT_WAV
#define DR_WAV_NO_STDIO
#include "dr_wav.h"
#endif

namespace tune::impl::sfx {

#ifdef WANT_WAV
    namespace {

        struct State {
            FsFile file{};
            s64 offset{};
            s64 size{};
            drwav wav{};
            bool open{};
            float volume{1.f};

            /* Source frames (converted to stereo) and linear-interpolation
               position between src[0] and src[1] (16.16 fixed point). */
            static constexpr size_t ChunkFrames = 256;
            s16 chunk[ChunkFrames * 2]{};
            size_t chunk_frames{};
            size_t chunk_pos{};
            s16 prev[2]{};
            s16 next[2]{};
            u32 frac{};
            bool primed{};
            bool ended{};
        };

        State g_state;
        Mutex g_mutex{};

        struct Lock {
            Lock() { mutexLock(&g_mutex); }
            ~Lock() { mutexUnlock(&g_mutex); }
        };

        size_t ReadCallback(void *user, void *out, size_t size) {
            auto *s = static_cast<State *>(user);
            u64 read = 0;
            if (R_FAILED(fsFileRead(&s->file, s->offset, out, size, FsReadOption_None, &read)))
                return 0;
            s->offset += static_cast<s64>(read);
            return static_cast<size_t>(read);
        }

        drwav_bool32 SeekCallback(void *user, int offset, drwav_seek_origin origin) {
            auto *s = static_cast<State *>(user);
            s64 target = 0;
            switch (origin) {
                case DRWAV_SEEK_SET: target = offset; break;
                case DRWAV_SEEK_CUR: target = s->offset + offset; break;
                case DRWAV_SEEK_END: target = s->size + offset; break;
                default: return DRWAV_FALSE;
            }
            if (target < 0 || target > s->size)
                return DRWAV_FALSE;
            s->offset = target;
            return DRWAV_TRUE;
        }

        drwav_bool32 TellCallback(void *user, drwav_int64 *cursor) {
            *cursor = static_cast<State *>(user)->offset;
            return DRWAV_TRUE;
        }

        void CloseLocked() {
            if (!g_state.open)
                return;
            drwav_uninit(&g_state.wav);
            fsFileClose(&g_state.file);
            g_state.open = false;
        }

        /* Next source frame as stereo; false at the end of the file. */
        bool NextFrame(s16 out[2]) {
            if (g_state.chunk_pos >= g_state.chunk_frames) {
                const u32 channels = g_state.wav.channels;
                /* Decode into the tail of the chunk buffer, then expand to
                   stereo in place (mono duplicated, >2 channels: first two). */
                static s16 raw[State::ChunkFrames * 8];
                const size_t max_frames = std::min<size_t>(State::ChunkFrames, sizeof(raw) / sizeof(raw[0]) / channels);
                const auto got = static_cast<size_t>(drwav_read_pcm_frames_s16(&g_state.wav, max_frames, raw));
                if (got == 0)
                    return false;
                for (size_t i = 0; i < got; ++i) {
                    const s16 l = raw[i * channels];
                    const s16 r = channels >= 2 ? raw[i * channels + 1] : l;
                    g_state.chunk[i * 2]     = l;
                    g_state.chunk[i * 2 + 1] = r;
                }
                g_state.chunk_frames = got;
                g_state.chunk_pos    = 0;
            }
            out[0] = g_state.chunk[g_state.chunk_pos * 2];
            out[1] = g_state.chunk[g_state.chunk_pos * 2 + 1];
            g_state.chunk_pos++;
            return true;
        }

        s16 Saturate(s32 v) {
            return static_cast<s16>(std::clamp<s32>(v, -32768, 32767));
        }

    }

    bool Start(const char *path, float volume) {
        Lock lk;
        CloseLocked();

        g_state = State{};
        g_state.volume = std::clamp(volume, 0.f, 1.f);

        Result rc = sdmc::OpenFile(&g_state.file, path);
        if (R_FAILED(rc)) {
            pm::Log("sfx: can't open %s (rc=0x%X)", path, rc);
            return false;
        }
        if (R_FAILED(fsFileGetSize(&g_state.file, &g_state.size)) ||
            !drwav_init(&g_state.wav, ReadCallback, SeekCallback, TellCallback, &g_state, nullptr)) {
            pm::Log("sfx: %s is not a readable WAV file", path);
            fsFileClose(&g_state.file);
            return false;
        }
        if (g_state.wav.channels == 0 || g_state.wav.sampleRate == 0) {
            pm::Log("sfx: %s has no audio", path);
            drwav_uninit(&g_state.wav);
            fsFileClose(&g_state.file);
            return false;
        }

        g_state.open = true;
        pm::Log("sfx: playing %s (%u Hz, %u ch, %llu frames, volume %.2f)", path,
                static_cast<unsigned>(g_state.wav.sampleRate), static_cast<unsigned>(g_state.wav.channels),
                static_cast<unsigned long long>(g_state.wav.totalPCMFrameCount), static_cast<double>(g_state.volume));
        return true;
    }

    bool Active() {
        Lock lk;
        return g_state.open;
    }

    size_t Mix(s16 *out, size_t frames, u32 out_rate) {
        Lock lk;
        if (!g_state.open || out_rate == 0)
            return 0;

        /* Source frames per output frame, 16.16 fixed point. */
        const u32 step = static_cast<u32>((static_cast<u64>(g_state.wav.sampleRate) << 16) / out_rate);
        const s32 gain = static_cast<s32>(g_state.volume * 256.f);

        if (!g_state.primed) {
            if (!NextFrame(g_state.prev)) {
                CloseLocked();
                return 0;
            }
            if (!NextFrame(g_state.next)) {
                g_state.next[0] = g_state.prev[0];
                g_state.next[1] = g_state.prev[1];
                g_state.ended = true;
            }
            g_state.primed = true;
        }

        size_t written = 0;
        while (written < frames) {
            for (int c = 0; c < 2; ++c) {
                const s32 a = g_state.prev[c];
                const s32 b = g_state.next[c];
                const s32 sample = a + static_cast<s32>((static_cast<s64>(b - a) * g_state.frac) >> 16);
                out[written * 2 + c] = Saturate(out[written * 2 + c] + ((sample * gain) >> 8));
            }
            written++;

            g_state.frac += step;
            while (g_state.frac >= 0x10000) {
                g_state.frac -= 0x10000;
                if (g_state.ended) {
                    CloseLocked();
                    return written;
                }
                g_state.prev[0] = g_state.next[0];
                g_state.prev[1] = g_state.next[1];
                if (!NextFrame(g_state.next)) {
                    /* Hold the last sample for the final interpolation step. */
                    g_state.next[0] = g_state.prev[0];
                    g_state.next[1] = g_state.prev[1];
                    g_state.ended = true;
                }
            }
        }
        return written;
    }

    void Stop() {
        Lock lk;
        CloseLocked();
    }

#else

    bool Start(const char *path, float) {
        pm::Log("sfx: built without WAV support, can't play %s", path);
        return false;
    }
    bool Active() { return false; }
    size_t Mix(s16 *, size_t, u32) { return 0; }
    void Stop() {}

#endif

}
