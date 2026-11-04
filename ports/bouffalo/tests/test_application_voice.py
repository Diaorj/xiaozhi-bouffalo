"""Actual Application and Bouffalo runtime controlling real audio/protocol queues."""
from test_reply_audio import ReplyAudioTests

class ApplicationVoiceTests(ReplyAudioTests):
    application_tests = True

del ReplyAudioTests
