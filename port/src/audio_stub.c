/*
 * audio_stub.c — replaces src/code/audio_stop_all_sfx.c during bring-up.
 * The audio engine's bank structures are uninitialized (no audio thread yet);
 * walking them spins forever. Real audio is a later phase.
 */
void AudioMgr_StopAllSfx(void) {}
