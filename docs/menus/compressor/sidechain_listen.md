# Sidechain Listen

When enabled, the compressor uses the global audio sidechain bus for its envelope detection
instead of the track's own audio. This allows true audio sidechain compression, where the
audio output of one track (the sender) triggers the compressor on this track (the receiver).

## Configuration

- Set the **sidechain send level** on the source track (e.g., a kick drum)
- Enable **Sidechain Listen** on the target track (e.g., a pad or bass)
- Configure the compressor threshold, ratio, attack, and release on the target track as desired
