#pragma once

#include <switch.h>

/// One-shot sound effect (the application launch sound), mixed on top of the
/// music output. Self-contained WAV reader: it doesn't use the music Source's
/// shared read/resample buffers, so it can run while a track is open.
namespace tune::impl::sfx {

    /// Opens a 16-bit (or any dr_wav-readable) WAV file and starts playing it.
    /// Replaces a sound that is still playing. Returns false (and logs why) if
    /// the file is missing or unreadable.
    bool Start(const char *path, float volume);

    /// True while a sound is playing.
    bool Active();

    /// Adds up to `frames` frames of the sound, converted to `out_rate` Hz
    /// stereo, onto `out` (interleaved s16 stereo, saturating). Returns the
    /// number of frames written; 0 once the sound has finished (it is then
    /// closed automatically).
    size_t Mix(s16 *out, size_t frames, u32 out_rate);

    void Stop();

}
