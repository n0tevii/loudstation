#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
static volatile sig_atomic_t interrupted;
static void stop(int sig) { interrupted = sig; }
static AudioObjectPropertyAddress addr(UInt32 sel, UInt32 scope) {
    return (AudioObjectPropertyAddress){sel, scope, kAudioObjectPropertyElementMain};
}
static void fail(const char *msg, OSStatus status) {
    fprintf(stderr, "%s (OSStatus %d)\n", msg, (int)status); exit(1);
}
static void *list(AudioObjectID id, UInt32 sel, UInt32 scope, UInt32 *size) {
    AudioObjectPropertyAddress a=addr(sel,scope);
    OSStatus s=AudioObjectGetPropertyDataSize(id,&a,0,NULL,size);
    if(s) fail("Cannot read property size",s);
    void *p=calloc(1,*size ? *size : 1);
    if(!p) fail("Out of memory",-1);
    if(*size && (s=AudioObjectGetPropertyData(id,&a,0,NULL,size,p))) fail("Cannot read property",s);
    return p;
}
static int readfmt(AudioStreamID id, AudioStreamBasicDescription *f) {
    AudioObjectPropertyAddress a=addr(kAudioStreamPropertyPhysicalFormat,kAudioObjectPropertyScopeGlobal);
    UInt32 n=sizeof(*f); return AudioObjectGetPropertyData(id,&a,0,NULL,&n,f);
}
static int equal(AudioStreamBasicDescription a, AudioStreamBasicDescription b) {
    return a.mSampleRate==b.mSampleRate && a.mFormatID==b.mFormatID &&
      a.mFormatFlags==b.mFormatFlags && a.mBytesPerPacket==b.mBytesPerPacket &&
      a.mFramesPerPacket==b.mFramesPerPacket && a.mBytesPerFrame==b.mBytesPerFrame &&
      a.mChannelsPerFrame==b.mChannelsPerFrame && a.mBitsPerChannel==b.mBitsPerChannel;
}
static int setfmt(AudioStreamID id, AudioStreamBasicDescription f) {
    AudioObjectPropertyAddress a=addr(kAudioStreamPropertyPhysicalFormat,kAudioObjectPropertyScopeGlobal);
    OSStatus s=AudioObjectSetPropertyData(id,&a,0,NULL,sizeof(f),&f);
    if(s) { fprintf(stderr,"Set format failed: %d\n",(int)s); return 0; }
    for(int i=0;i<30;i++) {
        usleep(100000);
        AudioStreamBasicDescription actual={0};
        if(!readfmt(id,&actual) && equal(actual,f)) return 1;
    }
    fprintf(stderr,"Timed out verifying format\n"); return 0;
}
int main(int argc,char **argv) {
    int check=argc==2 && !strcmp(argv[1],"check");
    UInt32 n=0; AudioDeviceID *ids=list(kAudioObjectSystemObject,kAudioHardwarePropertyDevices,kAudioObjectPropertyScopeGlobal,&n);
    AudioDeviceID device=0; unsigned matches=0;
    for(unsigned i=0;i<n/sizeof(*ids);i++) {
        CFStringRef name=NULL; UInt32 len=sizeof(name);
        AudioObjectPropertyAddress a=addr(kAudioObjectPropertyName,kAudioObjectPropertyScopeGlobal);
        if(!AudioObjectGetPropertyData(ids[i],&a,0,NULL,&len,&name) && name) {
            if(CFStringCompare(name,CFSTR("DELL U4021QW"),0)==kCFCompareEqualTo) {device=ids[i];matches++;}
            CFRelease(name);
        }
    }
    free(ids);
    if(matches!=1) fail("Expected exactly one DELL U4021QW; no changes made",-1);
    AudioStreamID *streams=list(device,kAudioDevicePropertyStreams,kAudioDevicePropertyScopeOutput,&n);
    if(n!=sizeof(*streams)) fail("Expected one output stream; no changes made",-1);
    AudioStreamID stream=streams[0]; free(streams);
    AudioStreamBasicDescription original={0}, alternate={0};
    OSStatus s=readfmt(stream,&original); if(s) fail("Cannot read original format",s);
    printf("DELL U4021QW: %.0f Hz, %u bit, %u channels\n",original.mSampleRate,(unsigned)original.mBitsPerChannel,(unsigned)original.mChannelsPerFrame);
    if(original.mFormatID!=kAudioFormatLinearPCM) fail("Not PCM; no changes made",-1);
    AudioObjectPropertyAddress a=addr(kAudioStreamPropertyPhysicalFormat,kAudioObjectPropertyScopeGlobal);
    Boolean writable=false;
    s=AudioObjectIsPropertySettable(stream,&a,&writable);
    if(s || !writable) fail("Format is not writable; no changes made",s);
    AudioStreamRangedDescription *formats=list(stream,kAudioStreamPropertyAvailablePhysicalFormats,kAudioObjectPropertyScopeGlobal,&n);
    for(unsigned i=0;i<n/sizeof(*formats);i++) {
        AudioStreamBasicDescription f=formats[i].mFormat;
        if(f.mFormatID==kAudioFormatLinearPCM && f.mChannelsPerFrame==original.mChannelsPerFrame &&
          f.mBitsPerChannel!=original.mBitsPerChannel && (f.mBitsPerChannel==16 || f.mBitsPerChannel==24 || f.mBitsPerChannel==32) &&
          original.mSampleRate>=formats[i].mSampleRateRange.mMinimum && original.mSampleRate<=formats[i].mSampleRateRange.mMaximum) {
            f.mSampleRate=original.mSampleRate; alternate=f; break;
        }
    }
    free(formats);
    if(!alternate.mBitsPerChannel) fail("No alternate bit depth at current sample rate; no changes made",-1);
    printf("Plan: %u bit -> %u bit -> %u bit (sample rate/channels unchanged)\n",(unsigned)original.mBitsPerChannel,(unsigned)alternate.mBitsPerChannel,(unsigned)original.mBitsPerChannel);
    if(check) {puts("Check only; no changes made.");return 0;}
    signal(SIGINT,stop); signal(SIGTERM,stop); signal(SIGHUP,stop);
    if(interrupted) return 1;
    int changed=setfmt(stream,alternate);
    if(changed && !interrupted) usleep(700000);
    // Always restore, including after a failed or interrupted first change.
    int restored=setfmt(stream,original);
    if(!restored) {
        fprintf(stderr,"RESTORE FAILED. In Audio MIDI Setup restore DELL to %.0f Hz / %u bit / %u channels.\n",original.mSampleRate,(unsigned)original.mBitsPerChannel,(unsigned)original.mChannelsPerFrame);
        return 1;
    }
    if(!changed || interrupted) {fprintf(stderr,"Original format restored; toggle interrupted or failed.\n");return 1;}
    puts("Format toggled and original restored. Play audio to verify recovery.");return 0;
}
