#ifndef PORTAUDIO_H
#define PORTAUDIO_H

typedef struct PaStreamCallbackTimeInfo {
    double inputBufferAdcTime;
    double currentTime;
    double outputBufferDacTime;
} PaStreamCallbackTimeInfo;

typedef unsigned long PaStreamCallbackFlags;

#endif
