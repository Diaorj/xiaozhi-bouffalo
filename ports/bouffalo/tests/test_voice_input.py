"""Actual queues/protocol and real Opus, with simulated microphone and network."""
import unittest
import test_reply_audio as playback

class VoiceInputTests(playback.ReplyAudioTests):
    voice_input_tests = True

if __name__ == "__main__":
    unittest.main()
