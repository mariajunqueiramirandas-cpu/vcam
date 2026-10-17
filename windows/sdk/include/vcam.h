/*
 * vcam.h - feed frames to a vcam virtual camera from any application.
 *
 *     vcam_producer* cam = NULL;
 *     vcam_options options = VCAM_OPTIONS_INIT;      // camera 1, fit, no mirroring
 *     if (vcam_open(&options, &cam) == VCAM_OK) {
 *         for (;;) {
 *             vcam_send_packed(cam, VCAM_FORMAT_BGRA, width, height, pixels, 0);
 *             ...
 *         }
 *         vcam_close(cam);
 *     }
 *
 * Frames can be any size and any of the formats below; vcam scales and converts
 * them to whatever the application using the camera asked for (vcam_get_status
 * tells you, so you can render at exactly that size and skip the scaling).
 * Send at your own pace: the camera repeats the last frame when you are slower
 * than the application's frame rate and drops frames when you are faster.
 *
 * The camera must have been installed with `vcam install`. A producer that runs
 * elevated can start before any application opens the camera; otherwise frames
 * are held until one does. Only one producer can feed a camera at a time.
 *
 * All functions are thread-safe. Plain C ABI (no C++ runtime types), usable from
 * any language with a C FFI; see python/vcam.py.
 */
#ifndef VCAM_H
#define VCAM_H

#include <stdint.h>

#if defined(_WIN32)
#if defined(VCAM_BUILD_DLL)
#define VCAM_API __declspec(dllexport)
#elif defined(VCAM_STATIC)
#define VCAM_API
#else
#define VCAM_API __declspec(dllimport)
#endif
#define VCAM_CALL __cdecl
#else
#define VCAM_API
#define VCAM_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define VCAM_API_VERSION 1

typedef enum vcam_format {
    VCAM_FORMAT_BGRA = 1,  /* B,G,R,A bytes (alpha ignored) - Windows bitmaps, Direct3D B8G8R8A8 */
    VCAM_FORMAT_RGBA = 2,  /* R,G,B,A bytes */
    VCAM_FORMAT_BGR24 = 3, /* B,G,R bytes - OpenCV */
    VCAM_FORMAT_RGB24 = 4, /* R,G,B bytes - PIL, most numpy code */
    VCAM_FORMAT_GRAY8 = 5, /* one byte of luma */
    VCAM_FORMAT_NV12 = 6,  /* Y plane, then interleaved U/V at half resolution */
    VCAM_FORMAT_I420 = 7,  /* Y, U and V planes, chroma at half resolution */
    VCAM_FORMAT_YUY2 = 8,  /* packed 4:2:2, Y0 U Y1 V */
    VCAM_FORMAT_UYVY = 9   /* packed 4:2:2, U Y0 V Y1 */
} vcam_format;

typedef enum vcam_scale {
    VCAM_SCALE_FIT = 0,    /* keep the aspect ratio, pad with the background colour */
    VCAM_SCALE_FILL = 1,   /* keep the aspect ratio, crop what does not fit */
    VCAM_SCALE_STRETCH = 2 /* fill the frame, ignoring the aspect ratio */
} vcam_scale;

typedef enum vcam_colorspace {
    VCAM_COLORSPACE_BT601 = 0,      /* limited range, the default for YUV input */
    VCAM_COLORSPACE_BT709 = 1,      /* limited range */
    VCAM_COLORSPACE_BT601_FULL = 2,
    VCAM_COLORSPACE_BT709_FULL = 3
} vcam_colorspace;

typedef enum vcam_result {
    VCAM_OK = 0,
    VCAM_ERROR_INVALID_ARGUMENT = -1,
    VCAM_ERROR_BUSY = -2,          /* another producer is feeding this camera */
    VCAM_ERROR_NOT_INSTALLED = -3, /* the camera number is out of range or vcam is not installed */
    VCAM_ERROR_OUT_OF_MEMORY = -4,
    VCAM_ERROR_SYSTEM = -5,        /* an operating system call failed */
    VCAM_ERROR_TIMEOUT = -6
} vcam_result;

typedef struct vcam_producer vcam_producer;

typedef struct vcam_options {
    uint32_t struct_size;    /* sizeof(vcam_options) */
    int32_t camera;          /* which camera, 1-based */
    int32_t scale;           /* vcam_scale */
    int32_t mirror;          /* non-zero: flip left/right */
    int32_t flip;            /* non-zero: flip upside down */
    uint32_t background;     /* 0xRRGGBB padding for VCAM_SCALE_FIT */
} vcam_options;

#define VCAM_OPTIONS_INIT {sizeof(vcam_options), 1, VCAM_SCALE_FIT, 0, 0, 0x000000}

typedef struct vcam_frame {
    int32_t format;          /* vcam_format */
    int32_t width;
    int32_t height;
    const void* planes[3];   /* NV12 uses planes 0-1, I420 0-2, everything else plane 0 */
    int32_t pitches[3];      /* bytes per row; 0 means tightly packed; negative means bottom-up */
    int32_t colorspace;      /* vcam_colorspace of YUV input */
    int64_t timestamp;       /* capture time in 100 ns units of QueryPerformanceCounter, 0 = now */
} vcam_frame;

typedef struct vcam_status {
    uint32_t struct_size;      /* sizeof(vcam_status) */
    int32_t connected;         /* attached to the camera's shared memory */
    int32_t consumer_active;   /* an application is streaming from the camera right now */
    int32_t width;             /* what the camera delivers; 0 until an application asked */
    int32_t height;
    int32_t format;            /* vcam_format */
    int32_t fps_numerator;
    int32_t fps_denominator;
    uint64_t frames_sent;      /* frames this producer wrote */
    uint64_t frames_delivered; /* new frames the camera handed to applications */
    uint64_t frames_repeated;  /* times the camera repeated a frame because no new one was ready */
} vcam_status;

/* Connects to camera options->camera (NULL: defaults). */
VCAM_API int VCAM_CALL vcam_open(const vcam_options* options, vcam_producer** producer);

/* Sends one frame. Returns quickly; the frame is converted and published before it returns. */
VCAM_API int VCAM_CALL vcam_send(vcam_producer* producer, const vcam_frame* frame);

/* Shorthand for single-plane formats (and NV12/I420 laid out contiguously with pitch = width). */
VCAM_API int VCAM_CALL vcam_send_packed(vcam_producer* producer, int32_t format, int32_t width, int32_t height,
                                        const void* data, int32_t pitch);

VCAM_API int VCAM_CALL vcam_get_status(vcam_producer* producer, vcam_status* status);

/* Waits up to timeout_ms (-1: forever) for an application to start streaming. VCAM_OK or VCAM_ERROR_TIMEOUT. */
VCAM_API int VCAM_CALL vcam_wait_for_consumer(vcam_producer* producer, int32_t timeout_ms);

VCAM_API void VCAM_CALL vcam_close(vcam_producer* producer);

VCAM_API const char* VCAM_CALL vcam_error_string(int result);
VCAM_API int VCAM_CALL vcam_api_version(void);

#ifdef __cplusplus
}
#endif

#endif /* VCAM_H */
