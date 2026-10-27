#pragma once
#include <memory>
#include "audio/audio_decoder.h"

std::unique_ptr<AudioDecoder> CreateBouffaloOpusDecoder();
