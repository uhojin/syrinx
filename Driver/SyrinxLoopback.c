/*
 * SyrinxLoopback.c
 *
 * A minimal, from-scratch CoreAudio AudioServerPlugIn (HAL driver) that
 * publishes *two* virtual audio devices sharing one ring buffer:
 *
 *   - "Syrinx Microphone" (input-capable) - what call apps pick as
 *     their microphone. Visible and selectable like any normal input
 *     device.
 *   - "Syrinx Microphone Output" (output-only) - what Syrinx's own
 *     AVAudioEngine renders into. Deliberately hidden
 *     (kAudioDevicePropertyIsHidden = 1) and marked ineligible as a
 *     default device, so it never shows up in System Settings' output
 *     picker or gets accidentally selected as someone's speakers.
 *
 * Audio written to the output device's stream is copied into the
 * shared ring buffer and read back out the input device's stream, so
 * anything Syrinx renders to "Syrinx Microphone Output" is picked up
 * by any app recording from "Syrinx Microphone" - the same mechanism
 * BlackHole and Apple's own NullAudio sample use, written clean-room
 * against Apple's public <CoreAudio/AudioServerPlugIn.h> contract (no
 * BlackHole source was read or copied; GPL-3.0 does not apply here).
 *
 * A single physical device exposing both an input and output stream
 * can't be selectively hidden - kAudioDevicePropertyIsHidden is a
 * whole-device-object flag, and most apps skip a hidden device from
 * every picker, input included. Splitting into two device objects
 * (still backed by the same ring buffer) is the only way to get
 * asymmetric visibility.
 *
 * This is intentionally the simplest correct implementation: fixed
 * format (2ch, 48kHz, interleaved Float32 - matching the native rate
 * of most modern input hardware so routing a real mic through this
 * device never requires a lossy resample), no controls, no
 * dynamically created devices, no persistence.
 */

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#pragma mark - Constants

// Fixed AudioObjectIDs for our static object graph.
enum
{
    kObjectID_PlugIn        = kAudioObjectPlugInObject, // 1
    kObjectID_Device_Input  = 2, // visible "Syrinx Microphone" (mic side)
    kObjectID_Stream_Input  = 3,
    kObjectID_Stream_Output = 4,
    kObjectID_Device_Output = 5  // hidden "Syrinx Microphone Output" (render target)
};

static const UInt32 kChannelCount        = 2;
static const Float64 kSampleRate         = 48000.0;
static const UInt32 kBitsPerChannel      = 32;
static const UInt32 kBytesPerFrame       = 2 /* channels */ * 4 /* bytes per Float32 */;
// Must be >= 10923 per AudioServerPlugIn.h's documented minimum.
static const UInt32 kZeroTimeStampPeriod = 12000; // 0.25s at 48kHz

// Ring buffer sized generously (2 seconds) so IO jitter can never wrap
// a reader into unwritten-yet data.
static const UInt32 kRingBufferFrames = 48000 * 2;

#pragma mark - Driver State

typedef struct
{
    Float32 *mData;         // kRingBufferFrames * kChannelCount samples
    pthread_mutex_t mLock;
} RingBuffer;

static RingBuffer gRing;

static _Atomic(uint32_t) gRefCount = 1;
static AudioServerPlugInHostRef gHost = NULL;

static _Atomic(uint32_t) gClientCount = 0;
static _Atomic(uint32_t) gOutputClientCount = 0;
static _Atomic(bool) gIsRunning = false;
static uint64_t gStartHostTime = 0; // mach_absolute_time() at first StartIO

#pragma mark - Forward declarations of the vtable methods

static HRESULT SyrinxLoopback_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface);
static ULONG SyrinxLoopback_AddRef(void *inDriver);
static ULONG SyrinxLoopback_Release(void *inDriver);
static OSStatus SyrinxLoopback_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus SyrinxLoopback_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo *inClientInfo, AudioObjectID *outDeviceObjectID);
static OSStatus SyrinxLoopback_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID);
static OSStatus SyrinxLoopback_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo *inClientInfo);
static OSStatus SyrinxLoopback_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo *inClientInfo);
static OSStatus SyrinxLoopback_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void *inChangeInfo);
static OSStatus SyrinxLoopback_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void *inChangeInfo);
static Boolean SyrinxLoopback_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress);
static OSStatus SyrinxLoopback_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, Boolean *outIsSettable);
static OSStatus SyrinxLoopback_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 *outDataSize);
static OSStatus SyrinxLoopback_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, UInt32 *outDataSize, void *outData);
static OSStatus SyrinxLoopback_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, const void *inData);
static OSStatus SyrinxLoopback_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus SyrinxLoopback_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus SyrinxLoopback_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed);
static OSStatus SyrinxLoopback_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean *outWillDo, Boolean *outWillDoInPlace);
static OSStatus SyrinxLoopback_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo);
static OSStatus SyrinxLoopback_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo, void *ioMainBuffer, void *ioSecondaryBuffer);
static OSStatus SyrinxLoopback_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo);

#pragma mark - The COM-style vtable instance

static AudioServerPlugInDriverInterface gDriverInterface =
{
    NULL, // _reserved
    SyrinxLoopback_QueryInterface,
    SyrinxLoopback_AddRef,
    SyrinxLoopback_Release,
    SyrinxLoopback_Initialize,
    SyrinxLoopback_CreateDevice,
    SyrinxLoopback_DestroyDevice,
    SyrinxLoopback_AddDeviceClient,
    SyrinxLoopback_RemoveDeviceClient,
    SyrinxLoopback_PerformDeviceConfigurationChange,
    SyrinxLoopback_AbortDeviceConfigurationChange,
    SyrinxLoopback_HasProperty,
    SyrinxLoopback_IsPropertySettable,
    SyrinxLoopback_GetPropertyDataSize,
    SyrinxLoopback_GetPropertyData,
    SyrinxLoopback_SetPropertyData,
    SyrinxLoopback_StartIO,
    SyrinxLoopback_StopIO,
    SyrinxLoopback_GetZeroTimeStamp,
    SyrinxLoopback_WillDoIOOperation,
    SyrinxLoopback_BeginIOOperation,
    SyrinxLoopback_DoIOOperation,
    SyrinxLoopback_EndIOOperation
};

static AudioServerPlugInDriverInterface *gInterfacePtr = &gDriverInterface;
static AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;

#pragma mark - Factory

void *SyrinxLoopback_Factory(CFAllocatorRef allocator, CFUUIDRef typeID);
void *SyrinxLoopback_Factory(CFAllocatorRef allocator, CFUUIDRef typeID)
{
    (void)allocator;
    if (!CFEqual(typeID, kAudioServerPlugInTypeUUID)) {
        return NULL;
    }
    atomic_fetch_add(&gRefCount, 1);
    return gDriverRef;
}

#pragma mark - IUnknown

static HRESULT SyrinxLoopback_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface)
{
    (void)inDriver;
    if (outInterface == NULL) {
        return E_POINTER;
    }

    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    if (requested == NULL) {
        return E_NOINTERFACE;
    }

    HRESULT result = E_NOINTERFACE;
    if (CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID) || CFEqual(requested, IUnknownUUID)) {
        atomic_fetch_add(&gRefCount, 1);
        *outInterface = gDriverRef;
        result = S_OK;
    }
    CFRelease(requested);
    return result;
}

static ULONG SyrinxLoopback_AddRef(void *inDriver)
{
    (void)inDriver;
    return atomic_fetch_add(&gRefCount, 1) + 1;
}

static ULONG SyrinxLoopback_Release(void *inDriver)
{
    (void)inDriver;
    uint32_t before = atomic_fetch_sub(&gRefCount, 1);
    return before > 0 ? before - 1 : 0;
}

#pragma mark - Basic Operations

static OSStatus SyrinxLoopback_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost)
{
    (void)inDriver;
    gHost = inHost;

    gRing.mData = (Float32 *)calloc(kRingBufferFrames * kChannelCount, sizeof(Float32));
    pthread_mutex_init(&gRing.mLock, NULL);

    return noErr;
}

// We publish exactly two static devices; dynamic device creation isn't
// supported.
static OSStatus SyrinxLoopback_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo *inClientInfo, AudioObjectID *outDeviceObjectID)
{
    (void)inDriver; (void)inDescription; (void)inClientInfo; (void)outDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus SyrinxLoopback_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID)
{
    (void)inDriver; (void)inDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus SyrinxLoopback_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo *inClientInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientInfo;
    return noErr;
}

static OSStatus SyrinxLoopback_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo *inClientInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientInfo;
    return noErr;
}

// We never call RequestDeviceConfigurationChange, so the host will
// never call these, but they must exist for the vtable.
static OSStatus SyrinxLoopback_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void *inChangeInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inChangeAction; (void)inChangeInfo;
    return noErr;
}

static OSStatus SyrinxLoopback_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void *inChangeInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inChangeAction; (void)inChangeInfo;
    return noErr;
}

#pragma mark - Property helpers

static Boolean ObjectIsDevice(AudioObjectID inObjectID)
{
    return inObjectID == kObjectID_Device_Input || inObjectID == kObjectID_Device_Output;
}

// Which device owns a given stream.
static AudioObjectID OwningDeviceForStream(AudioObjectID inStreamID)
{
    return (inStreamID == kObjectID_Stream_Input) ? kObjectID_Device_Input : kObjectID_Device_Output;
}

static void FillStreamFormat(AudioStreamBasicDescription *outFormat)
{
    memset(outFormat, 0, sizeof(AudioStreamBasicDescription));
    outFormat->mSampleRate = kSampleRate;
    outFormat->mFormatID = kAudioFormatLinearPCM;
    outFormat->mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    outFormat->mBytesPerPacket = kBytesPerFrame;
    outFormat->mFramesPerPacket = 1;
    outFormat->mBytesPerFrame = kBytesPerFrame;
    outFormat->mChannelsPerFrame = kChannelCount;
    outFormat->mBitsPerChannel = kBitsPerChannel;
}

#pragma mark - HasProperty / IsPropertySettable / GetPropertyDataSize / GetPropertyData / SetPropertyData

static Boolean SyrinxLoopback_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress)
{
    (void)inDriver; (void)inClientProcessID;
    UInt32 outSize = 0;
    OSStatus status = SyrinxLoopback_GetPropertyDataSize(inDriver, inObjectID, inClientProcessID, inAddress, 0, NULL, &outSize);
    return status == noErr;
}

static OSStatus SyrinxLoopback_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, Boolean *outIsSettable)
{
    (void)inDriver; (void)inObjectID; (void)inClientProcessID;
    // Nothing in this driver is settable by clients.
    *outIsSettable = false;
    switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyBundleID:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            return noErr;
        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus SyrinxLoopback_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 *outDataSize)
{
    (void)inDriver; (void)inClientProcessID; (void)inQualifierDataSize; (void)inQualifierData;

    switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
            *outDataSize = sizeof(AudioObjectID);
            return noErr;

        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
            *outDataSize = sizeof(CFStringRef);
            return noErr;

        case kAudioObjectPropertyOwnedObjects:
            if (inObjectID == kObjectID_PlugIn) {
                *outDataSize = sizeof(AudioObjectID) * 2; // the two devices
            } else if (ObjectIsDevice(inObjectID)) {
                *outDataSize = sizeof(AudioObjectID) * 1; // its one stream
            } else {
                *outDataSize = 0;
            }
            return noErr;

        case kAudioPlugInPropertyDeviceList:
            *outDataSize = sizeof(AudioObjectID) * 2;
            return noErr;

        case kAudioPlugInPropertyBundleID:
            *outDataSize = sizeof(CFStringRef);
            return noErr;

        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
            *outDataSize = sizeof(CFStringRef);
            return noErr;

        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
            *outDataSize = sizeof(UInt32);
            return noErr;

        case kAudioDevicePropertyRelatedDevices:
            *outDataSize = sizeof(AudioObjectID) * 1; // just itself
            return noErr;

        case kAudioDevicePropertyStreams: {
            UInt32 count = 0;
            if (inObjectID == kObjectID_Device_Input) {
                count = (inAddress->mScope == kAudioObjectPropertyScopeOutput) ? 0 : 1;
            } else if (inObjectID == kObjectID_Device_Output) {
                count = (inAddress->mScope == kAudioObjectPropertyScopeInput) ? 0 : 1;
            }
            *outDataSize = sizeof(AudioObjectID) * count;
            return noErr;
        }

        case kAudioObjectPropertyControlList:
            *outDataSize = 0; // no controls
            return noErr;

        case kAudioDevicePropertyNominalSampleRate:
            *outDataSize = sizeof(Float64);
            return noErr;

        case kAudioDevicePropertyAvailableNominalSampleRates:
            *outDataSize = sizeof(AudioValueRange) * 1;
            return noErr;

        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            *outDataSize = sizeof(AudioStreamBasicDescription);
            return noErr;

        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            *outDataSize = sizeof(AudioValueRange) * 1; // reported via a single-format range wrapper below
            return noErr;

        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus SyrinxLoopback_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, UInt32 *outDataSize, void *outData)
{
    (void)inDriver; (void)inClientProcessID; (void)inQualifierDataSize; (void)inQualifierData; (void)inDataSize;

    switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass: {
            *(AudioClassID *)outData = kAudioObjectClassID;
            *outDataSize = sizeof(AudioClassID);
            return noErr;
        }
        case kAudioObjectPropertyClass: {
            AudioClassID cls;
            if (inObjectID == kObjectID_PlugIn) cls = kAudioPlugInClassID;
            else if (ObjectIsDevice(inObjectID)) cls = kAudioDeviceClassID;
            else cls = kAudioStreamClassID;
            *(AudioClassID *)outData = cls;
            *outDataSize = sizeof(AudioClassID);
            return noErr;
        }
        case kAudioObjectPropertyOwner: {
            AudioObjectID owner;
            if (inObjectID == kObjectID_PlugIn) owner = kAudioObjectUnknown;
            else if (ObjectIsDevice(inObjectID)) owner = kObjectID_PlugIn;
            else owner = OwningDeviceForStream(inObjectID);
            *(AudioObjectID *)outData = owner;
            *outDataSize = sizeof(AudioObjectID);
            return noErr;
        }
        case kAudioObjectPropertyName: {
            CFStringRef name;
            if (inObjectID == kObjectID_Device_Input) name = CFSTR("Syrinx Microphone");
            else if (inObjectID == kObjectID_Device_Output) name = CFSTR("Syrinx Microphone Output");
            else if (inObjectID == kObjectID_Stream_Input) name = CFSTR("Syrinx Microphone In");
            else if (inObjectID == kObjectID_Stream_Output) name = CFSTR("Syrinx Microphone Out");
            else name = CFSTR("Syrinx Microphone Driver");
            CFRetain(name);
            *(CFStringRef *)outData = name;
            *outDataSize = sizeof(CFStringRef);
            return noErr;
        }
        case kAudioObjectPropertyManufacturer: {
            CFStringRef manufacturer = CFSTR("Syrinx");
            CFRetain(manufacturer);
            *(CFStringRef *)outData = manufacturer;
            *outDataSize = sizeof(CFStringRef);
            return noErr;
        }
        case kAudioObjectPropertyOwnedObjects: {
            if (inObjectID == kObjectID_PlugIn) {
                ((AudioObjectID *)outData)[0] = kObjectID_Device_Input;
                ((AudioObjectID *)outData)[1] = kObjectID_Device_Output;
                *outDataSize = sizeof(AudioObjectID) * 2;
            } else if (inObjectID == kObjectID_Device_Input) {
                ((AudioObjectID *)outData)[0] = kObjectID_Stream_Input;
                *outDataSize = sizeof(AudioObjectID) * 1;
            } else if (inObjectID == kObjectID_Device_Output) {
                ((AudioObjectID *)outData)[0] = kObjectID_Stream_Output;
                *outDataSize = sizeof(AudioObjectID) * 1;
            } else {
                *outDataSize = 0;
            }
            return noErr;
        }
        case kAudioPlugInPropertyDeviceList: {
            ((AudioObjectID *)outData)[0] = kObjectID_Device_Input;
            ((AudioObjectID *)outData)[1] = kObjectID_Device_Output;
            *outDataSize = sizeof(AudioObjectID) * 2;
            return noErr;
        }
        case kAudioPlugInPropertyBundleID: {
            CFStringRef bundleID = CFSTR("com.hojin.syrinx.loopback");
            CFRetain(bundleID);
            *(CFStringRef *)outData = bundleID;
            *outDataSize = sizeof(CFStringRef);
            return noErr;
        }
        case kAudioDevicePropertyDeviceUID: {
            CFStringRef uid = (inObjectID == kObjectID_Device_Output)
                ? CFSTR("com.hojin.syrinx.loopback.output.device")
                : CFSTR("com.hojin.syrinx.loopback.device");
            CFRetain(uid);
            *(CFStringRef *)outData = uid;
            *outDataSize = sizeof(CFStringRef);
            return noErr;
        }
        case kAudioDevicePropertyModelUID: {
            CFStringRef uid = CFSTR("com.hojin.syrinx.loopback.model");
            CFRetain(uid);
            *(CFStringRef *)outData = uid;
            *outDataSize = sizeof(CFStringRef);
            return noErr;
        }
        case kAudioDevicePropertyTransportType:
            *(UInt32 *)outData = kAudioDeviceTransportTypeVirtual;
            *outDataSize = sizeof(UInt32);
            return noErr;

        case kAudioDevicePropertyRelatedDevices:
            // Deliberately just itself, not its counterpart - so the
            // hidden output device can never surface indirectly
            // through a "related devices" lookup on the visible one.
            ((AudioObjectID *)outData)[0] = inObjectID;
            *outDataSize = sizeof(AudioObjectID) * 1;
            return noErr;

        case kAudioDevicePropertyClockDomain:
            *(UInt32 *)outData = 0; // not synced to any other clock domain
            *outDataSize = sizeof(UInt32);
            return noErr;

        case kAudioDevicePropertyDeviceIsAlive:
            *(UInt32 *)outData = 1;
            *outDataSize = sizeof(UInt32);
            return noErr;

        case kAudioDevicePropertyDeviceIsRunning:
            *(UInt32 *)outData = atomic_load(&gIsRunning) ? 1 : 0;
            *outDataSize = sizeof(UInt32);
            return noErr;

        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            // The output device must never become anyone's default
            // output ("speakers") - that's the whole point of the
            // split. The input device keeps its previous behavior.
            *(UInt32 *)outData = (inObjectID == kObjectID_Device_Output) ? 0 : 1;
            *outDataSize = sizeof(UInt32);
            return noErr;

        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
            *(UInt32 *)outData = 0;
            *outDataSize = sizeof(UInt32);
            return noErr;

        case kAudioDevicePropertyIsHidden:
            // The output device is hidden from every normal device
            // picker (System Settings' Output list included). The
            // input device stays visible - it's what call apps pick
            // as their microphone.
            *(UInt32 *)outData = (inObjectID == kObjectID_Device_Output) ? 1 : 0;
            *outDataSize = sizeof(UInt32);
            return noErr;

        case kAudioDevicePropertyZeroTimeStampPeriod:
            *(UInt32 *)outData = kZeroTimeStampPeriod;
            *outDataSize = sizeof(UInt32);
            return noErr;

        case kAudioDevicePropertyStreams: {
            if (inObjectID == kObjectID_Device_Input) {
                if (inAddress->mScope == kAudioObjectPropertyScopeOutput) {
                    *outDataSize = 0;
                } else {
                    ((AudioObjectID *)outData)[0] = kObjectID_Stream_Input;
                    *outDataSize = sizeof(AudioObjectID) * 1;
                }
            } else if (inObjectID == kObjectID_Device_Output) {
                if (inAddress->mScope == kAudioObjectPropertyScopeInput) {
                    *outDataSize = 0;
                } else {
                    ((AudioObjectID *)outData)[0] = kObjectID_Stream_Output;
                    *outDataSize = sizeof(AudioObjectID) * 1;
                }
            } else {
                *outDataSize = 0;
            }
            return noErr;
        }

        case kAudioObjectPropertyControlList:
            *outDataSize = 0;
            return noErr;

        case kAudioDevicePropertyNominalSampleRate:
            *(Float64 *)outData = kSampleRate;
            *outDataSize = sizeof(Float64);
            return noErr;

        case kAudioDevicePropertyAvailableNominalSampleRates:
            ((AudioValueRange *)outData)[0].mMinimum = kSampleRate;
            ((AudioValueRange *)outData)[0].mMaximum = kSampleRate;
            *outDataSize = sizeof(AudioValueRange) * 1;
            return noErr;

        case kAudioStreamPropertyIsActive:
            *(UInt32 *)outData = 1;
            *outDataSize = sizeof(UInt32);
            return noErr;

        case kAudioStreamPropertyDirection:
            *(UInt32 *)outData = (inObjectID == kObjectID_Stream_Input) ? 1 : 0;
            *outDataSize = sizeof(UInt32);
            return noErr;

        case kAudioStreamPropertyTerminalType:
            *(UInt32 *)outData = (inObjectID == kObjectID_Stream_Input)
                ? kAudioStreamTerminalTypeMicrophone
                : kAudioStreamTerminalTypeSpeaker;
            *outDataSize = sizeof(UInt32);
            return noErr;

        case kAudioStreamPropertyStartingChannel:
            *(UInt32 *)outData = 1;
            *outDataSize = sizeof(UInt32);
            return noErr;

        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            FillStreamFormat((AudioStreamBasicDescription *)outData);
            *outDataSize = sizeof(AudioStreamBasicDescription);
            return noErr;

        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            AudioStreamBasicDescription fmt;
            FillStreamFormat(&fmt);
            ((AudioValueRange *)outData)[0].mMinimum = kSampleRate;
            ((AudioValueRange *)outData)[0].mMaximum = kSampleRate;
            *outDataSize = sizeof(AudioValueRange) * 1;
            return noErr;
        }

        default:
            return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus SyrinxLoopback_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize, const void *inQualifierData, UInt32 inDataSize, const void *inData)
{
    (void)inDriver; (void)inObjectID; (void)inClientProcessID; (void)inAddress;
    (void)inQualifierDataSize; (void)inQualifierData; (void)inDataSize; (void)inData;
    // Nothing is settable in this minimal driver.
    return kAudioHardwareUnknownPropertyError;
}

#pragma mark - IO Operations

static void ClearRingBuffer(void)
{
    if (gRing.mData == NULL) {
        return;
    }
    pthread_mutex_lock(&gRing.mLock);
    memset(gRing.mData, 0, (size_t)kRingBufferFrames * kChannelCount * sizeof(Float32));
    pthread_mutex_unlock(&gRing.mLock);
}

static OSStatus SyrinxLoopback_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID)
{
    (void)inDriver; (void)inClientID;
    if (atomic_fetch_add(&gClientCount, 1) == 0) {
        gStartHostTime = mach_absolute_time();
        atomic_store(&gIsRunning, true);
    }
    if (inDeviceObjectID == kObjectID_Device_Output) {
        if (atomic_fetch_add(&gOutputClientCount, 1) == 0) {
            // A fresh writer is starting from silence — clear any
            // audio left over from a prior session (e.g. a crash that
            // skipped a clean StopIO) so it can't be mistaken for
            // live input.
            ClearRingBuffer();
        }
    }
    return noErr;
}

static OSStatus SyrinxLoopback_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID)
{
    (void)inDriver; (void)inClientID;
    uint32_t before = atomic_fetch_sub(&gClientCount, 1);
    if (before <= 1) {
        atomic_store(&gIsRunning, false);
    }
    if (inDeviceObjectID == kObjectID_Device_Output) {
        uint32_t outputBefore = atomic_fetch_sub(&gOutputClientCount, 1);
        if (outputBefore <= 1) {
            // The last writer just stopped (e.g. Call Mode turned
            // off), but a reader may still be attached (a call app
            // that kept "Syrinx Microphone" selected). Without this,
            // its read position keeps advancing and wraps around the
            // fixed-size ring buffer every ~2 seconds, replaying
            // whatever audio was last written on a visible, repeating
            // loop — clearing it here means a reader with no active
            // writer gets genuine silence instead.
            ClearRingBuffer();
        }
    }
    return noErr;
}

static uint64_t HostTicksPerSecond(void)
{
    static mach_timebase_info_data_t sTimebase;
    static bool sInitialized = false;
    if (!sInitialized) {
        mach_timebase_info(&sTimebase);
        sInitialized = true;
    }
    // mach_absolute_time ticks -> nanoseconds is (ticks * numer / denom).
    // We want ticks per second: 1e9 / (numer/denom) = 1e9 * denom / numer.
    return (uint64_t)(1000000000.0 * (double)sTimebase.denom / (double)sTimebase.numer);
}

static OSStatus SyrinxLoopback_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;

    uint64_t now = mach_absolute_time();
    uint64_t ticksPerSecond = HostTicksPerSecond();
    double elapsedSeconds = (double)(now - gStartHostTime) / (double)ticksPerSecond;
    double elapsedSamples = elapsedSeconds * kSampleRate;

    // Snap to the last zero-timestamp-period boundary, as the host expects.
    double periodsElapsed = floor(elapsedSamples / (double)kZeroTimeStampPeriod);
    double sampleTime = periodsElapsed * (double)kZeroTimeStampPeriod;
    double hostTimeSeconds = sampleTime / kSampleRate;
    uint64_t hostTime = gStartHostTime + (uint64_t)(hostTimeSeconds * (double)ticksPerSecond);

    *outSampleTime = sampleTime;
    *outHostTime = hostTime;
    *outSeed = 1; // our clock never resynchronizes to a new time line
    return noErr;
}

static OSStatus SyrinxLoopback_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean *outWillDo, Boolean *outWillDoInPlace)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID;
    Boolean willDo = false;
    switch (inOperationID) {
        case kAudioServerPlugInIOOperationReadInput:
        case kAudioServerPlugInIOOperationWriteMix:
            willDo = true;
            break;
        default:
            willDo = false;
            break;
    }
    *outWillDo = willDo;
    *outWillDoInPlace = true;
    return noErr;
}

static OSStatus SyrinxLoopback_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID; (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    return noErr;
}

static OSStatus SyrinxLoopback_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID; (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    return noErr;
}

static OSStatus SyrinxLoopback_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo, void *ioMainBuffer, void *ioSecondaryBuffer)
{
    (void)inDriver; (void)inDeviceObjectID; (void)inClientID; (void)ioSecondaryBuffer;
    if (gRing.mData == NULL || ioMainBuffer == NULL) {
        return noErr;
    }

    Float32 *samples = (Float32 *)ioMainBuffer;

    if (inOperationID == kAudioServerPlugInIOOperationWriteMix && inStreamObjectID == kObjectID_Stream_Output) {
        SInt64 startFrame = (SInt64)llround(inIOCycleInfo->mOutputTime.mSampleTime);
        pthread_mutex_lock(&gRing.mLock);
        for (UInt32 frame = 0; frame < inIOBufferFrameSize; frame++) {
            SInt64 ringFrame = ((startFrame + frame) % (SInt64)kRingBufferFrames + (SInt64)kRingBufferFrames) % (SInt64)kRingBufferFrames;
            for (UInt32 ch = 0; ch < kChannelCount; ch++) {
                gRing.mData[(size_t)ringFrame * kChannelCount + ch] = samples[(size_t)frame * kChannelCount + ch];
            }
        }
        pthread_mutex_unlock(&gRing.mLock);
    } else if (inOperationID == kAudioServerPlugInIOOperationReadInput && inStreamObjectID == kObjectID_Stream_Input) {
        SInt64 startFrame = (SInt64)llround(inIOCycleInfo->mInputTime.mSampleTime);
        pthread_mutex_lock(&gRing.mLock);
        for (UInt32 frame = 0; frame < inIOBufferFrameSize; frame++) {
            SInt64 ringFrame = ((startFrame + frame) % (SInt64)kRingBufferFrames + (SInt64)kRingBufferFrames) % (SInt64)kRingBufferFrames;
            for (UInt32 ch = 0; ch < kChannelCount; ch++) {
                samples[(size_t)frame * kChannelCount + ch] = gRing.mData[(size_t)ringFrame * kChannelCount + ch];
            }
        }
        pthread_mutex_unlock(&gRing.mLock);
    }

    return noErr;
}
