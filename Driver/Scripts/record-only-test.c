/*
 * record-only-test.c
 *
 * Records from the real installed "Syrinx Microphone" input for a
 * fixed window and reports RMS energy, without playing anything
 * itself. Used to verify that Syrinx's Call Mode actually routes live
 * pad playback into the device while the app is driven through its
 * real UI.
 */
#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#define SAMPLE_RATE 48000.0
#define CAPTURE_SECONDS 6
#define CAPTURE_FRAME_CAPACITY 288000 // SAMPLE_RATE * CAPTURE_SECONDS

static Float32 gCaptured[CAPTURE_FRAME_CAPACITY * 2];
static volatile UInt32 gCapturedFrames = 0;
static AudioUnit gInputUnit;

static AudioDeviceID FindDeviceByUID(CFStringRef targetUID)
{
    AudioObjectPropertyAddress addr = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL, &size) != noErr) return kAudioObjectUnknown;
    UInt32 count = size / sizeof(AudioDeviceID);
    AudioDeviceID *devices = malloc(size);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, devices) != noErr) { free(devices); return kAudioObjectUnknown; }
    AudioDeviceID found = kAudioObjectUnknown;
    AudioObjectPropertyAddress uidAddr = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    for (UInt32 i = 0; i < count; i++) {
        CFStringRef uid = NULL;
        UInt32 uidSize = sizeof(uid);
        if (AudioObjectGetPropertyData(devices[i], &uidAddr, 0, NULL, &uidSize, &uid) == noErr && uid != NULL) {
            if (CFEqual(uid, targetUID)) { found = devices[i]; CFRelease(uid); break; }
            CFRelease(uid);
        }
    }
    free(devices);
    return found;
}

static OSStatus InputCallback(void *inRefCon, AudioUnitRenderActionFlags *ioActionFlags, const AudioTimeStamp *inTimeStamp, UInt32 inBusNumber, UInt32 inNumberFrames, AudioBufferList *ioData)
{
    (void)inRefCon; (void)ioData;
    static Float32 buf[8192 * 2];
    if (inNumberFrames > 8192) return noErr;
    AudioBufferList list;
    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = 2;
    list.mBuffers[0].mDataByteSize = inNumberFrames * 2 * sizeof(Float32);
    list.mBuffers[0].mData = buf;
    OSStatus status = AudioUnitRender(gInputUnit, ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, &list);
    if (status != noErr) return status;
    UInt32 framesAvailable = (UInt32)CAPTURE_FRAME_CAPACITY - gCapturedFrames;
    UInt32 framesToCopy = inNumberFrames < framesAvailable ? inNumberFrames : framesAvailable;
    memcpy(&gCaptured[gCapturedFrames * 2], buf, framesToCopy * 2 * sizeof(Float32));
    gCapturedFrames += framesToCopy;
    return noErr;
}

int main(void)
{
    CFStringRef targetUID = CFSTR("com.hojin.syrinx.loopback.device");
    AudioDeviceID deviceID = FindDeviceByUID(targetUID);
    if (deviceID == kAudioObjectUnknown) {
        printf("FAIL: device not found\n");
        return 1;
    }
    printf("Found Syrinx Loopback deviceID = %u — recording for %ds...\n", deviceID, CAPTURE_SECONDS);
    fflush(stdout);

    AudioComponentDescription desc = { kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    AudioComponentInstanceNew(comp, &gInputUnit);

    UInt32 enableIO = 1, disableIO = 0;
    AudioUnitSetProperty(gInputUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &enableIO, sizeof(enableIO));
    AudioUnitSetProperty(gInputUnit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &disableIO, sizeof(disableIO));
    AudioUnitSetProperty(gInputUnit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &deviceID, sizeof(deviceID));

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
    AudioUnitSetProperty(gInputUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &fmt, sizeof(fmt));

    AURenderCallbackStruct inputCB = { InputCallback, NULL };
    AudioUnitSetProperty(gInputUnit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 1, &inputCB, sizeof(inputCB));

    AudioUnitInitialize(gInputUnit);
    AudioOutputUnitStart(gInputUnit);

    // Report a running RMS every second so a caller can see WHEN
    // energy appears (i.e. correlate with UI actions), not just a
    // single final number.
    for (int second = 0; second < CAPTURE_SECONDS; second++) {
        sleep(1);
        UInt32 frames = gCapturedFrames;
        UInt32 windowStart = frames > (UInt32)SAMPLE_RATE ? frames - (UInt32)SAMPLE_RATE : 0;
        double sumSquares = 0;
        UInt32 counted = 0;
        for (UInt32 i = windowStart; i < frames; i++) {
            double s = gCaptured[i * 2];
            sumSquares += s * s;
            counted++;
        }
        double rms = counted > 0 ? sqrt(sumSquares / counted) : 0;
        printf("  t=%ds  RMS(last 1s)=%.5f\n", second + 1, rms);
        fflush(stdout);
    }

    AudioOutputUnitStop(gInputUnit);
    AudioUnitUninitialize(gInputUnit);

    printf("captured %u frames total\n", gCapturedFrames);
    return 0;
}
