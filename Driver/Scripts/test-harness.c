/*
 * test-harness.c
 *
 * Loads SyrinxLoopback.driver in an isolated throwaway process and
 * drives it the way coreaudiod would: get the factory, QueryInterface
 * for the driver interface, Initialize, then exercise the property
 * and IO vtable methods for the plug-in, both devices (input-side
 * "Syrinx Microphone" and the hidden output-side device), and both
 * streams. If this crashes, it only kills this test process — never
 * the real coreaudiod / system audio.
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static OSStatus HostPropertiesChanged(AudioServerPlugInHostRef inHost, AudioObjectID inObjectID, UInt32 inNumberAddresses, const AudioObjectPropertyAddress *inAddresses)
{
    (void)inHost; (void)inObjectID; (void)inNumberAddresses; (void)inAddresses;
    return noErr;
}
static OSStatus HostCopyFromStorage(AudioServerPlugInHostRef inHost, CFStringRef inKey, CFPropertyListRef *outData)
{
    (void)inHost; (void)inKey;
    *outData = NULL;
    return noErr;
}
static OSStatus HostWriteToStorage(AudioServerPlugInHostRef inHost, CFStringRef inKey, CFPropertyListRef inData)
{
    (void)inHost; (void)inKey; (void)inData;
    return noErr;
}
static OSStatus HostDeleteFromStorage(AudioServerPlugInHostRef inHost, CFStringRef inKey)
{
    (void)inHost; (void)inKey;
    return noErr;
}
static OSStatus HostRequestDeviceConfigurationChange(AudioServerPlugInHostRef inHost, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void *inChangeInfo)
{
    (void)inHost; (void)inDeviceObjectID; (void)inChangeAction; (void)inChangeInfo;
    return noErr;
}

static const AudioServerPlugInHostInterface gFakeHostInterface = {
    HostPropertiesChanged,
    HostCopyFromStorage,
    HostWriteToStorage,
    HostDeleteFromStorage,
    HostRequestDeviceConfigurationChange
};

#define CHECK(label, cond) do { \
    printf("%-55s %s\n", label, (cond) ? "OK" : "FAIL"); \
    if (!(cond)) { failures++; } \
} while (0)

int main(int argc, const char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s /path/to/SyrinxLoopback.driver\n", argv[0]);
        return 1;
    }
    int failures = 0;

    // Fixed AudioObjectIDs, mirroring Driver/SyrinxLoopback.c's static graph.
    const AudioObjectID kDeviceInput = 2;
    const AudioObjectID kStreamInput = 3;
    const AudioObjectID kStreamOutput = 4;
    const AudioObjectID kDeviceOutput = 5;

    CFStringRef path = CFStringCreateWithCString(NULL, argv[1], kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, path, kCFURLPOSIXPathStyle, true);
    CFBundleRef bundle = CFBundleCreate(NULL, url);
    CHECK("CFBundleCreate", bundle != NULL);
    if (bundle == NULL) { return 1; }

    CHECK("CFBundleLoadExecutable", CFBundleLoadExecutable(bundle));

    CFBundleGetFunctionPointersForNames(bundle, NULL, NULL); // no-op sanity call

    void *rawFactory = CFBundleGetFunctionPointerForName(bundle, CFSTR("SyrinxLoopback_Factory"));
    CHECK("Found SyrinxLoopback_Factory symbol", rawFactory != NULL);
    if (rawFactory == NULL) { return 1; }

    typedef void *(*FactoryFn)(CFAllocatorRef, CFUUIDRef);
    FactoryFn factory = (FactoryFn)rawFactory;

    CFUUIDRef bogusUUID = CFUUIDCreateFromString(NULL, CFSTR("00000000-0000-0000-0000-000000000000"));
    void *wrongType = factory(NULL, bogusUUID);
    CFRelease(bogusUUID);
    CHECK("Factory rejects unknown type UUID", wrongType == NULL);

    AudioServerPlugInDriverRef driverRef = (AudioServerPlugInDriverRef)factory(NULL, kAudioServerPlugInTypeUUID);
    CHECK("Factory returns driver ref for correct type UUID", driverRef != NULL);
    if (driverRef == NULL) { return 1; }

    AudioServerPlugInDriverInterface **interfacePtr = (AudioServerPlugInDriverInterface **)driverRef;
    AudioServerPlugInDriverInterface *vtbl = *interfacePtr;
    CHECK("Vtable pointer non-null", vtbl != NULL);

    OSStatus status = vtbl->Initialize(driverRef, (AudioServerPlugInHostRef)&gFakeHostInterface);
    CHECK("Initialize succeeds", status == noErr);

    // --- PlugIn object property checks ---
    AudioObjectPropertyAddress addr = { kAudioObjectPropertyClass, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    Boolean has = vtbl->HasProperty(driverRef, kAudioObjectPlugInObject, 0, &addr);
    CHECK("PlugIn HasProperty(Class)", has);

    UInt32 dataSize = 0;
    status = vtbl->GetPropertyDataSize(driverRef, kAudioObjectPlugInObject, 0, &addr, 0, NULL, &dataSize);
    CHECK("PlugIn GetPropertyDataSize(Class) == sizeof(AudioClassID)", status == noErr && dataSize == sizeof(AudioClassID));

    AudioClassID cls = 0;
    UInt32 outSize = 0;
    status = vtbl->GetPropertyData(driverRef, kAudioObjectPlugInObject, 0, &addr, 0, NULL, sizeof(cls), &outSize, &cls);
    CHECK("PlugIn GetPropertyData(Class) == kAudioPlugInClassID", status == noErr && cls == kAudioPlugInClassID);

    addr.mSelector = kAudioPlugInPropertyDeviceList;
    AudioObjectID deviceList[4] = {0};
    outSize = 0;
    status = vtbl->GetPropertyData(driverRef, kAudioObjectPlugInObject, 0, &addr, 0, NULL, sizeof(deviceList), &outSize, deviceList);
    CHECK("PlugIn DeviceList has exactly 2 devices (input, output)", status == noErr && outSize == sizeof(AudioObjectID) * 2 && deviceList[0] == kDeviceInput && deviceList[1] == kDeviceOutput);

    // --- Input device (visible "Syrinx Microphone") property checks ---
    addr.mSelector = kAudioObjectPropertyName;
    CFStringRef name = NULL;
    outSize = 0;
    status = vtbl->GetPropertyData(driverRef, kDeviceInput, 0, &addr, 0, NULL, sizeof(name), &outSize, &name);
    char nameBuf[256] = {0};
    if (name) { CFStringGetCString(name, nameBuf, sizeof(nameBuf), kCFStringEncodingUTF8); }
    printf("  input device name = \"%s\"\n", nameBuf);
    CHECK("Input device Name == \"Syrinx Microphone\"", status == noErr && name != NULL && strcmp(nameBuf, "Syrinx Microphone") == 0);
    if (name) CFRelease(name);

    addr.mSelector = kAudioDevicePropertyIsHidden;
    UInt32 isHidden = 0xFF;
    outSize = 0;
    status = vtbl->GetPropertyData(driverRef, kDeviceInput, 0, &addr, 0, NULL, sizeof(isHidden), &outSize, &isHidden);
    CHECK("Input device IsHidden == 0 (visible)", status == noErr && isHidden == 0);

    addr.mSelector = kAudioDevicePropertyDeviceUID;
    CFStringRef uid = NULL;
    outSize = 0;
    status = vtbl->GetPropertyData(driverRef, kDeviceInput, 0, &addr, 0, NULL, sizeof(uid), &outSize, &uid);
    CHECK("Input device UID retrievable", status == noErr && uid != NULL);
    if (uid) CFRelease(uid);

    addr.mSelector = kAudioDevicePropertyStreams;
    addr.mScope = kAudioObjectPropertyScopeInput;
    AudioObjectID inputStreams[4] = {0};
    outSize = 0;
    status = vtbl->GetPropertyData(driverRef, kDeviceInput, 0, &addr, 0, NULL, sizeof(inputStreams), &outSize, inputStreams);
    CHECK("Input device has 1 input stream == ID 3", status == noErr && outSize == sizeof(AudioObjectID) && inputStreams[0] == kStreamInput);

    addr.mScope = kAudioObjectPropertyScopeOutput;
    outSize = 0;
    status = vtbl->GetPropertyData(driverRef, kDeviceInput, 0, &addr, 0, NULL, sizeof(inputStreams), &outSize, inputStreams);
    CHECK("Input device has 0 output streams (split from output device)", status == noErr && outSize == 0);

    // --- Output device (hidden "Syrinx Microphone Output") property checks ---
    addr.mSelector = kAudioObjectPropertyName;
    outSize = 0;
    status = vtbl->GetPropertyData(driverRef, kDeviceOutput, 0, &addr, 0, NULL, sizeof(name), &outSize, &name);
    memset(nameBuf, 0, sizeof(nameBuf));
    if (name) { CFStringGetCString(name, nameBuf, sizeof(nameBuf), kCFStringEncodingUTF8); }
    printf("  output device name = \"%s\"\n", nameBuf);
    CHECK("Output device Name == \"Syrinx Microphone Output\"", status == noErr && name != NULL && strcmp(nameBuf, "Syrinx Microphone Output") == 0);
    if (name) CFRelease(name);

    addr.mSelector = kAudioDevicePropertyIsHidden;
    isHidden = 0;
    outSize = 0;
    status = vtbl->GetPropertyData(driverRef, kDeviceOutput, 0, &addr, 0, NULL, sizeof(isHidden), &outSize, &isHidden);
    CHECK("Output device IsHidden == 1 (hidden from pickers)", status == noErr && isHidden == 1);

    addr.mSelector = kAudioDevicePropertyDeviceCanBeDefaultDevice;
    UInt32 canBeDefault = 0xFF;
    outSize = 0;
    status = vtbl->GetPropertyData(driverRef, kDeviceOutput, 0, &addr, 0, NULL, sizeof(canBeDefault), &outSize, &canBeDefault);
    CHECK("Output device CanBeDefaultDevice == 0 (never someone's speakers)", status == noErr && canBeDefault == 0);

    addr.mSelector = kAudioDevicePropertyDeviceUID;
    outSize = 0;
    status = vtbl->GetPropertyData(driverRef, kDeviceOutput, 0, &addr, 0, NULL, sizeof(uid), &outSize, &uid);
    CHECK("Output device UID retrievable", status == noErr && uid != NULL);
    if (uid) CFRelease(uid);

    addr.mSelector = kAudioDevicePropertyStreams;
    addr.mScope = kAudioObjectPropertyScopeOutput;
    AudioObjectID outputStreams[4] = {0};
    outSize = 0;
    status = vtbl->GetPropertyData(driverRef, kDeviceOutput, 0, &addr, 0, NULL, sizeof(outputStreams), &outSize, outputStreams);
    CHECK("Output device has 1 output stream == ID 4", status == noErr && outSize == sizeof(AudioObjectID) && outputStreams[0] == kStreamOutput);

    addr.mScope = kAudioObjectPropertyScopeInput;
    outSize = 0;
    status = vtbl->GetPropertyData(driverRef, kDeviceOutput, 0, &addr, 0, NULL, sizeof(outputStreams), &outSize, outputStreams);
    CHECK("Output device has 0 input streams (split from input device)", status == noErr && outSize == 0);

    addr.mSelector = kAudioStreamPropertyVirtualFormat;
    addr.mScope = kAudioObjectPropertyScopeGlobal;
    AudioStreamBasicDescription fmt;
    memset(&fmt, 0, sizeof(fmt));
    outSize = 0;
    status = vtbl->GetPropertyData(driverRef, kStreamOutput, 0, &addr, 0, NULL, sizeof(fmt), &outSize, &fmt);
    printf("  stream format: %.0fHz %uch %ubit\n", fmt.mSampleRate, (unsigned)fmt.mChannelsPerFrame, (unsigned)fmt.mBitsPerChannel);
    CHECK("Stream format is 48000Hz/2ch/32bit", status == noErr && fmt.mSampleRate == 48000.0 && fmt.mChannelsPerFrame == 2 && fmt.mBitsPerChannel == 32);

    // --- IO cycle simulation: write a known tone, read it back, verify loopback ---
    AudioServerPlugInClientInfo fakeClient;
    memset(&fakeClient, 0, sizeof(fakeClient));
    fakeClient.mClientID = 0;
    fakeClient.mProcessID = getpid();
    fakeClient.mIsNativeEndian = true;
    status = vtbl->AddDeviceClient(driverRef, kDeviceOutput, &fakeClient);
    CHECK("AddDeviceClient(output device)", status == noErr);
    status = vtbl->AddDeviceClient(driverRef, kDeviceInput, &fakeClient);
    CHECK("AddDeviceClient(input device)", status == noErr);
    status = vtbl->StartIO(driverRef, kDeviceOutput, 0);
    CHECK("StartIO(output device)", status == noErr);
    status = vtbl->StartIO(driverRef, kDeviceInput, 0);
    CHECK("StartIO(input device)", status == noErr);

    Float64 sampleTime = 0;
    UInt64 hostTime = 0, seed = 0;
    status = vtbl->GetZeroTimeStamp(driverRef, kDeviceInput, 0, &sampleTime, &hostTime, &seed);
    CHECK("GetZeroTimeStamp succeeds", status == noErr);
    printf("  zero timestamp: sampleTime=%.0f hostTime=%llu seed=%llu\n", sampleTime, hostTime, seed);

    Boolean willDoWrite = false, inPlace = false;
    status = vtbl->WillDoIOOperation(driverRef, kDeviceOutput, 0, kAudioServerPlugInIOOperationWriteMix, &willDoWrite, &inPlace);
    CHECK("WillDoIOOperation(WriteMix) == true", status == noErr && willDoWrite);
    Boolean willDoRead = false;
    status = vtbl->WillDoIOOperation(driverRef, kDeviceInput, 0, kAudioServerPlugInIOOperationReadInput, &willDoRead, &inPlace);
    CHECK("WillDoIOOperation(ReadInput) == true", status == noErr && willDoRead);

    const UInt32 frameCount = 512;
    Float32 writeBuf[512 * 2];
    for (UInt32 i = 0; i < frameCount; i++) {
        writeBuf[i * 2 + 0] = (Float32)i / (Float32)frameCount;       // left ramp
        writeBuf[i * 2 + 1] = -(Float32)i / (Float32)frameCount;      // right ramp
    }

    AudioServerPlugInIOCycleInfo cycle;
    memset(&cycle, 0, sizeof(cycle));
    cycle.mOutputTime.mSampleTime = 1000; // arbitrary fixed position in the ring
    cycle.mInputTime.mSampleTime = 1000;  // read back the exact same position

    status = vtbl->DoIOOperation(driverRef, kDeviceOutput, kStreamOutput, 0, kAudioServerPlugInIOOperationWriteMix, frameCount, &cycle, writeBuf, NULL);
    CHECK("DoIOOperation(WriteMix) succeeds", status == noErr);

    Float32 readBuf[512 * 2];
    memset(readBuf, 0, sizeof(readBuf));
    status = vtbl->DoIOOperation(driverRef, kDeviceInput, kStreamInput, 0, kAudioServerPlugInIOOperationReadInput, frameCount, &cycle, readBuf, NULL);
    CHECK("DoIOOperation(ReadInput) succeeds", status == noErr);

    Boolean matches = true;
    for (UInt32 i = 0; i < frameCount * 2; i++) {
        if (writeBuf[i] != readBuf[i]) { matches = false; break; }
    }
    CHECK("Loopback: samples written to output device == samples read from input device", matches);

    status = vtbl->StopIO(driverRef, kDeviceOutput, 0);
    CHECK("StopIO(output device)", status == noErr);

    // The writer just stopped, but the reader (input device) is
    // still running. Reading the exact same ring position again
    // should now return silence, not the tone written earlier —
    // proving the ring buffer clears when the writer disappears
    // instead of letting a still-attached reader loop stale audio.
    Float32 readAfterStop[512 * 2];
    memset(readAfterStop, 0xFF, sizeof(readAfterStop)); // poison, not zero
    status = vtbl->DoIOOperation(driverRef, kDeviceInput, kStreamInput, 0, kAudioServerPlugInIOOperationReadInput, frameCount, &cycle, readAfterStop, NULL);
    CHECK("DoIOOperation(ReadInput) after writer stopped succeeds", status == noErr);
    Boolean isSilent = true;
    for (UInt32 i = 0; i < frameCount * 2; i++) {
        if (readAfterStop[i] != 0.0f) { isSilent = false; break; }
    }
    CHECK("Ring buffer cleared to silence after last writer stopped (no stale loop)", isSilent);

    status = vtbl->StopIO(driverRef, kDeviceInput, 0);
    CHECK("StopIO(input device)", status == noErr);

    ULONG refsAfterRelease = vtbl->Release(driverRef);
    printf("  refcount after one Release: %lu\n", (unsigned long)refsAfterRelease);

    printf("\n%s\n", failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
