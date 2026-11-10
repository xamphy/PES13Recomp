// miniaudio (thirdparty/miniaudio, v0.11.21, public domain / MIT-0): the implementation is
// compiled once, here. Only playback is used (runtime/audio/xaudio_imports.cpp); the backends
// are WASAPI on Windows and Core Audio on macOS / iOS.
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#include "miniaudio.h"
