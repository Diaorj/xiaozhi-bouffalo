#pragma once
#include <memory>
#include "audio/audio_encoder.h"
std::unique_ptr<AudioEncoder> CreateBouffaloOpusEncoder();
