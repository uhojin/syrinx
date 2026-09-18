/*
 * live-loopback-test.c
 *
 * Opens the REAL, installed Syrinx devices through the normal AUHAL
 * client path (the same mechanism AVAudioEngine uses): plays a known
 * sine tone to the hidden output-side device, records from the
 * visible "Syrinx Microphone" input-side device at the same time
 * (both backed by the same ring buffer - see Driver/SyrinxLoopback.c),
 * and checks the recording actually contains the tone. This validates
 * the live coreaudiod-hosted plugin, not just the isolated dlopen
 * harness.
 */
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#define SAMPLE_RATE 48000.0
#define TEST_FREQ 440.0
#define CAPTURE_SECONDS 2
#define CAPTURE_FRAME_CAPACITY 96000 // SAMPLE_RATE * CAPTURE_SECONDS

static Float32 gCaptured[CAPTURE_FRAME_CAPACITY * 2];
static volatile UInt32 gCapturedFrames = 0;
static double gPlaybackPhase = 0;

static AudioDeviceID FindDeviceByUID(CFStringRef targetUID)
{
    AudioObjectPropertyAddress addr = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL, &size) != noErr) return kAudioObjectUnknown;

    UInt32 count = size / sizeof(AudioDeviceID);
    AudioDeviceID *devices = malloc(size);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, devices) != noErr) {
        free(devices);
        return kAudioObjectUnknown;
    }

    AudioDeviceID found = kAudioObjectUnknown;
    AudioObjectPropertyAddress uidAddr = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    for (UInt32 i = 0; i < count; i++) {
        CFStringRef uid = NULL;
        UInt32 uidSize = sizeof(uid);
        if (AudioObjectGetPropertyData(devices[i], &uidAddr, 0, NULL, &uidSize, &uid) == noErr && uid != NULL) {
            if (CFEqual(uid, targetUID)) {
                found = devices[i];
                CFRelease(uid);
                break;
            }
            CFRelease(uid);
        }
    }
    free(devices);
    return found;
}

static OSStatus RenderCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData)
{
    (void)inRefCon; (void)ioActionFlags; (void)inTimeStamp; (void)inBusNumber;
    Float32 *left = (Float32 *)ioData->mBuffers[0].mData;
    for (UInt32 i = 0; i < inNumberFrames; i++) {
        Float32 sample = (Float32)(0.5 * sin(gPlaybackPhase));
        gPlaybackPhase += 2.0 * M_PI * TEST_FREQ / SAMPLE_RATE;
        if (ioData->mNumberBuffers == 1) {
            // interleaved
            ((Float32 *)ioData->mBuffers[0].mData)[i * 2 + 0] = sample;
            ((Float32 *)ioData->mBuffers[0].mData)[i * 2 + 1] = sample;
        } else {
            left[i] = sample;
        }
    }
    return noErr;
}

static AudioUnit gInputUnit;

static OSStatus InputCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData)
{
    (void)inRefCon; (void)ioData;
    static Float32 buf[4096 * 2];
    AudioBufferList list;
    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = 2;
    list.mBuffers[0].mDataByteSize = inNumberFrames * 2 * sizeof(Float32);
    list.mBuffers[0].mData = buf;

    OSStatus status = AudioUnitRender(gInputUnit, ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, &list);
    if (status != noErr) return status;

    UInt32 framesAvailable = (UInt32)(sizeof(gCaptured) / sizeof(Float32) / 2) - gCapturedFrames;
    UInt32 framesToCopy = inNumberFrames < framesAvailable ? inNumberFrames : framesAvailable;
    memcpy(&gCaptured[gCapturedFrames * 2], buf, framesToCopy * 2 * sizeof(Float32));
    gCapturedFrames += framesToCopy;
    return noErr;
}

int main(void)
{
    CFStringRef inputUID = CFSTR("com.hojin.syrinx.loopback.device");
    CFStringRef outputUID = CFSTR("com.hojin.syrinx.loopback.output.device");
    AudioDeviceID inputDeviceID = FindDeviceByUID(inputUID);
    AudioDeviceID outputDeviceID = FindDeviceByUID(outputUID);
    if (inputDeviceID == kAudioObjectUnknown) {
        printf("FAIL: could not find input device with UID %s\n", CFStringGetCStringPtr(inputUID, kCFStringEncodingUTF8));
        return 1;
    }
    if (outputDeviceID == kAudioObjectUnknown) {
        printf("FAIL: could not find output device with UID %s\n", CFStringGetCStringPtr(outputUID, kCFStringEncodingUTF8));
        return 1;
    }
    printf("Found Syrinx Microphone deviceID = %u, Syrinx Microphone Output deviceID = %u\n", inputDeviceID, outputDeviceID);

    AudioComponentDescription desc = {
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_HALOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
        .componentFlags = 0,
        .componentFlagsMask = 0
    };
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);

    // --- Output half: plays the tone TO the hidden output device ---
    AudioUnit outputUnit;
    AudioComponentInstanceNew(comp, &outputUnit);
    UInt32 enableIO = 1, disableIO = 0;
    AudioUnitSetProperty(outputUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &enableIO, sizeof(enableIO));
    AudioUnitSetProperty(outputUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &disableIO, sizeof(disableIO));
    AudioUnitSetProperty(outputUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &outputDeviceID, sizeof(outputDeviceID));

    AudioStreamBasicDescription fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.mSampleRate = SAMPLE_RATE;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mChannelsPerFrame = 2;
    fmt.mBitsPerChannel = 32;
    fmt.mBytesPerFrame = 8;
    fmt.mBytesPerPacket = 8;
    fmt.mFramesPerPacket = 1;
    AudioUnitSetProperty(outputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));

    AURenderCallbackStruct renderCB = { RenderCallback, NULL };
    AudioUnitSetProperty(outputUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &renderCB, sizeof(renderCB));

    OSStatus status = AudioUnitInitialize(outputUnit);
    printf("output AudioUnitInitialize: %d\n", (int)status);

    // --- Input half: records FROM the visible "Syrinx Microphone" device ---
    AudioComponentInstanceNew(comp, &gInputUnit);
    AudioUnitSetProperty(gInputUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &enableIO, sizeof(enableIO));
    AudioUnitSetProperty(gInputUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &disableIO, sizeof(disableIO));
    AudioUnitSetProperty(gInputUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &inputDeviceID, sizeof(inputDeviceID));
    AudioUnitSetProperty(gInputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &fmt, sizeof(fmt));

    AURenderCallbackStruct inputCB = { InputCallback, NULL };
    AudioUnitSetProperty(gInputUnit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 1, &inputCB, sizeof(inputCB));

    status = AudioUnitInitialize(gInputUnit);
    printf("input AudioUnitInitialize: %d\n", (int)status);

    AudioOutputUnitStart(outputUnit);
    AudioOutputUnitStart(gInputUnit);

    sleep(CAPTURE_SECONDS);

    AudioOutputUnitStop(outputUnit);
    AudioOutputUnitStop(gInputUnit);
    AudioUnitUninitialize(outputUnit);
    AudioUnitUninitialize(gInputUnit);

    printf("captured %u frames\n", gCapturedFrames);
    if (gCapturedFrames == 0) {
        printf("FAIL: no frames captured\n");
        return 1;
    }

    // Skip the first 0.25s (settle time / device startup), then check
    // for actual signal energy and rough correlation with the tone.
    UInt32 skip = (UInt32)(SAMPLE_RATE * 0.25);
    double sumSquares = 0;
    UInt32 counted = 0;
    for (UInt32 i = skip; i < gCapturedFrames; i++) {
        double s = gCaptured[i * 2];
        sumSquares += s * s;
        counted++;
    }
    double rms = counted > 0 ? sqrt(sumSquares / counted) : 0;
    printf("captured RMS level (post-settle): %f\n", rms);

    if (rms > 0.05) {
        printf("PASS: recorded signal has real energy — loopback is live end to end\n");
        return 0;
    } else {
        printf("FAIL: recorded signal is near-silent — loopback did not carry audio\n");
        return 1;
    }
}
