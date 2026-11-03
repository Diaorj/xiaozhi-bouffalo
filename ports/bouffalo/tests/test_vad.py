# Actual energy VAD: DC rejection, long idle, short clicks, speech and end silence.
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_ota_check import PORT

class VadTests(unittest.TestCase):
    def test_noise_voice_silence_and_bounded_idle(self):
        with tempfile.TemporaryDirectory() as directory:
            work=Path(directory)
            (work/'check.c').write_text(r'''
#include <assert.h>
#include <stdint.h>
#include "audio_vad.h"
static audio_vad_event_t block(audio_vad_t* v,int amplitude,int dc) {
    int16_t pcm[160];for(unsigned i=0;i<160;i++)pcm[i]=dc+(i&1?amplitude:-amplitude);
    return audio_vad_process(v,pcm,160);
}
int main(void) {
    audio_vad_t v;audio_vad_init_listener(&v,160000);
    for(unsigned i=0;i<29;i++)assert(block(&v,0,16000)==AUDIO_VAD_CONTINUE);
    assert(block(&v,0,16000)==AUDIO_VAD_READY && v.noise==0);
    for(unsigned i=0;i<100000;i++)assert(block(&v,0,16000)==AUDIO_VAD_CONTINUE);
    assert(!v.speaking && !v.finished && v.total<6000);
    for(unsigned i=0;i<8;i++)block(&v,500,16000);
    assert(v.speaking && !v.valid_voice);
    for(unsigned i=0;i<81;i++)block(&v,0,16000);
    assert(!v.speaking && !v.valid_voice && !v.finished);
    for(unsigned i=0;i<20;i++)block(&v,500,16000);
    assert(v.speaking && v.valid_voice);
    for(unsigned i=0;i<79;i++)assert(block(&v,0,16000)==AUDIO_VAD_CONTINUE);
    assert(block(&v,0,16000)==AUDIO_VAD_END_SILENCE && v.finished);
    audio_vad_init_listener(&v,160000);
    for(unsigned i=0;i<30;i++)block(&v,5,0);
    assert(v.noise==5 && v.start_threshold>=40 && v.hold_threshold>=25);
    audio_vad_event_t event=AUDIO_VAD_CONTINUE;
    for(unsigned i=0;i<1100 && !v.finished;i++)event=block(&v,500,0);
    assert(event==AUDIO_VAD_END_LIMIT && v.valid_voice);
}
''')
            subprocess.run(['gcc','-std=c99','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(PORT),str(PORT/'audio_vad.c'),str(work/'check.c'),'-o',str(work/'check')],check=True)
            subprocess.run([str(work/'check')],check=True,timeout=10)

if __name__=='__main__':unittest.main()
