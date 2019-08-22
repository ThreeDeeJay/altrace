/**
 * alTrace; a debugging tool for OpenAL.
 *
 * Please see the file LICENSE.txt in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

#include "altrace_playback.h"

static int logfd = -1;
static uint32 trace_scope = 0;
static void *guserdata = NULL;

static void quit_altrace_playback(void);

// don't bother doing a full hash map for devices and contexts, since you'll
//  usually never have more than one or two and they live basically the entire
//  lifetime of your app.
static void free_hash_item_device(ALCdevice *from, ALCdevice *to) { /* no-op */ }
SIMPLE_MAP(device, ALCdevice *, ALCdevice *)

static void free_hash_item_context(ALCcontext *from, ALCcontext *to) { /* no-op */ }
SIMPLE_MAP(context, ALCcontext *, ALCcontext *)

static void free_hash_item_devicelabel(ALCdevice *from, char *to) { free(to); }
SIMPLE_MAP(devicelabel, ALCdevice *, char *)

static void free_hash_item_contextlabel(ALCcontext *from, char *to) { free(to); }
SIMPLE_MAP(contextlabel, ALCcontext *, char *)

static void free_hash_item_alname(ALuint from, ALuint to) { /* no-op */ }
static uint8 hash_alname(const ALuint name) {
    /* since these are usually small numbers that increment from 0, they distribute pretty well on their own. */
    return (uint8) (name & 0xFF);
}

#define free_hash_item_source free_hash_item_alname
#define hash_source hash_alname
HASH_MAP(source, ALuint, ALuint)

#define free_hash_item_buffer free_hash_item_alname
#define hash_buffer hash_alname
HASH_MAP(buffer, ALuint, ALuint)

static void free_hash_item_alname_label(ALuint from, char *to) { free(to); }

#define free_hash_item_sourcelabel free_hash_item_alname_label
#define hash_sourcelabel hash_alname
HASH_MAP(sourcelabel, ALuint, char *)

#define free_hash_item_bufferlabel free_hash_item_alname_label
#define hash_bufferlabel hash_alname
HASH_MAP(bufferlabel, ALuint, char *)


static void free_hash_item_stackframe(void *from, char *to) { free(to); }
static uint8 hash_stackframe(void *from) {
    // everything is going to end in a multiple of pointer size, so flatten down.
    const size_t val = ((size_t) from) / (sizeof (void *));
    return (uint8) (val & 0xFF);  // good enough, I guess.
}
HASH_MAP(stackframe, void *, char *)

static void free_hash_item_threadid(uint64 from, uint32 to) { /* no-op */ }
static uint32 next_mapped_threadid = 0;
SIMPLE_MAP(threadid, uint64, uint32);

static int io_failure = 0;
static void IO_READ_FAIL(const int eof)
{
    if (!io_failure) {
        fprintf(stderr, "%s: Failed to read from log: %s\n", GAppName, eof ? "end of file" : strerror(errno));
        io_failure = 1;
    }
}

static uint32 readle32(void)
{
    uint32 retval = 0;
    if (!io_failure) {
        const ssize_t br = read(logfd, &retval, sizeof (retval));
        if (br != ((ssize_t) sizeof (retval))) {
            IO_READ_FAIL(br >= 0);
        }
    }
    return swap32(retval);
}

static uint64 readle64(void)
{
    uint64 retval = 0;
    if (!io_failure) {
        const ssize_t br = read(logfd, &retval, sizeof (retval));
        if (br != ((ssize_t) sizeof (retval))) {
            IO_READ_FAIL(br >= 0);
        }
    }
    return swap64(retval);
}

static int32 IO_INT32(void)
{
    union { int32 si32; uint32 ui32; } cvt;
    cvt.ui32 = readle32();
    return cvt.si32;
}

static uint32 IO_UINT32(void)
{
    return readle32();
}

static uint64 IO_UINT64(void)
{
    return readle64();
}

static ALCsizei IO_ALCSIZEI(void)
{
    return (ALCsizei) IO_UINT64();
}

static ALsizei IO_ALSIZEI(void)
{
    return (ALsizei) IO_UINT64();
}

static float IO_FLOAT(void)
{
    union { float f; uint32 ui32; } cvt;
    cvt.ui32 = readle32();
    return cvt.f;
}

static double IO_DOUBLE(void)
{
    union { double d; uint64 ui64; } cvt;
    cvt.ui64 = readle64();
    return cvt.d;
}

static uint8 *IO_BLOB(uint64 *_len)
{
    const uint64 len = IO_UINT64();
    const size_t slen = (size_t) len;
    uint8 *ptr;
    ssize_t br;

    if (io_failure) {
        return NULL;
    }

    if (len == 0xFFFFFFFFFFFFFFFFull) {
        *_len = 0;
        return NULL;
    }

    *_len = len;

    ptr = (uint8 *) get_ioblob(slen + 1);
    br = read(logfd, ptr, slen);
    if (br != ((ssize_t) slen)) {
        IO_READ_FAIL(br >= 0);
    }
    ptr[slen] = '\0';

    return ptr;
}

static const char *IO_STRING(void)
{
    uint64 len;
    return (const char *) IO_BLOB(&len);
}

static EventEnum IO_EVENTENUM(void)
{
    return (EventEnum) IO_UINT32();
}

static void *IO_PTR(void)
{
    return (void *) (size_t) IO_UINT64();  // !!! FIXME: probably need to map this on 32-bit systems.
}

static ALCenum IO_ALCENUM(void)
{
    return (ALCenum) IO_UINT32();
}

static ALenum IO_ENUM(void)
{
    return (ALenum) IO_UINT32();
}

static ALCboolean IO_ALCBOOLEAN(void)
{
    return (ALCboolean) IO_UINT32();
}

static ALboolean IO_BOOLEAN(void)
{
    return (ALboolean) IO_UINT32();
}

static void IO_ENTRYINFO(CallerInfo *callerinfo)
{
    const uint32 wait_until = IO_UINT32();
    const uint64 logthreadid = IO_UINT64();
    const uint32 frames = IO_UINT32();
    uint32 threadid;
    uint32 i;

    if (io_failure) {
        return;
    }

    threadid = get_mapped_threadid(logthreadid);

    if (!threadid) {
        threadid = ++next_mapped_threadid;
        add_threadid_to_map(logthreadid, threadid);
    }

    callerinfo->num_callstack_frames = (frames < MAX_CALLSTACKS) ? frames : MAX_CALLSTACKS;
    callerinfo->threadid = threadid;
    callerinfo->trace_scope = trace_scope;
    callerinfo->wait_until = wait_until;
    callerinfo->userdata = guserdata;

    for (i = 0; i < frames; i++) {
        void *ptr = IO_PTR();
        if ((!io_failure) && (i < MAX_CALLSTACKS)) {
            callerinfo->callstack[i].frame = ptr;
            callerinfo->callstack[i].sym = get_mapped_stackframe(ptr);
        }
    }

    callerinfo->fdoffset = lseek(logfd, 0, SEEK_CUR);
}

#define IO_START(e) { CallerInfo callerinfo; IO_ENTRYINFO(&callerinfo); if (!io_failure) {
#define IO_END() } }


static int init_altrace_playback(const char *filename)
{
    int okay = 1;

    io_failure = 0;

    logfd = open(filename, O_RDONLY);
    if (logfd == -1) {
        fprintf(stderr, "%s: Failed to open OpenAL log file '%s': %s\n", GAppName, filename, strerror(errno));
        okay = 0;
    }

    fflush(stderr);

    if (okay) {
        if (IO_UINT32() != ALTRACE_LOG_FILE_MAGIC) {
            fprintf(stderr, "%s: File '%s' does not appear to be an OpenAL log file.\n", GAppName, filename);
            okay = 0;
        } else if (IO_UINT32() != ALTRACE_LOG_FILE_FORMAT) {
            fprintf(stderr, "%s: File '%s' is an unsupported log file format version.\n", GAppName, filename);
            okay = 0;
        }
    }

    if (!okay) {
        quit_altrace_playback();
    }

    return okay;
}

static void quit_altrace_playback(void)
{
    const int io = logfd;

    logfd = -1;

    fflush(stdout);

    if ((io != -1) && (close(io) < 0)) {
        fprintf(stderr, "%s: Failed to close OpenAL log file: %s\n", GAppName, strerror(errno));
    }

    free_device_map();
    free_context_map();
    free_source_map();
    free_buffer_map();
    free_stackframe_map();
    free_threadid_map();
    free_devicelabel_map();
    free_contextlabel_map();
    free_sourcelabel_map();
    free_bufferlabel_map();
    free_ioblobs();

    fflush(stderr);
}

const char *alcboolString(const ALCboolean x)
{
    switch (x) {
        case ALC_TRUE: return "ALC_TRUE";
        case ALC_FALSE: return "ALC_FALSE";
        default: break;
    }

    return sprintf_alloc("0x%X", (uint) x);
}

const char *alboolString(const ALCboolean x)
{
    switch (x) {
        case AL_TRUE: return "AL_TRUE";
        case AL_FALSE: return "AL_FALSE";
        default: break;
    }

    return sprintf_alloc("0x%X", (uint) x);
}

const char *alcenumString(const ALCenum x)
{
    switch (x) {
        #define ENUM_TEST(e) case e: return #e
        ENUM_TEST(ALC_FREQUENCY);
        ENUM_TEST(ALC_REFRESH);
        ENUM_TEST(ALC_SYNC);
        ENUM_TEST(ALC_MONO_SOURCES);
        ENUM_TEST(ALC_STEREO_SOURCES);
        ENUM_TEST(ALC_NO_ERROR);
        ENUM_TEST(ALC_INVALID_DEVICE);
        ENUM_TEST(ALC_INVALID_CONTEXT);
        ENUM_TEST(ALC_INVALID_ENUM);
        ENUM_TEST(ALC_INVALID_VALUE);
        ENUM_TEST(ALC_OUT_OF_MEMORY);
        ENUM_TEST(ALC_MAJOR_VERSION);
        ENUM_TEST(ALC_MINOR_VERSION);
        ENUM_TEST(ALC_ATTRIBUTES_SIZE);
        ENUM_TEST(ALC_ALL_ATTRIBUTES);
        ENUM_TEST(ALC_DEFAULT_DEVICE_SPECIFIER);
        ENUM_TEST(ALC_DEVICE_SPECIFIER);
        ENUM_TEST(ALC_EXTENSIONS);
        ENUM_TEST(ALC_CAPTURE_DEVICE_SPECIFIER);
        ENUM_TEST(ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER);
        ENUM_TEST(ALC_CAPTURE_SAMPLES);
        ENUM_TEST(ALC_DEFAULT_ALL_DEVICES_SPECIFIER);
        ENUM_TEST(ALC_ALL_DEVICES_SPECIFIER);
        ENUM_TEST(ALC_CONNECTED);
        #undef ENUM_TEST
        default: break;
    }

    return sprintf_alloc("0x%X", (uint) x);
}

const char *alenumString(const ALCenum x)
{
    switch (x) {
        #define ENUM_TEST(e) case e: return #e
        ENUM_TEST(AL_NONE);
        ENUM_TEST(AL_SOURCE_RELATIVE);
        ENUM_TEST(AL_CONE_INNER_ANGLE);
        ENUM_TEST(AL_CONE_OUTER_ANGLE);
        ENUM_TEST(AL_PITCH);
        ENUM_TEST(AL_POSITION);
        ENUM_TEST(AL_DIRECTION);
        ENUM_TEST(AL_VELOCITY);
        ENUM_TEST(AL_LOOPING);
        ENUM_TEST(AL_BUFFER);
        ENUM_TEST(AL_GAIN);
        ENUM_TEST(AL_MIN_GAIN);
        ENUM_TEST(AL_MAX_GAIN);
        ENUM_TEST(AL_ORIENTATION);
        ENUM_TEST(AL_SOURCE_STATE);
        ENUM_TEST(AL_INITIAL);
        ENUM_TEST(AL_PLAYING);
        ENUM_TEST(AL_PAUSED);
        ENUM_TEST(AL_STOPPED);
        ENUM_TEST(AL_BUFFERS_QUEUED);
        ENUM_TEST(AL_BUFFERS_PROCESSED);
        ENUM_TEST(AL_REFERENCE_DISTANCE);
        ENUM_TEST(AL_ROLLOFF_FACTOR);
        ENUM_TEST(AL_CONE_OUTER_GAIN);
        ENUM_TEST(AL_MAX_DISTANCE);
        ENUM_TEST(AL_SEC_OFFSET);
        ENUM_TEST(AL_SAMPLE_OFFSET);
        ENUM_TEST(AL_BYTE_OFFSET);
        ENUM_TEST(AL_SOURCE_TYPE);
        ENUM_TEST(AL_STATIC);
        ENUM_TEST(AL_STREAMING);
        ENUM_TEST(AL_UNDETERMINED);
        ENUM_TEST(AL_FORMAT_MONO8);
        ENUM_TEST(AL_FORMAT_MONO16);
        ENUM_TEST(AL_FORMAT_STEREO8);
        ENUM_TEST(AL_FORMAT_STEREO16);
        ENUM_TEST(AL_FREQUENCY);
        ENUM_TEST(AL_BITS);
        ENUM_TEST(AL_CHANNELS);
        ENUM_TEST(AL_SIZE);
        ENUM_TEST(AL_UNUSED);
        ENUM_TEST(AL_PENDING);
        ENUM_TEST(AL_PROCESSED);
        ENUM_TEST(AL_INVALID_NAME);
        ENUM_TEST(AL_INVALID_ENUM);
        ENUM_TEST(AL_INVALID_VALUE);
        ENUM_TEST(AL_INVALID_OPERATION);
        ENUM_TEST(AL_OUT_OF_MEMORY);
        ENUM_TEST(AL_VENDOR);
        ENUM_TEST(AL_VERSION);
        ENUM_TEST(AL_RENDERER);
        ENUM_TEST(AL_EXTENSIONS);
        ENUM_TEST(AL_DOPPLER_FACTOR);
        ENUM_TEST(AL_DOPPLER_VELOCITY);
        ENUM_TEST(AL_SPEED_OF_SOUND);
        ENUM_TEST(AL_DISTANCE_MODEL);
        ENUM_TEST(AL_INVERSE_DISTANCE);
        ENUM_TEST(AL_INVERSE_DISTANCE_CLAMPED);
        ENUM_TEST(AL_LINEAR_DISTANCE);
        ENUM_TEST(AL_LINEAR_DISTANCE_CLAMPED);
        ENUM_TEST(AL_EXPONENT_DISTANCE);
        ENUM_TEST(AL_EXPONENT_DISTANCE_CLAMPED);
        ENUM_TEST(AL_FORMAT_MONO_FLOAT32);
        ENUM_TEST(AL_FORMAT_STEREO_FLOAT32);
        #undef ENUM_TEST
        default: break;
    }

    return sprintf_alloc("0x%X", (uint) x);
}

const char *litString(const char *str)
{
    if (str) {
        const size_t slen = (strlen(str) + 1) * 2;
        char *retval = (char *) get_ioblob(slen);
        char *ptr = retval;
        *(ptr++) = '"';
        while (1) {
            const char ch = *(str++);
            if (ch == '\0') {
                *(ptr++) = '"';
                *(ptr++) = '\0';
                break;
            } else if (ch == '"') {
                *(ptr++) = '\\';
                *(ptr++) = '"';
            } else {
                *(ptr++) = ch;
            }
        }
        return retval;
    }
    return "NULL";
}

const char *ptrString(const void *ptr)
{
    return ptr ? sprintf_alloc("%p", ptr) : "NULL";
}

const char *ctxString(ALCcontext *ctx)
{
    char *label = ctx ? get_mapped_contextlabel(ctx) : NULL;
    return label ? sprintf_alloc("%s<%s>", ptrString(ctx), label) : ptrString(ctx);
}

const char *deviceString(ALCdevice *device)
{
    char *label = device ? get_mapped_devicelabel(device) : NULL;
    return label ? sprintf_alloc("%s<%s>", ptrString(device), label) : ptrString(device);
}

const char *sourceString(const ALuint name)
{
    char *label = name ? get_mapped_sourcelabel(name) : NULL;
    return label ? sprintf_alloc("%u<%s>", (uint) name, label) : sprintf_alloc("%u", (uint) name);
}

const char *bufferString(const ALuint name)
{
    char *label = name ? get_mapped_bufferlabel(name) : NULL;
    return label ? sprintf_alloc("%u<%s>", (uint) name, label) : sprintf_alloc("%u", (uint) name);
}


static void decode_alcGetCurrentContext(void)
{
    IO_START(alcGetCurrentContext);
    ALCcontext *retval = (ALCcontext *) IO_PTR();
    if (!io_failure) visit_alcGetCurrentContext(&callerinfo, retval);
    IO_END();
}

static void decode_alcGetContextsDevice(void)
{
    IO_START(alcGetContextsDevice);
    ALCcontext *context = (ALCcontext *) IO_PTR();
    ALCdevice *retval = (ALCdevice *) IO_PTR();
    if (!io_failure) visit_alcGetContextsDevice(&callerinfo, retval, context);
    IO_END();
}

static void decode_alcIsExtensionPresent(void)
{
    IO_START(alcIsExtensionPresent);
    ALCdevice *device = (ALCdevice *) IO_PTR();
    const ALCchar *extname = (const ALCchar *) IO_STRING();
    const ALCboolean retval = IO_ALCBOOLEAN();
    if (!io_failure) visit_alcIsExtensionPresent(&callerinfo, retval, device, extname);
    IO_END();
}

static void decode_alcGetProcAddress(void)
{
    IO_START(alcGetProcAddress);
    ALCdevice *device = (ALCdevice *) IO_PTR();
    const ALCchar *funcname = (const ALCchar *) IO_STRING();
    void *retval = IO_PTR();
    if (!io_failure) visit_alcGetProcAddress(&callerinfo, retval, device, funcname);
    IO_END();

}

static void decode_alcGetEnumValue(void)
{
    IO_START(alcGetEnumValue);
    ALCdevice *device = (ALCdevice *) IO_PTR();
    const ALCchar *enumname = (const ALCchar *) IO_STRING();
    const ALCenum retval = IO_ALCENUM();
    if (!io_failure) visit_alcGetEnumValue(&callerinfo, retval, device, enumname);
    IO_END();
}

static void decode_alcGetString(void)
{
    IO_START(alcGetEnumValue);
    ALCdevice *device = (ALCdevice *) IO_PTR();
    const ALCenum param = IO_ALCENUM();
    const ALCchar *retval = (const ALCchar *) IO_STRING();
    if (!io_failure) visit_alcGetString(&callerinfo, retval, device, param);
    IO_END();
}

static void decode_alcCaptureOpenDevice(void)
{
    IO_START(alcCaptureOpenDevice);
    const ALCchar *devicename = (const ALCchar *) IO_STRING();
    const ALCuint frequency = IO_UINT32();
    const ALCenum format = IO_ALCENUM();
    const ALCsizei buffersize = IO_ALSIZEI();
    ALCdevice *retval = (ALCdevice *) IO_PTR();
    const ALint major_version = retval ? IO_INT32() : 0;
    const ALint minor_version = retval ? IO_INT32() : 0;
    const ALCchar *devspec = (const ALCchar *) (retval ? IO_STRING() : NULL);
    const ALCchar *extensions = (const ALCchar *) (retval ? IO_STRING() : NULL);
    if (!io_failure) visit_alcCaptureOpenDevice(&callerinfo, retval, devicename, frequency, format, buffersize, major_version, minor_version, devspec, extensions);
    IO_END();
}

static void decode_alcCaptureCloseDevice(void)
{
    IO_START(alcCaptureCloseDevice);
    ALCdevice *device = (ALCdevice *) IO_PTR();
    const ALCboolean retval = IO_ALCBOOLEAN();
    if (!io_failure) visit_alcCaptureCloseDevice(&callerinfo, retval, device);
    add_devicelabel_to_map(device, NULL);
    IO_END();
}

static void decode_alcOpenDevice(void)
{
    IO_START(alcOpenDevice);
    const ALCchar *devicename = IO_STRING();
    ALCdevice *retval = (ALCdevice *) IO_PTR();
    const ALint major_version = retval ? IO_INT32() : 0;
    const ALint minor_version = retval ? IO_INT32() : 0;
    const ALCchar *devspec = (const ALCchar *) (retval ? IO_STRING() : NULL);
    const ALCchar *extensions = (const ALCchar *) (retval ? IO_STRING() : NULL);
    if (!io_failure) visit_alcOpenDevice(&callerinfo, retval, devicename, major_version, minor_version, devspec, extensions);
    IO_END();
}

static void decode_alcCloseDevice(void)
{
    IO_START(alcCloseDevice);
    ALCdevice *device = (ALCdevice *) IO_PTR();
    const ALCboolean retval = IO_ALCBOOLEAN();
    if (!io_failure) visit_alcCloseDevice(&callerinfo, retval, device);
    add_devicelabel_to_map(device, NULL);
    IO_END();
}

static void decode_alcCreateContext(void)
{
    IO_START(alcCreateContext);
    ALCcontext *retval;
    ALCdevice *device = (ALCdevice *) IO_PTR();
    ALCint *origattrlist = (ALCint *) IO_PTR();
    const uint32 attrcount = IO_UINT32();
    ALCint *attrlist = NULL;
    if (attrcount) {
        ALCint i;
        attrlist = (ALCint *) get_ioblob(sizeof (ALCint) * attrcount);
        for (i = 0; i < attrcount; i++) {
            attrlist[i] = (ALCint) IO_INT32();
        }
    }
    retval = (ALCcontext *) IO_PTR();

    if (!io_failure) visit_alcCreateContext(&callerinfo, retval, device, origattrlist, attrcount, attrlist);

    IO_END();

}

static void decode_alcMakeContextCurrent(void)
{
    IO_START(alcMakeContextCurrent);
    ALCcontext *ctx = (ALCcontext *) IO_PTR();
    const ALCboolean retval = IO_ALCBOOLEAN();
    if (!io_failure) visit_alcMakeContextCurrent(&callerinfo, retval, ctx);
    IO_END();
}

static void decode_alcProcessContext(void)
{
    IO_START(alcProcessContext);
    ALCcontext *ctx = (ALCcontext *) IO_PTR();
    if (!io_failure) visit_alcProcessContext(&callerinfo, ctx);
    IO_END();
}

static void decode_alcSuspendContext(void)
{
    IO_START(alcSuspendContext);
    ALCcontext *ctx = (ALCcontext *) IO_PTR();
    if (!io_failure) visit_alcSuspendContext(&callerinfo, ctx);
    IO_END();
}

static void decode_alcDestroyContext(void)
{
    IO_START(alcDestroyContext);
    ALCcontext *ctx = (ALCcontext *) IO_PTR();
    if (!io_failure) visit_alcDestroyContext(&callerinfo, ctx);
    add_contextlabel_to_map(ctx, NULL);
    IO_END();
}

static void decode_alcGetError(void)
{
    IO_START(alcGetError);
    ALCdevice *device = (ALCdevice *) IO_PTR();
    const ALCenum retval = IO_ALCENUM();
    if (!io_failure) visit_alcGetError(&callerinfo, retval, device);
    IO_END();
}

static void decode_alcGetIntegerv(void)
{
    IO_START(alcGetIntegerv);
    ALCdevice *device = (ALCdevice *) IO_PTR();
    const ALCenum param = IO_ALCENUM();
    const ALCsizei size = IO_ALCSIZEI();
    ALCint *origvalues = (ALint *) IO_PTR();
    ALCint *values = (ALCint *) (origvalues ? get_ioblob(size * sizeof (ALCint)) : NULL);
    ALCsizei i;
    ALCboolean isbool = ALC_FALSE;

    if (origvalues) {
        for (i = 0; i < size; i++) {
            values[i] = IO_INT32();
        }
    }

    switch (param) {
        case ALC_CONNECTED: isbool = ALC_TRUE; break;
        default: break;
    }

    if (!io_failure) visit_alcGetIntegerv(&callerinfo, device, param, size, origvalues, isbool, values);

    IO_END();
}

static void decode_alcCaptureStart(void)
{
    IO_START(alcCaptureStart);
    ALCdevice *device = (ALCdevice *) IO_PTR();
    if (!io_failure) visit_alcCaptureStart(&callerinfo, device);
    IO_END();
}

static void decode_alcCaptureStop(void)
{
    IO_START(alcCaptureStop);
    ALCdevice *device = (ALCdevice *) IO_PTR();
    if (!io_failure) visit_alcCaptureStop(&callerinfo, device);
    IO_END();
}

static void decode_alcCaptureSamples(void)
{
    IO_START(alcCaptureSamples);
    ALCdevice *device = (ALCdevice *) IO_PTR();
    void *origbuffer = IO_PTR();
    const ALCsizei samples = IO_ALCSIZEI();
    uint64 bloblen;
    uint8 *blob = IO_BLOB(&bloblen);
    if (!io_failure) visit_alcCaptureSamples(&callerinfo, device, origbuffer, blob, bloblen, samples);
    IO_END();
}

static void decode_alDopplerFactor(void)
{
    IO_START(alDopplerFactor);
    const ALfloat value = IO_FLOAT();
    if (!io_failure) visit_alDopplerFactor(&callerinfo, value);
    IO_END();
}

static void decode_alDopplerVelocity(void)
{
    IO_START(alDopplerVelocity);
    const ALfloat value = IO_FLOAT();
    if (!io_failure) visit_alDopplerVelocity(&callerinfo, value);
    IO_END();
}

static void decode_alSpeedOfSound(void)
{
    IO_START(alSpeedOfSound);
    const ALfloat value = IO_FLOAT();
    if (!io_failure) visit_alSpeedOfSound(&callerinfo, value);
    IO_END();
}

static void decode_alDistanceModel(void)
{
    IO_START(alDistanceModel);
    const ALenum model = IO_ENUM();
    if (!io_failure) visit_alDistanceModel(&callerinfo, model);
    IO_END();
}

static void decode_alEnable(void)
{
    IO_START(alEnable);
    const ALenum capability = IO_ENUM();
    if (!io_failure) visit_alEnable(&callerinfo, capability);
    IO_END();
}

static void decode_alDisable(void)
{
    IO_START(alDisable);
    const ALenum capability = IO_ENUM();
    if (!io_failure) visit_alDisable(&callerinfo, capability);
    IO_END();
}

static void decode_alIsEnabled(void)
{
    IO_START(alIsEnabled);
    const ALenum capability = IO_ENUM();
    const ALboolean retval = IO_BOOLEAN();
    if (!io_failure) visit_alIsEnabled(&callerinfo, retval, capability);
    IO_END();
}

static void decode_alGetString(void)
{
    IO_START(alGetString);
    const ALenum param = IO_ENUM();
    const ALchar *retval = (const ALchar *) IO_STRING();
    if (!io_failure) visit_alGetString(&callerinfo, retval, param);
    IO_END();
}

static void decode_alGetBooleanv(void)
{
    IO_START(alGetBooleanv);
    const ALenum param = IO_ENUM();
    ALboolean *origvalues = (ALboolean *) IO_PTR();
    const uint32 numvals = IO_UINT32();
    ALboolean *values = (ALboolean *) (numvals ? get_ioblob(numvals * sizeof (ALboolean)) : NULL);
    ALsizei i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_BOOLEAN();
    }

    if (!io_failure) visit_alGetBooleanv(&callerinfo, param, origvalues, numvals, values);

    IO_END();
}

static void decode_alGetIntegerv(void)
{
    IO_START(alGetIntegerv);
    const ALenum param = IO_ENUM();
    ALint *origvalues = (ALint *) IO_PTR();
    const uint32 numvals = IO_UINT32();
    ALint *values = (ALint *) (numvals ? get_ioblob(numvals * sizeof (ALint)) : NULL);
    ALsizei i;
    ALboolean isenum = AL_FALSE;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_INT32();
    }

    switch (param) {
        case AL_DISTANCE_MODEL: isenum = AL_TRUE; break;
        default: break;
    }

    if (!io_failure) visit_alGetIntegerv(&callerinfo, param, origvalues, numvals, isenum, values);

    IO_END();
}

static void decode_alGetFloatv(void)
{
    IO_START(alGetFloatv);
    const ALenum param = IO_ENUM();
    ALfloat *origvalues = (ALfloat *) IO_PTR();
    const uint32 numvals = IO_UINT32();
    ALfloat *values = (ALfloat *) (numvals ? get_ioblob(numvals * sizeof (ALfloat)) : NULL);
    ALsizei i;
    for (i = 0; i < numvals; i++) {
        values[i] = IO_FLOAT();
    }

    if (!io_failure) visit_alGetFloatv(&callerinfo, param, origvalues, numvals, values);

    IO_END();
}

static void decode_alGetDoublev(void)
{
    IO_START(alGetDoublev);
    const ALenum param = IO_ENUM();
    ALdouble *origvalues = (ALdouble *) IO_PTR();
    const uint32 numvals = IO_UINT32();
    ALdouble *values = (ALdouble *) (numvals ? get_ioblob(numvals * sizeof (ALdouble)) : NULL);
    ALsizei i;
    for (i = 0; i < numvals; i++) {
        values[i] = IO_DOUBLE();
    }

    if (!io_failure) visit_alGetDoublev(&callerinfo, param, origvalues, numvals, values);

    IO_END();
}

static void decode_alGetBoolean(void)
{
    IO_START(alGetBoolean);
    const ALenum param = IO_ENUM();
    const ALboolean retval = IO_BOOLEAN();
    if (!io_failure) visit_alGetBoolean(&callerinfo, retval, param);
    IO_END();
}

static void decode_alGetInteger(void)
{
    IO_START(alGetInteger);
    const ALenum param = IO_ENUM();
    const ALint retval = IO_INT32();
#warning fixme isenum?
    if (!io_failure) visit_alGetInteger(&callerinfo, retval, param);
    IO_END();
}

static void decode_alGetFloat(void)
{
    IO_START(alGetFloat);
    const ALenum param = IO_ENUM();
    const ALfloat retval = IO_FLOAT();
    if (!io_failure) visit_alGetFloat(&callerinfo, retval, param);
    IO_END();
}

static void decode_alGetDouble(void)
{
    IO_START(alGetDouble);
    const ALenum param = IO_ENUM();
    const ALdouble retval = IO_DOUBLE();
    if (!io_failure) visit_alGetDouble(&callerinfo, retval, param);
    IO_END();
}

static void decode_alIsExtensionPresent(void)
{
    IO_START(alIsExtensionPresent);
    const ALchar *extname = (const ALchar *) IO_STRING();
    const ALboolean retval = IO_BOOLEAN();
    if (!io_failure) visit_alIsExtensionPresent(&callerinfo, retval, extname);
    IO_END();
}

static void decode_alGetError(void)
{
    IO_START(alGetError);
    const ALenum retval = IO_ENUM();
    if (!io_failure) visit_alGetError(&callerinfo, retval);
    IO_END();
}

static void decode_alGetProcAddress(void)
{
    IO_START(alGetProcAddress);
    const ALchar *funcname = (const ALchar *) IO_STRING();
    void *retval = IO_PTR();
    if (!io_failure) visit_alGetProcAddress(&callerinfo, retval, funcname);
    IO_END();
}

static void decode_alGetEnumValue(void)
{
    IO_START(alGetProcAddress);
    const ALchar *enumname = (const ALchar *) IO_STRING();
    const ALenum retval = IO_ENUM();
    if (!io_failure) visit_alGetEnumValue(&callerinfo, retval, enumname);
    IO_END();
}

static void decode_alListenerfv(void)
{
    IO_START(alListenerfv);
    const ALenum param = IO_ENUM();
    ALfloat *origvalues = (ALfloat *) IO_PTR();
    const uint32 numvals = IO_UINT32();
    ALfloat *values = (ALfloat *) (numvals ? get_ioblob(sizeof (ALfloat) * numvals) : NULL);
    uint32 i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_FLOAT();
    }

    if (!io_failure) visit_alListenerfv(&callerinfo, param, origvalues, numvals, values);

    IO_END();
}

static void decode_alListenerf(void)
{
    IO_START(alListenerf);
    const ALenum param = IO_ENUM();
    const ALfloat value = IO_FLOAT();
    if (!io_failure) visit_alListenerf(&callerinfo, param, value);
    IO_END();
}

static void decode_alListener3f(void)
{
    IO_START(alListener3f);
    const ALenum param = IO_ENUM();
    const ALfloat value1 = IO_FLOAT();
    const ALfloat value2 = IO_FLOAT();
    const ALfloat value3 = IO_FLOAT();
    if (!io_failure) visit_alListener3f(&callerinfo, param, value1, value2, value3);
    IO_END();
}

static void decode_alListeneriv(void)
{
    IO_START(alListeneriv);
    const ALenum param = IO_ENUM();
    ALint *origvalues = (ALint *) IO_PTR();
    const uint32 numvals = IO_UINT32();
    ALint *values = (ALint *) (numvals ? get_ioblob(sizeof (ALint) * numvals) : NULL);
    uint32 i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_INT32();
    }

    if (!io_failure) visit_alListeneriv(&callerinfo, param, origvalues, numvals, values);

    IO_END();
}

static void decode_alListeneri(void)
{
    IO_START(alListeneri);
    const ALenum param = IO_ENUM();
    const ALint value = IO_INT32();
    if (!io_failure) visit_alListeneri(&callerinfo, param, value);
    IO_END();
}

static void decode_alListener3i(void)
{
    IO_START(alListener3i);
    const ALenum param = IO_ENUM();
    const ALint value1 = IO_INT32();
    const ALint value2 = IO_INT32();
    const ALint value3 = IO_INT32();
    if (!io_failure) visit_alListener3i(&callerinfo, param, value1, value2, value3);
    IO_END();
}

static void decode_alGetListenerfv(void)
{
    IO_START(alGetListenerfv);
    const ALenum param = IO_ENUM();
    ALfloat *origvalues = (ALfloat *) IO_PTR();
    const uint32 numvals = IO_UINT32();
    ALfloat *values = (ALfloat *) (numvals ? get_ioblob(sizeof (ALfloat) * numvals) : NULL);
    uint32 i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_FLOAT();
    }

    if (!io_failure) visit_alGetListenerfv(&callerinfo, param, origvalues, numvals, values);

    IO_END();
}

static void decode_alGetListenerf(void)
{
    IO_START(alGetListenerf);
    const ALenum param = IO_ENUM();
    ALfloat *origvalue = (ALfloat *) IO_PTR();
    const ALfloat value = IO_FLOAT();
    if (!io_failure) visit_alGetListenerf(&callerinfo, param, origvalue, value);
    IO_END();
}

static void decode_alGetListener3f(void)
{
    IO_START(alGetListener3f);
    const ALenum param = IO_ENUM();
    ALfloat *origvalue1 = (ALfloat *) IO_PTR();
    ALfloat *origvalue2 = (ALfloat *) IO_PTR();
    ALfloat *origvalue3 = (ALfloat *) IO_PTR();
    const ALfloat value1 = IO_FLOAT();
    const ALfloat value2 = IO_FLOAT();
    const ALfloat value3 = IO_FLOAT();
    if (!io_failure) visit_alGetListener3f(&callerinfo, param, origvalue1, origvalue2, origvalue3, value1, value2, value3);
    IO_END();
}

static void decode_alGetListeneriv(void)
{
    IO_START(alGetListeneriv);
    const ALenum param = IO_ENUM();
    ALint *origvalues = (ALint *) IO_PTR();
    const uint32 numvals = IO_UINT32();
    ALint *values = (ALint *) (numvals ? get_ioblob(sizeof (ALint) * numvals) : NULL);
    uint32 i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_INT32();
    }

    if (!io_failure) visit_alGetListeneriv(&callerinfo, param, origvalues, numvals, values);

    IO_END();
}

static void decode_alGetListeneri(void)
{
    IO_START(alGetListeneri);
    const ALenum param = IO_ENUM();
    ALint *origvalue = (ALint *) IO_PTR();
    const ALint value = IO_INT32();

    if (!io_failure) visit_alGetListeneri(&callerinfo, param, origvalue, value);

    IO_END();
}

static void decode_alGetListener3i(void)
{
    IO_START(alGetListener3i);
    const ALenum param = IO_ENUM();
    ALint *origvalue1 = (ALint *) IO_PTR();
    ALint *origvalue2 = (ALint *) IO_PTR();
    ALint *origvalue3 = (ALint *) IO_PTR();
    const ALint value1 = IO_INT32();
    const ALint value2 = IO_INT32();
    const ALint value3 = IO_INT32();
    if (!io_failure) visit_alGetListener3i(&callerinfo, param, origvalue1, origvalue2, origvalue3, value1, value2, value3);
    IO_END();
}

static void decode_alGenSources(void)
{
    IO_START(alGenSources);
    const ALsizei n = IO_ALSIZEI();
    ALuint *orignames = (ALuint *) IO_PTR();
    ALuint *names = (ALuint *) get_ioblob(sizeof (ALuint) * n);
    ALsizei i;

    for (i = 0; i < n; i++) {
        names[i] = IO_UINT32();
    }

    if (!io_failure) visit_alGenSources(&callerinfo, n, orignames, names);

    IO_END();
}

static void decode_alDeleteSources(void)
{
    IO_START(alDeleteSources);
    const ALsizei n = IO_ALSIZEI();
    ALuint *orignames = (ALuint *) IO_PTR();
    ALuint *names = (ALuint *) get_ioblob(sizeof (ALuint) * n);
    ALsizei i;

    for (i = 0; i < n; i++) {
        names[i] = IO_UINT32();
    }

    if (!io_failure) visit_alDeleteSources(&callerinfo, n, orignames, names);

    for (i = 0; i < n; i++) {
        add_sourcelabel_to_map(names[i], NULL);
    }

    IO_END();
}

static void decode_alIsSource(void)
{
    IO_START(alIsSource);
    const ALuint name = IO_UINT32();
    const ALboolean retval = IO_BOOLEAN();
    if (!io_failure) visit_alIsSource(&callerinfo, retval, name);
    IO_END();
}

static void decode_alSourcefv(void)
{
    IO_START(alSourcefv);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALfloat *origvalues = (ALfloat *) IO_PTR();
    const uint32 numvals = IO_UINT32();
    ALfloat *values = (ALfloat *) get_ioblob(sizeof (ALfloat) * numvals);
    uint32 i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_FLOAT();
    }

    if (!io_failure) visit_alSourcefv(&callerinfo, name, param, origvalues, numvals, values);

    IO_END();
}

static void decode_alSourcef(void)
{
    IO_START(alSourcef);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALfloat value = IO_FLOAT();
    if (!io_failure) visit_alSourcef(&callerinfo, name, param, value);
    IO_END();
}

static void decode_alSource3f(void)
{
    IO_START(alSource3f);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALfloat value1 = IO_FLOAT();
    const ALfloat value2 = IO_FLOAT();
    const ALfloat value3 = IO_FLOAT();
    if (!io_failure) visit_alSource3f(&callerinfo, name, param, value1, value2, value3);
    IO_END();
}

static void decode_alSourceiv(void)
{
    IO_START(alSourceiv);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALint *origvalues = (ALint *) IO_PTR();
    const uint32 numvals = IO_UINT32();
    ALint *values = (ALint *) get_ioblob(sizeof (ALint) * numvals);
    uint32 i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_INT32();
    }

    if (!io_failure) visit_alSourceiv(&callerinfo, name, param, origvalues, numvals, values);

    IO_END();
}

static void decode_alSourcei(void)
{
#pragma warning AL_LOOPING is bool, others might be enum
    IO_START(alSourcei);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALint value = IO_INT32();
    if (!io_failure) visit_alSourcei(&callerinfo, name, param, value);
    IO_END();
}

static void decode_alSource3i(void)
{
    IO_START(alSource3i);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALint value1 = IO_INT32();
    const ALint value2 = IO_INT32();
    const ALint value3 = IO_INT32();
    if (!io_failure) visit_alSource3i(&callerinfo, name, param, value1, value2, value3);
    IO_END();
}

static void decode_alGetSourcefv(void)
{
    IO_START(alGetSourcefv);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    ALfloat *origvalues = (ALfloat *) IO_PTR();
    const uint32 numvals = IO_UINT32();
    ALfloat *values = (ALfloat *) (numvals ? get_ioblob(sizeof (ALfloat) * numvals) : NULL);
    uint32 i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_FLOAT();
    }

    if (!io_failure) visit_alGetSourcefv(&callerinfo, name, param, origvalues, numvals, values);

    IO_END();
}

static void decode_alGetSourcef(void)
{
    IO_START(alGetSourcef);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    ALfloat *origvalue = (ALfloat *) IO_PTR();
    const ALfloat value = IO_FLOAT();
    if (!io_failure) visit_alGetSourcef(&callerinfo, name, param, origvalue, value);
    IO_END();
}

static void decode_alGetSource3f(void)
{
    IO_START(alGetSource3f);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    ALfloat *origvalue1 = (ALfloat *) IO_PTR();
    ALfloat *origvalue2 = (ALfloat *) IO_PTR();
    ALfloat *origvalue3 = (ALfloat *) IO_PTR();
    const ALfloat value1 = IO_FLOAT();
    const ALfloat value2 = IO_FLOAT();
    const ALfloat value3 = IO_FLOAT();
    if (!io_failure) visit_alGetSource3f(&callerinfo, name, param, origvalue1, origvalue2, origvalue3, value1, value2, value3);
    IO_END();
}

static void decode_alGetSourceiv(void)
{
    IO_START(alGetSourceiv);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    ALint *origvalues = (ALint *) IO_PTR();
    const uint32 numvals = IO_UINT32();
    ALint *values = (ALint *) (numvals ? get_ioblob(sizeof (ALfloat) * numvals) : NULL);
    ALboolean isenum = AL_FALSE;
    uint32 i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_INT32();
    }

    switch (param) {
        case AL_SOURCE_STATE: isenum = AL_TRUE;
        default: break;
    }

    if (!io_failure) visit_alGetSourceiv(&callerinfo, name, param, isenum, origvalues, numvals, values);

    IO_END();
}

static void decode_alGetSourcei(void)
{
    IO_START(alGetSourcei);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    ALint *origvalue = (ALint *) IO_PTR();
    const ALint value = IO_INT32();
    ALboolean isenum = AL_FALSE;

    switch (param) {
        case AL_SOURCE_STATE: isenum = AL_TRUE;
        default: break;
    }

    if (!io_failure) visit_alGetSourcei(&callerinfo, name, param, isenum, origvalue, value);

    IO_END();
}

static void decode_alGetSource3i(void)
{
    IO_START(alGetSource3i);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    ALint *origvalue1 = (ALint *) IO_PTR();
    ALint *origvalue2 = (ALint *) IO_PTR();
    ALint *origvalue3 = (ALint *) IO_PTR();
    const ALint value1 = IO_INT32();
    const ALint value2 = IO_INT32();
    const ALint value3 = IO_INT32();
    if (!io_failure) visit_alGetSource3i(&callerinfo, name, param, origvalue1, origvalue2, origvalue3, value1, value2, value3);
    IO_END();
}

static void decode_alSourcePlay(void)
{
    IO_START(alSourcePlay);
    const ALuint name = IO_UINT32();
    if (!io_failure) visit_alSourcePlay(&callerinfo, name);
    IO_END();
}

static void decode_alSourcePlayv(void)
{
    IO_START(alSourcePlayv);
    const ALsizei n = IO_ALSIZEI();
    ALuint *orignames = (ALuint *) IO_PTR();
    ALuint *names = (ALuint *) get_ioblob(sizeof (ALuint) * n);
    ALsizei i;

    for (i = 0; i < n; i++) {
        names[i] = IO_UINT32();
    }

    if (!io_failure) visit_alSourcePlayv(&callerinfo, n, orignames, names);

    IO_END();
}

static void decode_alSourcePause(void)
{
    IO_START(alSourcePause);
    const ALuint name = IO_UINT32();
    if (!io_failure) visit_alSourcePause(&callerinfo, name);
    IO_END();
}

static void decode_alSourcePausev(void)
{
    IO_START(alSourcePausev);
    const ALsizei n = IO_ALSIZEI();
    ALuint *orignames = (ALuint *) IO_PTR();
    ALuint *names = (ALuint *) get_ioblob(sizeof (ALuint) * n);
    ALsizei i;

    for (i = 0; i < n; i++) {
        names[i] = IO_UINT32();
    }

    if (!io_failure) visit_alSourcePausev(&callerinfo, n, orignames, names);

    IO_END();
}

static void decode_alSourceRewind(void)
{
    IO_START(alSourceRewind);
    const ALuint name = IO_UINT32();
    if (!io_failure) visit_alSourceRewind(&callerinfo, name);
    IO_END();
}

static void decode_alSourceRewindv(void)
{
    IO_START(alSourceRewindv);
    const ALsizei n = IO_ALSIZEI();
    ALuint *orignames = (ALuint *) IO_PTR();
    ALuint *names = (ALuint *) get_ioblob(sizeof (ALuint) * n);
    ALsizei i;

    for (i = 0; i < n; i++) {
        names[i] = IO_UINT32();
    }

    if (!io_failure) visit_alSourceRewindv(&callerinfo, n, orignames, names);

    IO_END();
}

static void decode_alSourceStop(void)
{
    IO_START(alSourceStop);
    const ALuint name = IO_UINT32();
    if (!io_failure) visit_alSourceStop(&callerinfo, name);
    IO_END();
}

static void decode_alSourceStopv(void)
{
    IO_START(alSourceStopv);
    const ALsizei n = IO_ALSIZEI();
    ALuint *orignames = (ALuint *) IO_PTR();
    ALuint *names = (ALuint *) get_ioblob(sizeof (ALuint) * n);
    ALsizei i;

    for (i = 0; i < n; i++) {
        names[i] = IO_UINT32();
    }

    if (!io_failure) visit_alSourceStopv(&callerinfo, n, orignames, names);

    IO_END();
}

static void decode_alSourceQueueBuffers(void)
{
    IO_START(alSourceQueueBuffers);
    const ALuint name = IO_UINT32();
    const ALsizei nb = IO_ALSIZEI();
    ALuint *orignames = (ALuint *) IO_PTR();
    ALuint *names = (ALuint *) get_ioblob(sizeof (ALuint) * nb);
    ALsizei i;

    for (i = 0; i < nb; i++) {
        names[i] = IO_UINT32();
    }

    if (!io_failure) visit_alSourceQueueBuffers(&callerinfo, name, nb, orignames, names);

    IO_END();
}

static void decode_alSourceUnqueueBuffers(void)
{
    IO_START(alSourceUnqueueBuffers);
    const ALuint name = IO_UINT32();
    const ALsizei nb = IO_ALSIZEI();
    ALuint *orignames = (ALuint *) IO_PTR();
    ALuint *names = (ALuint *) get_ioblob(sizeof (ALuint) * nb);
    ALsizei i;

    for (i = 0; i < nb; i++) {
        names[i] = IO_UINT32();
    }

    if (!io_failure) visit_alSourceUnqueueBuffers(&callerinfo, name, nb, orignames, names);

    IO_END();
}

static void decode_alGenBuffers(void)
{
    IO_START(alGenBuffers);
    const ALsizei n = IO_ALSIZEI();
    ALuint *orignames = (ALuint *) IO_PTR();
    ALuint *names = (ALuint *) get_ioblob(sizeof (ALuint) * n);
    ALsizei i;

    for (i = 0; i < n; i++) {
        names[i] = IO_UINT32();
    }

    if (!io_failure) visit_alGenBuffers(&callerinfo, n, orignames, names);

    IO_END();
}

static void decode_alDeleteBuffers(void)
{
    IO_START(alDeleteBuffers);
    const ALsizei n = IO_ALSIZEI();
    ALuint *orignames = (ALuint *) IO_PTR();
    ALuint *names = (ALuint *) get_ioblob(sizeof (ALuint) * n);
    ALsizei i;

    for (i = 0; i < n; i++) {
        names[i] = IO_UINT32();
    }

    if (!io_failure) visit_alDeleteBuffers(&callerinfo, n, orignames, names);

    for (i = 0; i < n; i++) {
        add_bufferlabel_to_map(names[i], NULL);
    }

    IO_END();
}

static void decode_alIsBuffer(void)
{
    IO_START(alIsBuffer);
    const ALuint name = IO_UINT32();
    const ALboolean retval = IO_BOOLEAN();
    if (!io_failure) visit_alIsBuffer(&callerinfo, retval, name);
    IO_END();
}

static void decode_alBufferData(void)
{
    IO_START(alBufferData);
    uint64 size = 0;
    const ALuint name = IO_UINT32();
    const ALenum alfmt = IO_ENUM();
    const ALsizei freq = IO_ALSIZEI();
    const ALvoid *origdata = (const ALvoid *) IO_PTR();
    const ALvoid *data = (const ALvoid *) IO_BLOB(&size);
    if (!io_failure) visit_alBufferData(&callerinfo, name, alfmt, origdata, data, (ALsizei) size, freq);
    IO_END();
}

static void decode_alBufferfv(void)
{
    IO_START(alBufferfv);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALfloat *origvalues = (ALfloat *) IO_PTR();
    const uint32 numvals = IO_UINT32();
    ALfloat *values = (ALfloat *) get_ioblob(sizeof (ALfloat) * numvals);
    uint32 i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_INT32();
    }

    if (!io_failure) visit_alBufferfv(&callerinfo, name, param, origvalues, numvals, values);

    IO_END();
}

static void decode_alBufferf(void)
{
    IO_START(alBufferf);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALfloat value = IO_FLOAT();
    if (!io_failure) visit_alBufferf(&callerinfo, name, param, value);
    IO_END();
}

static void decode_alBuffer3f(void)
{
    IO_START(alBuffer3f);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALfloat value1 = IO_FLOAT();
    const ALfloat value2 = IO_FLOAT();
    const ALfloat value3 = IO_FLOAT();
    if (!io_failure) visit_alBuffer3f(&callerinfo, name, param, value1, value2, value3);
    IO_END();
}

static void decode_alBufferiv(void)
{
    IO_START(alBufferiv);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALint *origvalues = (ALint *) IO_PTR();
    const uint32 numvals = IO_UINT32();
    ALint *values = (ALint *) get_ioblob(sizeof (ALint) * numvals);
    uint32 i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_INT32();
    }

    if (!io_failure) visit_alBufferiv(&callerinfo, name, param, origvalues, numvals, values);

    IO_END();
}

static void decode_alBufferi(void)
{
    IO_START(alBufferi);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALint value = IO_INT32();
    if (!io_failure) visit_alBufferi(&callerinfo, name, param, value);
    IO_END();
}

static void decode_alBuffer3i(void)
{
    IO_START(alBuffer3i);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALint value1 = IO_INT32();
    const ALint value2 = IO_INT32();
    const ALint value3 = IO_INT32();
    if (!io_failure) visit_alBuffer3i(&callerinfo, name, param, value1, value2, value3);
    IO_END();
}

static void decode_alGetBufferfv(void)
{
    IO_START(alGetBufferfv);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    ALfloat *origvalues = (ALfloat *) IO_PTR();
    const uint32 numvals = IO_UINT32();
    ALfloat *values = (ALfloat *) (numvals ? get_ioblob(sizeof (ALfloat) * numvals) : NULL);
    uint32 i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_FLOAT();
    }

    if (!io_failure) visit_alGetBufferfv(&callerinfo, name, param, origvalues, numvals, values);

    IO_END();
}

static void decode_alGetBufferf(void)
{
    IO_START(alGetBufferf);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    ALfloat *origvalue = (ALfloat *) IO_PTR();
    const ALfloat value = IO_FLOAT();
    if (!io_failure) visit_alGetBufferf(&callerinfo, name, param, origvalue, value);
    IO_END();
}

static void decode_alGetBuffer3f(void)
{
    IO_START(alGetBuffer3f);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    ALfloat *origvalue1 = (ALfloat *) IO_PTR();
    ALfloat *origvalue2 = (ALfloat *) IO_PTR();
    ALfloat *origvalue3 = (ALfloat *) IO_PTR();
    const ALfloat value1 = IO_FLOAT();
    const ALfloat value2 = IO_FLOAT();
    const ALfloat value3 = IO_FLOAT();
    if (!io_failure) visit_alGetBuffer3f(&callerinfo, name, param, origvalue1, origvalue2, origvalue3, value1, value2, value3);
    IO_END();
}

static void decode_alGetBufferi(void)
{
    IO_START(alGetBufferi);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    ALint *origvalue = (ALint *) IO_PTR();
    const ALint value = IO_INT32();
    if (!io_failure) visit_alGetBufferi(&callerinfo, name, param, origvalue, value);
    IO_END();
}

static void decode_alGetBuffer3i(void)
{
    IO_START(alGetBuffer3i);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    ALint *origvalue1 = (ALint *) IO_PTR();
    ALint *origvalue2 = (ALint *) IO_PTR();
    ALint *origvalue3 = (ALint *) IO_PTR();
    const ALint value1 = IO_INT32();
    const ALint value2 = IO_INT32();
    const ALint value3 = IO_INT32();
    if (!io_failure) visit_alGetBuffer3i(&callerinfo, name, param, origvalue1, origvalue2, origvalue3, value1, value2, value3);
    IO_END();
}

static void decode_alGetBufferiv(void)
{
    IO_START(alGetBufferiv);
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    ALint *origvalues = (ALint *) IO_PTR();
    const uint32 numvals = IO_UINT32();
    ALint *values = (ALint *) (numvals ? get_ioblob(sizeof (ALfloat) * numvals) : NULL);
    uint32 i;

    for (i = 0; i < numvals; i++) {
        values[i] = IO_INT32();
    }

    if (!io_failure) visit_alGetBufferiv(&callerinfo, name, param, origvalues, numvals, values);

    IO_END();
}

static void decode_alTracePushScope(void)
{
    IO_START(alTracePushScope);
    const ALchar *str = IO_STRING();
    if (!io_failure) visit_alTracePushScope(&callerinfo, str);
    trace_scope++;
    IO_END();
}

static void decode_alTracePopScope(void)
{
    IO_START(alTracePopScope);
    callerinfo.trace_scope--;
    trace_scope--;
    if (!io_failure) visit_alTracePopScope(&callerinfo);
    IO_END();
}

static void decode_alTraceMessage(void)
{
    IO_START(alTraceMessage);
    const ALchar *str = IO_STRING();
    if (!io_failure) visit_alTraceMessage(&callerinfo, str);
    IO_END();
}

static void decode_alTraceBufferLabel(void)
{
    IO_START(alTraceBufferLabel);
    const ALuint name = IO_UINT32();
    const ALchar *str = IO_STRING();
    if (name) {
        char *dup = str ? strdup(str) : NULL;
        if (dup || !str) {
            add_bufferlabel_to_map(name, dup);
        }
    }
    if (!io_failure) visit_alTraceBufferLabel(&callerinfo, name, str);
    IO_END();
}

static void decode_alTraceSourceLabel(void)
{
    IO_START(alTraceSourceLabel);
    const ALuint name = IO_UINT32();
    const ALchar *str = IO_STRING();
    if (name) {
        char *dup = str ? strdup(str) : NULL;
        if (dup || !str) {
            add_sourcelabel_to_map(name, dup);
        }
    }
    if (!io_failure) visit_alTraceSourceLabel(&callerinfo, name, str);
    IO_END();
}

static void decode_alcTraceDeviceLabel(void)
{
    IO_START(alcTraceDeviceLabel);
    ALCdevice *device = (ALCdevice *) IO_PTR();
    const ALCchar *str = IO_STRING();
    if (device) {
        char *dup = str ? strdup(str) : NULL;
        if (dup || !str) {
            add_devicelabel_to_map(device, dup);
        }
    }
    if (!io_failure) visit_alcTraceDeviceLabel(&callerinfo, device, str);
    IO_END();
}

static void decode_alcTraceContextLabel(void)
{
    IO_START(alcTraceContextLabel);
    ALCcontext *ctx = (ALCcontext *) IO_PTR();
    const ALCchar *str = IO_STRING();
    if (ctx) {
        char *dup = str ? strdup(str) : NULL;
        if (dup || !str) {
            add_contextlabel_to_map(ctx, dup);
        }
    }
    if (!io_failure) visit_alcTraceContextLabel(&callerinfo, ctx, str);
    IO_END();
}


// this one doesn't have a visitor; we handle compiling the symbol map here.
static void decode_callstack_syms_event(void)
{
    const uint32 num_new_strings = IO_UINT32();
    uint32 i;
    for (i = 0; i < num_new_strings; i++) {
        void *ptr = IO_PTR();
        const char *str = IO_STRING();
        if (io_failure) break;
        if (str) {
            char *dup = strdup(str);
            if (ptr && dup) {
                add_stackframe_to_map(ptr, dup);
            }
        }
    }
}


static void decode_al_error_event(void)
{
    const ALenum err = IO_ENUM();
    if (!io_failure) visit_al_error_event(guserdata, err);
}

static void decode_alc_error_event(void)
{
    ALCdevice *device = (ALCdevice *) IO_PTR();
    const ALCenum err = IO_ALCENUM();
    if (!io_failure) visit_alc_error_event(guserdata, device, err);
}

static void decode_device_state_changed_int(void)
{
    ALCdevice *dev = (ALCdevice *) IO_PTR();
    const ALCenum param = IO_ALCENUM();
    const ALCint newval = IO_INT32();
    if (!io_failure) visit_device_state_changed_int(guserdata, dev, param, newval);
}

static void decode_context_state_changed_enum(void)
{
    ALCcontext *ctx = (ALCcontext *) IO_PTR();
    const ALenum param = IO_ENUM();
    const ALenum newval = IO_ENUM();
    if (!io_failure) visit_context_state_changed_enum(guserdata, ctx, param, newval);
}

static void decode_context_state_changed_float(void)
{
    ALCcontext *ctx = (ALCcontext *) IO_PTR();
    const ALenum param = IO_ENUM();
    const ALfloat newval = IO_FLOAT();
    if (!io_failure) visit_context_state_changed_float(guserdata, ctx, param, newval);
}

static void decode_context_state_changed_string(void)
{
    ALCcontext *ctx = (ALCcontext *) IO_PTR();
    const ALenum param = IO_ENUM();
    const char *newval = IO_STRING();
    if (!io_failure) visit_context_state_changed_string(guserdata, ctx, param, newval);
}

static void decode_listener_state_changed_floatv(void)
{
    ALCcontext *ctx = (ALCcontext *) IO_PTR();
    const ALenum param = IO_ENUM();
    const uint32 numfloats = IO_UINT32();
    ALfloat *values = (ALfloat *) get_ioblob(numfloats * sizeof (ALfloat));
    uint32 i;

    for (i = 0; i < numfloats; i++) {
        values[i] = IO_FLOAT();
    }

    if (!io_failure) visit_listener_state_changed_floatv(guserdata, ctx, param, numfloats, values);
}

static void decode_source_state_changed_bool(void)
{
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALboolean newval = IO_BOOLEAN();
    if (!io_failure) visit_source_state_changed_bool(guserdata, name, param, newval);
}

static void decode_source_state_changed_enum(void)
{
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALenum newval = IO_ENUM();
    if (!io_failure) visit_source_state_changed_enum(guserdata, name, param, newval);
}

static void decode_source_state_changed_int(void)
{
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALint newval = IO_INT32();
    if (!io_failure) visit_source_state_changed_int(guserdata, name, param, newval);
}

static void decode_source_state_changed_uint(void)
{
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALuint newval = IO_UINT32();
    if (!io_failure) visit_source_state_changed_uint(guserdata, name, param, newval);
}

static void decode_source_state_changed_float(void)
{
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALfloat newval = IO_FLOAT();
    if (!io_failure) visit_source_state_changed_float(guserdata, name, param, newval);
}

static void decode_source_state_changed_float3(void)
{
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALfloat newval1 = IO_FLOAT();
    const ALfloat newval2 = IO_FLOAT();
    const ALfloat newval3 = IO_FLOAT();
    if (!io_failure) visit_source_state_changed_float3(guserdata, name, param, newval1, newval2, newval3);
}

static void decode_buffer_state_changed_int(void)
{
    const ALuint name = IO_UINT32();
    const ALenum param = IO_ENUM();
    const ALint newval = IO_INT32();
    if (!io_failure) visit_buffer_state_changed_int(guserdata, name, param, newval);
}

static void decode_eos(void)
{
    const uint32 ticks = IO_UINT32();
    if (!io_failure) visit_eos(guserdata, AL_TRUE, ticks);
}

// !!! FIXME: this has some globals, so it's not thread safe (you can't run
// !!! FIXME:  two logs on two threads at once). But you can run two logs
// !!! FIXME:  serially, fwiw. I think.
int process_tracelog(const char *fname, void *userdata)
{
    int retval = 1;
    int eos = 0;
    off_t fdoffset = 0;
    off_t fdsize = 0;

    if (!init_altrace_playback(fname)) {
        return 0;
    }

    fdoffset = lseek(logfd, 0, SEEK_CUR);
    fdsize = lseek(logfd, 0, SEEK_END);
    if ((lseek(logfd, fdoffset, SEEK_SET) == -1) || (fdoffset == -1) || (fdsize == -1)) {
        fprintf(stderr, "%s: Failed to seek in file: %s\n", GAppName, strerror(errno));
        io_failure = 1;
    }

    guserdata = userdata;
    trace_scope = 0;

    while (!eos) {
        if (!io_failure) {
            fdoffset = lseek(logfd, 0, SEEK_CUR);
            if (fdoffset == -1) {
                fprintf(stderr, "%s: Failed to get current file offset: %s\n", GAppName, strerror(errno));
                io_failure = 1;
            }
        }

        if (io_failure) {
            visit_eos(guserdata, AL_FALSE, 0);
            retval = 0;
            eos = 1;
            break;
        }

        if (!visit_progress(guserdata, fdoffset, fdsize)) {
            fprintf(stderr, "%s: Application cancelled file processing!\n", GAppName);
            visit_eos(guserdata, AL_FALSE, 0);
            retval = -1;
            eos = 1;
            break;
        }

        switch (IO_EVENTENUM()) {
            #define ENTRYPOINT(ret,name,params,args,numargs,visitparams,visitargs) case ALEE_##name: decode_##name(); break;
            #include "altrace_entrypoints.h"

            case ALEE_NEW_CALLSTACK_SYMS:
                decode_callstack_syms_event();
                break;

            case ALEE_ALERROR_TRIGGERED:
                decode_al_error_event();
                break;

            case ALEE_ALCERROR_TRIGGERED:
                decode_alc_error_event();
                break;

            case ALEE_DEVICE_STATE_CHANGED_INT:
                decode_device_state_changed_int();
                break;

            case ALEE_CONTEXT_STATE_CHANGED_ENUM:
                decode_context_state_changed_enum();
                break;

            case ALEE_CONTEXT_STATE_CHANGED_FLOAT:
                decode_context_state_changed_float();
                break;

            case ALEE_CONTEXT_STATE_CHANGED_STRING:
                decode_context_state_changed_string();
                break;

            case ALEE_LISTENER_STATE_CHANGED_FLOATV:
                decode_listener_state_changed_floatv();
                break;

            case ALEE_SOURCE_STATE_CHANGED_BOOL:
                decode_source_state_changed_bool();
                break;

            case ALEE_SOURCE_STATE_CHANGED_ENUM:
                decode_source_state_changed_enum();
                break;

            case ALEE_SOURCE_STATE_CHANGED_INT:
                decode_source_state_changed_int();
                break;

            case ALEE_SOURCE_STATE_CHANGED_UINT:
                decode_source_state_changed_uint();
                break;

            case ALEE_SOURCE_STATE_CHANGED_FLOAT:
                decode_source_state_changed_float();
                break;

            case ALEE_SOURCE_STATE_CHANGED_FLOAT3:
                decode_source_state_changed_float3();
                break;

            case ALEE_BUFFER_STATE_CHANGED_INT:
                decode_buffer_state_changed_int();
                break;

            case ALEE_EOS:
                decode_eos();
                eos = 1;
                break;

            default:
                visit_eos(guserdata, AL_FALSE, 0);
                retval = 0;
                eos = 1;
                break;
        }
    }

    guserdata = NULL;
    quit_altrace_playback();

    return retval;
}

// end of altrace_playback.c ...

