/*
** Copyright 2008, Google Inc.
** Copyright (c) 2011-2012 The Linux Foundation. All rights reserved.
** Copyright (C) 2014 Rudolf Tammekivi <rtammekivi@gmail.com>
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "QualcommCameraHardware"

#include "QualcommCameraHardware.h"
#include <QComOMXMetadata.h>

#include <cutils/properties.h>
#include <math.h>

extern "C" {
#include <fcntl.h>

#define DEFAULT_PICTURE_WIDTH  640
#define DEFAULT_PICTURE_HEIGHT 480
#define DEFAULT_PICTURE_WIDTH_3D 1920
#define DEFAULT_PICTURE_HEIGHT_3D 1080
#define FOCUS_AREA_INIT "(-1000,-1000,1000,1000,1000)"

#define NOT_FOUND -1
// Number of video buffers held by kernal (initially 1,2 &3)
#define ACTIVE_VIDEO_BUFFERS 3
#define ACTIVE_PREVIEW_BUFFERS 3
#define ACTIVE_ZSL_BUFFERS 3
#define APP_ORIENTATION 90
#define HDR_HAL_FRAME 2

#define FLASH_AUTO 24
#define FLASH_SNAP 32

#define FLOOR16(X) ((X) & 0xFFF0)

#ifdef DLOPEN_LIBMMCAMERA
#include <dlfcn.h>
static void *libmmcamera;
static void (*LINK_cam_frame)(void *data);
static void (*LINK_wait_cam_frame_thread_ready)(void);
static void (*LINK_cam_frame_set_exit_flag)(int flag);
static void (*LINK_jpeg_encoder_join)(void);
static void (*LINK_camframe_terminate)(void);
static void (*LINK_camframe_add_frame)(cam_frame_type_t type, struct msm_frame *frame);
static void (*LINK_camframe_release_all_frames)(cam_frame_type_t type);

mm_camera_status_t (*LINK_mm_camera_init)(mm_camera_config *, mm_camera_notify *, mm_camera_ops *, uint8_t);
mm_camera_status_t (*LINK_mm_camera_deinit)(void);
mm_camera_status_t (*LINK_mm_camera_destroy)(void);
mm_camera_status_t (*LINK_mm_camera_exec)(void);
mm_camera_status_t (*LINK_mm_camera_get_camera_info)(mm_camera_info_t *p_cam_info, int *p_num_cameras);

// callbacks
static void (*LINK_cancel_liveshot)(void);
static int8_t (*LINK_set_liveshot_params)
    (uint32_t a_width, uint32_t a_height, exif_tags_info_t *a_exif_data,
    int a_exif_numEntries, uint8_t *a_out_buffer, uint32_t a_outbuffer_size);
static void (*LINK_set_liveshot_frame)(struct msm_frame *liveshot_frame);
#else
#define LINK_cam_frame cam_frame
#define LINK_wait_cam_frame_thread_ready wait_cam_frame_thread_ready
#define LINK_cam_frame cam_frame_set_exit_flag
#define LINK_jpeg_encoder_join jpeg_encoder_join
#define LINK_camframe_terminate camframe_terminate
#define LINK_mm_camera_init mm_camera_config_init
#define LINK_mm_camera_deinit mm_camera_config_deinit
#define LINK_mm_camera_destroy mm_camera_config_destroy
#define LINK_mm_camera_exec mm_camera_exec
#define LINK_camframe_add_frame camframe_add_frame
#define LINK_camframe_release_all_frames camframe_release_all_frames
#define LINK_mm_camera_get_camera_info mm_camera_get_camera_info

extern void (*mmcamera_camframe_callback)(struct msm_frame *frame);
extern void (*mmcamera_camstats_callback)(camstats_type stype, camera_preview_histogram_info *histinfo);
extern void (*mmcamera_jpegfragment_callback)(uint8_t *buff_ptr, uint32_t buff_size);
extern void (*mmcamera_jpeg_callback)(jpeg_event_t status);
extern void (*mmcamera_liveshot_callback)(liveshot_status status, uint32_t jpeg_size);
#define LINK_set_liveshot_params set_liveshot_params
#define LINK_set_liveshot_frame set_liveshot_frame
#endif

} // extern "C"

//Default to VGA
#define DEFAULT_PREVIEW_WIDTH 640
#define DEFAULT_PREVIEW_HEIGHT 480
#define DEFAULT_PREVIEW_WIDTH_3D 1280
#define DEFAULT_PREVIEW_HEIGHT_3D 720

//Default FPS
#define MINIMUM_FPS 5
#define MAXIMUM_FPS 31
#define DEFAULT_FPS MAXIMUM_FPS
#define DEFAULT_FIXED_FPS_VALUE 30
/*
 * Modifying preview size requires modification
 * in bitmasks for boardproperties
 */
static uint32_t PREVIEW_SIZE_COUNT;
static uint32_t HFR_SIZE_COUNT;

static const board_property boardProperties[] = {
    { TARGET_MSM7625, 0x00000fff, false, false, false },
    { TARGET_MSM7625A, 0x00000fff, false, false, false },
    { TARGET_MSM7627, 0x000006ff, false, false, false },
    { TARGET_MSM7627A, 0x000006ff, false, false, false },
    { TARGET_MSM7630, 0x00000fff, true, true, false },
    { TARGET_MSM8660, 0x00001fff, true, true, false },
    { TARGET_QSD8250, 0x00000fff, false, false, false }
};

/*       TODO
 * Ideally this should be a populated by lower layers.
 * But currently this is no API to do that at lower layer.
 * Hence populating with default sizes for now. This needs
 * to be changed once the API is supported.
 */
//sorted on column basis
static const struct camera_size_type zsl_picture_sizes[] = {
    { 1024, 768 },   // 1MP XGA
    { 800, 600 },    // SVGA
    { 800, 480 },    // WVGA
    { 640, 480 },    // VGA
    { 352, 288 },    // CIF
    { 320, 240 },    // QVGA
    { 176, 144 }     // QCIF
};

static const struct camera_size_type for_3D_picture_sizes[] = {
    { 1920, 1080 },
};

static int data_counter = 0;
static int sensor_rotation = 0;
static int record_flag = 0;
static camera_size_type *picture_sizes;
static camera_size_type *preview_sizes;
static camera_size_type *hfr_sizes;
static unsigned int PICTURE_SIZE_COUNT;
static const camera_size_type *picture_sizes_ptr;
static int supportedPictureSizesCount;
static liveshotState liveshot_state = LIVESHOT_DONE;

#define Q12 4096

static const target_map targetList[] = {
    { "msm7625", TARGET_MSM7625 },
    { "msm7625a", TARGET_MSM7625A },
    { "msm7x27", TARGET_MSM7627 },
    { "msm7x27a", TARGET_MSM7627A },
    { "qsd8250", TARGET_QSD8250 },
    { "msm7x30", TARGET_MSM7630 },
    { "msm8660", TARGET_MSM8660 }
};
static targetType mCurrentTarget = TARGET_MAX;

typedef struct {
    uint32_t aspect_ratio;
    uint32_t width;
    uint32_t height;
} thumbnail_size_type;

static thumbnail_size_type thumbnail_sizes[] = {
    { 7281, 512, 288 }, //1.777778
    { 6826, 480, 288 }, //1.666667
    { 6808, 256, 154 }, //1.662337
    { 6144, 432, 288 }, //1.5
    { 5461, 512, 384 }, //1.333333
    { 5006, 352, 288 }, //1.222222
};
#define THUMBNAIL_SIZE_COUNT (sizeof(thumbnail_sizes) / sizeof(thumbnail_size_type))
#define DEFAULT_THUMBNAIL_SETTING 4
#define THUMBNAIL_WIDTH_STR "512"
#define THUMBNAIL_HEIGHT_STR "384"
#define THUMBNAIL_SMALL_HEIGHT 144
static camera_size_type jpeg_thumbnail_sizes[] = {
    { 512, 288 },
    { 480, 288 },
    { 432, 288 },
    { 512, 384 },
    { 352, 288 },
    { 0,0 }
};
#define JPEG_THUMBNAIL_SIZE_COUNT (sizeof(jpeg_thumbnail_sizes) / sizeof(camera_size_type))

static android::FPSRange FpsRangesSupported[] = {
    android::FPSRange(MINIMUM_FPS*1000,MAXIMUM_FPS*1000)
};
#define FPS_RANGES_SUPPORTED_COUNT (sizeof(FpsRangesSupported) / sizeof(FpsRangesSupported[0]))

static int attr_lookup(const str_map arr[], int len, const char *name)
{
    if (name) {
        for (int i = 0; i < len; i++) {
            if (!strcmp(arr[i].desc, name))
                return arr[i].val;
        }
    }
    return NOT_FOUND;
}

static int exif_table_numEntries = 0;
#define MAX_EXIF_TABLE_ENTRIES 14
exif_tags_info_t exif_data[MAX_EXIF_TABLE_ENTRIES];
static android_native_rect_t zoomCropInfo;
#define RECORD_BUFFERS 9
#define RECORD_BUFFERS_8x50 8
static int kRecordBufferCount;
/* controls whether VPE is avialable for the target
 * under consideration.
 * 1: VPE support is available
 * 0: VPE support is not available (default)
 */
static bool mVpeEnabled = false;
static cam_frame_start_parms camframeParams;

static int HAL_numOfCameras;
static mm_camera_info_t HAL_cameraInfo[MSM_MAX_CAMERA_SENSORS];
static int HAL_currentCameraId;
static camera_mode_t HAL_currentCameraMode;
static mm_camera_config mCfgControl;
static bool mCameraOpen;

static int HAL_currentSnapshotMode;
static int previewWidthToNativeZoom;
static int previewHeightToNativeZoom;
#define CAMERA_SNAPSHOT_NONZSL 0x04
#define CAMERA_SNAPSHOT_ZSL 0x08

static const int PICTURE_FORMAT_JPEG = 1;
static const int PICTURE_FORMAT_RAW = 2;

static const str_map whitebalance[] = {
    { CameraParameters::WHITE_BALANCE_AUTO,            CAMERA_WB_AUTO },
    { CameraParameters::WHITE_BALANCE_INCANDESCENT,    CAMERA_WB_INCANDESCENT },
    { CameraParameters::WHITE_BALANCE_FLUORESCENT,     CAMERA_WB_FLUORESCENT },
    { CameraParameters::WHITE_BALANCE_DAYLIGHT,        CAMERA_WB_DAYLIGHT },
    { CameraParameters::WHITE_BALANCE_CLOUDY_DAYLIGHT, CAMERA_WB_CLOUDY_DAYLIGHT }
};

static const str_map effects[] = {
    { CameraParameters::EFFECT_NONE,       CAMERA_EFFECT_OFF },
    { CameraParameters::EFFECT_MONO,       CAMERA_EFFECT_MONO },
    { CameraParameters::EFFECT_NEGATIVE,   CAMERA_EFFECT_NEGATIVE },
    { CameraParameters::EFFECT_SOLARIZE,   CAMERA_EFFECT_SOLARIZE },
    { CameraParameters::EFFECT_SEPIA,      CAMERA_EFFECT_SEPIA },
    { CameraParameters::EFFECT_POSTERIZE,  CAMERA_EFFECT_POSTERIZE },
    { CameraParameters::EFFECT_WHITEBOARD, CAMERA_EFFECT_WHITEBOARD },
    { CameraParameters::EFFECT_BLACKBOARD, CAMERA_EFFECT_BLACKBOARD },
    { CameraParameters::EFFECT_AQUA,       CAMERA_EFFECT_AQUA }
};

static const str_map autoexposure[] = {
    { CameraParameters::AUTO_EXPOSURE_FRAME_AVG,  CAMERA_AEC_FRAME_AVERAGE },
    { CameraParameters::AUTO_EXPOSURE_CENTER_WEIGHTED, CAMERA_AEC_CENTER_WEIGHTED },
    { CameraParameters::AUTO_EXPOSURE_SPOT_METERING, CAMERA_AEC_SPOT_METERING }
};

static const str_map antibanding[] = {
    { CameraParameters::ANTIBANDING_OFF,  CAMERA_ANTIBANDING_OFF },
    { CameraParameters::ANTIBANDING_50HZ, CAMERA_ANTIBANDING_50HZ },
    { CameraParameters::ANTIBANDING_60HZ, CAMERA_ANTIBANDING_60HZ },
    { CameraParameters::ANTIBANDING_AUTO, CAMERA_ANTIBANDING_AUTO }
};

static const str_map antibanding_3D[] = {
    { CameraParameters::ANTIBANDING_OFF,  CAMERA_ANTIBANDING_OFF },
    { CameraParameters::ANTIBANDING_50HZ, CAMERA_ANTIBANDING_50HZ },
    { CameraParameters::ANTIBANDING_60HZ, CAMERA_ANTIBANDING_60HZ }
};

static const str_map scenemode[] = {
    { CameraParameters::SCENE_MODE_AUTO,           CAMERA_BESTSHOT_OFF },
    { CameraParameters::SCENE_MODE_ASD,            CAMERA_BESTSHOT_AUTO },
    { CameraParameters::SCENE_MODE_ACTION,         CAMERA_BESTSHOT_ACTION },
    { CameraParameters::SCENE_MODE_PORTRAIT,       CAMERA_BESTSHOT_PORTRAIT },
    { CameraParameters::SCENE_MODE_LANDSCAPE,      CAMERA_BESTSHOT_LANDSCAPE },
    { CameraParameters::SCENE_MODE_NIGHT,          CAMERA_BESTSHOT_NIGHT },
    { CameraParameters::SCENE_MODE_NIGHT_PORTRAIT, CAMERA_BESTSHOT_NIGHT_PORTRAIT },
    { CameraParameters::SCENE_MODE_THEATRE,        CAMERA_BESTSHOT_THEATRE },
    { CameraParameters::SCENE_MODE_BEACH,          CAMERA_BESTSHOT_BEACH },
    { CameraParameters::SCENE_MODE_SNOW,           CAMERA_BESTSHOT_SNOW },
    { CameraParameters::SCENE_MODE_SUNSET,         CAMERA_BESTSHOT_SUNSET },
    { CameraParameters::SCENE_MODE_STEADYPHOTO,    CAMERA_BESTSHOT_ANTISHAKE },
    { CameraParameters::SCENE_MODE_FIREWORKS ,     CAMERA_BESTSHOT_FIREWORKS },
    { CameraParameters::SCENE_MODE_SPORTS ,        CAMERA_BESTSHOT_SPORTS },
    { CameraParameters::SCENE_MODE_PARTY,          CAMERA_BESTSHOT_PARTY },
    { CameraParameters::SCENE_MODE_CANDLELIGHT,    CAMERA_BESTSHOT_CANDLELIGHT },
    { CameraParameters::SCENE_MODE_BACKLIGHT,      CAMERA_BESTSHOT_BACKLIGHT },
    { CameraParameters::SCENE_MODE_FLOWERS,        CAMERA_BESTSHOT_FLOWERS },
    { CameraParameters::SCENE_MODE_AR,             CAMERA_BESTSHOT_AR },
};

static const str_map scenedetect[] = {
    { CameraParameters::SCENE_DETECT_OFF, FALSE  },
    { CameraParameters::SCENE_DETECT_ON, TRUE },
};

static const str_map flash[] = {
    { CameraParameters::FLASH_MODE_OFF,  LED_MODE_OFF },
    { CameraParameters::FLASH_MODE_AUTO, LED_MODE_AUTO },
    { CameraParameters::FLASH_MODE_ON, LED_MODE_ON },
    { CameraParameters::FLASH_MODE_TORCH, LED_MODE_TORCH}
};

static const str_map iso[] = {
    { CameraParameters::ISO_AUTO,  CAMERA_ISO_AUTO},
    { CameraParameters::ISO_HJR,   CAMERA_ISO_DEBLUR},
    { CameraParameters::ISO_100,   CAMERA_ISO_100},
    { CameraParameters::ISO_200,   CAMERA_ISO_200},
    { CameraParameters::ISO_400,   CAMERA_ISO_400},
    { CameraParameters::ISO_800,   CAMERA_ISO_800 },
    { CameraParameters::ISO_1600,  CAMERA_ISO_1600 }
};

static const str_map iso_3D[] = {
    { CameraParameters::ISO_AUTO,  CAMERA_ISO_AUTO},
    { CameraParameters::ISO_100,   CAMERA_ISO_100},
    { CameraParameters::ISO_200,   CAMERA_ISO_200},
    { CameraParameters::ISO_400,   CAMERA_ISO_400},
    { CameraParameters::ISO_800,   CAMERA_ISO_800 },
    { CameraParameters::ISO_1600,  CAMERA_ISO_1600 }
};

#define DONT_CARE AF_MODE_MAX
static const str_map focus_modes[] = {
    { CameraParameters::FOCUS_MODE_AUTO,     AF_MODE_AUTO},
    { CameraParameters::FOCUS_MODE_INFINITY, DONT_CARE },
    { CameraParameters::FOCUS_MODE_NORMAL,   AF_MODE_NORMAL },
    { CameraParameters::FOCUS_MODE_MACRO,    AF_MODE_MACRO },
    { CameraParameters::FOCUS_MODE_CONTINUOUS_VIDEO, AF_MODE_CAF_VID }
};

static const str_map lensshade[] = {
    { CameraParameters::LENSSHADE_ENABLE, TRUE },
    { CameraParameters::LENSSHADE_DISABLE, FALSE }
};

static const str_map hfr[] = {
    { CameraParameters::VIDEO_HFR_OFF, CAMERA_HFR_MODE_OFF },
    { CameraParameters::VIDEO_HFR_2X, CAMERA_HFR_MODE_60FPS },
    { CameraParameters::VIDEO_HFR_3X, CAMERA_HFR_MODE_90FPS },
    { CameraParameters::VIDEO_HFR_4X, CAMERA_HFR_MODE_120FPS },
};

static const str_map mce[] = {
    { CameraParameters::MCE_ENABLE, TRUE },
    { CameraParameters::MCE_DISABLE, FALSE }
};

static const str_map hdr[] = {
    { CameraParameters::HDR_ENABLE, TRUE },
    { CameraParameters::HDR_DISABLE, FALSE }
};

static const str_map histogram[] = {
    { CameraParameters::HISTOGRAM_ENABLE, TRUE },
    { CameraParameters::HISTOGRAM_DISABLE, FALSE }
};

static const str_map skinToneEnhancement[] = {
    { CameraParameters::SKIN_TONE_ENHANCEMENT_ENABLE, TRUE },
    { CameraParameters::SKIN_TONE_ENHANCEMENT_DISABLE, FALSE }
};

static const str_map denoise[] = {
    { CameraParameters::DENOISE_OFF, FALSE },
    { CameraParameters::DENOISE_ON, TRUE }
};

static const str_map selectable_zone_af[] = {
    { CameraParameters::SELECTABLE_ZONE_AF_AUTO,  AUTO },
    { CameraParameters::SELECTABLE_ZONE_AF_SPOT_METERING, SPOT },
    { CameraParameters::SELECTABLE_ZONE_AF_CENTER_WEIGHTED, CENTER_WEIGHTED },
    { CameraParameters::SELECTABLE_ZONE_AF_FRAME_AVERAGE, AVERAGE }
};

static const str_map facedetection[] = {
    { CameraParameters::FACE_DETECTION_OFF, FALSE },
    { CameraParameters::FACE_DETECTION_ON, TRUE }
};

static const str_map touchafaec[] = {
    { CameraParameters::TOUCH_AF_AEC_OFF, FALSE },
    { CameraParameters::TOUCH_AF_AEC_ON, TRUE }
};

static const str_map redeye_reduction[] = {
    { CameraParameters::REDEYE_REDUCTION_ENABLE, TRUE },
    { CameraParameters::REDEYE_REDUCTION_DISABLE, FALSE }
};

static const str_map zsl_modes[] = {
    { CameraParameters::ZSL_OFF, FALSE  },
//  { CameraParameters::ZSL_ON, TRUE },
};

#define DONT_CARE_COORDINATE -1
#define CAMERA_HISTOGRAM_ENABLE 1
#define CAMERA_HISTOGRAM_DISABLE 0
#define HISTOGRAM_STATS_SIZE 257

#define EXPOSURE_COMPENSATION_MAXIMUM_NUMERATOR 12
#define EXPOSURE_COMPENSATION_MINIMUM_NUMERATOR -12
#define EXPOSURE_COMPENSATION_DEFAULT_NUMERATOR 0
#define EXPOSURE_COMPENSATION_DENOMINATOR 6
#define EXPOSURE_COMPENSATION_STEP (1/EXPOSURE_COMPENSATION_DENOMINATOR)

static const str_map picture_formats[] = {
    { CameraParameters::PIXEL_FORMAT_JPEG, PICTURE_FORMAT_JPEG },
    { CameraParameters::PIXEL_FORMAT_RAW, PICTURE_FORMAT_RAW }
};

static const str_map recording_Hints[] = {
    { "false", FALSE },
    { "true",  TRUE }
};

static const str_map picture_formats_zsl[] = {
    { CameraParameters::PIXEL_FORMAT_JPEG, PICTURE_FORMAT_JPEG }
};

static const str_map frame_rate_modes[] = {
    { CameraParameters::KEY_PREVIEW_FRAME_RATE_AUTO_MODE, FPS_MODE_AUTO },
    { CameraParameters::KEY_PREVIEW_FRAME_RATE_FIXED_MODE, FPS_MODE_FIXED }
};

static int mPreviewFormat;
static const str_map preview_formats[] = {
    {CameraParameters::PIXEL_FORMAT_YUV420SP, CAMERA_YUV_420_NV21 },
    {CameraParameters::PIXEL_FORMAT_YUV420SP_ADRENO, CAMERA_YUV_420_NV21_ADRENO },
//  {CameraParameters::PIXEL_FORMAT_YV12, CAMERA_YUV_420_YV12 }
};
static const str_map preview_formats1[] = {
    { CameraParameters::PIXEL_FORMAT_YUV420SP, CAMERA_YUV_420_NV21 },
    { CameraParameters::PIXEL_FORMAT_YV12, CAMERA_YUV_420_YV12 }
};

static const str_map app_preview_formats[] = {
    { CameraParameters::PIXEL_FORMAT_YUV420SP, HAL_PIXEL_FORMAT_YCrCb_420_SP }, //nv21
//  { CameraParameters::PIXEL_FORMAT_YUV420SP_ADRENO, HAL_PIXEL_FORMAT_YCrCb_420_SP_ADRENO }, //nv21_adreno
    { CameraParameters::PIXEL_FORMAT_YUV420P, HAL_PIXEL_FORMAT_YV12 }, //YV12
};

static bool parameter_string_initialized = false;
static String8 preview_size_values;
static String8 hfr_size_values;
static String8 picture_size_values;
static String8 fps_ranges_supported_values;
static String8 jpeg_thumbnail_size_values;
static String8 antibanding_values;
static String8 effect_values;
static String8 autoexposure_values;
static String8 whitebalance_values;
static String8 flash_values;
static String8 focus_mode_values;
static String8 iso_values;
static String8 lensshade_values;
static String8 mce_values;
static String8 hdr_values;
static String8 histogram_values;
static String8 skinToneEnhancement_values;
static String8 touchafaec_values;
static String8 picture_format_values;
static String8 scenemode_values;
static String8 denoise_values;
static String8 zoom_ratio_values;
static String8 preview_frame_rate_values;
static String8 frame_rate_mode_values;
static String8 scenedetect_values;
static String8 preview_format_values;
static String8 selectable_zone_af_values;
static String8 facedetection_values;
static String8 hfr_values;
static String8 redeye_reduction_values;
static String8 zsl_values;

static mm_camera_notify mCamNotify;
static mm_camera_ops mCamOps;
static mm_camera_buffer_t mEncodeOutputBuffer[MAX_SNAPSHOT_BUFFERS];
static encode_params_t mImageEncodeParms;
static capture_params_t mImageCaptureParms;
static raw_capture_params_t mRawCaptureParms;
static zsl_capture_params_t mZslCaptureParms;
static zsl_params_t mZslParms;

static String8 create_sizes_str(const camera_size_type *sizes, int len) {
    String8 str;
    char buffer[32];

    if (len > 0) {
        sprintf(buffer, "%dx%d", sizes[0].width, sizes[0].height);
        str.append(buffer);
    }
    for (int i = 1; i < len; i++) {
        sprintf(buffer, ",%dx%d", sizes[i].width, sizes[i].height);
        str.append(buffer);
    }
    return str;
}

static String8 create_fps_str(const android:: FPSRange* fps, int len) {
    String8 str;
    char buffer[32];

    if (len > 0) {
        sprintf(buffer, "(%d,%d)", fps[0].minFPS, fps[0].maxFPS);
        str.append(buffer);
    }
    for (int i = 1; i < len; i++) {
        sprintf(buffer, ",(%d,%d)", fps[i].minFPS, fps[i].maxFPS);
        str.append(buffer);
    }
    return str;
}

static String8 create_values_str(const str_map *values, int len) {
    String8 str;

    if (len > 0) {
        str.append(values[0].desc);
    }
    for (int i = 1; i < len; i++) {
        str.append(",");
        str.append(values[i].desc);
    }
    return str;
}

static String8 create_str(int16_t *arr, int length)
{
    String8 str;
    char buffer[32];

    if (length > 0) {
        snprintf(buffer, sizeof(buffer), "%d", arr[0]);
        str.append(buffer);
    }

    for (int i =1;i<length;i++) {
        snprintf(buffer, sizeof(buffer), ",%d",arr[i]);
        str.append(buffer);
    }
    return str;
}

static String8 create_values_range_str(int min, int max)
{
    String8 str;
    char buffer[32];

    if (min <= max) {
        snprintf(buffer, sizeof(buffer), "%d", min);
        str.append(buffer);

        for (int i = min + 1; i <= max; i++) {
            snprintf(buffer, sizeof(buffer), ",%d", i);
            str.append(buffer);
        }
    }
    return str;
}

static struct fifo_queue g_busy_frame_queue =
    { 0, 0, 0, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, (char *)"video_busy_q" };

static void cam_frame_wait_video(void)
{
    ALOGV("cam_frame_wait_video E ");
    if (g_busy_frame_queue.num_of_frames <= 0) {
        pthread_cond_wait(&(g_busy_frame_queue.wait), &(g_busy_frame_queue.mut));
    }
    ALOGV("cam_frame_wait_video X");
}

static void cam_frame_flush_video(void)
{
    ALOGV("cam_frame_flush_video: in n = %d\n", g_busy_frame_queue.num_of_frames);
    pthread_mutex_lock(&(g_busy_frame_queue.mut));

    while (g_busy_frame_queue.front) {
        //dequeue from the busy queue
        struct fifo_node *node  = dequeue (&g_busy_frame_queue);
        if (node)
            free(node);

        ALOGV("cam_frame_flush_video: node \n");
    }
    pthread_mutex_unlock(&(g_busy_frame_queue.mut));
    ALOGV("cam_frame_flush_video: out n = %d\n", g_busy_frame_queue.num_of_frames);
}

static struct msm_frame *cam_frame_get_video(void)
{
    struct msm_frame *p = NULL;
    ALOGV("cam_frame_get_video... in\n");
    ALOGV("cam_frame_get_video... got lock\n");
    if (g_busy_frame_queue.front) {
        //dequeue
        struct fifo_node *node  = dequeue (&g_busy_frame_queue);
        if (node) {
            p = (struct msm_frame *)node->f;
            free (node);
        }
        ALOGV("cam_frame_get_video... out = %lx\n", p->buffer);
    }
    return p;
}

// Parse string like "(1, 2, 3, 4, ..., N)"
// num is pointer to an allocated array of size N
static int parseNDimVector_HAL(const char *str, int *num, int N, char delim = ',')
{
    char *start, *end;
    if (num == NULL) {
        ALOGE("Invalid output array (num == NULL)");
        return -1;
    }
    //check if string starts and ends with parantheses
    if (str[0] != '(' || str[strlen(str)-1] != ')') {
        ALOGE("Invalid format of string %s, valid format is (n1, n2, n3, n4 ...)", str);
        return -1;
    }
    start = (char*) str;
    start++;
    for (int i = 0; i < N; i++) {
        *(num+i) = (int) strtol(start, &end, 10);
        if (*end != delim && i < (N-1)) {
            ALOGE("Cannot find delimeter '%c' in string \"%s\". end = %c", delim, str, *end);
            return -1;
        }
        start = end + 1;
    }
    return 0;
}

static int countChar(const char *str, char ch)
{
    int noOfChar = 0;

    for (int i = 0; str[i] != '\0'; i++) {
        if (str[i] == ch)
            noOfChar = noOfChar + 1;
    }

    return noOfChar;
}
static int checkAreaParameters(const char *str)
{
    int areaValues[6];
    int left, right, top, bottom, weight;

    if (countChar(str, ',') > 4) {
        ALOGE("%s: No of area parameters exceeding the expected number %s", __FUNCTION__, str);
        return -1;
    }

    if (parseNDimVector_HAL(str, areaValues, 5) != 0) {
        ALOGE("%s: Failed to parse the input string %s", __FUNCTION__, str);
        return -1;
    }

    ALOGV("%s: Area values are %d,%d,%d,%d,%d", __FUNCTION__,
        areaValues[0], areaValues[1], areaValues[2], areaValues[3], areaValues[4]);

    left = areaValues[0];
    top = areaValues[1];
    right = areaValues[2];
    bottom = areaValues[3];
    weight = areaValues[4];

    // left should >= -1000
    if (!(left >= -1000))
        return -1;
    // top should >= -1000
    if (!(top >= -1000))
        return -1;
    // right should <= 1000
    if (!(right <= 1000))
        return -1;
    // bottom should <= 1000
    if (!(bottom <= 1000))
        return -1;
    // weight should >= 1
    // weight should <= 1000
    if (!((1 <= weight) && (weight <= 1000)))
        return -1;
    // left should < right
    if (!(left < right))
        return -1;
    // top should < bottom
    if (!(top < bottom))
        return -1;

    return 0;
}

static void cam_frame_post_video(struct msm_frame *p)
{
    if (!p) {
        ALOGE("post video , buffer is null");
        return;
    }

    ALOGV("cam_frame_post_video... in = %x\n", (unsigned int)(p->buffer));
    pthread_mutex_lock(&(g_busy_frame_queue.mut));
    ALOGV("post_video got lock. q count before enQ %d", g_busy_frame_queue.num_of_frames);
    //enqueue to busy queue
    struct fifo_node *node = (struct fifo_node *)malloc(sizeof(struct fifo_node));
    if (node) {
        ALOGV(" post video , enqueing in busy queue");
        node->f = p;
        node->next = NULL;
        enqueue(&g_busy_frame_queue, node);
        ALOGV("post_video got lock. q count after enQ %d", g_busy_frame_queue.num_of_frames);
    } else {
        ALOGE("cam_frame_post_video error... out of memory\n");
    }

    pthread_mutex_unlock(&(g_busy_frame_queue.mut));
    pthread_cond_signal(&(g_busy_frame_queue.wait));

    ALOGV("cam_frame_post_video... out = %lx\n", p->buffer);
}

QualcommCameraHardware::FrameQueue::FrameQueue()
{
    mInitialized = false;
}

QualcommCameraHardware::FrameQueue::~FrameQueue()
{
    flush();
}

void QualcommCameraHardware::FrameQueue::init()
{
    Mutex::Autolock l(&mQueueLock);
    mInitialized = true;
    mQueueWait.signal();
}

void QualcommCameraHardware::FrameQueue::deinit()
{
    Mutex::Autolock l(&mQueueLock);
    mInitialized = false;
    mQueueWait.signal();
}

bool QualcommCameraHardware::FrameQueue::isInitialized()
{
    Mutex::Autolock l(&mQueueLock);
    return mInitialized;
}

bool QualcommCameraHardware::FrameQueue::add(struct msm_frame *element)
{
    Mutex::Autolock l(&mQueueLock);
    if (mInitialized == false)
        return false;

    mContainer.add(element);
    mQueueWait.signal();
    return true;
}

struct msm_frame *QualcommCameraHardware::FrameQueue::get()
{
    struct msm_frame *frame;
    mQueueLock.lock();
    while (mInitialized && mContainer.isEmpty()) {
        mQueueWait.wait(mQueueLock);
    }

    if (!mInitialized) {
        mQueueLock.unlock();
        return NULL;
    }

    frame = mContainer.itemAt(0);
    mContainer.removeAt(0);
    mQueueLock.unlock();
    return frame;
}

void QualcommCameraHardware::FrameQueue::flush()
{
    Mutex::Autolock l(&mQueueLock);
    mContainer.clear();
}

void QualcommCameraHardware::storeTargetType(void)
{
    char mDeviceName[PROPERTY_VALUE_MAX];
    property_get("ro.board.platform", mDeviceName, " ");
    mCurrentTarget = TARGET_MAX;
    for (int i = 0; i < TARGET_MAX; i++) {
        if (!strncmp(mDeviceName, targetList[i].targetStr, 7)) {
            mCurrentTarget = targetList[i].targetEnum;
        }
    }
    ALOGV(" Storing the current target type as %d ", mCurrentTarget);
}

void *openCamera(void *data)
{
    ALOGV(" openCamera : E");
    mCameraOpen = false;

    if (!libmmcamera) {
        ALOGE("FATAL ERROR: could not dlopen liboemcamera.so: %s", dlerror());
        return NULL;
    }

    *(void **)&LINK_mm_camera_init =
        ::dlsym(libmmcamera, "mm_camera_init");

    *(void **)&LINK_mm_camera_deinit =
        ::dlsym(libmmcamera, "mm_camera_deinit");

    *(void **)&LINK_mm_camera_destroy =
        ::dlsym(libmmcamera, "mm_camera_destroy");

    *(void **)&LINK_mm_camera_exec =
        ::dlsym(libmmcamera, "mm_camera_exec");

    *(void **)&LINK_mm_camera_get_camera_info =
        ::dlsym(libmmcamera, "mm_camera_get_camera_info");

    if (MM_CAMERA_SUCCESS != LINK_mm_camera_init(&mCfgControl, &mCamNotify, &mCamOps, 0)) {
        ALOGE("startCamera: mm_camera_init failed:");
        return NULL;
    }

    uint8_t camera_id8 = HAL_currentCameraId;
    if (MM_CAMERA_SUCCESS != mCfgControl.mm_camera_set_parm(CAMERA_PARM_CAMERA_ID, &camera_id8)) {
        ALOGE("setting camera id failed");
        LINK_mm_camera_deinit();
        return NULL;
    }

    camera_mode_t mode = HAL_currentCameraMode;
    if (MM_CAMERA_SUCCESS != mCfgControl.mm_camera_set_parm(CAMERA_PARM_MODE, &mode)) {
        ALOGE("startCamera: CAMERA_PARM_MODE failed:");
        LINK_mm_camera_deinit();
        return NULL;
    }

    if (MM_CAMERA_SUCCESS != LINK_mm_camera_exec()) {
        ALOGE("startCamera: mm_camera_exec failed:");
        return NULL;
    }
    mCameraOpen = true;
    ALOGV(" openCamera : X");
    if (CAMERA_MODE_3D == mode) {
        camera_3d_frame_t snapshotFrame;
        snapshotFrame.frame_type = CAM_SNAPSHOT_FRAME;
        if (MM_CAMERA_SUCCESS !=
            mCfgControl.mm_camera_get_parm(CAMERA_PARM_3D_FRAME_FORMAT,
                &snapshotFrame)) {
            ALOGE("%s: get 3D format failed", __func__);
            LINK_mm_camera_deinit();
            return NULL;
        }
        QualcommCameraHardware *obj = QualcommCameraHardware::getInstance();
        if (obj != 0) {
            obj->mSnapshot3DFormat = snapshotFrame.format;
            ALOGI("%s: 3d format  snapshot %d", __func__, obj->mSnapshot3DFormat);
        }
    }

    ALOGV("openCamera : X");
    return NULL;
}
//-------------------------------------------------------------------------------------
static void receive_camframe_callback(struct msm_frame *frame);
static void receive_liveshot_callback(liveshot_status status, uint32_t jpeg_size);
static void receive_camstats_callback(camstats_type stype, camera_preview_histogram_info* histinfo);
static void receive_camframe_video_callback(struct msm_frame *frame);
static int8_t receive_event_callback(mm_camera_event* event);
static void receive_camframe_error_callback(camera_error_type err);

static int32_t mMaxZoom = 0;
static bool zoomSupported = false;
static int16_t *zoomRatios;

QualcommCameraHardware::QualcommCameraHardware()
    : mParameters(),
      mCameraRunning(false),
      mPreviewInitialized(false),
      mPreviewThreadRunning(false),
      mHFRThreadRunning(false),
      mFrameThreadRunning(false),
      mVideoThreadRunning(false),
      mSmoothzoomThreadExit(false),
      mSmoothzoomThreadRunning(false),
      mSnapshotThreadRunning(false),
      mJpegThreadRunning(false),
      mInSnapshotMode(false),
      mEncodePending(false),
      mBuffersInitialized(false),
      mSnapshotFormat(0),
      mStoreMetaDataInFrame(0),
      mReleasedRecordingFrame(false),
      mPreviewFrameSize(0),
      mRawSize(0),
      mCbCrOffsetRaw(0),
      mYOffset(0),
      mAutoFocusThreadRunning(false),
      mInitialized(false),
      mBrightness(0),
      mSkinToneEnhancement(0),
      mHJR(0),
      mPreviewWindow(NULL),
      mIs3DModeOn(0),
      mMsgEnabled(0),
      mNotifyCallback(0),
      mDataCallback(0),
      mDataCallbackTimestamp(0),
      mCallbackCookie(0),
      mSnapshotDone(0),
      maxSnapshotWidth(0),
      maxSnapshotHeight(0),
      mHasAutoFocusSupport(0),
      mDisEnabled(0),
      mRotation(0),
      mResetWindowCrop(false),
      mThumbnailWidth(0),
      mThumbnailHeight(0),
      strTexturesOn(false),
      mPictureWidth(0),
      mPictureHeight(0),
      mPostviewWidth(0),
      mPostviewHeight(0),
      mTotalPreviewBufferCount(0),
      mDenoiseValue(0),
      mZslEnable(false),
      mZslPanorama(false),
      mZslFlashEnable(false),
      mSnapshotCancel(false),
      mHFRMode(false),
      mActualPictWidth(0),
      mActualPictHeight(0),
      mPreviewStopping(false),
      mInHFRThread(false),
      mHdrMode(false),
      mExpBracketMode(false),
      mRecordingState(0)
{
    ALOGI("QualcommCameraHardware constructor E");
    mMMCameraDLRef = MMCameraDL::getInstance();
    libmmcamera = mMMCameraDLRef->pointer();
    char value[PROPERTY_VALUE_MAX];
    mCameraOpen = false;
    if (HAL_currentSnapshotMode == CAMERA_SNAPSHOT_ZSL) {
        ALOGI("%s: this is ZSL mode", __FUNCTION__);
        mZslEnable = true;
    }

    property_get("persist.camera.hal.multitouchaf", value, "0");
    mMultiTouch = atoi(value);

    storeTargetType();

    mRawSnapshotMapped = NULL;
    mJpegCopyMapped = NULL;
    mJpegLiveSnapMapped = NULL;

    for (int i = 0; i < MAX_SNAPSHOT_BUFFERS; i++) {
        mRawMapped[i] = NULL;
        mJpegMapped[i] = NULL;
        mThumbnailMapped[i] = NULL;
        mThumbnailBuffer[i] = NULL;
    }

    for (int i = 0; i < RECORD_BUFFERS; i++)
        mRecordMapped[i] = NULL;

    for (int i =0; i < 3; i++)
        mStatsMapped[i] = NULL;

    if (HAL_currentCameraMode == CAMERA_MODE_3D)
        mIs3DModeOn = true;

    /* TODO: Will remove this command line interface at end */
    property_get("persist.camera.hal.3dmode", value, "0");
    int mode = atoi(value);
    if (mode == 1) {
        mIs3DModeOn = true;
        HAL_currentCameraMode = CAMERA_MODE_3D;
    }

    if ((pthread_create(&mDeviceOpenThread, NULL, openCamera, NULL)) != 0) {
        ALOGE(" openCamera thread creation failed ");
    }
    memset(&mDimension, 0, sizeof(mDimension));
    memset(&zoomCropInfo, 0, sizeof(android_native_rect_t));

    if (mCurrentTarget == TARGET_MSM7630 || mCurrentTarget == TARGET_MSM8660) {
        kRecordBufferCount = RECORD_BUFFERS;
        recordframes = new msm_frame[kRecordBufferCount];
        record_buffers_tracking_flag = new bool[kRecordBufferCount];
    } else {
        if (mCurrentTarget == TARGET_QSD8250) {
            kRecordBufferCount = RECORD_BUFFERS_8x50;
            recordframes = new msm_frame[kRecordBufferCount];
            record_buffers_tracking_flag = new bool[kRecordBufferCount];
        }
    }
    mTotalPreviewBufferCount = kTotalPreviewBufferCount;
    if (mCurrentTarget != TARGET_MSM7630 && mCurrentTarget != TARGET_QSD8250
        && mCurrentTarget != TARGET_MSM8660) {
        for (int i = 0; i < mTotalPreviewBufferCount; i++)
            metadata_memory[i] = NULL;
    } else {
        for (int i = 0; i < kRecordBufferCount; i++)
            metadata_memory[i] = NULL;
    }
    // Initialize with default format values. The format values can be
    // overriden when application requests.
    mDimension.prev_format     = CAMERA_YUV_420_NV21;
    mPreviewFormat             = CAMERA_YUV_420_NV21;
    mDimension.enc_format      = CAMERA_YUV_420_NV21;
    if (mCurrentTarget == TARGET_MSM7630 || mCurrentTarget == TARGET_MSM8660)
        mDimension.enc_format  = CAMERA_YUV_420_NV12;
    mDimension.main_img_format = CAMERA_YUV_420_NV21;
    mDimension.thumb_format    = CAMERA_YUV_420_NV21;

    if (mCurrentTarget == TARGET_MSM7630 || mCurrentTarget == TARGET_MSM8660) {
        /* DIS is disabled all the time in VPE support targets.
         * No provision for the user to control this.
         */
        mDisEnabled = 0;
        /* Get the DIS value from properties, to check whether
         * DIS is disabled or not. If the property is not found
         * default to DIS disabled.*/
        property_get("persist.camera.hal.dis", value, "0");
        mDisEnabled = atoi(value);
        mVpeEnabled = true;
    }
    if (mIs3DModeOn) {
        mDisEnabled = 0;
    }
    ALOGV("constructor EX");
}

void QualcommCameraHardware::hasAutoFocusSupport()
{
    if (!mCamOps.mm_camera_is_supported(CAMERA_OPS_FOCUS)) {
        ALOGI("AutoFocus is not supported");
        mHasAutoFocusSupport = false;
    } else {
        mHasAutoFocusSupport = true;
    }
    if (mZslEnable)
        mHasAutoFocusSupport = false;
}

//filter Picture sizes based on max width and height
void QualcommCameraHardware::filterPictureSizes()
{
    if (PICTURE_SIZE_COUNT <= 0)
        return;
    maxSnapshotWidth = picture_sizes[0].width;
    maxSnapshotHeight = picture_sizes[0].height;
    // Iterate through all the width and height to find the max value
    for (unsigned int i = 0; i < PICTURE_SIZE_COUNT; i++) {
        if (maxSnapshotWidth < picture_sizes[i].width &&
            maxSnapshotHeight <= picture_sizes[i].height) {
            maxSnapshotWidth = picture_sizes[i].width;
            maxSnapshotHeight = picture_sizes[i].height;
        }
    }
    if (mZslEnable) {
        // due to lack of PMEM we restrict to lower resolution
        picture_sizes_ptr = zsl_picture_sizes;
        supportedPictureSizesCount = 7;
    }
    else if (mIs3DModeOn) {
        // In 3D mode we only want 1080p picture size
        picture_sizes_ptr = for_3D_picture_sizes;
        supportedPictureSizesCount = 1;
    } else {
        picture_sizes_ptr = picture_sizes;
        supportedPictureSizesCount = PICTURE_SIZE_COUNT;
    }
}

bool QualcommCameraHardware::supportsSceneDetection()
{
    for (unsigned int prop = 0; prop < sizeof(boardProperties) / sizeof(board_property); prop++) {
        if (mCurrentTarget == boardProperties[prop].target &&
            boardProperties[prop].hasSceneDetect == true) {
            return true;
        }
    }
    return false;
}

bool QualcommCameraHardware::supportsSelectableZoneAf()
{
    for (unsigned int prop = 0; prop < sizeof(boardProperties) / sizeof(board_property); prop++) {
        if (mCurrentTarget == boardProperties[prop].target &&
            boardProperties[prop].hasSelectableZoneAf == true) {
            return true;
        }
    }
    return false;
}

bool QualcommCameraHardware::supportsFaceDetection()
{
    for (unsigned int prop = 0; prop < sizeof(boardProperties) / sizeof(board_property); prop++) {
        if (mCurrentTarget == boardProperties[prop].target &&
            boardProperties[prop].hasFaceDetect == true) {
            return true;
        }
    }
    return false;
}

void QualcommCameraHardware::initDefaultParameters()
{
    ALOGI("initDefaultParameters E");
    mDimension.picture_width = DEFAULT_PICTURE_WIDTH;
    mDimension.picture_height = DEFAULT_PICTURE_HEIGHT;
    mDimension.ui_thumbnail_width = thumbnail_sizes[DEFAULT_THUMBNAIL_SETTING].width;
    mDimension.ui_thumbnail_height = thumbnail_sizes[DEFAULT_THUMBNAIL_SETTING].height;
    bool ret = native_set_parms(CAMERA_PARM_DIMENSION, sizeof(cam_ctrl_dimension_t), &mDimension);
    if (ret != true) {
        ALOGE("CAMERA_PARM_DIMENSION failed!!!");
        return;
    }

    hasAutoFocusSupport();

    //Disable DIS for Web Camera
    if (!mCfgControl.mm_camera_is_supported(CAMERA_PARM_VIDEO_DIS)) {
        ALOGV("DISABLE DIS");
        mDisEnabled = 0;
    } else {
        ALOGV("Enable DIS");
    }

    // Initialize constant parameter strings. This will happen only once in the
    // lifetime of the mediaserver process.
    if (!parameter_string_initialized) {
        if (mIs3DModeOn) {
            antibanding_values = create_values_str(
                antibanding_3D, sizeof(antibanding_3D) / sizeof(str_map));
        } else {
            antibanding_values = create_values_str(
                antibanding, sizeof(antibanding) / sizeof(str_map));
        }
        effect_values = create_values_str(
            effects, sizeof(effects) / sizeof(str_map));
        autoexposure_values = create_values_str(
            autoexposure, sizeof(autoexposure) / sizeof(str_map));
        whitebalance_values = create_values_str(
            whitebalance, sizeof(whitebalance) / sizeof(str_map));
        filterPictureSizes();
        picture_size_values = create_sizes_str(
            picture_sizes_ptr, supportedPictureSizesCount);
        preview_size_values = create_sizes_str(
            preview_sizes,  PREVIEW_SIZE_COUNT);

        mParameters.set(CameraParameters::KEY_SUPPORTED_PREVIEW_SIZES,
                            preview_size_values.string());
        mParameters.set(CameraParameters::KEY_SUPPORTED_VIDEO_SIZES,
                            preview_size_values.string());
        mParameters.set(CameraParameters::KEY_SUPPORTED_PICTURE_SIZES,
                            picture_size_values.string());
        mParameters.set(CameraParameters::KEY_VIDEO_SNAPSHOT_SUPPORTED,
                            "true");
        mParameters.set(CameraParameters::KEY_SUPPORTED_FOCUS_MODES,
                       CameraParameters::FOCUS_MODE_INFINITY);
        mParameters.set(CameraParameters::KEY_FOCUS_MODE,
                       CameraParameters::FOCUS_MODE_INFINITY);
        mParameters.set(CameraParameters::KEY_MAX_NUM_FOCUS_AREAS, "1");

        mParameters.set(CameraParameters::KEY_FOCUS_AREAS, FOCUS_AREA_INIT);
        mParameters.set(CameraParameters::KEY_METERING_AREAS, FOCUS_AREA_INIT);
        if (!mIs3DModeOn) {
            hfr_size_values = create_sizes_str(hfr_sizes, HFR_SIZE_COUNT);
        }
        fps_ranges_supported_values = create_fps_str(
            FpsRangesSupported,FPS_RANGES_SUPPORTED_COUNT );
        mParameters.set(
            CameraParameters::KEY_SUPPORTED_PREVIEW_FPS_RANGE,
            fps_ranges_supported_values);
        mParameters.setPreviewFpsRange(MINIMUM_FPS*1000,MAXIMUM_FPS*1000);

        flash_values = create_values_str(flash, sizeof(flash) / sizeof(str_map));
        if (mHasAutoFocusSupport) {
            focus_mode_values = create_values_str(
                    focus_modes, sizeof(focus_modes) / sizeof(str_map));
        }
        if (mIs3DModeOn) {
            iso_values = create_values_str(iso_3D,sizeof(iso_3D)/sizeof(str_map));
        } else {
            iso_values = create_values_str(iso,sizeof(iso)/sizeof(str_map));
        }
        lensshade_values = create_values_str(
            lensshade,sizeof(lensshade)/sizeof(str_map));
        mce_values = create_values_str(
            mce,sizeof(mce)/sizeof(str_map));
        if (!mIs3DModeOn) {
            hfr_values = create_values_str(hfr,sizeof(hfr)/sizeof(str_map));
        }
        if (mCurrentTarget == TARGET_MSM8660)
            hdr_values = create_values_str(
                hdr,sizeof(hdr)/sizeof(str_map));
        //Currently Enabling Histogram for 8x60
        if (mCurrentTarget == TARGET_MSM8660) {
            histogram_values = create_values_str(
                histogram,sizeof(histogram)/sizeof(str_map));
        }
        //Currently Enabling Skin Tone Enhancement for 8x60 and 7630
        if ((mCurrentTarget == TARGET_MSM8660)||(mCurrentTarget == TARGET_MSM7630)) {
            skinToneEnhancement_values = create_values_str(
                skinToneEnhancement,sizeof(skinToneEnhancement)/sizeof(str_map));
        }
        if (mHasAutoFocusSupport) {
            touchafaec_values = create_values_str(
                touchafaec,sizeof(touchafaec)/sizeof(str_map));
        }
        zsl_values = create_values_str(
            zsl_modes,sizeof(zsl_modes)/sizeof(str_map));

        if (mZslEnable) {
            picture_format_values = create_values_str(
                picture_formats_zsl, sizeof(picture_formats_zsl)/sizeof(str_map));
        } else {
            picture_format_values = create_values_str(
                picture_formats, sizeof(picture_formats)/sizeof(str_map));
        }
        if (mCurrentTarget == TARGET_MSM8660 ||
            mCurrentTarget == TARGET_MSM7625A ||
            mCurrentTarget == TARGET_MSM7627A) {
            denoise_values = create_values_str(
                denoise, sizeof(denoise) / sizeof(str_map));
        }
        if (mCfgControl.mm_camera_query_parms(CAMERA_PARM_ZOOM_RATIO,
            (void **)&zoomRatios, (uint32_t *) &mMaxZoom) == MM_CAMERA_SUCCESS) {
            mMaxZoom = (mMaxZoom + 1) / 2;
            zoomSupported = true;
            if (mMaxZoom > 0) {
                if (zoomRatios != NULL) {
                    zoom_ratio_values = create_str(zoomRatios, mMaxZoom);
                } else {
                    ALOGE("Failed to get zoomratios ..");
                }
            } else {
                zoomSupported = false;
            }
        } else {
            zoomSupported = false;
            ALOGE("Failed to get maximum zoom value...setting max zoom to zero");
            mMaxZoom = 0;
        }
        preview_frame_rate_values = create_values_range_str(MINIMUM_FPS, MAXIMUM_FPS);

        scenemode_values = create_values_str(
            scenemode, sizeof(scenemode) / sizeof(str_map));

        if (supportsSceneDetection()) {
            scenedetect_values = create_values_str(
                scenedetect, sizeof(scenedetect) / sizeof(str_map));
        }

        if (mHasAutoFocusSupport && supportsSelectableZoneAf()) {
            selectable_zone_af_values = create_values_str(
                selectable_zone_af, sizeof(selectable_zone_af) / sizeof(str_map));
        }

        if (mHasAutoFocusSupport && supportsFaceDetection()) {
            facedetection_values = create_values_str(
                facedetection, sizeof(facedetection) / sizeof(str_map));
        }

        redeye_reduction_values = create_values_str(
            redeye_reduction, sizeof(redeye_reduction) / sizeof(str_map));

        parameter_string_initialized = true;
    }
    if (mIs3DModeOn) {
       ALOGE("In initDefaultParameters - 3D mode on so set the default preview to 1280 x 720");
       mParameters.setPreviewSize(DEFAULT_PREVIEW_WIDTH_3D, DEFAULT_PREVIEW_HEIGHT_3D);
       mDimension.display_width = DEFAULT_PREVIEW_WIDTH_3D;
       mDimension.display_height = DEFAULT_PREVIEW_HEIGHT_3D;
    } else {
       mParameters.setPreviewSize(DEFAULT_PREVIEW_WIDTH, DEFAULT_PREVIEW_HEIGHT);
       mDimension.display_width = DEFAULT_PREVIEW_WIDTH;
       mDimension.display_height = DEFAULT_PREVIEW_HEIGHT;
    }
    mParameters.setPreviewFrameRate(DEFAULT_FPS);
    if (mCfgControl.mm_camera_is_supported(CAMERA_PARM_FPS)) {
        mParameters.set(
            CameraParameters::KEY_SUPPORTED_PREVIEW_FRAME_RATES,
            preview_frame_rate_values.string());
    } else {
        mParameters.setPreviewFrameRate(DEFAULT_FIXED_FPS_VALUE);
        mParameters.set(CameraParameters::KEY_SUPPORTED_PREVIEW_FRAME_RATES,
            DEFAULT_FIXED_FPS_VALUE);
    }
    mParameters.setPreviewFrameRateMode("frame-rate-auto");
    mParameters.setPreviewFormat("yuv420sp"); // informative
    mParameters.set("overlay-format", HAL_PIXEL_FORMAT_YCbCr_420_SP);
    if (mIs3DModeOn) {
        mParameters.setPictureSize(DEFAULT_PICTURE_WIDTH_3D, DEFAULT_PICTURE_HEIGHT_3D);
    } else {
        mParameters.setPictureSize(DEFAULT_PICTURE_WIDTH, DEFAULT_PICTURE_HEIGHT);
    }
    mParameters.setPictureFormat(CameraParameters::PIXEL_FORMAT_JPEG); // informative

    mParameters.set(CameraParameters::KEY_VIDEO_FRAME_FORMAT, CameraParameters::PIXEL_FORMAT_YUV420SP);

    mParameters.set(CameraParameters::KEY_JPEG_QUALITY, "85"); // max quality

    mParameters.set(CameraParameters::KEY_POWER_MODE_SUPPORTED, "false");

    mParameters.set(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH,
                    THUMBNAIL_WIDTH_STR); // informative
    mParameters.set(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT,
                    THUMBNAIL_HEIGHT_STR); // informative
    mDimension.ui_thumbnail_width =
            thumbnail_sizes[DEFAULT_THUMBNAIL_SETTING].width;
    mDimension.ui_thumbnail_height =
            thumbnail_sizes[DEFAULT_THUMBNAIL_SETTING].height;
    mParameters.set(CameraParameters::KEY_JPEG_THUMBNAIL_QUALITY, "90");

    String8 valuesStr = create_sizes_str(jpeg_thumbnail_sizes, JPEG_THUMBNAIL_SIZE_COUNT);
    mParameters.set(CameraParameters::KEY_SUPPORTED_JPEG_THUMBNAIL_SIZES,
                valuesStr.string());

    // Define CAMERA_SMOOTH_ZOOM in Android.mk file , to enable smoothzoom
#ifdef CAMERA_SMOOTH_ZOOM
    mParameters.set(CameraParameters::KEY_SMOOTH_ZOOM_SUPPORTED, "true");
#endif

    if (zoomSupported) {
        mParameters.set(CameraParameters::KEY_ZOOM_SUPPORTED, "true");
        ALOGV("max zoom is %d", mMaxZoom-1);
        /* mMaxZoom value that the query interface returns is the size
         * of zoom table. So the actual max zoom value will be one
         * less than that value.
         */
        mParameters.set(CameraParameters::KEY_MAX_ZOOM,mMaxZoom-1);
        mParameters.set(CameraParameters::KEY_ZOOM_RATIOS,
                            zoom_ratio_values);
    } else {
        mParameters.set(CameraParameters::KEY_ZOOM_SUPPORTED, "false");
    }
    /* Enable zoom support for video application if VPE enabled */
    if (zoomSupported && mVpeEnabled) {
        mParameters.set("video-zoom-support", "true");
    } else {
        mParameters.set("video-zoom-support", "false");
    }

    mParameters.set(CameraParameters::KEY_CAMERA_MODE,0);

    mParameters.set(CameraParameters::KEY_ANTIBANDING,
                    CameraParameters::ANTIBANDING_OFF);
    mParameters.set(CameraParameters::KEY_EFFECT,
                    CameraParameters::EFFECT_NONE);
    mParameters.set(CameraParameters::KEY_AUTO_EXPOSURE,
                    CameraParameters::AUTO_EXPOSURE_FRAME_AVG);
    mParameters.set(CameraParameters::KEY_WHITE_BALANCE,
                    CameraParameters::WHITE_BALANCE_AUTO);
    if ( (mCurrentTarget != TARGET_MSM7630)
        && (mCurrentTarget != TARGET_QSD8250)
        && (mCurrentTarget != TARGET_MSM8660)
        && (mCurrentTarget != TARGET_MSM7627A)) {
        mParameters.set(CameraParameters::KEY_SUPPORTED_PREVIEW_FORMATS,
                    "yuv420sp");
    } else if (mCurrentTarget == TARGET_MSM7627A || mCurrentTarget == TARGET_MSM7627) {
        preview_format_values = create_values_str(
            preview_formats1, sizeof(preview_formats1) / sizeof(str_map));
        mParameters.set(CameraParameters::KEY_SUPPORTED_PREVIEW_FORMATS,
                preview_format_values.string());
    } else {
        preview_format_values = create_values_str(
            preview_formats, sizeof(preview_formats) / sizeof(str_map));
        mParameters.set(CameraParameters::KEY_SUPPORTED_PREVIEW_FORMATS,
                preview_format_values.string());
    }

    frame_rate_mode_values = create_values_str(
            frame_rate_modes, sizeof(frame_rate_modes) / sizeof(str_map));
    if ( mCfgControl.mm_camera_is_supported(CAMERA_PARM_FPS_MODE)) {
        mParameters.set(CameraParameters::KEY_SUPPORTED_PREVIEW_FRAME_RATE_MODES,
                    frame_rate_mode_values.string());
    }

    mParameters.set(CameraParameters::KEY_SUPPORTED_PREVIEW_SIZES,
                    preview_size_values.string());
    mParameters.set(CameraParameters::KEY_SUPPORTED_PICTURE_SIZES,
                    picture_size_values.string());
    mParameters.set(CameraParameters::KEY_SUPPORTED_ANTIBANDING,
                    antibanding_values);
    mParameters.set(CameraParameters::KEY_SUPPORTED_EFFECTS, effect_values);
    mParameters.set(CameraParameters::KEY_SUPPORTED_AUTO_EXPOSURE, autoexposure_values);
    mParameters.set(CameraParameters::KEY_SUPPORTED_WHITE_BALANCE,
                    whitebalance_values);

    if (mHasAutoFocusSupport) {
        mParameters.set(CameraParameters::KEY_SUPPORTED_FOCUS_MODES,
            focus_mode_values);
        mParameters.set(CameraParameters::KEY_FOCUS_MODE,
                    CameraParameters::FOCUS_MODE_AUTO);
    } else {
        mParameters.set(CameraParameters::KEY_SUPPORTED_FOCUS_MODES,
            CameraParameters::FOCUS_MODE_INFINITY);
        mParameters.set(CameraParameters::KEY_FOCUS_MODE,
            CameraParameters::FOCUS_MODE_INFINITY);
    }

    mParameters.set(CameraParameters::KEY_SUPPORTED_PICTURE_FORMATS,
                    picture_format_values);

    if (mCfgControl.mm_camera_is_supported(CAMERA_PARM_LED_MODE)) {
        mParameters.set(CameraParameters::KEY_FLASH_MODE,
            CameraParameters::FLASH_MODE_OFF);
        mParameters.set(CameraParameters::KEY_SUPPORTED_FLASH_MODES,
            flash_values);
    }

    mParameters.set(CameraParameters::KEY_MAX_SHARPNESS,
            CAMERA_MAX_SHARPNESS);
    mParameters.set(CameraParameters::KEY_MAX_CONTRAST,
            CAMERA_MAX_CONTRAST);
    mParameters.set(CameraParameters::KEY_MAX_SATURATION,
            CAMERA_MAX_SATURATION);

    mParameters.set(
            CameraParameters::KEY_MAX_EXPOSURE_COMPENSATION,
            EXPOSURE_COMPENSATION_MAXIMUM_NUMERATOR);
    mParameters.set(
            CameraParameters::KEY_MIN_EXPOSURE_COMPENSATION,
            EXPOSURE_COMPENSATION_MINIMUM_NUMERATOR);
    mParameters.set(
            CameraParameters::KEY_EXPOSURE_COMPENSATION,
            EXPOSURE_COMPENSATION_DEFAULT_NUMERATOR);
    mParameters.setFloat(
            CameraParameters::KEY_EXPOSURE_COMPENSATION_STEP,
            EXPOSURE_COMPENSATION_STEP);

    mParameters.set("luma-adaptation", "3");
    mParameters.set("skinToneEnhancement", "0");
    mParameters.set(CameraParameters::KEY_ZOOM_SUPPORTED, "true");
    mParameters.set(CameraParameters::KEY_ZOOM, 0);
    mParameters.set(CameraParameters::KEY_PICTURE_FORMAT,
                    CameraParameters::PIXEL_FORMAT_JPEG);

    mParameters.set(CameraParameters::KEY_SHARPNESS,
                    CAMERA_DEF_SHARPNESS);
    mParameters.set(CameraParameters::KEY_CONTRAST,
                    CAMERA_DEF_CONTRAST);
    mParameters.set(CameraParameters::KEY_SATURATION,
                    CAMERA_DEF_SATURATION);

    mParameters.set(CameraParameters::KEY_ISO_MODE,
                    CameraParameters::ISO_AUTO);
    mParameters.set(CameraParameters::KEY_LENSSHADE,
                    CameraParameters::LENSSHADE_ENABLE);
    mParameters.set(CameraParameters::KEY_SUPPORTED_ISO_MODES,
                    iso_values);
    mParameters.set(CameraParameters::KEY_SUPPORTED_LENSSHADE_MODES,
                    lensshade_values);
    mParameters.set(CameraParameters::KEY_MEMORY_COLOR_ENHANCEMENT,
                    CameraParameters::MCE_ENABLE);
    mParameters.set(CameraParameters::KEY_SUPPORTED_MEM_COLOR_ENHANCE_MODES,
                    mce_values);
    if (mCfgControl.mm_camera_is_supported(CAMERA_PARM_HFR) && !(mIs3DModeOn)) {
        mParameters.set(CameraParameters::KEY_VIDEO_HIGH_FRAME_RATE,
                    CameraParameters::VIDEO_HFR_OFF);
        mParameters.set(CameraParameters::KEY_SUPPORTED_HFR_SIZES,
                    hfr_size_values.string());
        mParameters.set(CameraParameters::KEY_SUPPORTED_VIDEO_HIGH_FRAME_RATE_MODES,
                    hfr_values);
    } else
        mParameters.set(CameraParameters::KEY_SUPPORTED_HFR_SIZES,"");

    mParameters.set(CameraParameters::KEY_HIGH_DYNAMIC_RANGE_IMAGING,
                    CameraParameters::MCE_DISABLE);
    mParameters.set(CameraParameters::KEY_SUPPORTED_HDR_IMAGING_MODES,
                    hdr_values);
    mParameters.set(CameraParameters::KEY_HISTOGRAM,
                    CameraParameters::HISTOGRAM_DISABLE);
    mParameters.set(CameraParameters::KEY_SUPPORTED_HISTOGRAM_MODES,
                    histogram_values);
    mParameters.set(CameraParameters::KEY_SKIN_TONE_ENHANCEMENT,
                    CameraParameters::SKIN_TONE_ENHANCEMENT_DISABLE);
    mParameters.set(CameraParameters::KEY_SUPPORTED_SKIN_TONE_ENHANCEMENT_MODES,
                    skinToneEnhancement_values);
    mParameters.set(CameraParameters::KEY_SCENE_MODE,
                    CameraParameters::SCENE_MODE_AUTO);
    mParameters.set("strtextures", "OFF");

    mParameters.set(CameraParameters::KEY_SUPPORTED_SCENE_MODES,
                    scenemode_values);
    mParameters.set(CameraParameters::KEY_DENOISE,
                    CameraParameters::DENOISE_OFF);
    mParameters.set(CameraParameters::KEY_SUPPORTED_DENOISE,
                    denoise_values);

    //touch af/aec parameters
    mParameters.set(CameraParameters::KEY_TOUCH_AF_AEC,
                    CameraParameters::TOUCH_AF_AEC_OFF);
    mParameters.set(CameraParameters::KEY_SUPPORTED_TOUCH_AF_AEC,
                    touchafaec_values);
    mParameters.set("touchAfAec-dx","100");
    mParameters.set("touchAfAec-dy","100");
    mParameters.set(CameraParameters::KEY_MAX_NUM_FOCUS_AREAS, "1");
    mParameters.set(CameraParameters::KEY_MAX_NUM_METERING_AREAS, "1");

    mParameters.set(CameraParameters::KEY_SCENE_DETECT,
                    CameraParameters::SCENE_DETECT_OFF);
    mParameters.set(CameraParameters::KEY_SUPPORTED_SCENE_DETECT,
                    scenedetect_values);
    mParameters.set(CameraParameters::KEY_SELECTABLE_ZONE_AF,
                    CameraParameters::SELECTABLE_ZONE_AF_AUTO);
    mParameters.set(CameraParameters::KEY_SUPPORTED_SELECTABLE_ZONE_AF,
                    selectable_zone_af_values);
    mParameters.set(CameraParameters::KEY_FACE_DETECTION,
                    CameraParameters::FACE_DETECTION_OFF);
    mParameters.set(CameraParameters::KEY_SUPPORTED_FACE_DETECTION,
                    facedetection_values);
    mParameters.set(CameraParameters::KEY_REDEYE_REDUCTION,
                    CameraParameters::REDEYE_REDUCTION_DISABLE);
    mParameters.set(CameraParameters::KEY_SUPPORTED_REDEYE_REDUCTION,
                    redeye_reduction_values);
    mParameters.set(CameraParameters::KEY_ZSL,
                    CameraParameters::ZSL_OFF);
    mParameters.set(CameraParameters::KEY_SUPPORTED_ZSL_MODES,
                    zsl_values);

    float focalLength = 0.0f;
    float horizontalViewAngle = 0.0f;
    float verticalViewAngle = 0.0f;

    mCfgControl.mm_camera_get_parm(CAMERA_PARM_FOCAL_LENGTH, &focalLength);
    mParameters.setFloat(CameraParameters::KEY_FOCAL_LENGTH, focalLength);
    mCfgControl.mm_camera_get_parm(CAMERA_PARM_HORIZONTAL_VIEW_ANGLE, &horizontalViewAngle);
    mParameters.setFloat(CameraParameters::KEY_HORIZONTAL_VIEW_ANGLE, horizontalViewAngle);
    mCfgControl.mm_camera_get_parm(CAMERA_PARM_VERTICAL_VIEW_ANGLE, &verticalViewAngle);
    mParameters.setFloat(CameraParameters::KEY_VERTICAL_VIEW_ANGLE, verticalViewAngle);

    numCapture = 1;
    if (mZslEnable) {
        int maxSnapshot = MAX_SNAPSHOT_BUFFERS - 2;
        char value[PROPERTY_VALUE_MAX];
        property_get("persist.camera.hal.capture", value, "1");
        numCapture = atoi(value);
        if (numCapture > maxSnapshot)
            numCapture = maxSnapshot;
        else if (numCapture < 1)
            numCapture = 1;
        mParameters.set("capture-burst-captures-values", maxSnapshot);
        mParameters.set("capture-burst-interval-supported", "false");
    }
    mParameters.set("num-snaps-per-shutter", numCapture);
    ALOGI("%s: setting num-snaps-per-shutter to %d", __FUNCTION__, numCapture);
    if (mIs3DModeOn)
        mParameters.set("3d-frame-format", "left-right");

    switch (mCurrentTarget) {
        case TARGET_MSM7627:
        case TARGET_QSD8250:
        case TARGET_MSM7630:
            mParameters.set(CameraParameters::KEY_PREFERRED_PREVIEW_SIZE_FOR_VIDEO, "800x480");
            break;
        case TARGET_MSM7627A:
            mParameters.set(CameraParameters::KEY_PREFERRED_PREVIEW_SIZE_FOR_VIDEO, "864x480");
            break;
        case TARGET_MSM8660:
            mParameters.set(CameraParameters::KEY_PREFERRED_PREVIEW_SIZE_FOR_VIDEO, "1920x1088");
            break;
        default:
            mParameters.set(CameraParameters::KEY_PREFERRED_PREVIEW_SIZE_FOR_VIDEO, "640x480");
            break;
    }

    if (setParameters(mParameters) != NO_ERROR) {
        ALOGE("Failed to set default parameters?!");
    }

    /* Initialize the camframe_timeout_flag*/
    Mutex::Autolock l(&mCamframeTimeoutLock);
    camframe_timeout_flag = FALSE;

    mInitialized = true;
    strTexturesOn = false;

    ALOGI("initDefaultParameters X");
}

bool QualcommCameraHardware::startCamera()
{
    ALOGV("startCamera E");
    if (mCurrentTarget == TARGET_MAX) {
        ALOGE(" Unable to determine the target type. Camera will not work ");
        return false;
    }

#ifdef DLOPEN_LIBMMCAMERA
    ALOGV("loading liboemcamera at %p", libmmcamera);
    if (!libmmcamera) {
        ALOGE("FATAL ERROR: could not dlopen liboemcamera.so: %s", dlerror());
        return false;
    }

    *(void **)&LINK_cam_frame =
        ::dlsym(libmmcamera, "cam_frame");
    *(void **)&LINK_wait_cam_frame_thread_ready =
        ::dlsym(libmmcamera, "wait_cam_frame_thread_ready");
    *(void **)&LINK_cam_frame_set_exit_flag =
        ::dlsym(libmmcamera, "cam_frame_set_exit_flag");
    *(void **)&LINK_camframe_terminate =
        ::dlsym(libmmcamera, "camframe_terminate");
    *(void **)&LINK_jpeg_encoder_join =
        ::dlsym(libmmcamera, "jpeg_encoder_join");
    *(void **)&LINK_camframe_add_frame =
        ::dlsym(libmmcamera, "camframe_add_frame");
    *(void **)&LINK_camframe_release_all_frames =
        ::dlsym(libmmcamera, "camframe_release_all_frames");
    *(void **)&LINK_cancel_liveshot =
        ::dlsym(libmmcamera, "cancel_liveshot");
    *(void **)&LINK_set_liveshot_params =
        ::dlsym(libmmcamera, "set_liveshot_params");
    *(void **)&LINK_set_liveshot_frame =
        ::dlsym(libmmcamera, "set_liveshot_frame");
#endif // DLOPEN_LIBMMCAMERA

    mCamNotify.on_event = &receive_event_callback;
    mCamNotify.video_frame_cb = &receive_camframe_video_callback;
    mCamNotify.preview_frame_cb = &receive_camframe_callback;
    mCamNotify.on_error_event = &receive_camframe_error_callback;
    mCamNotify.camstats_cb = &receive_camstats_callback;
    mCamNotify.on_liveshot_event = &receive_liveshot_callback;

    if (pthread_join(mDeviceOpenThread, NULL) != 0) {
        ALOGE("openCamera thread exit failed");
        return false;
    }

    if (!mCameraOpen) {
        ALOGE("openCamera() failed");
        return false;
    }

    mCfgControl.mm_camera_query_parms(CAMERA_PARM_PICT_SIZE, (void **)&picture_sizes, &PICTURE_SIZE_COUNT);
    if ((picture_sizes == NULL) || (!PICTURE_SIZE_COUNT)) {
        ALOGE("startCamera X: could not get snapshot sizes");
        return false;
    }
    ALOGV("startCamera picture_sizes %p PICTURE_SIZE_COUNT %d", picture_sizes, PICTURE_SIZE_COUNT);
    mCfgControl.mm_camera_query_parms(CAMERA_PARM_PREVIEW_SIZE, (void **)&preview_sizes, &PREVIEW_SIZE_COUNT);
    if ((preview_sizes == NULL) || (!PREVIEW_SIZE_COUNT)) {
        ALOGE("startCamera X: could not get preview sizes");
        return false;
    }
    ALOGV("startCamera preview_sizes %p previewSizeCount %d", preview_sizes, PREVIEW_SIZE_COUNT);

    mCfgControl.mm_camera_query_parms(CAMERA_PARM_HFR_SIZE, (void **)&hfr_sizes, &HFR_SIZE_COUNT);
    if ((hfr_sizes == NULL) || (!HFR_SIZE_COUNT)) {
        ALOGE("startCamera X: could not get hfr sizes");
        return false;
    }
    ALOGV("startCamera hfr_sizes %p hfrSizeCount %d", hfr_sizes, HFR_SIZE_COUNT);

    ALOGV("startCamera X");
    return true;
}

/* Issue ioctl calls related to starting Camera Operations*/
static bool native_start_ops(mm_camera_ops_type_t type, void *value)
{
    if (mCamOps.mm_camera_start(type, value, NULL) != MM_CAMERA_SUCCESS) {
        ALOGE("native_start_ops: type %d error %s",
            type, strerror(errno));
        return false;
    }
    return true;
}

/* Issue ioctl calls related to stopping Camera Operations*/
static bool native_stop_ops(mm_camera_ops_type_t type, void *value)
{
    if (mCamOps.mm_camera_stop(type, value, NULL) != MM_CAMERA_SUCCESS) {
        ALOGE("native_stop_ops: type %d error %s",
            type, strerror(errno));
        return false;
    }
    return true;
}

/*==========================================================================*/

#define GPS_PROCESSING_METHOD_SIZE  101
#define FOCAL_LENGTH_DECIMAL_PRECISON 100

static const char ExifAsciiPrefix[] = { 0x41, 0x53, 0x43, 0x49, 0x49, 0x0, 0x0, 0x0 };
#define EXIF_ASCII_PREFIX_SIZE (sizeof(ExifAsciiPrefix))

static rat_t latitude[3];
static rat_t longitude[3];
static char lonref[2];
static char latref[2];
static rat_t altitude;
static rat_t gpsTimestamp[3];
static char gpsDatestamp[20];
static rat_t focalLength;
static uint16_t flashMode;
static int iso_arr[] = { 0, 1, 100, 200, 400, 800, 1600 };
static uint16_t isoMode;
static char gpsProcessingMethod[EXIF_ASCII_PREFIX_SIZE + GPS_PROCESSING_METHOD_SIZE];
static char exif_maker[PROPERTY_VALUE_MAX];
static char exif_model[PROPERTY_VALUE_MAX];
static char exif_date[20];
static void addExifTag(exif_tag_id_t tagid, exif_tag_type_t type,
    uint32_t count, uint8_t copy, void *data)
{
    if (exif_table_numEntries == MAX_EXIF_TABLE_ENTRIES) {
        ALOGE("Number of entries exceeded limit");
        return;
    }

    int index = exif_table_numEntries;
    exif_data[index].tag_id = tagid;
    exif_data[index].tag_entry.type = type;
    exif_data[index].tag_entry.count = count;
    exif_data[index].tag_entry.copy = copy;
    if (type == EXIF_RATIONAL && count > 1)
        exif_data[index].tag_entry.data._rats = (rat_t *)data;
    if (type == EXIF_RATIONAL && count == 1)
        exif_data[index].tag_entry.data._rat = *(rat_t *)data;
    else if (type == EXIF_ASCII)
        exif_data[index].tag_entry.data._ascii = (char *)data;
    else if (type == EXIF_BYTE)
        exif_data[index].tag_entry.data._byte = *(uint8_t *)data;
    else if (type == EXIF_SHORT && count > 1)
        exif_data[index].tag_entry.data._shorts = (uint16_t *)data;
    else if (type == EXIF_SHORT && count == 1)
        exif_data[index].tag_entry.data._short = *(uint16_t *)data;
    // Increase number of entries
    exif_table_numEntries++;
}

static void parseLatLong(const char *latlonString, uint32_t *pDegrees,
    uint32_t *pMinutes, uint32_t *pSeconds)
{
    double value = atof(latlonString);
    value = fabs(value);
    int degrees = (int) value;

    double remainder = value - degrees;
    int minutes = (int) (remainder * 60);
    int seconds = (int) (((remainder * 60) - minutes) * 60 * 1000);

    *pDegrees = degrees;
    *pMinutes = minutes;
    *pSeconds = seconds;
}

static void setLatLon(exif_tag_id_t tag, const char *latlonString)
{
    uint32_t degrees, minutes, seconds;

    parseLatLong(latlonString, &degrees, &minutes, &seconds);

    rat_t value[3] = {
        { degrees, 1 },
        { minutes, 1 },
        { seconds, 1000 } };

    if (tag == EXIFTAGID_GPS_LATITUDE) {
        memcpy(latitude, value, sizeof(latitude));
        addExifTag(EXIFTAGID_GPS_LATITUDE, EXIF_RATIONAL, 3, 1, latitude);
    } else {
        memcpy(longitude, value, sizeof(longitude));
        addExifTag(EXIFTAGID_GPS_LONGITUDE, EXIF_RATIONAL, 3, 1, longitude);
    }
}

void QualcommCameraHardware::setGpsParameters(void)
{
    const char *str = NULL;

    str = mParameters.get(CameraParameters::KEY_GPS_PROCESSING_METHOD);
    if (str) {
        memcpy(gpsProcessingMethod, ExifAsciiPrefix, EXIF_ASCII_PREFIX_SIZE);
        strncpy(gpsProcessingMethod + EXIF_ASCII_PREFIX_SIZE, str,
            GPS_PROCESSING_METHOD_SIZE - 1);
        gpsProcessingMethod[EXIF_ASCII_PREFIX_SIZE + GPS_PROCESSING_METHOD_SIZE-1] = '\0';
        addExifTag(EXIFTAGID_GPS_PROCESSINGMETHOD, EXIF_ASCII,
            EXIF_ASCII_PREFIX_SIZE + strlen(gpsProcessingMethod + EXIF_ASCII_PREFIX_SIZE) + 1,
            1, gpsProcessingMethod);
    }

    // set latitude
    str = mParameters.get(CameraParameters::KEY_GPS_LATITUDE);
    if (str) {
        setLatLon(EXIFTAGID_GPS_LATITUDE, str);
        //set latitude ref
        float latitudeValue = mParameters.getFloat(CameraParameters::KEY_GPS_LATITUDE);
        if (latitudeValue < 0)
            latref[0] = 'S';
        else
            latref[0] = 'N';
        latref[1] = '\0';
        mParameters.set(CameraParameters::KEY_GPS_LATITUDE_REF, latref);
        addExifTag(EXIFTAGID_GPS_LATITUDE_REF, EXIF_ASCII, 2, 1, latref);
    }

    // set longitude
    str = mParameters.get(CameraParameters::KEY_GPS_LONGITUDE);
    if (str) {
        setLatLon(EXIFTAGID_GPS_LONGITUDE, str);
        // set longitude ref
        float longitudeValue = mParameters.getFloat(CameraParameters::KEY_GPS_LONGITUDE);
        if (longitudeValue < 0)
            lonref[0] = 'W';
        else
            lonref[0] = 'E';
        lonref[1] = '\0';
        mParameters.set(CameraParameters::KEY_GPS_LONGITUDE_REF, lonref);
        addExifTag(EXIFTAGID_GPS_LONGITUDE_REF, EXIF_ASCII, 2, 1, lonref);
    }

    // set altitude
    str = mParameters.get(CameraParameters::KEY_GPS_ALTITUDE);
    if (str) {
        double value = atof(str);
        int ref = 0;
        if (value < 0) {
            ref = 1;
            value = -value;
        }
        uint32_t value_meter = value * 1000;
        rat_t alt_value = { value_meter, 1000 };
        memcpy(&altitude, &alt_value, sizeof(altitude));
        addExifTag(EXIFTAGID_GPS_ALTITUDE, EXIF_RATIONAL, 1, 1, &altitude);
        // set altitude ref
        mParameters.set(CameraParameters::KEY_GPS_ALTITUDE_REF, ref);
        addExifTag(EXIFTAGID_GPS_ALTITUDE_REF, EXIF_BYTE, 1, 1, &ref);
    }

    // set gps timestamp
    str = mParameters.get(CameraParameters::KEY_GPS_TIMESTAMP);
    if (str) {
        long value = atol(str);
        time_t unixTime;
        struct tm *UTCTimestamp;

        unixTime = (time_t)value;
        UTCTimestamp = gmtime(&unixTime);

        strftime(gpsDatestamp, sizeof(gpsDatestamp), "%Y:%m:%d", UTCTimestamp);
        addExifTag(EXIFTAGID_GPS_DATESTAMP, EXIF_ASCII,
            strlen(gpsDatestamp) + 1, 1, &gpsDatestamp);

        rat_t time_value[3] = { { (uint32_t)UTCTimestamp->tm_hour, 1 },
                                { (uint32_t)UTCTimestamp->tm_min, 1  },
                                { (uint32_t)UTCTimestamp->tm_sec, 1  } };

        memcpy(&gpsTimestamp, &time_value, sizeof(gpsTimestamp));
        addExifTag(EXIFTAGID_GPS_TIMESTAMP, EXIF_RATIONAL, 3, 1, &gpsTimestamp);
    }
}

void QualcommCameraHardware::setExifInfo(void)
{
    // set manufacturer & model
    property_get("ro.product.manufacturer", exif_maker, "QCOM-AA");
    addExifTag(EXIFTAGID_MAKER, EXIF_ASCII, strlen(exif_maker) + 1, 1, exif_maker);

    property_get("ro.product.model", exif_model, "QCAM-AA");
    addExifTag(EXIFTAGID_MODEL, EXIF_ASCII, strlen(exif_model) + 1, 1, exif_model);

    // set timestamp
    const char *date_str = mParameters.get(CameraParameters::KEY_EXIF_DATETIME);
    if (date_str) {
        strncpy(exif_date, date_str, 20);
        addExifTag(EXIFTAGID_EXIF_DATE_TIME_ORIGINAL, EXIF_ASCII, strlen(exif_date) + 1, 1, exif_date);
        addExifTag(EXIFTAGID_EXIF_DATE_TIME_CREATED, EXIF_ASCII, strlen(exif_date) + 1, 1, exif_date);
    } else {
        time_t rawtime;
        struct tm *timeinfo = NULL;
        memset(&rawtime, 0, sizeof(rawtime));
        time(&rawtime);
        timeinfo = localtime(&rawtime);
        if (timeinfo) {
            // write datetime according to EXIF Spec
            // "YYYY:MM:DD HH:MM:SS" (20 chars including \0)
            snprintf(exif_date, 20, "%04d:%02d:%02d %02d:%02d:%02d",
                timeinfo->tm_year + 1900, timeinfo->tm_mon + 1,
                timeinfo->tm_mday, timeinfo->tm_hour,
                timeinfo->tm_min, timeinfo->tm_sec);
            addExifTag(EXIFTAGID_EXIF_DATE_TIME_ORIGINAL, EXIF_ASCII, strlen(exif_date) + 1, 1, exif_date);
            addExifTag(EXIFTAGID_EXIF_DATE_TIME_CREATED, EXIF_ASCII, strlen(exif_date) + 1, 1, exif_date);
        }
    }

    // set gps
    setGpsParameters();

    // set flash
    const char *flash_str = mParameters.get(CameraParameters::KEY_FLASH_MODE);
    if (flash_str) {
        int is_flash_fired = 0;
        if (mCfgControl.mm_camera_get_parm(CAMERA_PARM_QUERY_FALSH4SNAP,
            &is_flash_fired) != MM_CAMERA_SUCCESS) {
            flashMode = FLASH_SNAP; //for No Flash support,bit 5 will be 1
        } else {
            if (!strcmp(flash_str, "on"))
                flashMode = 1;

            if (!strcmp(flash_str, "off"))
                flashMode = 0;

            if (!strcmp(flash_str, "auto")) {
                //for AUTO bits 3 and 4 will be 1
                //for flash fired bit 0 will be 1, else 0
                flashMode = FLASH_AUTO;
                if (is_flash_fired)
                   flashMode = (is_flash_fired >> 1) | flashMode;
            }
        }
        addExifTag(EXIFTAGID_FLASH, EXIF_SHORT, 1, 1, &flashMode);
    }
    // set focal length
    uint32_t focalLengthValue = (mParameters.getFloat(
        CameraParameters::KEY_FOCAL_LENGTH) * FOCAL_LENGTH_DECIMAL_PRECISON);
    rat_t focalLengthRational = {focalLengthValue, FOCAL_LENGTH_DECIMAL_PRECISON};
    memcpy(&focalLength, &focalLengthRational, sizeof(focalLengthRational));
    addExifTag(EXIFTAGID_FOCAL_LENGTH, EXIF_RATIONAL, 1, 1, &focalLength);

    // set iso speed rating
    const char *iso_str = mParameters.get(CameraParameters::KEY_ISO_MODE);
    int iso_value = attr_lookup(iso, sizeof(iso) / sizeof(str_map), iso_str);
    isoMode = iso_arr[iso_value];
    addExifTag(EXIFTAGID_ISO_SPEED_RATING, EXIF_SHORT, 1, 1, &isoMode);
}

bool QualcommCameraHardware::initZslParameter(void)
{
    ALOGV("%s: E", __FUNCTION__);
    mParameters.getPictureSize(&mPictureWidth, &mPictureHeight);
    ALOGV("initZslParamter E: picture size=%dx%d", mPictureWidth, mPictureHeight);
    if (updatePictureDimension(mParameters, mPictureWidth, mPictureHeight)) {
        mDimension.picture_width = mPictureWidth;
        mDimension.picture_height = mPictureHeight;
    }

    /* use the default thumbnail sizes */
    mZslParms.picture_width = mPictureWidth;
    mZslParms.picture_height = mPictureHeight;
    mZslParms.preview_width =  mDimension.display_width;
    mZslParms.preview_height = mDimension.display_height;
    mZslParms.useExternalBuffers = TRUE;
    /* fill main image size, thumbnail size, postview size into capture_params_t*/
    memset(&mZslCaptureParms, 0, sizeof(zsl_capture_params_t));
    mZslCaptureParms.thumbnail_height = mPostviewHeight;
    mZslCaptureParms.thumbnail_width = mPostviewWidth;
    ALOGV("Number of snapshot to capture: %d",numCapture);
    mZslCaptureParms.num_captures = numCapture;
    return true;
}

bool QualcommCameraHardware::initImageEncodeParameters(int size)
{
    ALOGV("%s: E", __FUNCTION__);
    memset(&mImageEncodeParms, 0, sizeof(encode_params_t));
    int jpeg_quality = mParameters.getInt(CameraParameters::KEY_JPEG_QUALITY);
    bool ret;
    if (jpeg_quality >= 0) {
        ALOGV("initJpegParameters, current jpeg main img quality =%d",
             jpeg_quality);
        //Application can pass quality of zero
        //when there is no back sensor connected.
        //as jpeg quality of zero is not accepted at
        //camera stack, pass default value.
        if (jpeg_quality == 0)
            jpeg_quality = 85;
        mImageEncodeParms.quality = jpeg_quality;
        ret = native_set_parms(CAMERA_PARM_JPEG_MAINIMG_QUALITY, sizeof(int), &jpeg_quality);
        if (!ret) {
            ALOGE("initJpegParametersX: failed to set main image quality");
            return false;
        }
    }

    int thumbnail_quality = mParameters.getInt(CameraParameters::KEY_JPEG_THUMBNAIL_QUALITY);
    if (thumbnail_quality >= 0) {
        //Application can pass quality of zero
        //when there is no back sensor connected.
        //as quality of zero is not accepted at
        //camera stack, pass default value.
        if (thumbnail_quality == 0)
            thumbnail_quality = 85;
        ALOGV("initJpegParameters, current jpeg thumbnail quality =%d",
            thumbnail_quality);
        /* TODO: check with mm-camera? */
        mImageEncodeParms.quality = thumbnail_quality;
        ret = native_set_parms(CAMERA_PARM_JPEG_THUMB_QUALITY, sizeof(int), &thumbnail_quality);
        if (!ret) {
          ALOGE("initJpegParameters X: failed to set thumbnail quality");
          return false;
        }
    }

    int rotation = mParameters.getInt(CameraParameters::KEY_ROTATION);

    if (mIs3DModeOn)
        rotation = 0;
    if (rotation >= 0) {
        ALOGV("initJpegParameters, rotation = %d", rotation);
        mImageEncodeParms.rotation = rotation;
    }

    setExifInfo();

    if (mUseJpegDownScaling) {
        ALOGV("initImageEncodeParameters: update main image");
        mImageEncodeParms.output_picture_width = mActualPictWidth;
        mImageEncodeParms.output_picture_height = mActualPictHeight;
    }
    mImageEncodeParms.cbcr_offset = mCbCrOffsetRaw;
    if (mPreviewFormat == CAMERA_YUV_420_NV21_ADRENO)
        mImageEncodeParms.cbcr_offset = mCbCrOffsetRaw;
    /* TODO: check this */
    mImageEncodeParms.y_offset = 0;
    for (int i = 0; i < size; i++) {
        memset(&mEncodeOutputBuffer[i], 0, sizeof(mm_camera_buffer_t));
        mEncodeOutputBuffer[i].ptr = (uint8_t *)mJpegMapped[i]->data;
        mEncodeOutputBuffer[i].filled_size = mJpegMaxSize;
        mEncodeOutputBuffer[i].size = mJpegMaxSize;
        mEncodeOutputBuffer[i].fd = mJpegfd[i];
        mEncodeOutputBuffer[i].offset = 0;
    }
    mImageEncodeParms.p_output_buffer = mEncodeOutputBuffer;
    mImageEncodeParms.exif_data = exif_data;
    mImageEncodeParms.exif_numEntries = exif_table_numEntries;

    mImageEncodeParms.format3d = mIs3DModeOn;
    return true;
}

bool QualcommCameraHardware::native_set_parms(
    mm_camera_parm_type_t type, uint16_t length, void *value)
{
    if (mCfgControl.mm_camera_set_parm(type,value) != MM_CAMERA_SUCCESS) {
        ALOGE("native_set_parms failed: type %d length %d error %s",
            type, length, strerror(errno));
        return false;
    }
    return true;

}
bool QualcommCameraHardware::native_set_parms(
    mm_camera_parm_type_t type, uint16_t length, void *value, int *result)
{
    mm_camera_status_t status;
    status = mCfgControl.mm_camera_set_parm(type,value);
    ALOGV("native_set_parms status = %d", status);
    if (status == MM_CAMERA_SUCCESS || status == MM_CAMERA_ERR_INVALID_OPERATION) {
        *result = status;
        return true;
    }
    ALOGE("%s: type %d length %d error %s, status %d", __FUNCTION__,
        type, length, strerror(errno), status);
    *result = status;
    return false;
}

static bool register_buf(int size,
    int cbcr_offset,
    int yoffset,
    int pmempreviewfd,
    uint32_t offset,
    uint8_t *buf,
    int pmem_type,
    bool vfe_can_write,
    bool register_buffer = true)
{
    struct msm_pmem_info pmemBuf;
    memset(&pmemBuf, 0, sizeof(pmemBuf));

    pmemBuf.type     = pmem_type;
    pmemBuf.fd       = pmempreviewfd;
    pmemBuf.vaddr    = buf;
    pmemBuf.offset   = offset;
    pmemBuf.len      = size;
    pmemBuf.y_off    = yoffset;
    pmemBuf.cbcr_off = cbcr_offset;
    pmemBuf.active   = vfe_can_write;

    ALOGV("register_buf:  reg = %d buffer = %p", !register_buffer, buf);
    if (native_start_ops(register_buffer ? CAMERA_OPS_REGISTER_BUFFER :
        CAMERA_OPS_UNREGISTER_BUFFER, &pmemBuf) < 0) {
        ALOGE("register_buf: MSM_CAM_IOCTL_(UN)REGISTER_PMEM  error %s", strerror(errno));
        return false;
    }

    return true;
}

void QualcommCameraHardware::runFrameThread(void *data)
{
    ALOGV("runFrameThread E");
    int CbCrOffset = PAD_TO_WORD(previewWidth * previewHeight);

    if (libmmcamera) {
        LINK_cam_frame(data);
    }

    //waiting for preview thread to complete before clearing of the buffers
    mPreviewThreadWaitLock.lock();
    while (mPreviewThreadRunning) {
        ALOGI("runframethread: waiting for preview  thread to complete.");
        mPreviewThreadWait.wait(mPreviewThreadWaitLock);
        ALOGI("initPreview: old preview thread completed.");
    }
    mPreviewThreadWaitLock.unlock();

    // Cancelling previewBuffers and returning them to display before stopping preview
    // This will ensure that all preview buffers are available for dequeing when
    //startPreview is called again with the same ANativeWindow object (snapshot case). If the
    //ANativeWindow is a new one(camera-camcorder switch case) because the app passed a new
    //surface then buffers will be re-allocated and not returned from the old pool.
    relinquishBuffers();
    mPreviewBusyQueue.flush();
    /* Flush the Free Q */
    LINK_camframe_release_all_frames(CAM_PREVIEW_FRAME);

    if (mIs3DModeOn != true) {
           int mBufferSize = previewWidth * previewHeight * 3/2;
           int mCbCrOffset = PAD_TO_WORD(previewWidth * previewHeight);
           ALOGE("unregistering all preview buffers");
            //unregister preview buffers. we are not deallocating here.
            for (int cnt = 0; cnt < mTotalPreviewBufferCount; ++cnt) {
                register_buf(mBufferSize,
                    mCbCrOffset,
                    0,
                    frames[cnt].fd,
                    0,
                    (uint8_t *)frames[cnt].buffer,
                    MSM_PMEM_PREVIEW,
                    false,
                    false);
            }
    }
    if (!mZslEnable) {
        if (mCurrentTarget == TARGET_MSM7630 ||
            mCurrentTarget == TARGET_QSD8250 ||
            mCurrentTarget == TARGET_MSM8660) {
            int CbCrOffset = PAD_TO_2K(mDimension.video_width  * mDimension.video_height);
            for (int cnt = 0; cnt < kRecordBufferCount; cnt++) {
                int type = MSM_PMEM_VIDEO;
                ALOGE("%s: unregister record buffers[%d] with camera driver", __FUNCTION__, cnt);
                if (recordframes) {
                    register_buf(mRecordFrameSize,
                        CbCrOffset, 0,
                        recordframes[cnt].fd,
                        0,
                        (uint8_t *)recordframes[cnt].buffer,
                        type,
                        false, false);
                    if (mRecordMapped[cnt]) {
                        mRecordMapped[cnt]->release(mRecordMapped[cnt]);
                        mRecordMapped[cnt] = NULL;
                        close(mRecordfd[cnt]);
                        if (mStoreMetaDataInFrame && (metadata_memory[cnt] != NULL)) {
                            struct encoder_media_buffer_type * packet =
                                (struct encoder_media_buffer_type  *)metadata_memory[cnt]->data;
                            native_handle_delete(const_cast<native_handle_t *>(packet->meta_handle));
                            metadata_memory[cnt]->release(metadata_memory[cnt]);
                            metadata_memory[cnt] = NULL;
                        }
#ifdef USE_ION
                        deallocate_ion_memory(&record_main_ion_fd[cnt], &record_ion_info_fd[cnt]);
#endif
                    }
                }
            }
        }
    }

    mFrameThreadWaitLock.lock();
    mFrameThreadRunning = false;
    mFrameThreadWait.signal();
    mFrameThreadWaitLock.unlock();

    ALOGV("runFrameThread X");
}

void QualcommCameraHardware::runPreviewThread(void *data)
{
    static int hfr_count = 0;
    msm_frame *frame = NULL;
    status_t retVal = NO_ERROR;
    android_native_buffer_t *buffer;
    buffer_handle_t *handle = NULL;
    int bufferIndex = 0;

    while ((frame = mPreviewBusyQueue.get()) != NULL) {
        mCallbackLock.lock();
        int msgEnabled = mMsgEnabled;
        camera_data_callback pcb = mDataCallback;
        void *pdata = mCallbackCookie;
        camera_data_timestamp_callback rcb = mDataCallbackTimestamp;
        void *rdata = mCallbackCookie;
        camera_data_callback mcb = mDataCallback;
        void *mdata = mCallbackCookie;
        mCallbackLock.unlock();

        // signal smooth zoom thread , that a new preview frame is available
        mSmoothzoomThreadWaitLock.lock();
        if (mSmoothzoomThreadRunning) {
            mSmoothzoomThreadWait.signal();
        }
        mSmoothzoomThreadWaitLock.unlock();

        // Find the offset within the heap of the current buffer.
        ssize_t offset_addr = 0; // TODO , use proper value
        common_crop_t *crop = (common_crop_t *) (frame->cropinfo);

        if (crop->in1_w != 0 && crop->in1_h != 0) {
            zoomCropInfo.left = (crop->out1_w - crop->in1_w + 1) / 2 - 1;
            zoomCropInfo.top = (crop->out1_h - crop->in1_h + 1) / 2 - 1;
            /* There can be scenarios where the in1_wXin1_h and
             * out1_wXout1_h are same. In those cases, reset the
             * x and y to zero instead of negative for proper zooming
             */
            if (zoomCropInfo.left < 0)
                zoomCropInfo.left = 0;
            if (zoomCropInfo.top < 0)
                zoomCropInfo.top = 0;
            zoomCropInfo.right = zoomCropInfo.left + crop->in1_w;
            zoomCropInfo.bottom = zoomCropInfo.top + crop->in1_h;
            mPreviewWindow->set_crop(mPreviewWindow,
                                    zoomCropInfo.left,
                                    zoomCropInfo.top,
                                    zoomCropInfo.right,
                                    zoomCropInfo.bottom);
            /* Set mResetOverlayCrop to true, so that when there is
             * no crop information, setCrop will be called
             * with zero crop values.
             */
            mResetWindowCrop = true;

        } else {
            // Reset zoomCropInfo variables. This will ensure that
            // stale values wont be used for postview
            zoomCropInfo.left = 0;
            zoomCropInfo.top = 0;
            zoomCropInfo.right = crop->in1_w;
            zoomCropInfo.bottom = crop->in1_h;
            /* This reset is required, if not, overlay driver continues
             * to use the old crop information for these preview
             * frames which is not the correct behavior. To avoid
             * multiple calls, reset once.
             */
            if (mResetWindowCrop == true) {
                mPreviewWindow->set_crop(mPreviewWindow,
                                    zoomCropInfo.left,
                                    zoomCropInfo.top,
                                    zoomCropInfo.right,
                                    zoomCropInfo.bottom);
                mResetWindowCrop = false;
            }
        }

        bufferIndex = mapBuffer(frame);
        if (bufferIndex >= 0) {
            if (pcb != NULL && (msgEnabled & CAMERA_MSG_PREVIEW_FRAME)) {
                int previewBufSize;
                /* for CTS : Forcing preview memory buffer lenth to be
                    'previewWidth * previewHeight * 3/2'. Needed when gralloc allocated extra memory.*/
                if ( mPreviewFormat == CAMERA_YUV_420_NV21) {
                    previewBufSize = previewWidth * previewHeight * 3/2;
                    camera_memory_t *previewMem = mGetMemory(frames[bufferIndex].fd, previewBufSize, 1, mCallbackCookie);
                    if (!previewMem || !previewMem->data) {
                        ALOGE("%s: mGetMemory failed.\n", __func__);
                    } else {
                        pcb(CAMERA_MSG_PREVIEW_FRAME,previewMem,0,NULL,pdata);
                        previewMem->release(previewMem);
                    }
                } else
                    pcb(CAMERA_MSG_PREVIEW_FRAME,(camera_memory_t *) mPreviewMapped[bufferIndex],0,NULL,pdata);
            }

            // TODO : may have to reutn proper frame as pcb
            mDisplayLock.lock();
            if (mPreviewWindow != NULL) {
                const char *str = mParameters.get(CameraParameters::KEY_VIDEO_HIGH_FRAME_RATE);
                if (str != NULL) {
                    int is_hfr_off = 0;
                    hfr_count++;
                    if (!strcmp(str, CameraParameters::VIDEO_HFR_OFF)) {
                        is_hfr_off = 1;
                        retVal = mPreviewWindow->enqueue_buffer(mPreviewWindow,
                                            frame_buffer[bufferIndex].buffer);
                    } else if (!strcmp(str, CameraParameters::VIDEO_HFR_2X)) {
                        hfr_count %= 2;
                    } else if (!strcmp(str, CameraParameters::VIDEO_HFR_3X)) {
                        hfr_count %= 3;
                    } else if (!strcmp(str, CameraParameters::VIDEO_HFR_4X)) {
                        hfr_count %= 4;
                    }
                    if (hfr_count == 0)
                        retVal = mPreviewWindow->enqueue_buffer(mPreviewWindow,
                                            frame_buffer[bufferIndex].buffer);
                    else if (!is_hfr_off)
                        retVal = mPreviewWindow->cancel_buffer(mPreviewWindow,
                                            frame_buffer[bufferIndex].buffer);
                } else
                    retVal = mPreviewWindow->enqueue_buffer(mPreviewWindow,
                                            frame_buffer[bufferIndex].buffer);
                if (retVal != NO_ERROR)
                    ALOGE("%s: Failed while queueing buffer %d for display."
                        " Error = %d", __FUNCTION__, frames[bufferIndex].fd, retVal);
                int stride;
                retVal = mPreviewWindow->dequeue_buffer(mPreviewWindow,
                                            &handle,&stride);
                private_handle_t *bhandle = (private_handle_t *)(*handle);
                if (retVal != NO_ERROR) {
                    ALOGE("%s: Failed while dequeueing buffer from display."
                        " Error = %d", __FUNCTION__, retVal);
                } else {
                    retVal = mPreviewWindow->lock_buffer(mPreviewWindow,handle);
                    //yyan todo use handle to find out buffer
                    if (retVal != NO_ERROR)
                        ALOGE("%s: Failed while dequeueing buffer from"
                            "display. Error = %d", __FUNCTION__, retVal);
                }
            }
            mDisplayLock.unlock();
        } else
            ALOGE("Could not find the buffer");

        // If output  is NOT enabled (targets otherthan 7x30 , 8x50 and 8x60 currently..)

        nsecs_t timeStamp = nsecs_t(frame->ts.tv_sec)*1000000000LL + frame->ts.tv_nsec;

        if (mCurrentTarget != TARGET_MSM7630 &&
            mCurrentTarget != TARGET_QSD8250 &&
            mCurrentTarget != TARGET_MSM8660) {
            int flagwait = 1;
            if (rcb != NULL && (msgEnabled & CAMERA_MSG_VIDEO_FRAME) && (record_flag)) {
                if (mStoreMetaDataInFrame) {
                    flagwait = 1;
                    if (metadata_memory[bufferIndex]!= NULL)
                        rcb(timeStamp, CAMERA_MSG_VIDEO_FRAME, metadata_memory[bufferIndex],0,rdata);
                    else
                        flagwait = 0;
                } else {
                    rcb(timeStamp, CAMERA_MSG_VIDEO_FRAME, mPreviewMapped[bufferIndex],0, rdata);
                }
                if (flagwait) {
                    Mutex::Autolock rLock(&mRecordFrameLock);
                    if (mReleasedRecordingFrame != true) {
                        mRecordWait.wait(mRecordFrameLock);
                    }
                    mReleasedRecordingFrame = false;
                }
            }
        }

        if (mCurrentTarget == TARGET_MSM8660) {
            mMetaDataWaitLock.lock();
            if (mFaceDetectOn == true && mSendMetaData == true) {
                mSendMetaData = false;
                fd_roi_t *fd = (fd_roi_t *)(frame->roi_info.info);
                int faces_detected = fd->rect_num;
                int max_faces_detected = MAX_ROI * 4;
                int array[max_faces_detected + 1];

                array[0] = faces_detected * 4;
                for (int i = 1, j = 0;j < MAX_ROI; j++, i = i + 4) {
                    if (j < faces_detected) {
                        array[i]   = fd->faces[j].x;
                        array[i+1] = fd->faces[j].y;
                        array[i+2] = fd->faces[j].dx;
                        array[i+3] = fd->faces[j].dx;
                    } else {
                        array[i]   = -1;
                        array[i+1] = -1;
                        array[i+2] = -1;
                        array[i+3] = -1;
                    }
                }
            } else {
                mMetaDataWaitLock.unlock();
            }
        }
        bufferIndex = mapFrame(handle);
        if (bufferIndex >= 0) {
            LINK_camframe_add_frame(CAM_PREVIEW_FRAME, &frames[bufferIndex]);
        } else {
            ALOGE("Could not find the Frame");

            // Special Case: Stoppreview is issued which causes thumbnail buffer
            // to be cancelled. Frame thread has still not exited. In preview thread
            // dequeue returns incorrect buffer id (previously cancelled thumbnail buffer)
            // This will throw error "Could not find frame". We need to cancel the incorrectly
            // dequeued buffer here to ensure that all buffers are available for the next
            // startPreview call.

            mDisplayLock.lock();
            ALOGV(" error Cancelling preview buffers  ");
            retVal = mPreviewWindow->cancel_buffer(mPreviewWindow, handle);
            if (retVal != NO_ERROR)
                ALOGE("%s:  cancelBuffer failed for buffer", __FUNCTION__);
            mDisplayLock.unlock();
        }
    }
    mPreviewThreadWaitLock.lock();
    mPreviewThreadRunning = false;
    mPreviewThreadWait.signal();
    mPreviewThreadWaitLock.unlock();
}

int QualcommCameraHardware::mapBuffer(struct msm_frame *frame)
{
    int ret = -1;
    for (int cnt = 0; cnt < mTotalPreviewBufferCount; cnt++) {
        if (frame_buffer[cnt].frame->buffer == frame->buffer) {
            ret = cnt;
            break;
        }
    }
    return ret;
}

int QualcommCameraHardware::mapvideoBuffer(struct msm_frame *frame)
{
    int ret = -1;
    for (int cnt = 0; cnt < kRecordBufferCount; cnt++) {
        if ((unsigned int)mRecordMapped[cnt]->data == (unsigned int)frame->buffer) {
            ret = cnt;
            ALOGE("found match returning %d", ret);
            break;
        }
    }
    return ret;
}

int QualcommCameraHardware::mapRawBuffer(struct msm_frame *frame)
{
    int ret = -1;
    for (int cnt = 0; cnt < (mZslEnable? MAX_SNAPSHOT_BUFFERS : numCapture); cnt++) {
        if ((unsigned int)mRawMapped[cnt]->data == (unsigned int)frame->buffer) {
            ret = cnt;
            ALOGE("found match returning %d", ret);
            break;
        }
    }
    return ret;
}

int QualcommCameraHardware::mapThumbnailBuffer(struct msm_frame *frame)
{
    int ret = -1;
    for (int cnt = 0; cnt < (mZslEnable? MAX_SNAPSHOT_BUFFERS : numCapture); cnt++) {
        if ((unsigned int)(uint8_t *)mThumbnailMapped[cnt] == (unsigned int)frame->buffer) {
            ret = cnt;
            ALOGE("found match returning %d", ret);
            break;
        }
    }
    if (ret < 0) ALOGE("mapThumbnailBuffer, could not find match");
        return ret;
}

int QualcommCameraHardware::mapJpegBuffer(mm_camera_buffer_t *encode_buffer)
{
    int ret = -1;
    for (int cnt = 0; cnt < (mZslEnable? MAX_SNAPSHOT_BUFFERS : numCapture); cnt++) {
        if ((unsigned int)mJpegMapped[cnt]->data == (unsigned int)encode_buffer->ptr) {
            ret = cnt;
            ALOGE("found match returning %d", ret);
            break;
        }
    }
    return ret;
}

int QualcommCameraHardware::mapFrame(buffer_handle_t *buffer)
{
    int ret = -1;
    for (int cnt = 0; cnt < mTotalPreviewBufferCount; cnt++) {
        if (frame_buffer[cnt].buffer == buffer) {
            ret = cnt;
            break;
        }
    }
    return ret;
}

void *preview_thread(void *user)
{
    ALOGI("preview_thread E");
    QualcommCameraHardware  *obj = QualcommCameraHardware::getInstance();
    if (obj != 0) {
        obj->runPreviewThread(user);
    }
    else ALOGE("not starting preview thread: the object went away!");
    ALOGI("preview_thread X");
    return NULL;
}

void *hfr_thread(void *user)
{
    ALOGI("hfr_thread E");
    QualcommCameraHardware *obj = QualcommCameraHardware::getInstance();
    if (obj != 0) {
        obj->runHFRThread(user);
    }
    else ALOGE("not starting hfr thread: the object went away!");
    ALOGI("hfr_thread X");
    return NULL;
}

void QualcommCameraHardware::runHFRThread(void *data)
{
    ALOGD("runHFRThread E");
    mInHFRThread = true;

    ALOGI("%s: stopping Preview", __FUNCTION__);
    stopPreviewInternal();

    // Release thumbnail Buffers
    if ( mPreviewWindow != NULL ) {
        private_handle_t *handle;
        for (int cnt = 0; cnt < (mZslEnable? (MAX_SNAPSHOT_BUFFERS-2) : numCapture); cnt++) {
            if (mPreviewWindow != NULL && mThumbnailBuffer[cnt] != NULL) {
                handle = (private_handle_t *)(*mThumbnailBuffer[cnt]);
                ALOGV("%s:  Cancelling postview buffer %d ", __FUNCTION__, handle->fd);
                ALOGV("runHfrThread : display lock");
                mDisplayLock.lock();

                status_t retVal = mPreviewWindow->cancel_buffer(mPreviewWindow,
                                                              mThumbnailBuffer[cnt]);
                if (retVal != NO_ERROR)
                    ALOGE("%s: cancelBuffer failed for postview buffer %d",
                                                     __FUNCTION__, handle->fd);
                // unregister , unmap and release as well
                int mBufferSize = previewWidth * previewHeight * 3/2;
                int mCbCrOffset = PAD_TO_WORD(previewWidth * previewHeight);
                if (mThumbnailMapped[cnt] && (mSnapshotFormat == PICTURE_FORMAT_JPEG)) {
                    ALOGE("%s:  Unregistering Thumbnail Buffer %d ", __FUNCTION__, handle->fd);
                    register_buf(mBufferSize,
                        mCbCrOffset, 0,
                        handle->fd,
                        0,
                        (uint8_t *)mThumbnailMapped[cnt],
                        MSM_PMEM_THUMBNAIL,
                        false, false);
                    if (munmap(mThumbnailMapped[cnt],handle->size ) == -1) {
                      ALOGE("StopPreview : Error un-mmapping the thumbnail buffer %p", index);
                    }
                    mThumbnailMapped[cnt] = NULL;
                }
                mThumbnailBuffer[cnt] = NULL;
                ALOGV("runHfrThread : display unlock");
                mDisplayLock.unlock();
          }
       }
    }

    ALOGV("%s: setting parameters", __FUNCTION__);
    setParameters(mParameters);
    ALOGV("%s: starting Preview", __FUNCTION__);
    if ( mPreviewWindow == NULL) {
        startPreviewInternal();
    } else {
        getBuffersAndStartPreview();
    }

    mHFRMode = false;
    mInHFRThread = false;
}

void QualcommCameraHardware::runVideoThread(void *data)
{
    ALOGD("runVideoThread E");
    msm_frame* vframe = NULL;

    while (true) {
        pthread_mutex_lock(&(g_busy_frame_queue.mut));

        // Exit the thread , in case of stop recording..
        mVideoThreadWaitLock.lock();
        if (mVideoThreadExit) {
            ALOGV("Exiting video thread..");
            mVideoThreadWaitLock.unlock();
            pthread_mutex_unlock(&(g_busy_frame_queue.mut));
            break;
        }
        mVideoThreadWaitLock.unlock();

        ALOGV("in video_thread : wait for video frame ");
        // check if any frames are available in busyQ and give callback to
        // services/video encoder
        cam_frame_wait_video();
        ALOGV("video_thread, wait over..");

        // Exit the thread , in case of stop recording..
        mVideoThreadWaitLock.lock();
        if (mVideoThreadExit) {
            ALOGV("Exiting video thread..");
            mVideoThreadWaitLock.unlock();
            pthread_mutex_unlock(&(g_busy_frame_queue.mut));
            break;
        }
        mVideoThreadWaitLock.unlock();

        // Get the video frame to be encoded
        vframe = cam_frame_get_video ();
        pthread_mutex_unlock(&(g_busy_frame_queue.mut));
        ALOGE("in video_thread : got video frame %p",vframe);

        if (vframe != NULL) {
            /* Extract the timestamp of this frame */
            nsecs_t timeStamp = nsecs_t(vframe->ts.tv_sec)*1000000000LL + vframe->ts.tv_nsec;

            ALOGV("in video_thread : got video frame, before if check giving frame to services/encoder");
            mCallbackLock.lock();
            int msgEnabled = mMsgEnabled;
            camera_data_timestamp_callback rcb = mDataCallbackTimestamp;
            void *rdata = mCallbackCookie;
            mCallbackLock.unlock();

            /* When 3D mode is ON, the video thread will be ON even in preview
             * mode. We need to distinguish when recording is started. So, when
             * 3D mode is ON, check for the recordingState (which will be set
             * with start recording and reset in stop recording), before
             * calling rcb.
             */
            int index = mapvideoBuffer(vframe);
            if (!mIs3DModeOn) {
                record_buffers_tracking_flag[index] = true;
                if (rcb != NULL && (msgEnabled & CAMERA_MSG_VIDEO_FRAME) ) {
                    ALOGV("in video_thread : got video frame, giving frame to services/encoder index = %d", index);
                    if (mStoreMetaDataInFrame) {
                        rcb(timeStamp, CAMERA_MSG_VIDEO_FRAME, metadata_memory[index],0,rdata);
                    } else {
                        rcb(timeStamp, CAMERA_MSG_VIDEO_FRAME, mRecordMapped[index],0,rdata);
                    }
                }
            }
        } else ALOGE("in video_thread get frame returned null");

    } // end of while loop

    mVideoThreadWaitLock.lock();
    mVideoThreadRunning = false;
    mVideoThreadWait.signal();
    mVideoThreadWaitLock.unlock();

    ALOGV("runVideoThread X");
}

void *video_thread(void *user)
{
    ALOGV("video_thread E");

    QualcommCameraHardware *obj = QualcommCameraHardware::getInstance();
    if (obj != 0) {
        obj->runVideoThread(user);
    }
    else ALOGE("not starting video thread: the object went away!");
    ALOGV("video_thread X");
    return NULL;
}

void *frame_thread(void *user)
{
    ALOGD("frame_thread E");

    QualcommCameraHardware *obj = QualcommCameraHardware::getInstance();
    if (obj != 0) {
        obj->runFrameThread(user);
    }
    else ALOGW("not starting frame thread: the object went away!");
    ALOGD("frame_thread X");
    return NULL;
}

static int parse_size(const char *str, int &width, int &height)
{
    // Find the width.
    char *end;
    int w = (int)strtol(str, &end, 10);
    // If an 'x' or 'X' does not immediately follow, give up.
    if ( (*end != 'x') && (*end != 'X') )
        return -1;

    // Find the height, immediately after the 'x'.
    int h = (int)strtol(end+1, 0, 10);

    width = w;
    height = h;

    return 0;
}

#ifdef USE_ION
int QualcommCameraHardware::allocate_ion_memory(int *main_ion_fd, struct ion_allocation_data* alloc,
     struct ion_fd_data* ion_info_fd, int ion_type, int size, int *memfd)
{
    int rc = 0;
    struct ion_handle_data handle_data;

    *main_ion_fd = open("/dev/ion", O_RDONLY | O_SYNC);
    if (*main_ion_fd < 0) {
      ALOGE("Ion dev open failed\n");
      ALOGE("Error is %s\n", strerror(errno));
      goto ION_OPEN_FAILED;
    }
    alloc->len = size;
    /* to make it page size aligned */
    alloc->len = (alloc->len + 4095) & (~4095);
    alloc->align = 4096;
    alloc->heap_mask = (0x1 << ion_type);
    alloc->flags = ~ION_SECURE;

    rc = ioctl(*main_ion_fd, ION_IOC_ALLOC, alloc);
    if (rc < 0) {
      ALOGE("ION allocation failed\n");
      goto ION_ALLOC_FAILED;
    }

    ion_info_fd->handle = alloc->handle;
    rc = ioctl(*main_ion_fd, ION_IOC_SHARE, ion_info_fd);
    if (rc < 0) {
      ALOGE("ION map failed %s\n", strerror(errno));
      goto ION_MAP_FAILED;
    }
    *memfd = ion_info_fd->fd;
    return 0;

ION_MAP_FAILED:
    handle_data.handle = ion_info_fd->handle;
    ioctl(*main_ion_fd, ION_IOC_FREE, &handle_data);
ION_ALLOC_FAILED:
    close(*main_ion_fd);
ION_OPEN_FAILED:
    return -1;
}

int QualcommCameraHardware::deallocate_ion_memory(int *main_ion_fd, struct ion_fd_data* ion_info_fd)
{
    struct ion_handle_data handle_data;
    int rc = 0;

    handle_data.handle = ion_info_fd->handle;
    ioctl(*main_ion_fd, ION_IOC_FREE, &handle_data);
    close(*main_ion_fd);
    return rc;
}
#endif

bool QualcommCameraHardware::initPreview()
{
    mParameters.getPreviewSize(&previewWidth, &previewHeight);
    const char *recordSize = mParameters.get(CameraParameters::KEY_VIDEO_SIZE);
    ALOGD("%s Got preview dimension as %d x %d ", __func__, previewWidth, previewHeight);
    if (!recordSize) {
         //If application didn't set this parameter string, use the values from
         //getPreviewSize() as video dimensions.
         ALOGV("No Record Size requested, use the preview dimensions");
         videoWidth = previewWidth;
         videoHeight = previewHeight;
     } else {
         //Extract the record witdh and height that application requested.
         if (!parse_size(recordSize, videoWidth, videoHeight)) {
             //VFE output1 shouldn't be greater than VFE output2.
             if ( (previewWidth > videoWidth) || (previewHeight > videoHeight)) {
                 //Set preview sizes as record sizes.
                 ALOGI("Preview size %dx%d is greater than record size %dx%d,\
                    resetting preview size to record size",previewWidth,\
                      previewHeight, videoWidth, videoHeight);
                 previewWidth = videoWidth;
                 previewHeight = videoHeight;
                 mParameters.setPreviewSize(previewWidth, previewHeight);
             }
             if ( (mCurrentTarget != TARGET_MSM7630)
                 && (mCurrentTarget != TARGET_QSD8250)
                  && (mCurrentTarget != TARGET_MSM8660) ) {
                 //For Single VFE output targets, use record dimensions as preview dimensions.
                 previewWidth = videoWidth;
                 previewHeight = videoHeight;
                 mParameters.setPreviewSize(previewWidth, previewHeight);
             }
         } else {
             ALOGE("initPreview X: failed to parse parameter record-size (%s)", recordSize);
             return false;
         }
     }

     mDimension.display_width = previewWidth;
     mDimension.display_height= previewHeight;
     mDimension.ui_thumbnail_width =
             thumbnail_sizes[DEFAULT_THUMBNAIL_SETTING].width;
     mDimension.ui_thumbnail_height =
             thumbnail_sizes[DEFAULT_THUMBNAIL_SETTING].height;

    ALOGV("initPreview E: preview size=%dx%d videosize = %d x %d", previewWidth, previewHeight, videoWidth, videoHeight );

    if ( ( mCurrentTarget == TARGET_MSM7630 ) || (mCurrentTarget == TARGET_QSD8250) || (mCurrentTarget == TARGET_MSM8660)) {
        mDimension.video_width = CEILING16(videoWidth);
        /* Backup the video dimensions, as video dimensions in mDimension
         * will be modified when DIS is supported. Need the actual values
         * to pass ap part of VPE config
         */
        videoWidth = mDimension.video_width;
        mDimension.video_height = videoHeight;
        ALOGV("initPreview : preview size=%dx%d videosize = %d x %d", previewWidth, previewHeight,
          mDimension.video_width, mDimension.video_height);
    }

    // See comments in deinitPreview() for why we have to wait for the frame
    // thread here, and why we can't use pthread_join().
    mFrameThreadWaitLock.lock();
    while (mFrameThreadRunning) {
        ALOGI("initPreview: waiting for old frame thread to complete.");
        mFrameThreadWait.wait(mFrameThreadWaitLock);
        ALOGI("initPreview: old frame thread completed.");
    }
    mFrameThreadWaitLock.unlock();

    mInSnapshotModeWaitLock.lock();
    while (mInSnapshotMode) {
        ALOGI("initPreview: waiting for snapshot mode to complete.");
        mInSnapshotModeWait.wait(mInSnapshotModeWaitLock);
        ALOGI("initPreview: snapshot mode completed.");
    }
    mInSnapshotModeWaitLock.unlock();

    mPreviewFrameSize = previewWidth * previewHeight * 3/2;
    int CbCrOffset = PAD_TO_WORD(previewWidth * previewHeight);

    //Pass the yuv formats, display dimensions,
    //so that vfe will be initialized accordingly.
    mDimension.display_luma_width = previewWidth;
    mDimension.display_luma_height = previewHeight;
    mDimension.display_chroma_width = previewWidth;
    mDimension.display_chroma_height = previewHeight;
    if (mPreviewFormat == CAMERA_YUV_420_NV21_ADRENO) {
        mPreviewFrameSize = PAD_TO_4K(CEILING32(previewWidth) * CEILING32(previewHeight)) +
                                     2 * (CEILING32(previewWidth/2) * CEILING32(previewHeight/2));
        CbCrOffset = PAD_TO_4K(CEILING32(previewWidth) * CEILING32(previewHeight));
        mDimension.prev_format = CAMERA_YUV_420_NV21_ADRENO;
        mDimension.display_luma_width = CEILING32(previewWidth);
        mDimension.display_luma_height = CEILING32(previewHeight);
        mDimension.display_chroma_width = 2 * CEILING32(previewWidth/2);
        //Chroma Height is not needed as of now. Just sending with other dimensions.
        mDimension.display_chroma_height = CEILING32(previewHeight/2);
    }
    ALOGV("mDimension.prev_format = %d", mDimension.prev_format);
    ALOGV("mDimension.display_luma_width = %d", mDimension.display_luma_width);
    ALOGV("mDimension.display_luma_height = %d", mDimension.display_luma_height);
    ALOGV("mDimension.display_chroma_width = %d", mDimension.display_chroma_width);
    ALOGV("mDimension.display_chroma_height = %d", mDimension.display_chroma_height);

    //set DIS value to get the updated video width and height to calculate
    //the required record buffer size
    if (mVpeEnabled) {
        bool status = setDIS();
        if (status) {
            ALOGE("Failed to set DIS");
            return false;
        }
    }

    //Pass the original video width and height and get the required width
    //and height for record buffer allocation
    mDimension.orig_video_width = videoWidth;
    mDimension.orig_video_height = videoHeight;
    if (mZslEnable) {
        //Limitation of ZSL  where the thumbnail and display dimensions should be the same
        mDimension.ui_thumbnail_width = mDimension.display_width;
        mDimension.ui_thumbnail_height = mDimension.display_height;
        mParameters.getPictureSize(&mPictureWidth, &mPictureHeight);
        if (updatePictureDimension(mParameters, mPictureWidth,
          mPictureHeight)) {
          mDimension.picture_width = mPictureWidth;
          mDimension.picture_height = mPictureHeight;
        }
    }
    // mDimension will be filled with thumbnail_width, thumbnail_height,
    // orig_picture_dx, and orig_picture_dy after this function call. We need to
    // keep it for jpeg_encoder_encode.
    bool ret = native_set_parms(CAMERA_PARM_DIMENSION,
                               sizeof(cam_ctrl_dimension_t), &mDimension);

    if ( ( mCurrentTarget == TARGET_MSM7630 ) || (mCurrentTarget == TARGET_QSD8250) || (mCurrentTarget == TARGET_MSM8660)) {

        // Allocate video buffers after allocating preview buffers.
        bool status = initRecord();
        if (status != true) {
            ALOGE("Failed to allocate video bufers");
            return false;
        }
    }

    if (ret) {
        if (mIs3DModeOn != true) {
            mPreviewBusyQueue.init();
            LINK_camframe_release_all_frames(CAM_PREVIEW_FRAME);
            for (int i= ACTIVE_PREVIEW_BUFFERS; i < kPreviewBufferCount; i++)
                LINK_camframe_add_frame(CAM_PREVIEW_FRAME,&frames[i]);

            mPreviewThreadWaitLock.lock();
            pthread_attr_t pattr;
            pthread_attr_init(&pattr);
            pthread_attr_setdetachstate(&pattr, PTHREAD_CREATE_DETACHED);

            mPreviewThreadRunning = !pthread_create(&mPreviewThread,
                                      &pattr,
                                      preview_thread,
                                      (void*)NULL);
            ret = mPreviewThreadRunning;
            mPreviewThreadWaitLock.unlock();

            if (ret == false)
                return ret;
        }

        mFrameThreadWaitLock.lock();
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        if (mIs3DModeOn) {
            camframeParams.cammode = CAMERA_MODE_3D;
        } else {
            camframeParams.cammode = CAMERA_MODE_2D;
        }
        LINK_cam_frame_set_exit_flag(0);

        mFrameThreadRunning = !pthread_create(&mFrameThread,
                                              &attr,
                                              frame_thread,
                                              &camframeParams);
        ret = mFrameThreadRunning;
        mFrameThreadWaitLock.unlock();
        LINK_wait_cam_frame_thread_ready();
    }

    ALOGV("initPreview X: %d", ret);
    return ret;
}

void QualcommCameraHardware::deinitPreview(void)
{
    ALOGI("deinitPreview E");

    mPreviewBusyQueue.deinit();

    // When we call deinitPreview(), we signal to the frame thread that it
    // needs to exit, but we DO NOT WAIT for it to complete here.  The problem
    // is that deinitPreview is sometimes called from the frame-thread's
    // callback, when the refcount on the Camera client reaches zero.  If we
    // called pthread_join(), we would deadlock.  So, we just call
    // LINK_camframe_terminate() in deinitPreview(), which makes sure that
    // after the preview callback returns, the camframe thread will exit.  We
    // could call pthread_join() in initPreview() to join the last frame
    // thread.  However, we would also have to call pthread_join() in release
    // as well, shortly before we destroy the object; this would cause the same
    // deadlock, since release(), like deinitPreview(), may also be called from
    // the frame-thread's callback.  This we have to make the frame thread
    // detached, and use a separate mechanism to wait for it to complete.

    LINK_camframe_terminate();
    ALOGI("deinitPreview X");
}

bool QualcommCameraHardware::initRawSnapshot()
{
    ALOGV("initRawSnapshot E");

    //get width and height from Dimension Object
    bool ret = native_set_parms(CAMERA_PARM_DIMENSION,
                               sizeof(cam_ctrl_dimension_t), &mDimension);
    if (!ret) {
        ALOGE("initRawSnapshot X: failed to set dimension");
        return false;
    }
    int rawSnapshotSize = mDimension.raw_picture_height *
                           mDimension.raw_picture_width;

    ALOGV("raw_snapshot_buffer_size = %d, raw_picture_height = %d, "\
         "raw_picture_width = %d",
          rawSnapshotSize, mDimension.raw_picture_height,
          mDimension.raw_picture_width);

    // Create Memory for Raw Snapshot
    if ( createSnapshotMemory(numCapture, numCapture, false, PICTURE_FORMAT_RAW) == false ) {
        ALOGE("ERROR :  initRawSnapshot , createSnapshotMemory failed");
        return false;
    }

    mRawCaptureParms.num_captures = 1;
    mRawCaptureParms.raw_picture_width = mDimension.raw_picture_width;
    mRawCaptureParms.raw_picture_height = mDimension.raw_picture_height;

    ALOGV("initRawSnapshot X");
    return true;

}

bool QualcommCameraHardware::initZslBuffers(bool initJpegHeap)
{
    ALOGE("Init ZSL buffers E");
    int postViewBufferSize;

    mPostviewWidth = mDimension.display_width;
    mPostviewHeight =  mDimension.display_height;

    //postview buffer initialization
    postViewBufferSize  = mPostviewWidth * mPostviewHeight * 3 / 2;
    int CbCrOffsetPostview = PAD_TO_WORD(mPostviewWidth * mPostviewHeight);
    if (mPreviewFormat == CAMERA_YUV_420_NV21_ADRENO) {
        postViewBufferSize  = PAD_TO_4K(CEILING32(mPostviewWidth) * CEILING32(mPostviewHeight)) +
                                  2 * (CEILING32(mPostviewWidth/2) * CEILING32(mPostviewHeight/2));
        int CbCrOffsetPostview = PAD_TO_4K(CEILING32(mPostviewWidth) * CEILING32(mPostviewHeight));
    }

    //Snapshot buffer initialization
    mRawSize = mPictureWidth * mPictureHeight * 3 / 2;
    mCbCrOffsetRaw = PAD_TO_WORD(mPictureWidth * mPictureHeight);
    if (mPreviewFormat == CAMERA_YUV_420_NV21_ADRENO) {
        mRawSize = PAD_TO_4K(CEILING32(mPictureWidth) * CEILING32(mPictureHeight)) +
                            2 * (CEILING32(mPictureWidth/2) * CEILING32(mPictureHeight/2));
        mCbCrOffsetRaw = PAD_TO_4K(CEILING32(mPictureWidth) * CEILING32(mPictureHeight));
    }

    //Jpeg buffer initialization
    if ( mCurrentTarget == TARGET_MSM7627 ||
       (mCurrentTarget == TARGET_MSM7625A ||
        mCurrentTarget == TARGET_MSM7627A))
        mJpegMaxSize = CEILING16(mPictureWidth) * CEILING16(mPictureHeight) * 3 / 2;
    else {
        mJpegMaxSize = mPictureWidth * mPictureHeight * 3 / 2;
        if (mPreviewFormat == CAMERA_YUV_420_NV21_ADRENO) {
            mJpegMaxSize =
               PAD_TO_4K(CEILING32(mPictureWidth) * CEILING32(mPictureHeight)) +
                    2 * (CEILING32(mPictureWidth/2) * CEILING32(mPictureHeight/2));
        }
    }

    cam_buf_info_t buf_info;
    int yOffset = 0;
    buf_info.resolution.width = mPictureWidth;
    buf_info.resolution.height = mPictureHeight;
    if (mPreviewFormat != CAMERA_YUV_420_NV21_ADRENO) {
        mCfgControl.mm_camera_get_parm(CAMERA_PARM_BUFFER_INFO, &buf_info);
        mRawSize = buf_info.size;
        mJpegMaxSize = mRawSize;
        mCbCrOffsetRaw = buf_info.cbcr_offset;
        yOffset = buf_info.yoffset;
    }

    ALOGV("initZslBuffer: initializing mRawHeap.");
    //Main Raw Image
    if ( createSnapshotMemory(MAX_SNAPSHOT_BUFFERS, MAX_SNAPSHOT_BUFFERS, initJpegHeap) == false ) {
        ALOGE("ERROR :  initZslraw , createSnapshotMemory failed");
        return false;
    }
    /* frame all the exif and encode information into encode_params_t */
    initImageEncodeParameters(MAX_SNAPSHOT_BUFFERS);

    ALOGV("initZslRaw X");
    return true;
}

bool QualcommCameraHardware::deinitZslBuffers()
{
    ALOGE("deinitZslBuffers E");
    for (int cnt = 0; cnt < (mZslEnable? MAX_SNAPSHOT_BUFFERS : numCapture); cnt++) {
        if (NULL != mRawMapped[cnt]) {
            ALOGE("Unregister MAIN_IMG");
            register_buf(mJpegMaxSize,
                mCbCrOffsetRaw,0,
                mRawfd[cnt],0,
                (uint8_t *)mRawMapped[cnt]->data,
                MSM_PMEM_MAINIMG,
                0, 0);
            mRawMapped[cnt]->release(mRawMapped[cnt]);
            mRawMapped[cnt] = NULL;
            close(mRawfd[cnt]);
#ifdef USE_ION
            deallocate_ion_memory(&raw_main_ion_fd[cnt], &raw_ion_info_fd[cnt]);
#endif
        }
    }
    for (int cnt = 0; cnt < (mZslEnable? (MAX_SNAPSHOT_BUFFERS) : numCapture); cnt++) {
        if (mJpegMapped[cnt]) {
            mJpegMapped[cnt]->release(mJpegMapped[cnt]);
            mJpegMapped[cnt] = NULL;
        }
    }
    ALOGE("deinitZslBuffers X");
    return true;
}

bool QualcommCameraHardware::createSnapshotMemory (int numberOfRawBuffers, int numberOfJpegBuffers,
                                                   bool initJpegHeap, int snapshotFormat)
{
    int ret;
#ifdef USE_ION
    int ion_heap = ION_CP_MM_HEAP_ID;
#else
    const char *pmem_region;
    if (mCurrentTarget == TARGET_MSM8660) {
        pmem_region = "/dev/pmem_smipool";
    } else {
        pmem_region = "/dev/pmem_adsp";
    }
#endif

    if (snapshotFormat == PICTURE_FORMAT_JPEG) {
        // Create Raw memory for snapshot
        for (int cnt = 0; cnt < numberOfRawBuffers; cnt++) {
#ifdef USE_ION
            if (allocate_ion_memory(&raw_main_ion_fd[cnt], &raw_alloc[cnt], &raw_ion_info_fd[cnt],
                                    ion_heap, mJpegMaxSize, &mRawfd[cnt]) < 0) {
                ALOGE("%s: allocate ion memory failed!\n", __func__);
                return NULL;
            }
#else
            mRawfd[cnt] = open(pmem_region, O_RDWR|O_SYNC);
            if (mRawfd[cnt] <= 0) {
                ALOGE("%s: Open device %s failed!\n",__func__, pmem_region);
                    return false;
            }
#endif
            ALOGE("%s  Raw memory index: %d , fd is %d ", __func__, cnt, mRawfd[cnt]);
            mRawMapped[cnt] = mGetMemory(mRawfd[cnt], mJpegMaxSize, 1, mCallbackCookie);
            if (mRawMapped[cnt] == NULL) {
                ALOGE("Failed to get camera memory for mRawMapped heap index: %d", cnt);
                return false;
            } else {
                ALOGE("Received following info for raw mapped data:%p,handle:%p, size:%d,release:%p",
                mRawMapped[cnt]->data ,mRawMapped[cnt]->handle, mRawMapped[cnt]->size, mRawMapped[cnt]->release);
            }
            // Register Raw frames
            ALOGE("Registering buffer %d with fd :%d with kernel",cnt,mRawfd[cnt]);
            int active = (cnt < ACTIVE_ZSL_BUFFERS);  // TODO check ?
            register_buf(mJpegMaxSize,
                mCbCrOffsetRaw,
                mYOffset,
                mRawfd[cnt],0,
                (uint8_t *)mRawMapped[cnt]->data,
                MSM_PMEM_MAINIMG,
                active);
        }
        // Create Jpeg memory for snapshot
        if (initJpegHeap) {
            for (int cnt = 0; cnt < numberOfJpegBuffers; cnt++) {
                ALOGE("%s  Jpeg memory index: %d , fd is %d ", __func__, cnt, mJpegfd[cnt]);
                mJpegMapped[cnt] = mGetMemory(-1, mJpegMaxSize, 1, mCallbackCookie);
                if (mJpegMapped[cnt] == NULL) {
                    ALOGE("Failed to get camera memory for mJpegMapped heap index: %d", cnt);
                    return false;
                } else {
                    ALOGE("Received following info for jpeg mapped data:%p,handle:%p, size:%d,release:%p",
                        mJpegMapped[cnt]->data ,mJpegMapped[cnt]->handle, mJpegMapped[cnt]->size, mJpegMapped[cnt]->release);
                }
            }
        }
        // Lock Thumbnail buffers, and register them
        ALOGE("Locking and registering Thumbnail buffer(s)");
        for (int cnt = 0; cnt < (mZslEnable? (MAX_SNAPSHOT_BUFFERS-2) : numCapture); cnt++) {
            // TODO : change , lock all thumbnail buffers
            if ((mPreviewWindow != NULL) && (mThumbnailBuffer[cnt] != NULL)) {
                ALOGE("createsnapshotbuffers : display lock");
                mDisplayLock.lock();
                /* Lock the postview buffer before use */
                ALOGV(" Locking thumbnail/postview buffer %d", cnt);
                if ( (ret = mPreviewWindow->lock_buffer(mPreviewWindow,
                                 mThumbnailBuffer[cnt])) != NO_ERROR) {
                    ALOGE(" Error locking postview buffer. Error = %d ", ret);
                    ALOGE("createsnapshotbuffers : display unlock error");
                    mDisplayLock.unlock();
                    return false;
                }

                mDisplayLock.unlock();
                ALOGE("createsnapshotbuffers : display unlock");
            }

            private_handle_t *thumbnailHandle;
            int mBufferSize = previewWidth * previewHeight * 3/2;
            int mCbCrOffset = PAD_TO_WORD(previewWidth * previewHeight);

            if (mThumbnailBuffer[cnt]) {
                thumbnailHandle = (private_handle_t *)(*mThumbnailBuffer[cnt]);
                ALOGV("fd thumbnailhandle fd %d size %d", thumbnailHandle->fd, thumbnailHandle->size);
                mThumbnailMapped [cnt] = mmap(0, thumbnailHandle->size, PROT_READ|PROT_WRITE,
                MAP_SHARED, thumbnailHandle->fd, 0);
                if (mThumbnailMapped[cnt] == MAP_FAILED) {
                    ALOGE(" Couldnt map Thumbnail buffer %d", errno);
                    return false;
                }
                register_buf(mBufferSize,
                    mCbCrOffset, 0,
                    thumbnailHandle->fd,
                    0,
                    (uint8_t *)mThumbnailMapped[cnt],
                    MSM_PMEM_THUMBNAIL,
                    (cnt < ACTIVE_ZSL_BUFFERS));
            }
        } // for loop locking and registering thumbnail buffers
    } else { // End if Format is Jpeg , start if format is RAW
        if (numberOfRawBuffers ==1) {
            int rawSnapshotSize = mDimension.raw_picture_height * mDimension.raw_picture_width;
#ifdef USE_ION
            if (allocate_ion_memory(&raw_snapshot_main_ion_fd, &raw_snapshot_alloc, &raw_snapshot_ion_info_fd,
                ion_heap, rawSnapshotSize, &mRawSnapshotfd) < 0) {
                ALOGE("%s: allocate ion memory failed!\n", __func__);
                return false;
            }
#else
            mRawSnapshotfd = open(pmem_region, O_RDWR|O_SYNC);
            if (mRawSnapshotfd <= 0) {
                ALOGE("%s: Open device %s failed for rawnspashot!\n",__func__, pmem_region);
                return false;
            }
#endif
            ALOGE("%s  Raw snapshot memory , fd is %d ", __func__, mRawSnapshotfd);
            mRawSnapshotMapped = mGetMemory(mRawSnapshotfd, rawSnapshotSize, 1, mCallbackCookie);
            if (mRawSnapshotMapped == NULL) {
                ALOGE("Failed to get camera memory for mRawSnapshotMapped ");
                return false;
            } else {
                ALOGE("Received following info for raw mapped data:%p,handle:%p, size:%d,release:%p",
                    mRawSnapshotMapped->data, mRawSnapshotMapped->handle, mRawSnapshotMapped->size, mRawSnapshotMapped->release);
            }
                        // Register Raw frames
            ALOGE("Registering RawSnapshot buffer with fd :%d with kernel",mRawSnapshotfd);
            int active = 1;  // TODO check ?
            register_buf(rawSnapshotSize,
                0,
                0,
                mRawSnapshotfd,
                0,
                (uint8_t *)mRawSnapshotMapped->data,
                MSM_PMEM_RAW_MAINIMG,
                active);
        } else {
            ALOGE("Multiple raw snapshot capture not supported for now....");
            return false;
        }
    } // end else , if RAW format
    return true;
}

bool QualcommCameraHardware::initRaw(bool initJpegHeap)
{
    int postViewBufferSize;
    uint32_t pictureAspectRatio;
    uint32_t i;
    mParameters.getPictureSize(&mPictureWidth, &mPictureHeight);
    mActualPictWidth = mPictureWidth;
    mActualPictHeight = mPictureHeight;
    if (updatePictureDimension(mParameters, mPictureWidth, mPictureHeight)) {
        mDimension.picture_width = mPictureWidth;
        mDimension.picture_height = mPictureHeight;
    }
    ALOGV("initRaw E: picture size=%dx%d", mPictureWidth, mPictureHeight);
    int w_scale_factor = (mIs3DModeOn && mSnapshot3DFormat == _SIDE_BY_SIDE_FULL) ? 2 : 1;

    /* use the default thumbnail sizes */
    mThumbnailHeight = thumbnail_sizes[DEFAULT_THUMBNAIL_SETTING].height;
    mThumbnailWidth = (mThumbnailHeight * mPictureWidth)/ mPictureHeight;
    /* see if we can get better thumbnail sizes (not mandatory?) */
    pictureAspectRatio = (mPictureWidth * Q12) / mPictureHeight;
    for (i = 0; i < THUMBNAIL_SIZE_COUNT; i++ ) {
        if (thumbnail_sizes[i].aspect_ratio == pictureAspectRatio) {
            mThumbnailWidth = thumbnail_sizes[i].width;
            mThumbnailHeight = thumbnail_sizes[i].height;
            break;
        }
    }
    /* calculate thumbnail aspect ratio */
    if (mCurrentTarget == TARGET_MSM7627 ) {
        uint32_t thumbnail_aspect_ratio = (mThumbnailWidth * Q12) / mThumbnailHeight;

        if (thumbnail_aspect_ratio < pictureAspectRatio) {
            /* if thumbnail is narrower than main image, in other words wide mode
             * snapshot then we want to adjust the height of the thumbnail to match
             * the main image aspect ratio. */
            mThumbnailHeight = (mThumbnailWidth * Q12) / pictureAspectRatio;
        } else if (thumbnail_aspect_ratio != pictureAspectRatio) {
            /* if thumbnail is wider than main image we want to adjust width of the
             * thumbnail to match main image aspect ratio */
            mThumbnailWidth  = (mThumbnailHeight * pictureAspectRatio) / Q12;
        }
        /* make the dimensions multiple of 16 - JPEG requirement */
        mThumbnailWidth = FLOOR16(mThumbnailWidth);
        mThumbnailHeight = FLOOR16(mThumbnailHeight);
        ALOGV("the thumbnail sizes are %dx%d",mThumbnailWidth,mThumbnailHeight);
    }

    /* calculate postView size */
    mPostviewWidth = mThumbnailWidth;
    mPostviewHeight = mThumbnailHeight;
    /* Try to keep the postview dimensions near to preview for better
     * performance and userexperience. If the postview and preview dimensions
     * are same, then we can try to use the same overlay of preview for
     * postview also. If not, we need to reset the overlay for postview.
     * we will be getting the same dimensions for preview and postview
     * in most of the cases. The only exception is for applications
     * which won't use optimalPreviewSize based on picture size.
    */
    if ((mPictureHeight >= previewHeight) &&
       (mCurrentTarget != TARGET_MSM7627) && !mIs3DModeOn) {
        mPostviewHeight = previewHeight;
        mPostviewWidth = (previewHeight * mPictureWidth) / mPictureHeight;
    } else if (mActualPictHeight < mThumbnailHeight) {
        mPostviewHeight = THUMBNAIL_SMALL_HEIGHT;
        mPostviewWidth = (THUMBNAIL_SMALL_HEIGHT * mActualPictWidth)/ mActualPictHeight;
        mThumbnailWidth = mPostviewWidth;
        mThumbnailHeight = mPostviewHeight;
    }

    if (mPreviewFormat == CAMERA_YUV_420_NV21_ADRENO) {
        mDimension.main_img_format = CAMERA_YUV_420_NV21_ADRENO;
        mDimension.thumb_format = CAMERA_YUV_420_NV21_ADRENO;
    }

    mDimension.ui_thumbnail_width = mPostviewWidth;
    mDimension.ui_thumbnail_height = mPostviewHeight;

    // mDimension will be filled with thumbnail_width, thumbnail_height,
    // orig_picture_dx, and orig_picture_dy after this function call. We need to
    // keep it for jpeg_encoder_encode.
    bool ret = native_set_parms(CAMERA_PARM_DIMENSION,
                               sizeof(cam_ctrl_dimension_t), &mDimension);

    if (!ret) {
        ALOGE("initRaw X: failed to set dimension");
        return false;
    }

    //postview buffer initialization
    postViewBufferSize  = mPostviewWidth * w_scale_factor * mPostviewHeight * 3 / 2;
    int CbCrOffsetPostview = PAD_TO_WORD(mPostviewWidth * w_scale_factor * mPostviewHeight);

    //Snapshot buffer initialization
    mRawSize = mPictureWidth * w_scale_factor * mPictureHeight * 3 / 2;
    mCbCrOffsetRaw = PAD_TO_WORD(mPictureWidth * w_scale_factor * mPictureHeight);
    if (mPreviewFormat == CAMERA_YUV_420_NV21_ADRENO) {
        mRawSize = PAD_TO_4K(CEILING32(mPictureWidth * w_scale_factor) * CEILING32(mPictureHeight)) +
                            2 * (CEILING32(mPictureWidth * w_scale_factor/2) * CEILING32(mPictureHeight/2));
        mCbCrOffsetRaw = PAD_TO_4K(CEILING32(mPictureWidth * w_scale_factor) * CEILING32(mPictureHeight));
    }

    //Jpeg buffer initialization
    if ( mCurrentTarget == TARGET_MSM7627 ||
       (mCurrentTarget == TARGET_MSM7625A ||
        mCurrentTarget == TARGET_MSM7627A))
        mJpegMaxSize = CEILING16(mPictureWidth * w_scale_factor) * CEILING16(mPictureHeight) * 3 / 2;
    else {
        mJpegMaxSize = mPictureWidth * w_scale_factor * mPictureHeight * 3 / 2;
        if (mPreviewFormat == CAMERA_YUV_420_NV21_ADRENO) {
            mJpegMaxSize =
               PAD_TO_4K(CEILING32(mPictureWidth * w_scale_factor) * CEILING32(mPictureHeight)) +
                    2 * (CEILING32(mPictureWidth * w_scale_factor/2) * CEILING32(mPictureHeight/2));
        }
    }

    int rotation = mParameters.getInt(CameraParameters::KEY_ROTATION);

    if (mIs3DModeOn)
        rotation = 0;
    ret = native_set_parms(CAMERA_PARM_JPEG_ROTATION, sizeof(int), &rotation);
    if (!ret) {
        ALOGE("setting camera id failed");
        return false;
    }
    cam_buf_info_t buf_info;
    if (mIs3DModeOn == false) {
        buf_info.resolution.width = mPictureWidth * w_scale_factor;
        buf_info.resolution.height = mPictureHeight;
        mCfgControl.mm_camera_get_parm(CAMERA_PARM_BUFFER_INFO, &buf_info);
        mRawSize = buf_info.size;
        mJpegMaxSize = mRawSize;
        mCbCrOffsetRaw = buf_info.cbcr_offset;
        mYOffset = buf_info.yoffset;
    }
    int mBufferSize;
    int CbCrOffset;
    if (mCurrentTarget != TARGET_MSM7627 && mCurrentTarget != TARGET_MSM7627A) {
        mParameters.getPreviewSize(&previewWidth, &previewHeight);
        mBufferSize = previewWidth * previewHeight * 3/2;
        CbCrOffset = PAD_TO_WORD(previewWidth * previewHeight);
    } else {
        mBufferSize = mPostviewWidth * mPostviewHeight * 3/2;
        CbCrOffset = PAD_TO_WORD(mPostviewWidth * mPostviewHeight);
    }

    ALOGV("initRaw: initializing mRawHeap.");

    // Create memory for Raw YUV frames and Jpeg images
    if (createSnapshotMemory(numCapture, numCapture, initJpegHeap) == false) {
        ALOGE("ERROR :  initraw , createSnapshotMemory failed");
        return false;
    }
    /* frame all the exif and encode information into encode_params_t */

    initImageEncodeParameters(numCapture);
    /* fill main image size, thumbnail size, postview size into capture_params_t*/
    memset(&mImageCaptureParms, 0, sizeof(capture_params_t));
    mImageCaptureParms.num_captures = numCapture;
    mImageCaptureParms.picture_width = mPictureWidth;
    mImageCaptureParms.picture_height = mPictureHeight;
    mImageCaptureParms.postview_width = mPostviewWidth;
    mImageCaptureParms.postview_height = mPostviewHeight;

    int width = mParameters.getInt(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH);
    int height = mParameters.getInt(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT);
    if ((width != 0) && (height != 0)) {
        mImageCaptureParms.thumbnail_width = mThumbnailWidth;
        mImageCaptureParms.thumbnail_height = mThumbnailHeight;
    } else {
        mImageCaptureParms.thumbnail_width = 0;
        mImageCaptureParms.thumbnail_height = 0;
    }

    ALOGI("%s: picture size=%dx%d",__FUNCTION__,
        mImageCaptureParms.picture_width, mImageCaptureParms.picture_height);
    ALOGI("%s: postview size=%dx%d",__FUNCTION__,
        mImageCaptureParms.postview_width, mImageCaptureParms.postview_height);
    ALOGI("%s: thumbnail size=%dx%d",__FUNCTION__,
        mImageCaptureParms.thumbnail_width, mImageCaptureParms.thumbnail_height);

    ALOGV("initRaw X");
    return true;
}

void QualcommCameraHardware::deinitRawSnapshot()
{
    ALOGV("deinitRawSnapshot E");

    int rawSnapshotSize = mDimension.raw_picture_height * mDimension.raw_picture_width;
     // Unregister and de allocated memory for Raw Snapshot
    if (mRawSnapshotMapped) {
        register_buf(rawSnapshotSize,
            0,
            0,
            mRawSnapshotfd,
            0,
            (uint8_t *)mRawSnapshotMapped->data,
            MSM_PMEM_RAW_MAINIMG,
            false,
            false);
        mRawSnapshotMapped->release(mRawSnapshotMapped);
        mRawSnapshotMapped = NULL;
        close(mRawSnapshotfd);
#ifdef USE_ION
        deallocate_ion_memory(&raw_snapshot_main_ion_fd, &raw_snapshot_ion_info_fd);
#endif
    }
    ALOGV("deinitRawSnapshot X");
}

void QualcommCameraHardware::deinitRaw()
{
    ALOGV("deinitRaw E");
    ALOGV("deinitRaw , clearing raw memory and jpeg memory");
    for (int cnt = 0; cnt < (mZslEnable ? MAX_SNAPSHOT_BUFFERS : numCapture); cnt++) {
        if (NULL != mRawMapped[cnt]) {
            ALOGE("Unregister MAIN_IMG");
            register_buf(mJpegMaxSize,
                mCbCrOffsetRaw, 0,
                mRawfd[cnt], 0,
                (uint8_t *)mRawMapped[cnt]->data,
                MSM_PMEM_MAINIMG,
                0, 0);
            mRawMapped[cnt]->release(mRawMapped[cnt]);
            mRawMapped[cnt] = NULL;
            close(mRawfd[cnt]);
#ifdef USE_ION
            deallocate_ion_memory(&raw_main_ion_fd[cnt], &raw_ion_info_fd[cnt]);
#endif
        }
    }
    for (int cnt = 0; cnt < (mZslEnable ? MAX_SNAPSHOT_BUFFERS : numCapture); cnt++) {
        if (NULL != mJpegMapped[cnt]) {
            mJpegMapped[cnt]->release(mJpegMapped[cnt]);
            mJpegMapped[cnt] = NULL;
        }
    }
    if ( mPreviewWindow != NULL ) {
        ALOGE("deinitRaw , clearing/cancelling thumbnail buffers:");
        private_handle_t *handle;
        for (int cnt = 0; cnt < (mZslEnable? (MAX_SNAPSHOT_BUFFERS-2) : numCapture); cnt++) {
            if (mPreviewWindow != NULL && mThumbnailBuffer[cnt] != NULL) {
                handle = (private_handle_t *)(*mThumbnailBuffer[cnt]);
                ALOGE("%s:  Cancelling postview buffer %d ", __FUNCTION__, handle->fd);
                ALOGE("deinitraw : display lock");
                mDisplayLock.lock();

                status_t retVal = mPreviewWindow->cancel_buffer(mPreviewWindow,
                                                              mThumbnailBuffer[cnt]);
                if (retVal != NO_ERROR)
                    ALOGE("%s: cancelBuffer failed for postview buffer %d",
                                                     __FUNCTION__, handle->fd);
                   if (mStoreMetaDataInFrame && (metadata_memory[cnt] != NULL)) {
                       struct encoder_media_buffer_type * packet =
                               (struct encoder_media_buffer_type  *)metadata_memory[cnt]->data;
                       native_handle_delete(const_cast<native_handle_t *>(packet->meta_handle));
                       metadata_memory[cnt]->release(metadata_memory[cnt]);
                       metadata_memory[cnt] = NULL;
                   }
                // unregister , unmap and release as well

                int mBufferSize = previewWidth * previewHeight * 3/2;
                int mCbCrOffset = PAD_TO_WORD(previewWidth * previewHeight);
                if (mThumbnailMapped[cnt]) {
                    ALOGE("%s:  Unregistering Thumbnail Buffer %d ", __FUNCTION__, handle->fd);
                    register_buf(mBufferSize,
                        mCbCrOffset, 0,
                        handle->fd,
                        0,
                        (uint8_t *)mThumbnailMapped[cnt],
                        MSM_PMEM_THUMBNAIL,
                        false, false);
                     if (munmap(mThumbnailMapped[cnt],handle->size ) == -1) {
                       ALOGE("deinitraw : Error un-mmapping the thumbnail buffer %p", index);
                     }
                     mThumbnailBuffer[cnt] = NULL;
                     mThumbnailMapped[cnt] = NULL;
                }
                ALOGE("deinitraw : display unlock");
                mDisplayLock.unlock();
            }
        }
    }
    ALOGV("deinitRaw X");
}

void QualcommCameraHardware::relinquishBuffers()
{
    status_t retVal;
    ALOGV("%s: E ", __FUNCTION__);
    mDisplayLock.lock();
    if (mPreviewWindow != NULL) {
        for (int cnt = 0; cnt < mTotalPreviewBufferCount; cnt++) {
            retVal = mPreviewWindow->cancel_buffer(mPreviewWindow,
                frame_buffer[cnt].buffer);
            mPreviewMapped[cnt]->release(mPreviewMapped[cnt]);
            if (mStoreMetaDataInFrame && (metadata_memory[cnt] != NULL)) {
                struct encoder_media_buffer_type * packet =
                    (struct encoder_media_buffer_type  *)metadata_memory[cnt]->data;
                native_handle_delete(const_cast<native_handle_t *>(packet->meta_handle));
                metadata_memory[cnt]->release(metadata_memory[cnt]);
                metadata_memory[cnt] = NULL;
            }
            ALOGE("release preview buffers");
            if (retVal != NO_ERROR)
                ALOGE("%s: cancelBuffer failed for preview buffer %d ",
                    __FUNCTION__, frames[cnt].fd);
        }
    } else {
      ALOGV(" PreviewWindow is null, will not cancelBuffers ");
    }
    mDisplayLock.unlock();
    ALOGV("%s: X ", __FUNCTION__);
}

status_t QualcommCameraHardware::set_PreviewWindow(void *param)
{
    preview_stream_ops_t* window = (preview_stream_ops_t*)param;
    return setPreviewWindow(window);
}

status_t QualcommCameraHardware::setPreviewWindow(preview_stream_ops_t* window)
{
    status_t retVal = NO_ERROR;
    ALOGV(" %s: E ", __FUNCTION__);
    if ( window == NULL) {
        ALOGW(" Setting NULL preview window ");
    }
    ALOGE("Set preview window:: ");
    mDisplayLock.lock();
    mPreviewWindow = window;
    mDisplayLock.unlock();

    if ( (mPreviewWindow != NULL) && mCameraRunning) {
        /* Initial preview in progress. Stop it and start
         * the actual preview */
         stopInitialPreview();
         retVal = getBuffersAndStartPreview();
    }
    ALOGV(" %s : X ", __FUNCTION__ );
    return retVal;
}

status_t QualcommCameraHardware::getBuffersAndStartPreview() {
    status_t retVal = NO_ERROR;
    int stride;
    ALOGI(" %s : E ", __FUNCTION__);
    mFrameThreadWaitLock.lock();
    while (mFrameThreadRunning) {
        ALOGV("%s: waiting for old frame thread to complete.", __FUNCTION__);
        mFrameThreadWait.wait(mFrameThreadWaitLock);
        ALOGV("%s: old frame thread completed.",__FUNCTION__);
    }
    mFrameThreadWaitLock.unlock();

    if (mPreviewWindow != NULL) {
        ALOGV("%s: Calling native_window_set_buffer", __FUNCTION__);

        int numMinUndequeuedBufs = 0;

        int err = mPreviewWindow->get_min_undequeued_buffer_count(mPreviewWindow,
            &numMinUndequeuedBufs);

        if (err != 0) {
            ALOGW("NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS query failed: %s (%d)",
                    strerror(-err), -err);
            return err;
        }
        mTotalPreviewBufferCount = kPreviewBufferCount + numMinUndequeuedBufs;

        const char *str = mParameters.getPreviewFormat();
        int32_t previewFormat = attr_lookup(app_preview_formats,
            sizeof(app_preview_formats) / sizeof(str_map), str);
        if (previewFormat == NOT_FOUND) {
            previewFormat = HAL_PIXEL_FORMAT_YCrCb_420_SP;
        }

        retVal = mPreviewWindow->set_buffer_count(mPreviewWindow,
            mTotalPreviewBufferCount +
            (mZslEnable? (MAX_SNAPSHOT_BUFFERS-2) : numCapture) );

        if (retVal != NO_ERROR) {
            ALOGE("%s: Error while setting buffer count to %d ", __FUNCTION__, kPreviewBufferCount + 1);
            return retVal;
        }
        mParameters.getPreviewSize(&previewWidth, &previewHeight);

        retVal = mPreviewWindow->set_buffers_geometry(mPreviewWindow,
            previewWidth, previewHeight, previewFormat);

        if (retVal != NO_ERROR) {
            ALOGE("%s: Error while setting buffer geometry ", __FUNCTION__);
            return retVal;
        }

        mPreviewWindow->set_usage (mPreviewWindow,
            GRALLOC_USAGE_PRIVATE_MM_HEAP | GRALLOC_USAGE_PRIVATE_UNCACHED);

        int CbCrOffset = PAD_TO_WORD(previewWidth * previewHeight);
        int cnt = 0, active = 1;
        int mBufferSize = previewWidth * previewHeight * 3/2;
        for (cnt = 0; cnt < mTotalPreviewBufferCount; cnt++) {
            buffer_handle_t *bhandle = NULL;
            retVal = mPreviewWindow->dequeue_buffer(mPreviewWindow,
                &(bhandle),
                &(stride));

            if ((retVal == NO_ERROR)) {
                /* Acquire lock on the buffer if it was successfully
                 * dequeued from gralloc */
                ALOGV(" Locking buffer %d ", cnt);
                retVal = mPreviewWindow->lock_buffer(mPreviewWindow,
                                            bhandle);
                ALOGE(" Locked buffer %d successfully", cnt);
            } else {
                ALOGE("%s: dequeueBuffer failed for preview buffer. Error = %d",
                      __FUNCTION__, retVal);
                return retVal;
            }
            if (retVal == NO_ERROR) {
                private_handle_t *handle = (private_handle_t *)(*bhandle);
                ALOGE("Handle %p, Fd passed:%d, Base:%d, Size %d",
                handle,handle->fd,handle->base,handle->size);

                if (handle) {
                    ALOGV("fd mmap fd %d size %d", handle->fd, handle->size);
                    mPreviewMapped[cnt] = mGetMemory(handle->fd, handle->size, 1, mCallbackCookie);

                    if (mPreviewMapped[cnt] == NULL) {
                        ALOGE(" Failed to get camera memory for  Preview buffer %d ",cnt);
                    } else {
                        ALOGE(" Mapped Preview buffer %d", cnt);
                    }
                    ALOGE("Got the following from get_mem data: %p, handle :%p, release : %p, size: %d",
                        mPreviewMapped[cnt]->data,
                        mPreviewMapped[cnt]->handle,
                        mPreviewMapped[cnt]->release,
                        mPreviewMapped[cnt]->size);
                    ALOGE(" getbuffersandrestartpreview deQ %d", handle->fd);
                    frames[cnt].fd = handle->fd;
                    frames[cnt].buffer = (unsigned int)mPreviewMapped[cnt]->data;
                    if (((void *)frames[cnt].buffer == MAP_FAILED)
                        || (frames[cnt].buffer == 0)) {
                        ALOGE("%s: Couldnt map preview buffers", __FUNCTION__);
                        return UNKNOWN_ERROR;
                    }
                    frames[cnt].y_off = 0;
                    frames[cnt].cbcr_off= CbCrOffset;
                    frames[cnt].path = OUTPUT_TYPE_P;
                    frame_buffer[cnt].frame = &frames[cnt];
                    frame_buffer[cnt].buffer = bhandle;
                    frame_buffer[cnt].size = handle->size;
                    active = (cnt < ACTIVE_PREVIEW_BUFFERS);

                    ALOGE("Registering buffer %d with fd :%d with kernel",cnt,handle->fd);
                    register_buf(mBufferSize,
                        CbCrOffset, 0,
                        handle->fd,
                        0,
                        (uint8_t *)frames[cnt].buffer,
                        MSM_PMEM_PREVIEW,
                        active);
                    ALOGE("Came back from register call to kernel");
                } else
                    ALOGE("%s: setPreviewWindow: Could not get buffer handle", __FUNCTION__);
            } else {
                ALOGE("%s: lockBuffer failed for preview buffer. Error = %d",
                    __FUNCTION__, retVal);
                return retVal;
            }
        }

        // Dequeue Thumbnail/Postview  Buffers here , Consider ZSL/Multishot cases
        for (cnt = 0; cnt < (mZslEnable? (MAX_SNAPSHOT_BUFFERS-2) : numCapture); cnt++) {
            retVal = mPreviewWindow->dequeue_buffer(mPreviewWindow,
                &mThumbnailBuffer[cnt], &(stride));
            private_handle_t* handle = (private_handle_t *)(*mThumbnailBuffer[cnt]);
            ALOGE(" : dequeing thumbnail buffer fd %d", handle->fd);
            if (retVal != NO_ERROR) {
                ALOGE("%s: dequeueBuffer failed for postview buffer. Error = %d ",
                    __FUNCTION__, retVal);
            return retVal;
            }
        }

        // Cancel minUndequeuedBufs.
        for (cnt = kPreviewBufferCount; cnt < mTotalPreviewBufferCount; cnt++) {
            status_t retVal = mPreviewWindow->cancel_buffer(mPreviewWindow,
                frame_buffer[cnt].buffer);
            ALOGE(" Cancelling preview buffers %d ",frame_buffer[cnt].frame->fd);
        }
    } else {
        ALOGE("%s: Could not get Buffer from Surface", __FUNCTION__);
        return UNKNOWN_ERROR;
    }
    mPreviewBusyQueue.init();
    LINK_camframe_release_all_frames(CAM_PREVIEW_FRAME);
    for (int i = ACTIVE_PREVIEW_BUFFERS; i < kPreviewBufferCount; i++)
        LINK_camframe_add_frame(CAM_PREVIEW_FRAME,&frames[i]);

    mBuffersInitialized = true;

    ALOGE("setPreviewWindow: Starting preview after buffer allocation");
    startPreviewInternal();

    ALOGI(" %s : X ",__FUNCTION__);
    return NO_ERROR;
}

void QualcommCameraHardware::release()
{
    ALOGI("release E");
    Mutex::Autolock l(&mLock);
    ALOGI("release: mCameraRunning = %d", mCameraRunning);
    if (mCameraRunning) {
        if (mDataCallbackTimestamp && (mMsgEnabled & CAMERA_MSG_VIDEO_FRAME)) {
            mRecordFrameLock.lock();
            mReleasedRecordingFrame = true;
            mRecordWait.signal();
            mRecordFrameLock.unlock();
        }
        stopPreviewInternal();
        ALOGI("release: stopPreviewInternal done.");
    }
    LINK_jpeg_encoder_join();
    mm_camera_ops_type_t
        current_ops_type = (mSnapshotFormat == PICTURE_FORMAT_JPEG) ?
        CAMERA_OPS_CAPTURE_AND_ENCODE : CAMERA_OPS_RAW_CAPTURE;
    mCamOps.mm_camera_deinit(current_ops_type, NULL, NULL);

    //Signal the snapshot thread
    mJpegThreadWaitLock.lock();
    mJpegThreadRunning = false;
    mJpegThreadWait.signal();
    mJpegThreadWaitLock.unlock();

    // Wait for snapshot thread to complete before clearing the
    // resources.
    mSnapshotThreadWaitLock.lock();
    while (mSnapshotThreadRunning) {
        ALOGV("release: waiting for old snapshot thread to complete.");
        mSnapshotThreadWait.wait(mSnapshotThreadWaitLock);
        ALOGV("release: old snapshot thread completed.");
    }
    mSnapshotThreadWaitLock.unlock();

    {
        Mutex::Autolock l (&mRawPictureHeapLock);
        deinitRaw();
    }

    deinitRawSnapshot();
    ALOGI("release: clearing resources done.");
    LINK_mm_camera_deinit();

    ALOGI("release X: mCameraRunning = %d, mFrameThreadRunning = %d", mCameraRunning, mFrameThreadRunning);
    ALOGI("mVideoThreadRunning = %d, mSnapshotThreadRunning = %d, mJpegThreadRunning = %d", mVideoThreadRunning, mSnapshotThreadRunning, mJpegThreadRunning);
    ALOGI("camframe_timeout_flag = %d, mAutoFocusThreadRunning = %d", camframe_timeout_flag, mAutoFocusThreadRunning);
    mFrameThreadWaitLock.lock();
    while (mFrameThreadRunning) {
        ALOGV("release: waiting for old frame thread to complete.");
        mFrameThreadWait.wait(mFrameThreadWaitLock);
        ALOGV("release: old frame thread completed.");
    }
    mFrameThreadWaitLock.unlock();
}

QualcommCameraHardware::~QualcommCameraHardware()
{
    ALOGI("~QualcommCameraHardware E");

    if (mCurrentTarget == TARGET_MSM7630 ||
        mCurrentTarget == TARGET_QSD8250 ||
        mCurrentTarget == TARGET_MSM8660) {
        delete [] recordframes;
        recordframes = NULL;
        delete [] record_buffers_tracking_flag;
        record_buffers_tracking_flag = NULL;
    }
    mMMCameraDLRef.clear();
    ALOGI("~QualcommCameraHardware X");
}

status_t QualcommCameraHardware::startPreviewInternal()
{
    ALOGV("in startPreviewInternal : E");
    if (!mBuffersInitialized) {
        ALOGE("startPreviewInternal: Buffers not allocated. Cannot start preview");
        return NO_ERROR;
    }
    mPreviewStopping = false;
    if (mCameraRunning) {
        ALOGV("startPreview X: preview already running.");
        return NO_ERROR;
    }
    if (mZslEnable) {
        //call init
        ALOGI("ZSL Enable called");
        uint8_t is_zsl = 1;
        mm_camera_status_t status;
        if (MM_CAMERA_SUCCESS != mCfgControl.mm_camera_set_parm(CAMERA_PARM_ZSL_ENABLE, &is_zsl)) {
            ALOGE("ZSL Enable failed");
            return UNKNOWN_ERROR;
        }
    }

    if (!mPreviewInitialized) {
        mPreviewInitialized = initPreview();
        if (!mPreviewInitialized) {
            ALOGE("startPreview X initPreview failed.  Not starting preview.");
            mPreviewBusyQueue.deinit();
            return UNKNOWN_ERROR;
        }
    }

    /* For 3D mode, start the video output, as this need to be
     * used for display also.
     */
    if (mIs3DModeOn) {
        startRecordingInternal();
        if (!mVideoThreadRunning) {
            ALOGE("startPreview X startRecording failed.  Not starting preview.");
            return UNKNOWN_ERROR;
        }
    }

    {
        Mutex::Autolock cameraRunningLock(&mCameraRunningLock);
        if (mCurrentTarget != TARGET_MSM7630 &&
            mCurrentTarget != TARGET_QSD8250 &&
            mCurrentTarget != TARGET_MSM8660)
            mCameraRunning = native_start_ops(CAMERA_OPS_STREAMING_PREVIEW, NULL);
        else {
            if (!mZslEnable) {
                ALOGE("Calling CAMERA_OPS_STREAMING_VIDEO");
                mCameraRunning = native_start_ops(CAMERA_OPS_STREAMING_VIDEO, NULL);
                ALOGE(": Calling CAMERA_OPS_STREAMING_VIDEO %d", mCameraRunning);
            } else {
                initZslParameter();
                mCameraRunning = false;
                if (MM_CAMERA_SUCCESS == mCamOps.mm_camera_init(CAMERA_OPS_STREAMING_ZSL,
                    &mZslParms, NULL)) {
                    //register buffers for ZSL
                    bool status = initZslBuffers(true);
                    if (status != true) {
                        ALOGE("Failed to allocate ZSL buffers");
                        return false;
                    }
                    if (MM_CAMERA_SUCCESS == mCamOps.mm_camera_start(CAMERA_OPS_STREAMING_ZSL,NULL, NULL)) {
                        mCameraRunning = true;
                    }
                }
                if (mCameraRunning == false)
                    ALOGE("Starting  ZSL CAMERA_OPS_STREAMING_ZSL failed!!!");
            }
        }
    }

    if (!mCameraRunning) {
        deinitPreview();
        if (mZslEnable) {
            //deinit
            ALOGI("ZSL DISABLE called");
            uint8_t is_zsl = 0;
            mm_camera_status_t status;
            if (MM_CAMERA_SUCCESS != mCfgControl.mm_camera_set_parm(CAMERA_PARM_ZSL_ENABLE, &is_zsl)) {
                ALOGE("ZSL_Disable failed!!");
                return UNKNOWN_ERROR;
            }
        }
        /* Flush the Busy Q */
        cam_frame_flush_video();
        /* Need to flush the free Qs as these are initalized in initPreview.*/
        LINK_camframe_release_all_frames(CAM_VIDEO_FRAME);
        LINK_camframe_release_all_frames(CAM_PREVIEW_FRAME);
        mPreviewInitialized = false;
        ALOGE("startPreview X: native_start_ops: CAMERA_OPS_STREAMING_PREVIEW ioctl failed!");
        return UNKNOWN_ERROR;
    }

    //Reset the Gps Information
    exif_table_numEntries = 0;
    previewWidthToNativeZoom = previewWidth;
    previewHeightToNativeZoom = previewHeight;

    ALOGV("startPreviewInternal X");
    return NO_ERROR;
}

status_t QualcommCameraHardware::startInitialPreview()
{
   mCameraRunning = true;
   return NO_ERROR;
}

status_t QualcommCameraHardware::startPreview()
{
    status_t result;
    ALOGV("startPreview E");
    Mutex::Autolock l(&mLock);
    if (mPreviewWindow == NULL) {
        /* startPreview has been called before setting the preview
         * window. Start the camera with initial buffers because the
         * CameraService expects the preview to be enabled while
         * setting a valid preview window */
        ALOGV(" %s : Starting preview with initial buffers ", __FUNCTION__);
        result = startInitialPreview();
    } else {
        /* startPreview has been issued after a valid preview window
         * is set. Get the preview buffers from gralloc and start
         * preview normally */
        ALOGV(" %s : Starting normal preview ", __FUNCTION__);
        result = getBuffersAndStartPreview();
    }
    ALOGV("startPreview X");
    return result;
}

void QualcommCameraHardware::stopInitialPreview() {
   mCameraRunning = false;
}

void QualcommCameraHardware::stopPreviewInternal()
{
    ALOGI("stopPreviewInternal E: %d", mCameraRunning);
    mPreviewStopping = true;
    if (mCameraRunning && mPreviewWindow != NULL) {
        /* For 3D mode, we need to exit the video thread.*/
        if (mIs3DModeOn) {
            mRecordingState = 0;
            mVideoThreadWaitLock.lock();
            ALOGI("%s: 3D mode, exit video thread", __FUNCTION__);
            mVideoThreadExit = 1;
            mVideoThreadWaitLock.unlock();

            pthread_mutex_lock(&g_busy_frame_queue.mut);
            pthread_cond_signal(&g_busy_frame_queue.wait);
            pthread_mutex_unlock(&g_busy_frame_queue.mut);
        }

        // Cancel auto focus.
        if (mNotifyCallback && (mMsgEnabled & CAMERA_MSG_FOCUS)) {
            cancelAutoFocusInternal();
        }

        // make mSmoothzoomThreadExit true
        mSmoothzoomThreadLock.lock();
        mSmoothzoomThreadExit = true;
        mSmoothzoomThreadLock.unlock();
        // singal smooth zoom thread , so that it can exit gracefully
        mSmoothzoomThreadWaitLock.lock();
        if (mSmoothzoomThreadRunning)
            mSmoothzoomThreadWait.signal();

        mSmoothzoomThreadWaitLock.unlock();

        Mutex::Autolock l(&mCamframeTimeoutLock);
        {
            Mutex::Autolock cameraRunningLock(&mCameraRunningLock);
            if (!camframe_timeout_flag) {
                if (mCurrentTarget != TARGET_MSM7630 &&
                    mCurrentTarget != TARGET_QSD8250 &&
                    mCurrentTarget != TARGET_MSM8660)
                    mCameraRunning = !native_stop_ops(CAMERA_OPS_STREAMING_PREVIEW, NULL);
                else {
                    if (!mZslEnable) {
                        ALOGE("%s ops_streaming mCameraRunning b= %d",__FUNCTION__, mCameraRunning);
                        mCameraRunning = !native_stop_ops(CAMERA_OPS_STREAMING_VIDEO, NULL);
                        ALOGE("%s ops_streaming mCameraRunning = %d",__FUNCTION__, mCameraRunning);
                    } else {
                        mCameraRunning = true;
                        if (MM_CAMERA_SUCCESS == mCamOps.mm_camera_stop(CAMERA_OPS_STREAMING_ZSL,NULL, NULL)) {
                            deinitZslBuffers();
                            if (MM_CAMERA_SUCCESS == mCamOps.mm_camera_deinit(CAMERA_OPS_STREAMING_ZSL,
                                    &mZslParms, NULL)) {
                                mCameraRunning = false;
                            }
                        }
                        if (mCameraRunning)
                            ALOGE("Starting  ZSL CAMERA_OPS_STREAMING_ZSL failed!!!");
                    }
                }
            } else {
                /* This means that the camframetimeout was issued.
                 * But we did not issue native_stop_preview(), so we
                 * need to update mCameraRunning to indicate that
                 * Camera is no longer running. */
                ALOGE("%s, : MAKE MCAMER_RUNNING FALSE!!!",__FUNCTION__);
                mCameraRunning = false;
            }
        }
    }
    /* in 3D mode, wait for the video thread before clearing resources.*/
    if (mIs3DModeOn) {
        mVideoThreadWaitLock.lock();
        while (mVideoThreadRunning) {
            ALOGI("%s: waiting for video thread to complete.", __FUNCTION__);
            mVideoThreadWait.wait(mVideoThreadWaitLock);
            ALOGI("%s : video thread completed.", __FUNCTION__);
        }
        mVideoThreadWaitLock.unlock();
    }
    ALOGE("%s, J_mCameraRunning = %d", __FUNCTION__, mCameraRunning);
    if (!mCameraRunning) {
        ALOGE("%s, before calling deinitpre mPreviewInitialized = %d", __FUNCTION__, mPreviewInitialized);
        if (mPreviewInitialized) {
            ALOGE("before calling deinitpreview");
            deinitPreview();
            if (mCurrentTarget == TARGET_MSM7630 ||
                mCurrentTarget == TARGET_QSD8250 ||
                mCurrentTarget == TARGET_MSM8660) {
                mVideoThreadWaitLock.lock();
                ALOGV("in stopPreviewInternal: making mVideoThreadExit 1");
                mVideoThreadExit = 1;
                mVideoThreadWaitLock.unlock();
                //if stop is called, if so exit video thread.
                pthread_mutex_lock(&(g_busy_frame_queue.mut));
                pthread_cond_signal(&(g_busy_frame_queue.wait));
                pthread_mutex_unlock(&(g_busy_frame_queue.mut));

                ALOGE(" flush video and release all frames");
                /* Flush the Busy Q */
                cam_frame_flush_video();
                /* Flush the Free Q */
                LINK_camframe_release_all_frames(CAM_VIDEO_FRAME);
            }
            mPreviewInitialized = false;
        }
    }
    else ALOGI("stopPreviewInternal: Preview is stopped already");

    ALOGI("stopPreviewInternal X: %d", mCameraRunning);
}

void QualcommCameraHardware::stopPreview()
{
    ALOGV("stopPreview: E");
    Mutex::Autolock l(&mLock);
    {
        if (mDataCallbackTimestamp && (mMsgEnabled & CAMERA_MSG_VIDEO_FRAME))
            return;
    }
    if (mSnapshotThreadRunning) {
        ALOGV("In stopPreview during snapshot");
        return;
    }
    if (mPreviewWindow != NULL) {
        private_handle_t *handle;
        for (int cnt = 0; cnt < (mZslEnable? (MAX_SNAPSHOT_BUFFERS-2) : numCapture); cnt++) {
            if (mPreviewWindow != NULL && mThumbnailBuffer[cnt] != NULL) {
                handle = (private_handle_t *)(*mThumbnailBuffer[cnt]);
                ALOGE("%s:  Cancelling postview buffer %d ", __FUNCTION__, handle->fd);
                ALOGE("stoppreview : display lock");
                mDisplayLock.lock();

                status_t retVal = mPreviewWindow->cancel_buffer(mPreviewWindow,
                    mThumbnailBuffer[cnt]);
                ALOGE("stopPreview : after cancelling thumbnail buffer");
                if (retVal != NO_ERROR)
                    ALOGE("%s: cancelBuffer failed for postview buffer %d",
                        __FUNCTION__, handle->fd);
                // unregister , unmap and release as well
                int mBufferSize = previewWidth * previewHeight * 3/2;
                int mCbCrOffset = PAD_TO_WORD(previewWidth * previewHeight);
                if ((mThumbnailMapped[cnt] && (mSnapshotFormat == PICTURE_FORMAT_JPEG))
                    || mZslEnable) {
                    ALOGE("%s:  Unregistering Thumbnail Buffer %d ", __FUNCTION__, handle->fd);
                    register_buf(mBufferSize,
                        mCbCrOffset, 0,
                        handle->fd,
                        0,
                        (uint8_t *)mThumbnailMapped[cnt],
                        MSM_PMEM_THUMBNAIL,
                        false, false);
                    if (munmap(mThumbnailMapped[cnt],handle->size ) == -1) {
                        ALOGE("StopPreview : Error un-mmapping the thumbnail buffer %p", index);
                    }
                    mThumbnailMapped[cnt] = NULL;
                }
                mThumbnailBuffer[cnt] = NULL;
                ALOGE("stoppreview : display unlock");
                mDisplayLock.unlock();
            }
        }
    }
    stopPreviewInternal();
    ALOGV("stopPreview: X");
}

void QualcommCameraHardware::runAutoFocus()
{
    bool status = true;
    isp3a_af_mode_t afMode = AF_MODE_AUTO;

    mAutoFocusThreadLock.lock();
    // Skip autofocus if focus mode is infinity.

    const char *focusMode = mParameters.get(CameraParameters::KEY_FOCUS_MODE);
    if (mParameters.get(CameraParameters::KEY_FOCUS_MODE) == 0 ||
        strcmp(focusMode, CameraParameters::FOCUS_MODE_INFINITY) == 0 ||
        strcmp(focusMode, CameraParameters::FOCUS_MODE_CONTINUOUS_VIDEO) == 0) {
        goto done;
    }

    if (!libmmcamera) {
        ALOGE("FATAL ERROR: could not dlopen liboemcamera.so: %s", dlerror());
        mAutoFocusThreadRunning = false;
        mAutoFocusThreadLock.unlock();
        return;
    }

    afMode = (isp3a_af_mode_t)attr_lookup(focus_modes,
        sizeof(focus_modes) / sizeof(str_map),
        mParameters.get(CameraParameters::KEY_FOCUS_MODE));

    /* This will block until either AF completes or is cancelled. */
    ALOGV("af start (mode %d)", afMode);
    status_t err;
    err = mAfLock.tryLock();
    if (err == NO_ERROR) {
        {
            Mutex::Autolock cameraRunningLock(&mCameraRunningLock);
            if (mCameraRunning) {
                ALOGV("Start AF");
                status = native_start_ops(CAMERA_OPS_FOCUS, &afMode);
            } else {
                ALOGV("As Camera preview is not running, AF not issued");
                status = false;
            }
        }
        mAfLock.unlock();
    }
    else {
        //AF Cancel would have acquired the lock,
        //so, no need to perform any AF
        ALOGV("As Cancel auto focus is in progress, auto focus request "
                "is ignored");
        status = FALSE;
    }

    {
        Mutex::Autolock pl(&mParametersLock);
        if (mHasAutoFocusSupport && updateFocusDistances(focusMode) != NO_ERROR) {
            ALOGE("%s: updateFocusDistances failed for %s", __FUNCTION__, focusMode);
        }
    }

    ALOGV("af done: %d", (int)status);

done:
    mAutoFocusThreadRunning = false;
    mAutoFocusThreadLock.unlock();

    mCallbackLock.lock();
    bool autoFocusEnabled = mNotifyCallback && (mMsgEnabled & CAMERA_MSG_FOCUS);
    camera_notify_callback cb = mNotifyCallback;
    void *data = mCallbackCookie;
    mCallbackLock.unlock();
    if (autoFocusEnabled)
        cb(CAMERA_MSG_FOCUS, status, 0, data);

}

status_t QualcommCameraHardware::cancelAutoFocusInternal()
{
    ALOGV("cancelAutoFocusInternal E");
    bool afRunning = true;

    if (!mHasAutoFocusSupport) {
        ALOGV("cancelAutoFocusInternal X");
        return NO_ERROR;
    }

    status_t rc = NO_ERROR;
    status_t err;

    do {
        err = mAfLock.tryLock();
        if (err == NO_ERROR) {
            //Got Lock, means either AF hasn't started or
            // AF is done. So no need to cancel it, just change the state
            ALOGV("Auto Focus is not in progress, Cancel Auto Focus is ignored");
            mAfLock.unlock();

            mAutoFocusThreadLock.lock();
            afRunning = mAutoFocusThreadRunning;
            mAutoFocusThreadLock.unlock();
            if (afRunning) {
                usleep(5000);
            }
        }
    } while (err == NO_ERROR && afRunning);

    if (afRunning) {
        //AF is in Progess, So cancel it
        ALOGV("Lock busy...cancel AF");
        rc = native_stop_ops(CAMERA_OPS_FOCUS, NULL) ? NO_ERROR : UNKNOWN_ERROR;

        /*now just wait for auto focus thread to be finished*/
        mAutoFocusThreadLock.lock();
        mAutoFocusThreadLock.unlock();
    }
    ALOGV("cancelAutoFocusInternal X: %d", rc);
    return rc;
}

void *auto_focus_thread(void *user)
{
    ALOGV("auto_focus_thread E");

    QualcommCameraHardware *obj = QualcommCameraHardware::getInstance();
    if (obj != 0) {
        obj->runAutoFocus();
    }
    else ALOGW("not starting autofocus: the object went away!");
    ALOGV("auto_focus_thread X");
    return NULL;
}

status_t QualcommCameraHardware::autoFocus()
{
    ALOGV("autoFocus E");
    Mutex::Autolock l(&mLock);

    if (!mHasAutoFocusSupport) {
       /*
        * If autofocus is not supported HAL defaults
        * focus mode to infinity and supported mode to
        * infinity also. In this mode and fixed mode app
        * should not call auto focus.
        */
        ALOGE("Auto Focus not supported");
        ALOGV("autoFocus X");
        return INVALID_OPERATION;
    }
    {
        mAutoFocusThreadLock.lock();
        if (!mAutoFocusThreadRunning) {
            // Create a detached thread here so that we don't have to wait
            // for it when we cancel AF.
            pthread_t thr;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            mAutoFocusThreadRunning =
                !pthread_create(&thr, &attr, auto_focus_thread, NULL);
            if (!mAutoFocusThreadRunning) {
                ALOGE("failed to start autofocus thread");
                mAutoFocusThreadLock.unlock();
                return UNKNOWN_ERROR;
            }
        }
        mAutoFocusThreadLock.unlock();
    }

    ALOGV("autoFocus X");
    return NO_ERROR;
}

status_t QualcommCameraHardware::cancelAutoFocus()
{
    ALOGV("cancelAutoFocus E");
    Mutex::Autolock l(&mLock);

    int rc = NO_ERROR;
    if (mCameraRunning && mNotifyCallback && (mMsgEnabled & CAMERA_MSG_FOCUS)) {
        rc = cancelAutoFocusInternal();
    }

    ALOGV("cancelAutoFocus X");
    return rc;
}

void QualcommCameraHardware::runSnapshotThread(void *data)
{
    bool ret = true;

    ALOGI("runSnapshotThread E");

    if (!libmmcamera) {
        ALOGE("FATAL ERROR: could not dlopen liboemcamera.so: %s", dlerror());
    }
    mSnapshotCancelLock.lock();
    if (mSnapshotCancel == true) {
        mSnapshotCancel = false;
        mSnapshotCancelLock.unlock();
        ALOGI("%s: cancelpicture has been called..so abort taking snapshot", __FUNCTION__);
        deinitRaw();
        mInSnapshotModeWaitLock.lock();
        mInSnapshotMode = false;
        mInSnapshotModeWait.signal();
        mInSnapshotModeWaitLock.unlock();
        mSnapshotThreadWaitLock.lock();
        mSnapshotThreadRunning = false;
        mSnapshotThreadWait.signal();
        mSnapshotThreadWaitLock.unlock();
        return;
    }
    mSnapshotCancelLock.unlock();

    mJpegThreadWaitLock.lock();
    mJpegThreadRunning = true;
    mJpegThreadWait.signal();
    mJpegThreadWaitLock.unlock();
    mm_camera_ops_type_t current_ops_type = (mSnapshotFormat == PICTURE_FORMAT_JPEG) ?
        CAMERA_OPS_CAPTURE_AND_ENCODE : CAMERA_OPS_RAW_CAPTURE;
    if (strTexturesOn == true) {
        current_ops_type = CAMERA_OPS_CAPTURE;
        mCamOps.mm_camera_start(current_ops_type, &mImageCaptureParms, NULL);
    } else if (mSnapshotFormat == PICTURE_FORMAT_JPEG) {
        if (!mZslEnable || mZslFlashEnable) {
            mCamOps.mm_camera_start(current_ops_type, &mImageCaptureParms, &mImageEncodeParms);
        } else {
            notifyShutter(TRUE);
            initZslParameter();
            ALOGE("snapshot mZslCapture.thumbnail %d %d %d", mZslCaptureParms.thumbnail_width,
                mZslCaptureParms.thumbnail_height,mZslCaptureParms.num_captures);
            mCamOps.mm_camera_start(current_ops_type, &mZslCaptureParms, &mImageEncodeParms);
        }
        mJpegThreadWaitLock.lock();
        while (mJpegThreadRunning) {
            ALOGV("%s: waiting for jpeg callback.", __FUNCTION__);
            mJpegThreadWait.wait(mJpegThreadWaitLock);
            ALOGV("%s: jpeg callback received.", __FUNCTION__);
        }
        mJpegThreadWaitLock.unlock();

        //cleanup
        if (!mZslEnable || mZslFlashEnable)
            deinitRaw();
    } else if (mSnapshotFormat == PICTURE_FORMAT_RAW) {
        notifyShutter(TRUE);
        mCamOps.mm_camera_start(current_ops_type, &mRawCaptureParms, NULL);
        // Waiting for callback to come
        ALOGV("runSnapshotThread : waiting for callback to come");
        mJpegThreadWaitLock.lock();
        while (mJpegThreadRunning) {
            ALOGV("%s: waiting for jpeg callback.", __FUNCTION__);
            mJpegThreadWait.wait(mJpegThreadWaitLock);
            ALOGV("%s: jpeg callback received.", __FUNCTION__);
        }
        mJpegThreadWaitLock.unlock();
        ALOGV("runSnapshotThread : calling deinitRawSnapshot");
        deinitRawSnapshot();

    }

    if (!mZslEnable || mZslFlashEnable)
        mCamOps.mm_camera_deinit(current_ops_type, NULL, NULL);
    mZslFlashEnable  = false;
    mSnapshotThreadWaitLock.lock();
    mSnapshotThreadRunning = false;
    mSnapshotThreadWait.signal();
    mSnapshotThreadWaitLock.unlock();
    ALOGI("runSnapshotThread X");
}

void *snapshot_thread(void *user)
{
    ALOGD("snapshot_thread E");

    QualcommCameraHardware *obj = QualcommCameraHardware::getInstance();
    if (obj != 0) {
        obj->runSnapshotThread(user);
    }
    else ALOGW("not starting snapshot thread: the object went away!");
    ALOGD("snapshot_thread X");
    return NULL;
}

status_t QualcommCameraHardware::takePicture()
{
    ALOGE("takePicture(%d)", mMsgEnabled);
    Mutex::Autolock l(&mLock);
    if (mRecordingState) {
        return takeLiveSnapshotInternal();
    }

    if (strTexturesOn == true) {
        mEncodePendingWaitLock.lock();
        while (mEncodePending) {
            ALOGE("takePicture: Frame given to application, waiting for encode call");
            mEncodePendingWait.wait(mEncodePendingWaitLock);
            ALOGE("takePicture: Encode of the application data is done");
        }
        mEncodePendingWaitLock.unlock();
    }

    // Wait for old snapshot thread to complete.
    mSnapshotThreadWaitLock.lock();
    while (mSnapshotThreadRunning) {
        ALOGV("takePicture: waiting for old snapshot thread to complete.");
        mSnapshotThreadWait.wait(mSnapshotThreadWaitLock);
        ALOGV("takePicture: old snapshot thread completed.");
    }
    // if flash is enabled then run snapshot as normal mode and not zsl mode.
    // App should expect only 1 callback as multi snapshot in normal mode is not supported
    mZslFlashEnable = false;
    if (mZslEnable) {
        int is_flash_needed = 0;
        mm_camera_status_t status;
        status = mCfgControl.mm_camera_get_parm(CAMERA_PARM_QUERY_FALSH4SNAP,
            &is_flash_needed);
        if (is_flash_needed) {
            mZslFlashEnable = true;
        }
    }

    if (mParameters.getPictureFormat() != 0 &&
        !strcmp(mParameters.getPictureFormat(),
            CameraParameters::PIXEL_FORMAT_RAW)) {
        mSnapshotFormat = PICTURE_FORMAT_RAW;
        // HACK: Raw ZSL capture is not supported yet
        mZslFlashEnable = true;
    } else
        mSnapshotFormat = PICTURE_FORMAT_JPEG;

    if (!mZslEnable || mZslFlashEnable) {
        if (mSnapshotFormat == PICTURE_FORMAT_JPEG) {
            if (!native_start_ops(CAMERA_OPS_PREPARE_SNAPSHOT, NULL)) {
                mSnapshotThreadWaitLock.unlock();
                ALOGE("PREPARE SNAPSHOT: CAMERA_OPS_PREPARE_SNAPSHOT ioctl Failed");
                return UNKNOWN_ERROR;
            }
        }
    } else {
        int rotation = mParameters.getInt(CameraParameters::KEY_ROTATION);
        native_set_parms(CAMERA_PARM_JPEG_ROTATION, sizeof(int), &rotation);
    }

    if (!mZslEnable || mZslFlashEnable)
        stopPreviewInternal();

    mFrameThreadWaitLock.unlock();

    mm_camera_ops_type_t current_ops_type = (mSnapshotFormat == PICTURE_FORMAT_JPEG) ?
        CAMERA_OPS_CAPTURE_AND_ENCODE :
        CAMERA_OPS_RAW_CAPTURE;
    if (strTexturesOn == true)
        current_ops_type = CAMERA_OPS_CAPTURE;

    if (!mZslEnable || mZslFlashEnable)
        mCamOps.mm_camera_init(current_ops_type, NULL, NULL);

    if (mSnapshotFormat == PICTURE_FORMAT_JPEG) {
        if (!mZslEnable || mZslFlashEnable) {
            if (!initRaw(mDataCallback && (mMsgEnabled & CAMERA_MSG_COMPRESSED_IMAGE))) {
                ALOGE("initRaw failed.  Not taking picture.");
                mSnapshotThreadWaitLock.unlock();
                return UNKNOWN_ERROR;
            }
        }
    } else if (mSnapshotFormat == PICTURE_FORMAT_RAW) {
        if (!initRawSnapshot()) {
            ALOGE("initRawSnapshot failed. Not taking picture.");
            mSnapshotThreadWaitLock.unlock();
            return UNKNOWN_ERROR;
        }
    }

    mShutterLock.lock();
    mShutterPending = true;
    mShutterLock.unlock();

    mSnapshotCancelLock.lock();
    mSnapshotCancel = false;
    mSnapshotCancelLock.unlock();

    numJpegReceived = 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    mSnapshotThreadRunning = !pthread_create(&mSnapshotThread,
        &attr,
        snapshot_thread,
        NULL);
    mSnapshotThreadWaitLock.unlock();

    mInSnapshotModeWaitLock.lock();
    mInSnapshotMode = true;
    mInSnapshotModeWaitLock.unlock();

    ALOGE("takePicture: X");
    return mSnapshotThreadRunning ? NO_ERROR : UNKNOWN_ERROR;
}

status_t QualcommCameraHardware::takeLiveSnapshotInternal()
{
    ALOGV("takeLiveSnapshotInternal : E");
    if (liveshot_state == LIVESHOT_IN_PROGRESS || !mRecordingState) {
        return NO_ERROR;
    }

    if ((mCurrentTarget != TARGET_MSM7630) &&
        (mCurrentTarget != TARGET_MSM8660) &&
        (mCurrentTarget != TARGET_MSM7627A)) {
        ALOGI("LiveSnapshot not supported on this target");
        liveshot_state = LIVESHOT_STOPPED;
        return NO_ERROR;
    }

    liveshot_state = LIVESHOT_IN_PROGRESS;

    if (!initLiveSnapshot(videoWidth, videoHeight)) {
        ALOGE("takeLiveSnapshot: Jpeg Heap Memory allocation failed.  Not taking Live Snapshot.");
        liveshot_state = LIVESHOT_STOPPED;
        return UNKNOWN_ERROR;
    }

    setExifInfo();

    uint32_t maxjpegsize = videoWidth * videoHeight * 1.5;
    if (!LINK_set_liveshot_params(videoWidth, videoHeight,
        exif_data, exif_table_numEntries,
        (uint8_t *)mJpegLiveSnapMapped->data, maxjpegsize)) {
        ALOGE("Link_set_liveshot_params failed.");
        if (NULL != mJpegLiveSnapMapped) {
            ALOGV("initLiveSnapshot: clearing old mJpegLiveSnapMapped.");
            mJpegLiveSnapMapped->release(mJpegLiveSnapMapped);
            mJpegLiveSnapMapped = NULL;
        }
        return NO_ERROR;
    }
    if ((mCurrentTarget == TARGET_MSM7630) ||
        (mCurrentTarget == TARGET_MSM8660)) {
        if (!native_start_ops(CAMERA_OPS_LIVESHOT, NULL)) {
            ALOGE("start_liveshot ioctl failed");
            liveshot_state = LIVESHOT_STOPPED;
            if (NULL != mJpegLiveSnapMapped) {
                ALOGV("initLiveSnapshot: clearing old mJpegLiveSnapMapped.");
                mJpegLiveSnapMapped->release(mJpegLiveSnapMapped);
                mJpegLiveSnapMapped = NULL;
            }
            return UNKNOWN_ERROR;
        }
    }

    ALOGV("takeLiveSnapshotInternal: X");
    return NO_ERROR;
}

status_t QualcommCameraHardware::takeLiveSnapshot()
{
    ALOGV("takeLiveSnapshot: E ");
    Mutex::Autolock l(&mLock);
    ALOGV("takeLiveSnapshot: X ");
    return takeLiveSnapshotInternal();
}

bool QualcommCameraHardware::initLiveSnapshot(int videowidth, int videoheight)
{
    ALOGV("initLiveSnapshot E");

    if (NULL != mJpegLiveSnapMapped) {
        ALOGV("initLiveSnapshot: clearing old mJpegLiveSnapMapped.");
        mJpegLiveSnapMapped->release(mJpegLiveSnapMapped);
        mJpegLiveSnapMapped = NULL;
    }

    mJpegMaxSize = videowidth * videoheight * 1.5;
    ALOGV("initLiveSnapshot: initializing mJpegLiveSnapMapped.");
    mJpegLiveSnapMapped = mGetMemory(-1, mJpegMaxSize, 1, mCallbackCookie);
    if (mJpegLiveSnapMapped == NULL) {
        ALOGE("Failed to get camera memory for mJpegLibeSnapMapped" );
        return false;
    }
    ALOGV("initLiveSnapshot X");
    return true;
}

status_t QualcommCameraHardware::cancelPicture()
{
    status_t rc;
    ALOGI("cancelPicture: E");

    mSnapshotCancelLock.lock();
    ALOGI("%s: setting mSnapshotCancel to true", __FUNCTION__);
    mSnapshotCancel = true;
    mSnapshotCancelLock.unlock();

    if (mCurrentTarget == TARGET_MSM7627 ||
        mCurrentTarget == TARGET_MSM7625A ||
        mCurrentTarget == TARGET_MSM7627A) {
        mSnapshotDone = TRUE;
        mSnapshotThreadWaitLock.lock();
        while (mSnapshotThreadRunning) {
            ALOGV("cancelPicture: waiting for snapshot thread to complete.");
            mSnapshotThreadWait.wait(mSnapshotThreadWaitLock);
            ALOGV("cancelPicture: snapshot thread completed.");
        }
        mSnapshotThreadWaitLock.unlock();
    }
    rc = native_stop_ops(CAMERA_OPS_CAPTURE, NULL) ? NO_ERROR : UNKNOWN_ERROR;
    mSnapshotDone = FALSE;
    ALOGI("cancelPicture: X: %d", rc);
    return rc;
}

status_t QualcommCameraHardware::setParameters(const CameraParameters& params)
{
    ALOGV("setParameters: E params = %p", &params);

    Mutex::Autolock l(&mLock);
    Mutex::Autolock pl(&mParametersLock);
    status_t rc, final_rc = NO_ERROR;
    if (mSnapshotThreadRunning) {
        if ((rc = setCameraMode(params)))  final_rc = rc;
        if ((rc = setPreviewSize(params)))  final_rc = rc;
        if ((rc = setRecordSize(params)))  final_rc = rc;
        if ((rc = setPictureSize(params)))  final_rc = rc;
        if ((rc = setJpegThumbnailSize(params))) final_rc = rc;
        if ((rc = setJpegQuality(params)))  final_rc = rc;
        return final_rc;
    }
    if ((rc = setCameraMode(params)))  final_rc = rc;
    if ((rc = setPreviewSize(params)))  final_rc = rc;
    if ((rc = setRecordSize(params)))  final_rc = rc;
    if ((rc = setPictureSize(params)))  final_rc = rc;
    if ((rc = setJpegThumbnailSize(params))) final_rc = rc;
    if ((rc = setJpegQuality(params)))  final_rc = rc;
    if ((rc = setPictureFormat(params))) final_rc = rc;
    if ((rc = setPreviewFormat(params)))   final_rc = rc;
    if ((rc = setEffect(params)))       final_rc = rc;
    if ((rc = setGpsLocation(params)))  final_rc = rc;
    if ((rc = setRotation(params)))     final_rc = rc;
    if ((rc = setZoom(params)))         final_rc = rc;
    if ((rc = setOrientation(params)))  final_rc = rc;
    if ((rc = setLensshadeValue(params)))  final_rc = rc;
    if ((rc = setMCEValue(params)))  final_rc = rc;
    //if ((rc = setHDRImaging(params)))  final_rc = rc;
    if ((rc = setExpBracketing(params)))  final_rc = rc;
    if ((rc = setPictureFormat(params))) final_rc = rc;
    if ((rc = setSharpness(params)))    final_rc = rc;
    if ((rc = setSaturation(params)))   final_rc = rc;
    if ((rc = setTouchAfAec(params)))   final_rc = rc;
    if ((rc = setSceneMode(params)))    final_rc = rc;
    if ((rc = setContrast(params)))     final_rc = rc;
    if ((rc = setSceneDetect(params)))  final_rc = rc;
    if ((rc = setStrTextures(params)))   final_rc = rc;
    if ((rc = setPreviewFormat(params)))   final_rc = rc;
    if ((rc = setSkinToneEnhancement(params)))   final_rc = rc;
    if ((rc = setAntibanding(params)))  final_rc = rc;
    if ((rc = setRedeyeReduction(params)))  final_rc = rc;
    if ((rc = setDenoise(params)))  final_rc = rc;
    if ((rc = setPreviewFpsRange(params)))  final_rc = rc;
    if ((rc = setZslParam(params)))  final_rc = rc;
    if ((rc = setSnapshotCount(params)))  final_rc = rc;
    if ((rc = setRecordingHint(params)))   final_rc = rc;
    const char *str = params.get(CameraParameters::KEY_SCENE_MODE);
    int32_t value = attr_lookup(scenemode, sizeof(scenemode) / sizeof(str_map), str);

    if ((value != NOT_FOUND) && (value == CAMERA_BESTSHOT_OFF)) {
        if ((rc = setPreviewFrameRate(params))) final_rc = rc;
    //    if ((rc = setPreviewFrameRateMode(params))) final_rc = rc;
        if ((rc = setAutoExposure(params))) final_rc = rc;
        if ((rc = setExposureCompensation(params))) final_rc = rc;
        if ((rc = setWhiteBalance(params))) final_rc = rc;
        if ((rc = setFlash(params)))        final_rc = rc;
        if ((rc = setFocusMode(params)))    final_rc = rc;
        if ((rc = setBrightness(params)))   final_rc = rc;
        if ((rc = setISOValue(params)))  final_rc = rc;
        if ((rc = setFocusAreas(params)))  final_rc = rc;
        if ((rc = setMeteringAreas(params)))  final_rc = rc;
    }
    //selectableZoneAF needs to be invoked after continuous AF
    if ((rc = setSelectableZoneAf(params)))   final_rc = rc;
    // setHighFrameRate needs to be done at end, as there can
    // be a preview restart, and need to use the updated parameters
    if ((rc = setHighFrameRate(params)))  final_rc = rc;

    ALOGV("setParameters: X");
    return final_rc;
}

CameraParameters QualcommCameraHardware::getParameters() const
{
    ALOGV("getParameters: EX");
    return mParameters;
}

status_t QualcommCameraHardware::setHistogramOn()
{
    ALOGV("setHistogramOn: EX");
    mStatsWaitLock.lock();
    mSendData = true;
    if (mStatsOn == CAMERA_HISTOGRAM_ENABLE) {
        mStatsWaitLock.unlock();
        return NO_ERROR;
    }

    mStatSize = sizeof(uint32_t) * HISTOGRAM_STATS_SIZE;
    mCurrent = -1;
    /*Currently the Ashmem is multiplying the buffer size with total number
    of buffers and page aligning. This causes a crash in JNI as each buffer
    individually expected to be page aligned  */
    int page_size_minus_1 = getpagesize() - 1;
    int32_t mAlignedStatSize = ((mStatSize + page_size_minus_1) & (~page_size_minus_1));
    for (int cnt = 0; cnt < 3; cnt++) {
        mStatsMapped[cnt] = mGetMemory(-1, mStatSize, 1, mCallbackCookie);
        if (mStatsMapped[cnt] == NULL) {
            ALOGE("Failed to get camera memory for stats heap index: %d", cnt);
            mStatsWaitLock.unlock();
            return false;
        } else {
            ALOGV("Received following info for stats mapped data:%p,handle:%p, size:%d,release:%p",
            mStatsMapped[cnt]->data ,mStatsMapped[cnt]->handle, mStatsMapped[cnt]->size, mStatsMapped[cnt]->release);
        }
    }
    mStatsOn = CAMERA_HISTOGRAM_ENABLE;
    mStatsWaitLock.unlock();
    mCfgControl.mm_camera_set_parm(CAMERA_PARM_HISTOGRAM, &mStatsOn);
    return NO_ERROR;
}

status_t QualcommCameraHardware::setHistogramOff()
{
    ALOGV("setHistogramOff: EX");
    mStatsWaitLock.lock();
    if (mStatsOn == CAMERA_HISTOGRAM_DISABLE) {
        mStatsWaitLock.unlock();
        return NO_ERROR;
    }
    mStatsOn = CAMERA_HISTOGRAM_DISABLE;
    mStatsWaitLock.unlock();

    mCfgControl.mm_camera_set_parm(CAMERA_PARM_HISTOGRAM, &mStatsOn);

    mStatsWaitLock.lock();
    for (int i = 0; i < 3; i++) {
        if (mStatsMapped[i] != NULL) {
            mStatsMapped[i]->release(mStatsMapped[i]);
            mStatsMapped[i] = NULL;
        }
    }

    mStatsWaitLock.unlock();
    return NO_ERROR;
}

status_t QualcommCameraHardware::runFaceDetection()
{
    return BAD_VALUE;
}

void *smoothzoom_thread(void *user)
{
    // call runsmoothzoomthread
    ALOGV("smoothzoom_thread E");

    QualcommCameraHardware *obj = QualcommCameraHardware::getInstance();
    if (obj != 0) {
        obj->runSmoothzoomThread(user);
    }
    else ALOGE("not starting smooth zoom thread: the object went away!");
    ALOGV("Smoothzoom_thread X");
    return NULL;
}

status_t QualcommCameraHardware::sendCommand(int32_t command, int32_t arg1,
                                             int32_t arg2)
{
    ALOGV("sendCommand: EX");

    Mutex::Autolock l(&mLock);

    switch(command) {
    case CAMERA_CMD_HISTOGRAM_ON:
        ALOGV("histogram set to on");
        return setHistogramOn();
    case CAMERA_CMD_HISTOGRAM_OFF:
        ALOGV("histogram set to off");
        return setHistogramOff();
    case CAMERA_CMD_HISTOGRAM_SEND_DATA:
        mStatsWaitLock.lock();
        if (mStatsOn == CAMERA_HISTOGRAM_ENABLE)
            mSendData = true;
        mStatsWaitLock.unlock();
        return NO_ERROR;
    case CAMERA_CMD_ENABLE_FOCUS_MOVE_MSG: /* Stub this for now. */
        return NO_ERROR;
   }
   return BAD_VALUE;
}

void QualcommCameraHardware::runSmoothzoomThread(void *data) {

    ALOGV("runSmoothzoomThread: Current zoom %d - "
          "Target %d", mParameters.getInt(CameraParameters::KEY_ZOOM), mTargetSmoothZoom);
    int current_zoom = mParameters.getInt(CameraParameters::KEY_ZOOM);
    int step = (current_zoom > mTargetSmoothZoom)? -1: 1;

    if (current_zoom == mTargetSmoothZoom) {
        ALOGV("Smoothzoom target zoom value is same as "
             "current zoom value, return...");
        if (!mPreviewStopping)
            mNotifyCallback(CAMERA_MSG_ZOOM,
                current_zoom, 1, mCallbackCookie);
        else
            ALOGV("Not issuing callback since preview is stopping");
        return;
    }

    CameraParameters p = getParameters();

    mSmoothzoomThreadWaitLock.lock();
    mSmoothzoomThreadRunning = true;
    mSmoothzoomThreadWaitLock.unlock();

    int i = current_zoom;
    while (true) {  // Thread loop
        mSmoothzoomThreadLock.lock();
        if (mSmoothzoomThreadExit) {
            ALOGV("Exiting smoothzoom thread, as stop smoothzoom called");
            mSmoothzoomThreadLock.unlock();
            break;
        }
        mSmoothzoomThreadLock.unlock();

        if ((i < 0) || (i > mMaxZoom)) {
            ALOGE(" ERROR : beyond supported zoom values, break..");
            break;
        }
        // update zoom
        p.set(CameraParameters::KEY_ZOOM, i);
        setZoom(p);
        if (!mPreviewStopping) {
            // give call back to zoom listener in app
            mNotifyCallback(CAMERA_MSG_ZOOM, i, (mTargetSmoothZoom-i == 0)?1:0,
                    mCallbackCookie);
        } else {
            ALOGV("Preview is stopping. Breaking out of smooth zoom loop");
            break;
        }
        if (i == mTargetSmoothZoom)
            break;

        i+=step;

        /* wait on singal, which will be signalled on
         * receiving next preview frame */
        mSmoothzoomThreadWaitLock.lock();
        mSmoothzoomThreadWait.wait(mSmoothzoomThreadWaitLock);
        mSmoothzoomThreadWaitLock.unlock();
    } // while loop over, exiting thread

    mSmoothzoomThreadWaitLock.lock();
    mSmoothzoomThreadRunning = false;
    mSmoothzoomThreadWaitLock.unlock();
    ALOGV("Exiting Smooth Zoom Thread");
}

extern "C" QualcommCameraHardware *HAL_openCameraHardware(int cameraId)
{
    int i;
    ALOGI("openCameraHardware: call createInstance");
    for (i = 0; i < HAL_numOfCameras; i++) {
        if (i == cameraId) {
            ALOGI("openCameraHardware:Valid camera ID %d", cameraId);
            parameter_string_initialized = false;
            HAL_currentCameraId = cameraId;
            HAL_currentCameraMode = CAMERA_MODE_2D;
            HAL_currentSnapshotMode = CAMERA_SNAPSHOT_NONZSL;
            ALOGI("%s: HAL_currentSnapshotMode = %d HAL_currentCameraMode = %d", __FUNCTION__, HAL_currentSnapshotMode,
                 HAL_currentCameraMode);
            return QualcommCameraHardware::createInstance();
        }
    }
    ALOGE("openCameraHardware:Invalid camera ID %d", cameraId);
    return NULL;
}

static QualcommCameraHardware *hardware = NULL;

// If the hardware already exists, return a strong pointer to the current
// object. If not, create a new hardware object, put it in the singleton,
// and return it.
QualcommCameraHardware *QualcommCameraHardware::createInstance()
{
    ALOGI("createInstance: E");

    QualcommCameraHardware *cam = new QualcommCameraHardware();
    hardware = cam;

    ALOGI("createInstance: created hardware=%p", cam);
    if (!cam->startCamera()) {
        ALOGE("%s: startCamera failed!", __FUNCTION__);
        hardware = NULL;
        delete cam;
        return NULL;
    }

    cam->initDefaultParameters();
    ALOGI("createInstance: X");
    return cam;
}

// For internal use only, hence the strong pointer to the derived type.
QualcommCameraHardware *QualcommCameraHardware::getInstance()
{
    return hardware;
}

void QualcommCameraHardware::receiveRecordingFrame(struct msm_frame *frame)
{
    ALOGV("receiveRecordingFrame E");
    // post busy frame
    if (frame) {
        cam_frame_post_video(frame);
    }
    else
        ALOGE("in receiveRecordingFrame frame is NULL");
    ALOGV("receiveRecordingFrame X");
}

void QualcommCameraHardware::receiveLiveSnapshot(uint32_t jpeg_size)
{
    ALOGV("receiveLiveSnapshot E");
    Mutex::Autolock cbLock(&mCallbackLock);
    if (mDataCallback && (mMsgEnabled & CAMERA_MSG_COMPRESSED_IMAGE)) {
        mDataCallback(CAMERA_MSG_COMPRESSED_IMAGE, mJpegLiveSnapMapped ,data_counter,
            NULL, mCallbackCookie);

    }
    else
        ALOGV("JPEG callback was cancelled--not delivering image.");

    //Reset the Gps Information & relieve memory
    exif_table_numEntries = 0;

    liveshot_state = LIVESHOT_DONE;

    ALOGV("receiveLiveSnapshot X");
}

void QualcommCameraHardware::receivePreviewFrame(struct msm_frame *frame)
{
    ALOGV("receivePreviewFrame E");
    if (!mCameraRunning) {
        ALOGE("ignoring preview callback--camera has been stopped");
        LINK_camframe_add_frame(CAM_PREVIEW_FRAME,frame);
        return;
    }
    if (mCurrentTarget == TARGET_MSM7627A && liveshot_state == LIVESHOT_IN_PROGRESS) {
        LINK_set_liveshot_frame(frame);
    }
    if (mPreviewBusyQueue.add(frame) == false)
        LINK_camframe_add_frame(CAM_PREVIEW_FRAME, frame);
    ALOGV("receivePreviewFrame X");
}

void QualcommCameraHardware::receiveCameraStats(camstats_type stype, camera_preview_histogram_info *histinfo)
{
    if (!mCameraRunning) {
        ALOGE("ignoring stats callback--camera has been stopped");
        return;
    }

    mCallbackLock.lock();
    int msgEnabled = mMsgEnabled;
    camera_data_callback scb = mDataCallback;
    void *sdata = mCallbackCookie;
    mCallbackLock.unlock();
    mStatsWaitLock.lock();
    if (mStatsOn == CAMERA_HISTOGRAM_DISABLE) {
        mStatsWaitLock.unlock();
        return;
    }
    if (!mSendData) {
        mStatsWaitLock.unlock();
    } else {
        mSendData = false;
        mCurrent = (mCurrent+1)%3;
        *(uint32_t *)((unsigned int)(mStatsMapped[mCurrent]->data)) = histinfo->max_value;
        memcpy((uint32_t *)((unsigned int)mStatsMapped[mCurrent]->data + sizeof(int32_t)), (uint32_t *)histinfo->buffer,(sizeof(int32_t) * 256));

        mStatsWaitLock.unlock();

        if (scb != NULL && (msgEnabled & CAMERA_MSG_STATS_DATA))
            scb(CAMERA_MSG_STATS_DATA, mStatsMapped[mCurrent], data_counter, NULL,sdata);

    }
    ALOGV("receiveCameraStats X");
}

bool QualcommCameraHardware::initRecord()
{
    int CbCrOffset;
    int recordBufferSize;
    int active, type = 0;

    ALOGV("initREcord E");
    if (mZslEnable) {
        ALOGV("initRecord X.. Not intializing Record buffers in ZSL mode");
        return true;
    }

    ALOGI("initRecord: mDimension.video_width = %d mDimension.video_height = %d",
             mDimension.video_width, mDimension.video_height);
    // for 8x60 the Encoder expects the CbCr offset should be aligned to 2K.
    if (mCurrentTarget == TARGET_MSM8660) {
        CbCrOffset = PAD_TO_2K(mDimension.video_width  * mDimension.video_height);
        recordBufferSize = CbCrOffset + PAD_TO_2K((mDimension.video_width * mDimension.video_height)/2);
    } else {
        CbCrOffset = PAD_TO_WORD(mDimension.video_width  * mDimension.video_height);
        recordBufferSize = (mDimension.video_width  * mDimension.video_height *3)/2;
    }

    /* Buffersize and frameSize will be different when DIS is ON.
     * We need to pass the actual framesize with video heap, as the same
     * is used at camera MIO when negotiating with encoder.
     */
    mRecordFrameSize = PAD_TO_4K(recordBufferSize);
    bool dis_disable = 0;
    const char *str = mParameters.get(CameraParameters::KEY_VIDEO_HIGH_FRAME_RATE);
    if ((str != NULL) && (strcmp(str, CameraParameters::VIDEO_HFR_OFF))) {
        ALOGI("%s: HFR is ON, DIS has to be OFF", __FUNCTION__);
        dis_disable = 1;
    }
    if ((mVpeEnabled && mDisEnabled && (!dis_disable))|| mIs3DModeOn) {
        mRecordFrameSize = videoWidth * videoHeight * 3 / 2;
        if (mCurrentTarget == TARGET_MSM8660) {
            mRecordFrameSize = PAD_TO_4K(PAD_TO_2K(videoWidth * videoHeight)
                + PAD_TO_2K((videoWidth * videoHeight)/2));
        }
    }
    ALOGV("mRecordFrameSize = %d", mRecordFrameSize);

    for (int cnt = 0; cnt < kRecordBufferCount; cnt++) {
#ifdef USE_ION
        int ion_heap = ION_CP_MM_HEAP_ID;
        if (allocate_ion_memory(&record_main_ion_fd[cnt], &record_alloc[cnt], &record_ion_info_fd[cnt],
                ion_heap, mRecordFrameSize, &mRecordfd[cnt]) < 0) {
            ALOGE("%s: allocate ion memory failed!\n", __func__);
            return NULL;
        }
#else
        const char *pmem_region;
        if (mCurrentTarget == TARGET_MSM8660) {
            pmem_region = "/dev/pmem_smipool";
        } else {
            pmem_region = "/dev/pmem_adsp";
        }
        mRecordfd[cnt] = open(pmem_region, O_RDWR|O_SYNC);
        if (mRecordfd[cnt] <= 0) {
            ALOGE("%s: Open device %s failed!\n",__func__, pmem_region);
            return NULL;
        }
#endif
        ALOGE("%s  Record fd is %d ", __func__, mRecordfd[cnt]);
        mRecordMapped[cnt] = mGetMemory(mRecordfd[cnt], mRecordFrameSize, 1, mCallbackCookie);
        if (mRecordMapped[cnt] == NULL) {
            ALOGE("Failed to get camera memory for mRecordMapped heap");
        } else {
        ALOGE("Received following info for record mapped data:%p,handle:%p, size:%d,release:%p",
            mRecordMapped[cnt]->data ,mRecordMapped[cnt]->handle, mRecordMapped[cnt]->size, mRecordMapped[cnt]->release);
        }
        recordframes[cnt].buffer = (unsigned int)mRecordMapped[cnt]->data;
        recordframes[cnt].fd = mRecordfd[cnt];
        recordframes[cnt].y_off = 0;
        recordframes[cnt].cbcr_off = CbCrOffset;
        recordframes[cnt].path = OUTPUT_TYPE_V;
        record_buffers_tracking_flag[cnt] = false;
        ALOGV ("initRecord :  record heap , video buffers  buffer=%lu fd=%d y_off=%d cbcr_off=%d \n",
            (unsigned long)recordframes[cnt].buffer, recordframes[cnt].fd, recordframes[cnt].y_off,
            recordframes[cnt].cbcr_off);
        active = cnt < ACTIVE_VIDEO_BUFFERS;
        type = MSM_PMEM_VIDEO;
        if (mVpeEnabled && (cnt == kRecordBufferCount-1)) {
            type = MSM_PMEM_VIDEO_VPE;
            active = 1;
        }
        ALOGE("Registering buffer %d with kernel",cnt);
        register_buf(mRecordFrameSize,
            CbCrOffset, 0,
            recordframes[cnt].fd,
            0,
            (uint8_t *)recordframes[cnt].buffer,
            type,
            active);
        ALOGE("Came back from register call to kernel");
    }

    // initial setup : buffers 1,2,3 with kernel , 4 with camframe , 5,6,7,8 in free Q
    // flush the busy Q
    cam_frame_flush_video();

    mVideoThreadWaitLock.lock();
    while (mVideoThreadRunning) {
        ALOGV("initRecord: waiting for old video thread to complete.");
        mVideoThreadWait.wait(mVideoThreadWaitLock);
        ALOGV("initRecord : old video thread completed.");
    }
    mVideoThreadWaitLock.unlock();

    // flush free queue and add 5,6,7,8 buffers.
    LINK_camframe_release_all_frames(CAM_VIDEO_FRAME);
    if (mVpeEnabled) {
        //If VPE is enabled, the VPE buffer shouldn't be added to Free Q initally.
        for (int i = ACTIVE_VIDEO_BUFFERS; i < kRecordBufferCount-1; i++)
            LINK_camframe_add_frame(CAM_VIDEO_FRAME, &recordframes[i]);
    } else {
        for (int i = ACTIVE_VIDEO_BUFFERS; i < kRecordBufferCount; i++)
            LINK_camframe_add_frame(CAM_VIDEO_FRAME, &recordframes[i]);
    }
    ALOGV("initREcord X");

    return true;
}

status_t QualcommCameraHardware::setDIS()
{
    ALOGV("setDIS E");

    video_dis_param_ctrl_t disCtrl;
    bool ret = true;
    ALOGV("mDisEnabled = %d", mDisEnabled);

    int video_frame_cbcroffset;
    video_frame_cbcroffset = PAD_TO_WORD(videoWidth * videoHeight);
    if (mCurrentTarget == TARGET_MSM8660)
        video_frame_cbcroffset = PAD_TO_2K(videoWidth * videoHeight);

    disCtrl.dis_enable = mDisEnabled;
    const char *str = mParameters.get(CameraParameters::KEY_VIDEO_HIGH_FRAME_RATE);
    if (str != NULL && strcmp(str, CameraParameters::VIDEO_HFR_OFF)) {
        ALOGI("%s: HFR is ON, setting DIS as OFF", __FUNCTION__);
        disCtrl.dis_enable = 0;
    }
    disCtrl.video_rec_width = videoWidth;
    disCtrl.video_rec_height = videoHeight;
    disCtrl.output_cbcr_offset = video_frame_cbcroffset;

    ret = native_set_parms(CAMERA_PARM_VIDEO_DIS, sizeof(disCtrl), &disCtrl);

    ALOGV("setDIS X (%d)", ret);
    return ret ? NO_ERROR : UNKNOWN_ERROR;
}

status_t QualcommCameraHardware::setVpeParameters()
{
    ALOGV("setVpeParameters E");

    video_rotation_param_ctrl_t rotCtrl;
    if (sensor_rotation == 0)
        rotCtrl.rotation = ROT_NONE;
    else if (sensor_rotation == 90)
        rotCtrl.rotation = ROT_CLOCKWISE_90;
    else if (sensor_rotation == 180)
        rotCtrl.rotation = ROT_CLOCKWISE_180;
    else
        rotCtrl.rotation = ROT_CLOCKWISE_270;

    ALOGV("rotCtrl.rotation = %d", rotCtrl.rotation);

    bool ret = native_set_parms(CAMERA_PARM_VIDEO_ROT, sizeof(rotCtrl), &rotCtrl);

    ALOGV("setVpeParameters X (%d)", ret);
    return ret ? NO_ERROR : UNKNOWN_ERROR;
}

status_t QualcommCameraHardware::startRecording()
{
    ALOGV("startRecording E");
    int ret;
    Mutex::Autolock l(&mLock);
    mReleasedRecordingFrame = false;
    if ((ret = startPreviewInternal()) == NO_ERROR) {
        if (mVpeEnabled) {
            ALOGI("startRecording: VPE enabled, setting vpe parameters");
            bool status = setVpeParameters();
            if (status) {
                ALOGE("Failed to set VPE parameters");
                return status;
            }
        }
        if (mCurrentTarget == TARGET_MSM7630 ||
            mCurrentTarget == TARGET_QSD8250 ||
            mCurrentTarget == TARGET_MSM8660) {
            for (int cnt = 0; cnt < kRecordBufferCount; cnt++) {
                if (mStoreMetaDataInFrame) {
                    ALOGE("startRecording : meta data mode enabled");
                    metadata_memory[cnt] = mGetMemory(-1, sizeof(struct encoder_media_buffer_type), 1, mCallbackCookie);
                    struct encoder_media_buffer_type * packet =
                        (struct encoder_media_buffer_type  *)metadata_memory[cnt]->data;
                    packet->meta_handle = native_handle_create(1, 2); //1 fd, 1 offset and 1 size
                    packet->buffer_type = kMetadataBufferTypeCameraSource;
                    native_handle_t * nh = const_cast<native_handle_t *>(packet->meta_handle);
                    nh->data[0] = mRecordfd[cnt];
                    nh->data[1] = 0;
                    nh->data[2] = mRecordFrameSize;
                }
            }
            ALOGV(" in startREcording : calling start_recording");
            native_start_ops(CAMERA_OPS_VIDEO_RECORDING, NULL);
            mRecordingState = 1;
            // Remove the left out frames in busy Q and them in free Q.
            // this should be done before starting video_thread so that,
            // frames in previous recording are flushed out.
            ALOGV("frames in busy Q = %d", g_busy_frame_queue.num_of_frames);
            while (g_busy_frame_queue.num_of_frames > 0) {
                msm_frame *vframe = cam_frame_get_video();
                LINK_camframe_add_frame(CAM_VIDEO_FRAME, vframe);
            }
            ALOGV("frames in busy Q = %d after deQueing", g_busy_frame_queue.num_of_frames);
            //Clear the dangling buffers and put them in free queue
            for (int cnt = 0; cnt < kRecordBufferCount; cnt++) {
                if (record_buffers_tracking_flag[cnt] == true) {
                    ALOGI("Dangling buffer: offset = %d, buffer = %d", cnt,
                        (unsigned int)recordframes[cnt].buffer);
                    LINK_camframe_add_frame(CAM_VIDEO_FRAME,&recordframes[cnt]);
                    record_buffers_tracking_flag[cnt] = false;
                }
            }
            mVideoThreadWaitLock.lock();
            mVideoThreadExit = 0;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            mVideoThreadRunning = pthread_create(&mVideoThread,
                &attr,
                video_thread,
                NULL);
            mVideoThreadWaitLock.unlock();
        } else if (mCurrentTarget == TARGET_MSM7627A) {
            for (int cnt = 0; cnt < mTotalPreviewBufferCount; cnt++) {
                if (mStoreMetaDataInFrame && (metadata_memory[cnt] == NULL)) {
                    ALOGE("startRecording : meta data mode enabled filling metadata memory ");
                    metadata_memory[cnt] = mGetMemory(-1, sizeof(struct encoder_media_buffer_type), 1, mCallbackCookie);
                    struct encoder_media_buffer_type * packet =
                        (struct encoder_media_buffer_type  *)metadata_memory[cnt]->data;
                    packet->meta_handle = native_handle_create(1, 3); //1 fd, 1 offset and 1 size
                    packet->buffer_type = kMetadataBufferTypeCameraSource;
                    native_handle_t * nh = const_cast<native_handle_t *>(packet->meta_handle);
                    nh->data[0] = frames[cnt].fd;
                    nh->data[1] = 0;
                    nh->data[2] = previewWidth * previewHeight * 3/2;
                    nh->data[3] = (unsigned int)mPreviewMapped[cnt]->data;
                }
            }
        }
        record_flag = 1;
    }
    return ret;
}

status_t QualcommCameraHardware::startRecordingInternal()
{
    ALOGI("%s: E", __FUNCTION__);
    mReleasedRecordingFrame = false;

    /* In 3D mode, the video thread has to be started as part
     * of preview itself, because video buffers and video callback
     * need to be used for both display and encoding.
     * startRecordingInternal() will be called as part of startPreview().
     * This check is needed to support both 3D and non-3D mode.
     */
    if (mVideoThreadRunning) {
        ALOGI("Video Thread is in progress");
        return NO_ERROR;
    }

    if (mVpeEnabled) {
        ALOGI("startRecording: VPE enabled, setting vpe parameters");
        bool status = setVpeParameters();
        if (status) {
            ALOGE("Failed to set VPE parameters");
            return status;
        }
    }
    if (mCurrentTarget == TARGET_MSM7630 ||
        mCurrentTarget == TARGET_QSD8250 ||
        mCurrentTarget == TARGET_MSM8660) {
        // Remove the left out frames in busy Q and them in free Q.
        // this should be done before starting video_thread so that,
        // frames in previous recording are flushed out.
        ALOGV("frames in busy Q = %d", g_busy_frame_queue.num_of_frames);
        while (g_busy_frame_queue.num_of_frames > 0) {
            msm_frame *vframe = cam_frame_get_video();
            LINK_camframe_add_frame(CAM_VIDEO_FRAME, vframe);
        }
        ALOGV("frames in busy Q = %d after deQueing", g_busy_frame_queue.num_of_frames);

        //Clear the dangling buffers and put them in free queue
        for (int cnt = 0; cnt < kRecordBufferCount; cnt++) {
            if (record_buffers_tracking_flag[cnt] == true) {
                ALOGI("Dangling buffer: offset = %d, buffer = %d", cnt, (unsigned int)recordframes[cnt].buffer);
                LINK_camframe_add_frame(CAM_VIDEO_FRAME,&recordframes[cnt]);
                record_buffers_tracking_flag[cnt] = false;
            }
        }

        ALOGE(" in startREcording : calling start_recording");
        if (!mIs3DModeOn)
            native_start_ops(CAMERA_OPS_VIDEO_RECORDING, NULL);

        // Start video thread and wait for busy frames to be encoded, this thread
        // should be closed in stopRecording
        mVideoThreadWaitLock.lock();
        mVideoThreadExit = 0;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        mVideoThreadRunning = !pthread_create(&mVideoThread,
                                              &attr,
                                              video_thread,
                                              NULL);
        mVideoThreadWaitLock.unlock();
        // Remove the left out frames in busy Q and them in free Q.
    }
    ALOGV("%s: E", __FUNCTION__);
    return NO_ERROR;
}

void QualcommCameraHardware::stopRecording()
{
    ALOGV("stopRecording: E");
    record_flag = 0;
    Mutex::Autolock l(&mLock);
    {
        mRecordFrameLock.lock();
        mReleasedRecordingFrame = true;
        mRecordWait.signal();
        mRecordFrameLock.unlock();

        if (mDataCallback && mCurrentTarget != TARGET_QSD8250 &&
            mMsgEnabled & CAMERA_MSG_PREVIEW_FRAME) {
            ALOGV("stopRecording: X, preview still in progress");
            return;
        }
    }
    if (NULL != mJpegLiveSnapMapped) {
        ALOGI("initLiveSnapshot: clearing old mJpegLiveSnapMapped.");
        mJpegLiveSnapMapped->release(mJpegLiveSnapMapped);
        mJpegLiveSnapMapped = NULL;
    }

    // If output2 enabled, exit video thread, invoke stop recording ioctl
    if (mCurrentTarget == TARGET_MSM7630 ||
        mCurrentTarget == TARGET_QSD8250 ||
        mCurrentTarget == TARGET_MSM8660) {
        /* when 3D mode is ON, don't exit the video thread, as
         * we need to support the preview mode. Just set the recordingState
         * to zero, so that there won't be any rcb callbacks. video thread
         * will be terminated as part of stop preview.
         */
        if (mIs3DModeOn) {
            ALOGV("%s: 3D mode on, so don't exit video thread", __FUNCTION__);
            mRecordingState = 0;
            return;
        }

        mVideoThreadWaitLock.lock();
        mVideoThreadExit = 1;
        mVideoThreadWaitLock.unlock();
        native_stop_ops(CAMERA_OPS_VIDEO_RECORDING, NULL);

        pthread_mutex_lock(&(g_busy_frame_queue.mut));
        pthread_cond_signal(&(g_busy_frame_queue.wait));
        pthread_mutex_unlock(&(g_busy_frame_queue.mut));
        for (int cnt = 0; cnt < kRecordBufferCount; cnt++) {
            if (mStoreMetaDataInFrame && (metadata_memory[cnt] != NULL)) {
                struct encoder_media_buffer_type * packet =
                    (struct encoder_media_buffer_type  *)metadata_memory[cnt]->data;
                native_handle_delete(const_cast<native_handle_t *>(packet->meta_handle));
                metadata_memory[cnt]->release(metadata_memory[cnt]);
                metadata_memory[cnt] = NULL;
            }
        }
    } else if (mCurrentTarget == TARGET_MSM7627A) {
        for (int cnt = 0; cnt < mTotalPreviewBufferCount; cnt++) {
            if (mStoreMetaDataInFrame && (metadata_memory[cnt] != NULL)) {
                struct encoder_media_buffer_type * packet =
                    (struct encoder_media_buffer_type  *)metadata_memory[cnt]->data;
                native_handle_delete(const_cast<native_handle_t *>(packet->meta_handle));
                metadata_memory[cnt]->release(metadata_memory[cnt]);
                metadata_memory[cnt] = NULL;
            }
        }
    }
    mRecordingState = 0; // recording not started
    ALOGV("stopRecording: X");
}

void QualcommCameraHardware::releaseRecordingFrame(const void *opaque)
{
    ALOGE("%s : BEGIN, opaque = 0x%p",__func__, opaque);
    Mutex::Autolock rLock(&mRecordFrameLock);
    mReleasedRecordingFrame = true;
    mRecordWait.signal();

    // Ff 7x30 : add the frame to the free camframe queue
    if (mCurrentTarget == TARGET_MSM7630 ||
        mCurrentTarget == TARGET_QSD8250 ||
        mCurrentTarget == TARGET_MSM8660) {
        ssize_t offset;
        size_t size;
        msm_frame *releaseframe = NULL;
        int cnt;
        for (cnt = 0; cnt < kRecordBufferCount; cnt++) {
            if (mStoreMetaDataInFrame) {
                if (metadata_memory[cnt] && metadata_memory[cnt]->data == opaque) {
                    ALOGV("in release recording frame(meta) found match , releasing buffer %d", (unsigned int)recordframes[cnt].buffer);
                    releaseframe = &recordframes[cnt];
                    break;
                }
            } else {
                if (recordframes[cnt].buffer && ((unsigned long)opaque == recordframes[cnt].buffer) ) {
                    ALOGV("in release recording frame found match , releasing buffer %d", (unsigned int)recordframes[cnt].buffer);
                    releaseframe = &recordframes[cnt];
                    break;
                }
            }
        }
        if (cnt < kRecordBufferCount) {
            // do this only if frame thread is running
            mFrameThreadWaitLock.lock();
            if (mFrameThreadRunning) {
                //Reset the track flag for this frame buffer
                record_buffers_tracking_flag[cnt] = false;
                LINK_camframe_add_frame(CAM_VIDEO_FRAME,releaseframe);
            }

            mFrameThreadWaitLock.unlock();
        } else {
            ALOGE("in release recordingframe XXXXX error , buffer not found");
            for (int i = 0; i < kRecordBufferCount; i++) {
                ALOGE(" recordframes[%d].buffer = %d", i, (unsigned int)recordframes[i].buffer);
            }
        }
    }

    ALOGV("releaseRecordingFrame X");
}

bool QualcommCameraHardware::recordingEnabled()
{
    return mCameraRunning && mDataCallbackTimestamp && (mMsgEnabled & CAMERA_MSG_VIDEO_FRAME);
}

void QualcommCameraHardware::notifyShutter(bool mPlayShutterSoundOnly)
{
    mShutterLock.lock();

    if (mPlayShutterSoundOnly) {
        /* At this point, invoke Notify Callback to play shutter sound only.
         * We want to call notify callback again when we have the
         * yuv picture ready. This is to reduce blanking at the time
         * of displaying postview frame. Using ext2 to indicate whether
         * to play shutter sound only or register the postview buffers.
         */
        mNotifyCallback(CAMERA_MSG_SHUTTER, 0, mPlayShutterSoundOnly,
            mCallbackCookie);
        mShutterLock.unlock();
        return;
    }

    if (mShutterPending && mNotifyCallback && (mMsgEnabled & CAMERA_MSG_SHUTTER)) {
        /* Now, invoke Notify Callback to unregister preview buffer
         * and register postview buffer with surface flinger. Set ext2
         * as 0 to indicate not to play shutter sound.
         */
        mNotifyCallback(CAMERA_MSG_SHUTTER, 0, 0, mCallbackCookie);
        mShutterPending = false;
    }
    mShutterLock.unlock();
}

// ReceiveRawPicture for ICS
void QualcommCameraHardware::receiveRawPicture(status_t status,struct msm_frame *postviewframe, struct msm_frame *mainframe)
{
    ALOGE("%s: E", __FUNCTION__);

    mSnapshotThreadWaitLock.lock();
    if (mSnapshotThreadRunning == false) {
        ALOGE("%s called in wrong state, ignore", __FUNCTION__);
        return;
    }
    mSnapshotThreadWaitLock.unlock();

    if (status != NO_ERROR) {
        ALOGE("%s: Failed to get Snapshot Image", __FUNCTION__);
        if (mDataCallback && (mMsgEnabled & CAMERA_MSG_COMPRESSED_IMAGE)) {
            /* get picture failed. Give jpeg callback with NULL data
             * to the application to restore to preview mode
             */
            ALOGE("get picture failed, giving jpeg callback with NULL data");
            mDataCallback(CAMERA_MSG_COMPRESSED_IMAGE, NULL, data_counter, NULL, mCallbackCookie);
        }
        mShutterLock.lock();
        mShutterPending = false;
        mShutterLock.unlock();
        mJpegThreadWaitLock.lock();
        mJpegThreadRunning = false;
        mJpegThreadWait.signal();
        mJpegThreadWaitLock.unlock();
        mInSnapshotModeWaitLock.lock();
        mInSnapshotMode = false;
        mInSnapshotModeWait.signal();
        mInSnapshotModeWaitLock.unlock();
        return;
    }
    /* call notifyShutter to config surface and overlay
     * for postview rendering.
     * Its necessary to issue another notifyShutter here with
     * mPlayShutterSoundOnly as FALSE, since that is when the
     * preview buffers are unregistered with the surface flinger.
     * That is necessary otherwise the preview memory wont be
     * deallocated.
     */
    void *cropp = postviewframe->cropinfo;
    notifyShutter(FALSE);

    if (mSnapshotFormat == PICTURE_FORMAT_JPEG) {
        if (cropp != NULL) {
            common_crop_t *crop = (common_crop_t *)cropp;
            if (crop->in1_w != 0 && crop->in1_h != 0) {
                zoomCropInfo.left = (crop->out1_w - crop->in1_w + 1) / 2 - 1;
                zoomCropInfo.top = (crop->out1_h - crop->in1_h + 1) / 2 - 1;
                if (zoomCropInfo.left < 0)
                    zoomCropInfo.left = 0;
                if (zoomCropInfo.top < 0)
                    zoomCropInfo.top = 0;
                zoomCropInfo.right = zoomCropInfo.left + crop->in1_w;
                zoomCropInfo.bottom = zoomCropInfo.top + crop->in1_h;
                mPreviewWindow->set_crop(mPreviewWindow,
                    zoomCropInfo.left,
                    zoomCropInfo.top,
                    zoomCropInfo.right,
                    zoomCropInfo.bottom);
                mResetWindowCrop = true;
            } else {
                zoomCropInfo.left = 0;
                zoomCropInfo.top = 0;
                zoomCropInfo.right = mPostviewWidth;
                zoomCropInfo.bottom = mPostviewHeight;
                mPreviewWindow->set_crop(mPreviewWindow,
                    zoomCropInfo.left,
                    zoomCropInfo.top,
                    zoomCropInfo.right,
                    zoomCropInfo.bottom);
            }
        }
        ALOGE("receiverawpicture : display lock");
        mDisplayLock.lock();
        int index = mapThumbnailBuffer(postviewframe);
        ALOGE("receiveRawPicture : mapThumbnailBuffer returned %d", index);
        private_handle_t *handle;
        if (mThumbnailBuffer[index] != NULL && mZslEnable == false) {
            handle = (private_handle_t *)(*mThumbnailBuffer[index]);
            ALOGV("%s: Queueing postview buffer for display %d",
                __FUNCTION__,handle->fd);
            status_t retVal = mPreviewWindow->enqueue_buffer(mPreviewWindow,
                mThumbnailBuffer[index]);
            ALOGE(" enQ thumbnailbuffer");
            if ( retVal != NO_ERROR) {
                ALOGE("%s: Queuebuffer failed for postview buffer", __FUNCTION__);
            }

        }
        mDisplayLock.unlock();
        ALOGE("receiverawpicture : display unlock");
        /* Give the main Image as raw to upper layers */
        //Either CAMERA_MSG_RAW_IMAGE or CAMERA_MSG_RAW_IMAGE_NOTIFY will be set not both
        if (mDataCallback && (mMsgEnabled & CAMERA_MSG_RAW_IMAGE))
            mDataCallback(CAMERA_MSG_RAW_IMAGE, mRawMapped[index],data_counter,
                NULL, mCallbackCookie);
        else if (mNotifyCallback && (mMsgEnabled & CAMERA_MSG_RAW_IMAGE_NOTIFY))
            mNotifyCallback(CAMERA_MSG_RAW_IMAGE_NOTIFY, 0, 0, mCallbackCookie);

        if (strTexturesOn == true) {
            ALOGI("Raw Data given to app for processing...will wait for jpeg encode call");
            mEncodePending = true;
            mEncodePendingWaitLock.unlock();
            mJpegThreadWaitLock.lock();
            mJpegThreadWait.signal();
            mJpegThreadWaitLock.unlock();
        }
    } else {  // Not Jpeg snapshot, it is Raw Snapshot , handle later
        ALOGV("ReceiveRawPicture : raw snapshot not Jpeg, sending callback up");
        if (mDataCallback && (mMsgEnabled & CAMERA_MSG_COMPRESSED_IMAGE))
            mDataCallback(CAMERA_MSG_COMPRESSED_IMAGE,
                mRawSnapshotMapped,
                data_counter,
                NULL,
                mCallbackCookie);

        // TEMP
        ALOGE("receiveRawPicture : gave raw frame to app, giving signal");
        mJpegThreadWaitLock.lock();
        mJpegThreadRunning = false;
        mJpegThreadWait.signal();
        mJpegThreadWaitLock.unlock();
    }

    /* can start preview at this stage? early preview? */
    mInSnapshotModeWaitLock.lock();
    mInSnapshotMode = false;
    mInSnapshotModeWait.signal();
    mInSnapshotModeWaitLock.unlock();

    ALOGV("%s: X", __FUNCTION__);
}

void QualcommCameraHardware::receiveJpegPicture(status_t status, mm_camera_buffer_t *encoded_buffer)
{
    Mutex::Autolock cbLock(&mCallbackLock);
    numJpegReceived++;
    uint32_t offset ;
    int32_t index = -1;
    int32_t buffer_size = 0;
    if (encoded_buffer && status == NO_ERROR) {
        buffer_size = encoded_buffer->filled_size;
        ALOGV("receiveJpegPicture: E buffer_size %d mJpegMaxSize = %d",buffer_size, mJpegMaxSize);

        index = mapJpegBuffer(encoded_buffer);
        ALOGE("receiveJpegPicutre : mapJpegBuffer index : %d", index);
    }
    if (index < 0 || index >= (MAX_SNAPSHOT_BUFFERS-2)) {
        ALOGE("Jpeg index is not valid or fails. ");
        if (mDataCallback && (mMsgEnabled & CAMERA_MSG_COMPRESSED_IMAGE)) {
            mDataCallback(CAMERA_MSG_COMPRESSED_IMAGE, NULL, data_counter, NULL, mCallbackCookie);
        }
        mJpegThreadWaitLock.lock();
        mJpegThreadRunning = false;
        mJpegThreadWait.signal();
        mJpegThreadWaitLock.unlock();
    } else {
        ALOGV("receiveJpegPicture: Index of Jpeg is %d",index);

        if (mDataCallback && (mMsgEnabled & CAMERA_MSG_COMPRESSED_IMAGE)) {
            if (status == NO_ERROR) {
                ALOGE("receiveJpegPicture : giving jpeg image callback to services");
                mJpegCopyMapped = mGetMemory(-1, encoded_buffer->filled_size, 1, mCallbackCookie);
                if (!mJpegCopyMapped) {
                    ALOGE("%s: mGetMemory failed.\n", __func__);
                }
                memcpy(mJpegCopyMapped->data, mJpegMapped[index]->data, encoded_buffer->filled_size );
                mDataCallback(CAMERA_MSG_COMPRESSED_IMAGE,mJpegCopyMapped,data_counter,NULL,mCallbackCookie);
                if (NULL != mJpegCopyMapped) {
                    mJpegCopyMapped->release(mJpegCopyMapped);
                    mJpegCopyMapped = NULL;
                }
            }
        } else {
            ALOGE("JPEG callback was cancelled--not delivering image.");
        }
        if (numJpegReceived == numCapture) {
            mJpegThreadWaitLock.lock();
            mJpegThreadRunning = false;
            mJpegThreadWait.signal();
            mJpegThreadWaitLock.unlock();
        }
    }

    ALOGV("receiveJpegPicture: X callback done.");
}

bool QualcommCameraHardware::previewEnabled()
{
    /* If overlay is used the message CAMERA_MSG_PREVIEW_FRAME would
     * be disabled at CameraService layer. Hence previewEnabled would
     * return FALSE even though preview is running. Hence check for
     * mOverlay not being NULL to ensure that previewEnabled returns
     * accurate information.
     */

    ALOGE(" : mCameraRunning : %d mPreviewWindow = %p", mCameraRunning, mPreviewWindow);
    return mCameraRunning;
}
status_t QualcommCameraHardware::setRecordSize(const CameraParameters& params)
{
    const char *recordSize = NULL;
    recordSize = params.get(CameraParameters::KEY_VIDEO_SIZE);
    if (!recordSize) {
        mParameters.set(CameraParameters::KEY_VIDEO_SIZE, "");
        //If application didn't set this parameter string, use the values from
        //getPreviewSize() as video dimensions.
        ALOGV("No Record Size requested, use the preview dimensions");
        videoWidth = previewWidth;
        videoHeight = previewHeight;
    } else {
        //Extract the record witdh and height that application requested.
        ALOGI("%s: requested record size %s", __FUNCTION__, recordSize);
        if (!parse_size(recordSize, videoWidth, videoHeight)) {
            mParameters.set(CameraParameters::KEY_VIDEO_SIZE , recordSize);
            //VFE output1 shouldn't be greater than VFE output2.
            if (previewWidth > videoWidth || previewHeight > videoHeight) {
                //Set preview sizes as record sizes.
                ALOGI("Preview size %dx%d is greater than record size %dx%d,"
                    "resetting preview size to record size",
                    previewWidth, previewHeight, videoWidth, videoHeight);
                previewWidth = videoWidth;
                previewHeight = videoHeight;
                mParameters.setPreviewSize(previewWidth, previewHeight);
            }
            if (mCurrentTarget != TARGET_MSM7630 &&
                mCurrentTarget != TARGET_QSD8250 &&
                mCurrentTarget != TARGET_MSM8660) {
                //For Single VFE output targets, use record dimensions as preview dimensions.
                previewWidth = videoWidth;
                previewHeight = videoHeight;
                mParameters.setPreviewSize(previewWidth, previewHeight);
            }
            if (mIs3DModeOn == true) {
                /* As preview and video frames are same in 3D mode,
                 * preview size should be same as video size. This
                 * cahnge is needed to take of video resolutions
                 * like 720P and 1080p where the application can
                 * request different preview sizes like 768x432
                 */
                previewWidth = videoWidth;
                previewHeight = videoHeight;
                mParameters.setPreviewSize(previewWidth, previewHeight);
            }
        } else {
            mParameters.set(CameraParameters::KEY_VIDEO_SIZE, "");
            ALOGE("initPreview X: failed to parse parameter record-size (%s)", recordSize);
            return BAD_VALUE;
        }
    }
    ALOGI("%s: preview dimensions: %dx%d", __FUNCTION__, previewWidth, previewHeight);
    ALOGI("%s: video dimensions: %dx%d", __FUNCTION__, videoWidth, videoHeight);
    mDimension.display_width = previewWidth;
    mDimension.display_height = previewHeight;
    return NO_ERROR;
}

status_t QualcommCameraHardware::setCameraMode(const CameraParameters& params)
{
    int32_t value = params.getInt(CameraParameters::KEY_CAMERA_MODE);
    if (mCurrentTarget != TARGET_MSM8660 && value == 1) {
        return NO_ERROR; /* Not supported */
    }

    mZslEnable = value;
    mParameters.set(CameraParameters::KEY_CAMERA_MODE, value);
    return NO_ERROR;
}

status_t QualcommCameraHardware::setPreviewSize(const CameraParameters& params)
{
    int width, height;
    params.getPreviewSize(&width, &height);
    ALOGV("requested preview size %d x %d", width, height);

    // Validate the preview size
    for (size_t i = 0; i < PREVIEW_SIZE_COUNT; i++) {
        if (width == preview_sizes[i].width && height == preview_sizes[i].height) {
            mParameters.setPreviewSize(width, height);
            previewWidth = width;
            previewHeight = height;
            mDimension.display_width = width;
            mDimension.display_height = height;
            return NO_ERROR;
        }
    }
    ALOGE("Invalid preview size requested: %dx%d", width, height);
    return BAD_VALUE;
}

status_t QualcommCameraHardware::setPreviewFpsRange(const CameraParameters& params)
{
    int minFps, maxFps;
    params.getPreviewFpsRange(&minFps, &maxFps);
    ALOGE("FPS Range Values: %dx%d", minFps, maxFps);

    for (size_t i = 0;i < FPS_RANGES_SUPPORTED_COUNT; i++) {
        if (minFps == FpsRangesSupported[i].minFPS && maxFps == FpsRangesSupported[i].maxFPS) {
            mParameters.setPreviewFpsRange(minFps, maxFps);
            return NO_ERROR;
        }
    }
    return BAD_VALUE;
}

status_t QualcommCameraHardware::setPreviewFrameRate(const CameraParameters& params)
{
    if (!mCfgControl.mm_camera_is_supported(CAMERA_PARM_FPS)) {
        ALOGI("Set fps is not supported for this sensor");
        return NO_ERROR;
    }
    uint16_t previousFps = mParameters.getPreviewFrameRate();
    uint16_t fps = params.getPreviewFrameRate();
    ALOGV("requested preview frame rate  is %u", fps);

    if (mInitialized && fps == previousFps) {
        ALOGV("fps same as previous fps");
        return NO_ERROR;
    }

    if (MINIMUM_FPS <= fps && fps <= MAXIMUM_FPS) {
        mParameters.setPreviewFrameRate(fps);
        bool ret = native_set_parms(CAMERA_PARM_FPS, sizeof(fps), &fps);
        return ret ? NO_ERROR : UNKNOWN_ERROR;
    }
    return BAD_VALUE;
}

status_t QualcommCameraHardware::setPreviewFrameRateMode(const CameraParameters& params)
{
    if (!mCfgControl.mm_camera_is_supported(CAMERA_PARM_FPS_MODE) &&
        !mCfgControl.mm_camera_is_supported(CAMERA_PARM_FPS)) {
        ALOGI("set fps mode is not supported for this sensor");
        return NO_ERROR;
    }

    const char *previousMode = mParameters.getPreviewFrameRateMode();
    const char *str = params.getPreviewFrameRateMode();
    if (mInitialized && !strcmp(previousMode, str)) {
        ALOGV("frame rate mode same as previous mode %s", previousMode);
        return NO_ERROR;
    }
    int32_t frameRateMode = attr_lookup(frame_rate_modes, sizeof(frame_rate_modes) / sizeof(str_map), str);
    if (frameRateMode != NOT_FOUND) {
        ALOGV("setPreviewFrameRateMode: %s", str);
        mParameters.setPreviewFrameRateMode(str);
        bool ret = native_set_parms(CAMERA_PARM_FPS_MODE, sizeof(frameRateMode), &frameRateMode);
        if (!ret)
            return ret;
        //set the fps value when chaging modes
        int16_t fps = params.getPreviewFrameRate();
        if (MINIMUM_FPS <= fps && fps <= MAXIMUM_FPS) {
            mParameters.setPreviewFrameRate(fps);
            ret = native_set_parms(CAMERA_PARM_FPS, sizeof(fps), &fps);
            return ret ? NO_ERROR : UNKNOWN_ERROR;
        }
        ALOGE("Invalid preview frame rate value: %d", fps);
        return BAD_VALUE;
    }
    ALOGE("Invalid preview frame rate mode value: %s", (str == NULL) ? "NULL" : str);
    return BAD_VALUE;
}

status_t QualcommCameraHardware::setJpegThumbnailSize(const CameraParameters& params)
{
    int width = params.getInt(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH);
    int height = params.getInt(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT);
    ALOGV("requested jpeg thumbnail size %d x %d", width, height);

    // Validate the picture size
    for (unsigned int i = 0; i < JPEG_THUMBNAIL_SIZE_COUNT; ++i) {
        if (width == jpeg_thumbnail_sizes[i].width &&
            height == jpeg_thumbnail_sizes[i].height) {
            mParameters.set(CameraParameters::KEY_JPEG_THUMBNAIL_WIDTH, width);
            mParameters.set(CameraParameters::KEY_JPEG_THUMBNAIL_HEIGHT, height);
            return NO_ERROR;
        }
    }
    return BAD_VALUE;
}

bool QualcommCameraHardware::updatePictureDimension(const CameraParameters& params, int& width, int& height)
{
    bool retval = false;
    int previewWidth, previewHeight;
    params.getPreviewSize(&previewWidth, &previewHeight);
    ALOGV("updatePictureDimension: %dx%d <- %dx%d", width, height, previewWidth, previewHeight);
    if (width < previewWidth && height < previewHeight) {
        /*As we donot support jpeg downscaling for picture dimension < previewdimesnion/8 ,
        Adding support for the same for cts testcases*/
        mActualPictWidth = width;
        mActualPictHeight = height;
        if ((previewWidth / 8) > width ) {
            int ratio = previewWidth / width;
            int i;
            for (i = 0; i < ratio; i++) {
                if ((ratio >> i) < 8)
                    break;
            }
            width = width *i*2;
            height = height *i*2;
        } else {
            width = previewWidth;
            height = previewHeight;
        }
        mUseJpegDownScaling = true;
        retval = true;
    } else
        mUseJpegDownScaling = false;
    return retval;
}

status_t QualcommCameraHardware::setPictureSize(const CameraParameters& params)
{
    int width, height;
    params.getPictureSize(&width, &height);
    ALOGV("requested picture size %d x %d", width, height);

    // Validate the picture size
    for (int i = 0; i < supportedPictureSizesCount; ++i) {
        if (width == picture_sizes_ptr[i].width && height == picture_sizes_ptr[i].height) {
            mParameters.setPictureSize(width, height);
            mDimension.picture_width = width;
            mDimension.picture_height = height;
            return NO_ERROR;
        }
    }
    /* Dimension not among the ones in the list. Check if
     * its a valid dimension, if it is, then configure the
     * camera accordingly. else reject it.
     */
    if (isValidDimension(width, height)) {
        mParameters.setPictureSize(width, height);
        mDimension.picture_width = width;
        mDimension.picture_height = height;
        return NO_ERROR;
    } else
        ALOGE("Invalid picture size requested: %dx%d", width, height);
    return BAD_VALUE;
}

status_t QualcommCameraHardware::setJpegQuality(const CameraParameters& params)
{
    status_t rc = NO_ERROR;
    int quality = params.getInt(CameraParameters::KEY_JPEG_QUALITY);
    if (quality >= 0 && quality <= 100) {
        mParameters.set(CameraParameters::KEY_JPEG_QUALITY, quality);
    } else {
        ALOGE("Invalid jpeg quality=%d", quality);
        rc = BAD_VALUE;
    }

    quality = params.getInt(CameraParameters::KEY_JPEG_THUMBNAIL_QUALITY);
    if (quality >= 0 && quality <= 100) {
        mParameters.set(CameraParameters::KEY_JPEG_THUMBNAIL_QUALITY, quality);
    } else {
        ALOGE("Invalid jpeg thumbnail quality=%d", quality);
        rc = BAD_VALUE;
    }
    return rc;
}

status_t QualcommCameraHardware::setEffect(const CameraParameters& params)
{
    const char *str = params.get(CameraParameters::KEY_EFFECT);
    int result;

    if (str != NULL) {
        int32_t value = attr_lookup(effects, sizeof(effects) / sizeof(str_map), str);
        if (value != NOT_FOUND) {
            if (!mCfgControl.mm_camera_is_parm_supported(CAMERA_PARM_EFFECT, &value)) {
                ALOGE("Camera Effect - %s mode is not supported for this sensor",str);
                return NO_ERROR;
            } else {
                mParameters.set(CameraParameters::KEY_EFFECT, str);
                bool ret = native_set_parms(CAMERA_PARM_EFFECT, sizeof(value),
                    &value, &result);
                if (result == MM_CAMERA_ERR_INVALID_OPERATION) {
                    ALOGI("Camera Effect: %s is not set as the selected value is not supported ", str);
                }
            return ret ? NO_ERROR : UNKNOWN_ERROR;
            }
        }
    }
    ALOGE("Invalid effect value: %s", (str == NULL) ? "NULL" : str);
    return BAD_VALUE;
}

status_t QualcommCameraHardware::setRecordingHint(const CameraParameters& params)
{
    const char *str = params.get(CameraParameters::KEY_RECORDING_HINT);

    if (str != NULL) {
        int32_t value = attr_lookup(recording_Hints,
            sizeof(recording_Hints) / sizeof(str_map), str);
        if (value != NOT_FOUND) {
            native_set_parms(CAMERA_PARM_RECORDING_HINT, sizeof(value), &value);
            mParameters.set(CameraParameters::KEY_RECORDING_HINT, str);
        } else {
            ALOGE("Invalid Picture Format value: %s", str);
            return BAD_VALUE;
        }
    }
    return NO_ERROR;
}

status_t QualcommCameraHardware::setExposureCompensation(const CameraParameters & params)
{
    ALOGE("DEBBUG: %s E",__FUNCTION__);
    if (!mCfgControl.mm_camera_is_supported(CAMERA_PARM_EXPOSURE_COMPENSATION)) {
        ALOGI("Exposure Compensation is not supported for this sensor");
        return NO_ERROR;
    }

    int numerator = params.getInt(CameraParameters::KEY_EXPOSURE_COMPENSATION);
    if (EXPOSURE_COMPENSATION_MINIMUM_NUMERATOR <= numerator &&
        numerator <= EXPOSURE_COMPENSATION_MAXIMUM_NUMERATOR) {
        int16_t numerator16 = numerator & 0x0000ffff;
        uint16_t denominator16 = EXPOSURE_COMPENSATION_DENOMINATOR;
        uint32_t value = 0;
        value = numerator16 << 16 | denominator16;

        mParameters.set(CameraParameters::KEY_EXPOSURE_COMPENSATION, numerator);
        bool ret = native_set_parms(CAMERA_PARM_EXPOSURE_COMPENSATION, sizeof(value), &value);
        ALOGE("DEBBUG: %s ret = %d X",__FUNCTION__, ret);
        return ret ? NO_ERROR : UNKNOWN_ERROR;
    }
    ALOGE("Invalid Exposure Compensation");
    return BAD_VALUE;
}

status_t QualcommCameraHardware::setAutoExposure(const CameraParameters& params)
{
    if (!mCfgControl.mm_camera_is_supported(CAMERA_PARM_EXPOSURE)) {
        ALOGI("Auto Exposure not supported for this sensor");
        return NO_ERROR;
    }
    const char *str = params.get(CameraParameters::KEY_AUTO_EXPOSURE);
    if (str != NULL) {
        int32_t value = attr_lookup(autoexposure, sizeof(autoexposure) / sizeof(str_map), str);
        if (value != NOT_FOUND) {
            mParameters.set(CameraParameters::KEY_AUTO_EXPOSURE, str);
            bool ret = native_set_parms(CAMERA_PARM_EXPOSURE, sizeof(value), &value);
            return ret ? NO_ERROR : UNKNOWN_ERROR;
        }
    }
    ALOGE("Invalid auto exposure value: %s", (str == NULL) ? "NULL" : str);
    return BAD_VALUE;
}

status_t QualcommCameraHardware::setSharpness(const CameraParameters& params)
{
    if (!mCfgControl.mm_camera_is_supported(CAMERA_PARM_SHARPNESS)) {
        ALOGI("Sharpness not supported for this sensor");
        return NO_ERROR;
    }
    int sharpness = params.getInt(CameraParameters::KEY_SHARPNESS);
    if ((sharpness < CAMERA_MIN_SHARPNESS || sharpness > CAMERA_MAX_SHARPNESS))
        return UNKNOWN_ERROR;

    ALOGV("setting sharpness %d", sharpness);
    mParameters.set(CameraParameters::KEY_SHARPNESS, sharpness);
    bool ret = native_set_parms(CAMERA_PARM_SHARPNESS, sizeof(sharpness), &sharpness);
    return ret ? NO_ERROR : UNKNOWN_ERROR;
}

status_t QualcommCameraHardware::setContrast(const CameraParameters& params)
{
    if (!mCfgControl.mm_camera_is_supported(CAMERA_PARM_CONTRAST)) {
        ALOGI("Contrast not supported for this sensor");
        return NO_ERROR;
    }

    const char *str = params.get(CameraParameters::KEY_SCENE_MODE);
    int32_t value = attr_lookup(scenemode, sizeof(scenemode) / sizeof(str_map), str);

    if (value == CAMERA_BESTSHOT_OFF) {
        int contrast = params.getInt(CameraParameters::KEY_CONTRAST);
        if ((contrast < CAMERA_MIN_CONTRAST) || (contrast > CAMERA_MAX_CONTRAST))
            return UNKNOWN_ERROR;

        ALOGV("setting contrast %d", contrast);
        mParameters.set(CameraParameters::KEY_CONTRAST, contrast);
        bool ret = native_set_parms(CAMERA_PARM_CONTRAST, sizeof(contrast), &contrast);
        return ret ? NO_ERROR : UNKNOWN_ERROR;
    } else {
        ALOGI(" Contrast value will not be set when the scenemode selected is %s", str);
        return NO_ERROR;
    }
}

status_t QualcommCameraHardware::setSaturation(const CameraParameters& params)
{
    if (!mCfgControl.mm_camera_is_supported(CAMERA_PARM_SATURATION)) {
        ALOGI("Saturation not supported for this sensor");
        return NO_ERROR;
    }
    int result;
    int saturation = params.getInt(CameraParameters::KEY_SATURATION);

    if (saturation < CAMERA_MIN_SATURATION || saturation > CAMERA_MAX_SATURATION)
        return UNKNOWN_ERROR;

    ALOGV("Setting saturation %d", saturation);
    mParameters.set(CameraParameters::KEY_SATURATION, saturation);
    bool ret = native_set_parms(CAMERA_PARM_SATURATION, sizeof(saturation),
        &saturation, &result);
    if (result == MM_CAMERA_ERR_INVALID_OPERATION)
        ALOGI("Saturation Value: %d is not set as the selected value is not supported", saturation);

    return ret ? NO_ERROR : UNKNOWN_ERROR;
}

status_t QualcommCameraHardware::setPreviewFormat(const CameraParameters& params)
{
    const char *str = params.getPreviewFormat();
    int32_t previewFormat = attr_lookup(preview_formats, sizeof(preview_formats) / sizeof(str_map), str);
    if (previewFormat != NOT_FOUND) {
        mParameters.set(CameraParameters::KEY_PREVIEW_FORMAT, str);
        mPreviewFormat = previewFormat;
        if (HAL_currentCameraMode != CAMERA_MODE_3D) {
            ALOGI("Setting preview format to native");
            bool ret = native_set_parms(CAMERA_PARM_PREVIEW_FORMAT, sizeof(previewFormat),
                &previewFormat);
        } else {
            ALOGI("Skipping set preview format call to native");
        }
        return NO_ERROR;
    }
    ALOGE("Invalid preview format value: %s", (str == NULL) ? "NULL" : str);
    return BAD_VALUE;
}

status_t QualcommCameraHardware::setStrTextures(const CameraParameters& params)
{
    const char *str = params.get("strtextures");
    if (str != NULL) {
        ALOGV("strtextures = %s", str);
        mParameters.set("strtextures", str);
        if (!strncmp(str, "on", 2) || !strncmp(str, "ON", 2)) {
            strTexturesOn = true;
        } else if (!strncmp(str, "off", 3) || !strncmp(str, "OFF", 3)) {
            strTexturesOn = false;
        }
    }
    return NO_ERROR;
}

status_t QualcommCameraHardware::setBrightness(const CameraParameters& params)
{
    if (!mCfgControl.mm_camera_is_supported(CAMERA_PARM_BRIGHTNESS)) {
        ALOGI("Set Brightness not supported for this sensor");
        return NO_ERROR;
    }
    int brightness = params.getInt("luma-adaptation");
    if (mBrightness != brightness) {
        ALOGV(" new brightness value : %d ", brightness);
        mBrightness = brightness;
        mParameters.set("luma-adaptation", brightness);
        bool ret = native_set_parms(CAMERA_PARM_BRIGHTNESS, sizeof(mBrightness), &mBrightness);
        return ret ? NO_ERROR : UNKNOWN_ERROR;
    }
    return NO_ERROR;
}

status_t QualcommCameraHardware::setSkinToneEnhancement(const CameraParameters& params)
{
    if (!mCfgControl.mm_camera_is_supported(CAMERA_PARM_SCE_FACTOR)) {
        ALOGI("SkinToneEnhancement not supported for this sensor");
        return NO_ERROR;
    }
    int skinToneValue = params.getInt(CameraParameters::KEY_SKIN_TONE_ENHANCEMENT);
    if (mSkinToneEnhancement != skinToneValue) {
        ALOGV(" new skinTone correction value : %d ", skinToneValue);
        mSkinToneEnhancement = skinToneValue;
        mParameters.set("skinToneEnhancement", skinToneValue);
        bool ret = native_set_parms(CAMERA_PARM_SCE_FACTOR, sizeof(mSkinToneEnhancement),
            &mSkinToneEnhancement);
        return ret ? NO_ERROR : UNKNOWN_ERROR;
    }
    return NO_ERROR;
}

status_t QualcommCameraHardware::setWhiteBalance(const CameraParameters& params)
{
    if (!mCfgControl.mm_camera_is_supported(CAMERA_PARM_WHITE_BALANCE)) {
        ALOGI("WhiteBalance not supported for this sensor");
        return NO_ERROR;
    }

    int result;

    const char *str = params.get(CameraParameters::KEY_WHITE_BALANCE);
    if (str != NULL) {
        int32_t value = attr_lookup(whitebalance, sizeof(whitebalance) / sizeof(str_map), str);
        if (value != NOT_FOUND) {
            mParameters.set(CameraParameters::KEY_WHITE_BALANCE, str);
            bool ret = native_set_parms(CAMERA_PARM_WHITE_BALANCE, sizeof(value),
                &value, &result);
            if (result == MM_CAMERA_ERR_INVALID_OPERATION) {
                ALOGI("WhiteBalance Value: %s is not set as the selected value is not supported ", str);
            }
            return ret ? NO_ERROR : UNKNOWN_ERROR;
        }
    }
    ALOGE("Invalid whitebalance value: %s", (str == NULL) ? "NULL" : str);
    return BAD_VALUE;
}

status_t QualcommCameraHardware::setFlash(const CameraParameters& params)
{
    if (!mCfgControl.mm_camera_is_supported(CAMERA_PARM_LED_MODE)) {
        ALOGI("%s: flash not supported", __FUNCTION__);
        return NO_ERROR;
    }

    const char *str = params.get(CameraParameters::KEY_FLASH_MODE);
    if (str != NULL) {
        int32_t value = attr_lookup(flash, sizeof(flash) / sizeof(str_map), str);
        if (value != NOT_FOUND) {
            mParameters.set(CameraParameters::KEY_FLASH_MODE, str);
            bool ret = native_set_parms(CAMERA_PARM_LED_MODE, sizeof(value), &value);
            if (mZslEnable && (value != LED_MODE_OFF)) {
                mParameters.set("num-snaps-per-shutter", "1");
                ALOGI("%s Setting num-snaps-per-shutter to 1", __FUNCTION__);
                numCapture = 1;
            }
            return ret ? NO_ERROR : UNKNOWN_ERROR;
        }
    }
    ALOGE("Invalid flash mode value: %s", (str == NULL) ? "NULL" : str);
    return BAD_VALUE;
}

status_t QualcommCameraHardware::setAntibanding(const CameraParameters& params)
{
    int result;
    if (!mCfgControl.mm_camera_is_supported(CAMERA_PARM_ANTIBANDING)) {
        ALOGI("Parameter AntiBanding is not supported for this sensor");
        return NO_ERROR;
    }
    const char *str = params.get(CameraParameters::KEY_ANTIBANDING);
    if (str != NULL) {
        int value = (camera_antibanding_type)attr_lookup(
            antibanding, sizeof(antibanding) / sizeof(str_map), str);
        if (value != NOT_FOUND) {
            camera_antibanding_type temp = (camera_antibanding_type) value;
            mParameters.set(CameraParameters::KEY_ANTIBANDING, str);
            bool ret = native_set_parms(CAMERA_PARM_ANTIBANDING,
                sizeof(camera_antibanding_type), &temp, &result);
            if (result == MM_CAMERA_ERR_INVALID_OPERATION) {
                ALOGI("AntiBanding Value: %s is not supported for the given BestShot Mode", str);
            }
            return ret ? NO_ERROR : UNKNOWN_ERROR;
        }
    }
    ALOGE("Invalid antibanding value: %s", (str == NULL) ? "NULL" : str);
    return BAD_VALUE;
}

status_t QualcommCameraHardware::setMCEValue(const CameraParameters& params)
{
    if (!mCfgControl.mm_camera_is_supported(CAMERA_PARM_MCE)) {
        ALOGI("Parameter MCE is not supported for this sensor");
        return NO_ERROR;
    }

    const char *str = params.get(CameraParameters::KEY_MEMORY_COLOR_ENHANCEMENT);
    if (str != NULL) {
        int value = attr_lookup(mce, sizeof(mce) / sizeof(str_map), str);
        if (value != NOT_FOUND) {
            int8_t temp = value;
            ALOGI("%s: setting MCE value of %s", __FUNCTION__, str);
            mParameters.set(CameraParameters::KEY_MEMORY_COLOR_ENHANCEMENT, str);
            native_set_parms(CAMERA_PARM_MCE, sizeof(temp), &temp);
            return NO_ERROR;
        }
    }
    ALOGE("Invalid MCE value: %s", (str == NULL) ? "NULL" : str);
    return BAD_VALUE;
}

status_t QualcommCameraHardware::setHighFrameRate(const CameraParameters& params)
{
    if ((!mCfgControl.mm_camera_is_supported(CAMERA_PARM_HFR)) || (mIs3DModeOn)) {
        ALOGI("Parameter HFR is not supported for this sensor");
        return NO_ERROR;
    }

    const char *str = params.get(CameraParameters::KEY_VIDEO_HIGH_FRAME_RATE);
    if (str != NULL) {
        int value = attr_lookup(hfr, sizeof(hfr) / sizeof(str_map), str);
        if (value != NOT_FOUND) {
            int32_t temp = value;
            ALOGI("%s: setting HFR value of %s(%d)", __FUNCTION__, str, temp);
            //Check for change in HFR value
            const char *oldHfr = mParameters.get(CameraParameters::KEY_VIDEO_HIGH_FRAME_RATE);
            if (strcmp(oldHfr, str)) {
                ALOGI("%s: old HFR: %s, new HFR %s", __FUNCTION__, oldHfr, str);
                mParameters.set(CameraParameters::KEY_VIDEO_HIGH_FRAME_RATE, str);
                mHFRMode = true;
                if (mCameraRunning == true) {
                    mHFRThreadWaitLock.lock();
                    pthread_attr_t pattr;
                    pthread_attr_init(&pattr);
                    pthread_attr_setdetachstate(&pattr, PTHREAD_CREATE_DETACHED);
                    mHFRThreadRunning = !pthread_create(&mHFRThread,
                        &pattr,
                        hfr_thread,
                        (void*)NULL);
                    mHFRThreadWaitLock.unlock();
                    return NO_ERROR;
                }
            }
            native_set_parms(CAMERA_PARM_HFR, sizeof(temp), &temp);
            return NO_ERROR;
        }
    }
    ALOGE("Invalid HFR value: %s", (str == NULL) ? "NULL" : str);
    return BAD_VALUE;
}

status_t QualcommCameraHardware::setHDRImaging(const CameraParameters& params)
{
    if (!mCfgControl.mm_camera_is_supported(CAMERA_PARM_HDR) && mZslEnable) {
        ALOGI("Parameter HDR is not supported for this sensor/ ZSL mode");
        return NO_ERROR;
    }
    const char *str = params.get(CameraParameters::KEY_HIGH_DYNAMIC_RANGE_IMAGING);
    if (str != NULL) {
        int value = attr_lookup(hdr, sizeof(hdr) / sizeof(str_map), str);
        if (value != NOT_FOUND) {
            exp_bracketing_t temp;
            memset(&temp, 0, sizeof(temp));
            temp.hdr_enable = value;
            temp.mode = HDR_MODE;
            temp.total_frames = 3;
            temp.total_hal_frames = HDR_HAL_FRAME;
            mHdrMode = temp.hdr_enable;
            ALOGI("%s: setting HDR value of %s", __FUNCTION__, str);
            mParameters.set(CameraParameters::KEY_HIGH_DYNAMIC_RANGE_IMAGING, str);
            if (mHdrMode) {
                numCapture = temp.total_hal_frames;
            } else
                numCapture = 1;
            native_set_parms(CAMERA_PARM_HDR, sizeof(temp), &temp);
            return NO_ERROR;
        }
    }
    ALOGE("Invalid HDR value: %s", (str == NULL) ? "NULL" : str);
    return BAD_VALUE;
}

status_t QualcommCameraHardware::setExpBracketing(const CameraParameters& params)
{
    if (!mCfgControl.mm_camera_is_supported(CAMERA_PARM_HDR) && mZslEnable) {
        ALOGI("Parameter Exposure Bracketing is not supported for this sensor/ZSL mode");
        return NO_ERROR;
    }
    const char *str = params.get("capture-burst-exposures");
    if ((str != NULL) && (!mHdrMode)) {
        char exp_val[MAX_EXP_BRACKETING_LENGTH];
        exp_bracketing_t temp;
        memset(&temp, 0, sizeof(temp));

        mExpBracketMode = true;
        temp.mode = EXP_BRACKETING_MODE;
        temp.hdr_enable = true;
        /* App sets values separated by comma.
           Thus total number of snapshot to capture is strlen(str)/2
           eg: "-1,1,2" */
        strlcpy(exp_val, str, sizeof(exp_val));
        temp.total_frames = (strlen(exp_val) >  MAX_SNAPSHOT_BUFFERS -2) ?
            MAX_SNAPSHOT_BUFFERS -2 : strlen(exp_val);
        temp.total_hal_frames = temp.total_frames;
        strlcpy(temp.values, exp_val, MAX_EXP_BRACKETING_LENGTH);
        ALOGI("%s: setting Exposure Bracketing value of %s", __FUNCTION__, temp.values);
        mParameters.set("capture-burst-exposures", str);
        if (!mZslEnable) {
            numCapture = temp.total_frames;
        }
        native_set_parms(CAMERA_PARM_HDR, sizeof(temp), &temp);
        return NO_ERROR;
    } else
        mExpBracketMode = false;
    return NO_ERROR;
}

status_t QualcommCameraHardware::setLensshadeValue(const CameraParameters& params)
{
    if (!mCfgControl.mm_camera_is_supported(CAMERA_PARM_ROLLOFF)) {
        ALOGI("Parameter Rolloff is not supported for this sensor");
        return NO_ERROR;
    }

    const char *str = params.get(CameraParameters::KEY_LENSSHADE);
    if (str != NULL) {
        int value = attr_lookup(lensshade,
            sizeof(lensshade) / sizeof(str_map), str);
        if (value != NOT_FOUND) {
            int8_t temp = (int8_t)value;
            mParameters.set(CameraParameters::KEY_LENSSHADE, str);
            native_set_parms(CAMERA_PARM_ROLLOFF, sizeof(temp), &temp);
            return NO_ERROR;
        }
    }
    ALOGE("Invalid lensShade value: %s", (str == NULL) ? "NULL" : str);
    return NO_ERROR;
}

status_t QualcommCameraHardware::setSelectableZoneAf(const CameraParameters& params)
{
    if (mHasAutoFocusSupport && supportsSelectableZoneAf()) {
        const char *str = params.get(CameraParameters::KEY_SELECTABLE_ZONE_AF);
        if (str != NULL) {
            int32_t value = attr_lookup(selectable_zone_af, sizeof(selectable_zone_af) / sizeof(str_map), str);
            if (value != NOT_FOUND) {
                mParameters.set(CameraParameters::KEY_SELECTABLE_ZONE_AF, str);
                bool ret = native_set_parms(CAMERA_PARM_FOCUS_RECT, sizeof(value), &value);
                return ret ? NO_ERROR : UNKNOWN_ERROR;
            }
        }
        ALOGE("Invalid selectable zone af value: %s", (str == NULL) ? "NULL" : str);
        return BAD_VALUE;
    }
    return NO_ERROR;
}

status_t QualcommCameraHardware::setTouchAfAec(const CameraParameters& params)
{
    ALOGE("%s",__func__);
    if (mHasAutoFocusSupport) {
        int xAec, yAec, xAf, yAf;
        int cx, cy;
        int width, height;
        params.getMeteringAreaCenter(&cx, &cy);
        mParameters.getPreviewSize(&width, &height);

        // @Punit
        // The coords sent from upper layer is in range (-1000, -1000) to (1000, 1000)
        // So, they are transformed to range (0, 0) to (previewWidth, previewHeight)
        cx = cx + 1000;
        cy = cy + 1000;
        cx = cx * (width / 2000.0f);
        cy = cy * (height / 2000.0f);

        //Negative values are invalid and does not update anything
        ALOGE("Touch Area Center (cx, cy) = (%d, %d)", cx, cy);

        //Currently using same values for AF and AEC
        xAec = cx; yAec = cy;
        xAf = cx; yAf = cy;

        const char *str = params.get(CameraParameters::KEY_TOUCH_AF_AEC);
        if (str != NULL) {
            int value = attr_lookup(touchafaec,
                sizeof(touchafaec) / sizeof(str_map), str);
            if (value != NOT_FOUND) {
                //Dx,Dy will be same as defined in res/layout/camera.xml
                //passed down to HAL in a key.value pair.
                int FOCUS_RECTANGLE_DX = params.getInt("touchAfAec-dx");
                int FOCUS_RECTANGLE_DY = params.getInt("touchAfAec-dy");
                mParameters.set(CameraParameters::KEY_TOUCH_AF_AEC, str);
                mParameters.setTouchIndexAec(xAec, yAec);
                mParameters.setTouchIndexAf(xAf, yAf);

                cam_set_aec_roi_t aec_roi_value;
                roi_info_t af_roi_value;

                memset(&af_roi_value, 0, sizeof(roi_info_t));

                //If touch AF/AEC is enabled and touch event has occured then
                //call the ioctl with valid values.
                if (value == true
                    && (xAec >= 0 && yAec >= 0)
                    && (xAf >= 0 && yAf >= 0)) {
                    //Set Touch AEC params (Pass the center co-ordinate)
                    aec_roi_value.aec_roi_enable = AEC_ROI_ON;
                    aec_roi_value.aec_roi_type = AEC_ROI_BY_COORDINATE;
                    aec_roi_value.aec_roi_position.coordinate.x = xAec;
                    aec_roi_value.aec_roi_position.coordinate.y = yAec;

                    //Set Touch AF params (Pass the top left co-ordinate)
                    af_roi_value.num_roi = 1;
                    if ((xAf-(FOCUS_RECTANGLE_DX/2)) < 0)
                        af_roi_value.roi[0].x = 1;
                    else
                        af_roi_value.roi[0].x = xAf - (FOCUS_RECTANGLE_DX/2);

                    if ((yAf-(FOCUS_RECTANGLE_DY/2)) < 0)
                        af_roi_value.roi[0].y = 1;
                    else
                        af_roi_value.roi[0].y = yAf - (FOCUS_RECTANGLE_DY/2);

                    af_roi_value.roi[0].dx = FOCUS_RECTANGLE_DX;
                    af_roi_value.roi[0].dy = FOCUS_RECTANGLE_DY;
                    af_roi_value.is_multiwindow = mMultiTouch;
                    native_set_parms(CAMERA_PARM_AEC_ROI, sizeof(cam_set_aec_roi_t), &aec_roi_value);
                    native_set_parms(CAMERA_PARM_AF_ROI, sizeof(roi_info_t), &af_roi_value);
                }
                else if (value == false) {
                    //Set Touch AEC params
                    aec_roi_value.aec_roi_enable = AEC_ROI_OFF;
                    aec_roi_value.aec_roi_type = AEC_ROI_BY_COORDINATE;
                    aec_roi_value.aec_roi_position.coordinate.x = DONT_CARE_COORDINATE;
                    aec_roi_value.aec_roi_position.coordinate.y = DONT_CARE_COORDINATE;

                    //Set Touch AF params
                    af_roi_value.num_roi = 0;
                    native_set_parms(CAMERA_PARM_AEC_ROI, sizeof(cam_set_aec_roi_t), &aec_roi_value);
                    native_set_parms(CAMERA_PARM_AF_ROI, sizeof(roi_info_t), &af_roi_value);
                }
                //@Punit: If the values are negative, we dont send anything to the lower layer
            }
            return NO_ERROR;
        }
        ALOGE("Invalid Touch AF/AEC value: %s", (str == NULL) ? "NULL" : str);
        return BAD_VALUE;
    }
    return NO_ERROR;
}

status_t QualcommCameraHardware::setFaceDetection(const char *str)
{
    if (supportsFaceDetection() == false) {
        ALOGI("Face detection is not enabled");
        return NO_ERROR;
    }
    if (str != NULL) {
        int value = attr_lookup(facedetection,
            sizeof(facedetection) / sizeof(str_map), str);
        if (value != NOT_FOUND) {
            mMetaDataWaitLock.lock();
            mFaceDetectOn = value;
            mMetaDataWaitLock.unlock();
            mParameters.set(CameraParameters::KEY_FACE_DETECTION, str);
            return NO_ERROR;
        }
    }
    ALOGE("Invalid Face Detection value: %s", (str == NULL) ? "NULL" : str);
    return BAD_VALUE;
}

status_t QualcommCameraHardware::setRedeyeReduction(const CameraParameters& params)
{
    if (!mCfgControl.mm_camera_is_supported(CAMERA_PARM_REDEYE_REDUCTION)) {
        ALOGI("Parameter Redeye Reduction is not supported for this sensor");
        return NO_ERROR;
    }

    const char *str = params.get(CameraParameters::KEY_REDEYE_REDUCTION);
    if (str != NULL) {
        int value = attr_lookup(redeye_reduction, sizeof(redeye_reduction) / sizeof(str_map), str);
        if (value != NOT_FOUND) {
            int8_t temp = (int8_t)value;
            ALOGI("%s: setting Redeye Reduction value of %s", __FUNCTION__, str);
            mParameters.set(CameraParameters::KEY_REDEYE_REDUCTION, str);

            native_set_parms(CAMERA_PARM_REDEYE_REDUCTION, sizeof(temp), &temp);
            return NO_ERROR;
        }
    }
    ALOGE("Invalid Redeye Reduction value: %s", (str == NULL) ? "NULL" : str);
    return BAD_VALUE;
}

status_t  QualcommCameraHardware::setISOValue(const CameraParameters& params)
{
    int8_t temp_hjr;
    if (!mCfgControl.mm_camera_is_supported(CAMERA_PARM_ISO)) {
        ALOGE("Parameter ISO Value is not supported for this sensor");
        return NO_ERROR;
    }
    const char *str = params.get(CameraParameters::KEY_ISO_MODE);
    if (str != NULL) {
        int value = (camera_iso_mode_type)attr_lookup(iso, sizeof(iso) / sizeof(str_map), str);
        if (value != NOT_FOUND) {
            camera_iso_mode_type temp = (camera_iso_mode_type)value;
            if (value == CAMERA_ISO_DEBLUR) {
                temp_hjr = true;
                native_set_parms(CAMERA_PARM_HJR, sizeof(int8_t), &temp_hjr);
                mHJR = value;
            }
            else {
                if (mHJR == CAMERA_ISO_DEBLUR) {
                    temp_hjr = false;
                    native_set_parms(CAMERA_PARM_HJR, sizeof(int8_t), &temp_hjr);
                    mHJR = value;
                }
            }

            mParameters.set(CameraParameters::KEY_ISO_MODE, str);
            native_set_parms(CAMERA_PARM_ISO, sizeof(temp), &temp);
            return NO_ERROR;
        }
    }
    ALOGE("Invalid Iso value: %s", (str == NULL) ? "NULL" : str);
    return BAD_VALUE;
}

status_t QualcommCameraHardware::setSceneDetect(const CameraParameters& params)
{
    bool retParm1, retParm2;
    if (supportsSceneDetection()) {
        if (!mCfgControl.mm_camera_is_supported(CAMERA_PARM_BL_DETECTION) && !mCfgControl.mm_camera_is_supported(CAMERA_PARM_SNOW_DETECTION)) {
            ALOGE("Parameter Auto Scene Detection is not supported for this sensor");
            return NO_ERROR;
        }
        const char *str = params.get(CameraParameters::KEY_SCENE_DETECT);
        if (str != NULL) {
            int32_t value = attr_lookup(scenedetect, sizeof(scenedetect) / sizeof(str_map), str);
            if (value != NOT_FOUND) {
                mParameters.set(CameraParameters::KEY_SCENE_DETECT, str);

                retParm1 = native_set_parms(CAMERA_PARM_BL_DETECTION, sizeof(value), &value);
                retParm2 = native_set_parms(CAMERA_PARM_SNOW_DETECTION, sizeof(value), &value);

                //All Auto Scene detection modes should be all ON or all OFF.
                if (retParm1 == false || retParm2 == false) {
                    value = !value;
                    retParm1 = native_set_parms(CAMERA_PARM_BL_DETECTION, sizeof(value), &value);

                    retParm2 = native_set_parms(CAMERA_PARM_SNOW_DETECTION, sizeof(value), &value);
                }
                return (retParm1 && retParm2) ? NO_ERROR : UNKNOWN_ERROR;
            }
        }
        ALOGE("Invalid auto scene detection value: %s", (str == NULL) ? "NULL" : str);
        return BAD_VALUE;
    }
    return NO_ERROR;
}

status_t QualcommCameraHardware::setSceneMode(const CameraParameters& params)
{
    if (!mCfgControl.mm_camera_is_supported(CAMERA_PARM_BESTSHOT_MODE)) {
        ALOGE("Parameter Scenemode is not supported for this sensor");
        return NO_ERROR;
    }

    const char *str = params.get(CameraParameters::KEY_SCENE_MODE);

    if (str != NULL) {
        int32_t value = attr_lookup(scenemode, sizeof(scenemode) / sizeof(str_map), str);
        int32_t asd_val;
        if (value != NOT_FOUND) {
            mParameters.set(CameraParameters::KEY_SCENE_MODE, str);
            bool ret = native_set_parms(CAMERA_PARM_BESTSHOT_MODE, sizeof(value), &value);

            if (ret == NO_ERROR) {
                int retParm1,  retParm2;
                /*if value is auto, set ASD on, else set ASD off*/
                if (value == CAMERA_BESTSHOT_AUTO ) {
                    asd_val = TRUE;
                } else {
                    asd_val = FALSE;
                }

                /*note: we need to simplify this logic by using a single ctrl as in 8960*/
                retParm1 = native_set_parms(CAMERA_PARM_BL_DETECTION, sizeof(value), &asd_val);
                retParm2 = native_set_parms(CAMERA_PARM_SNOW_DETECTION, sizeof(value), &asd_val);
            }
            return ret ? NO_ERROR : UNKNOWN_ERROR;
        }
    }
    ALOGE("Invalid scenemode value: %s", (str == NULL) ? "NULL" : str);
    return BAD_VALUE;
}
status_t QualcommCameraHardware::setGpsLocation(const CameraParameters& params)
{
    const char *method = params.get(CameraParameters::KEY_GPS_PROCESSING_METHOD);
    if (method) {
        mParameters.set(CameraParameters::KEY_GPS_PROCESSING_METHOD, method);
    } else {
        mParameters.remove(CameraParameters::KEY_GPS_PROCESSING_METHOD);
    }

    const char *latitude = params.get(CameraParameters::KEY_GPS_LATITUDE);
    if (latitude) {
        ALOGE("latitude %s",latitude);
        mParameters.set(CameraParameters::KEY_GPS_LATITUDE, latitude);
    } else {
        mParameters.remove(CameraParameters::KEY_GPS_LATITUDE);
    }

    const char *latitudeRef = params.get(CameraParameters::KEY_GPS_LATITUDE_REF);
    if (latitudeRef) {
        mParameters.set(CameraParameters::KEY_GPS_LATITUDE_REF, latitudeRef);
    } else {
        mParameters.remove(CameraParameters::KEY_GPS_LATITUDE_REF);
    }

    const char *longitude = params.get(CameraParameters::KEY_GPS_LONGITUDE);
    if (longitude) {
        mParameters.set(CameraParameters::KEY_GPS_LONGITUDE, longitude);
    } else {
        mParameters.remove(CameraParameters::KEY_GPS_LONGITUDE);
    }

    const char *longitudeRef = params.get(CameraParameters::KEY_GPS_LONGITUDE_REF);
    if (longitudeRef) {
        mParameters.set(CameraParameters::KEY_GPS_LONGITUDE_REF, longitudeRef);
    } else {
        mParameters.remove(CameraParameters::KEY_GPS_LONGITUDE_REF);
    }

    const char *altitudeRef = params.get(CameraParameters::KEY_GPS_ALTITUDE_REF);
    if (altitudeRef) {
        mParameters.set(CameraParameters::KEY_GPS_ALTITUDE_REF, altitudeRef);
    } else {
        mParameters.remove(CameraParameters::KEY_GPS_ALTITUDE_REF);
    }

    const char *altitude = params.get(CameraParameters::KEY_GPS_ALTITUDE);
    if (altitude) {
        mParameters.set(CameraParameters::KEY_GPS_ALTITUDE, altitude);
    } else {
        mParameters.remove(CameraParameters::KEY_GPS_ALTITUDE);
    }

    const char *status = params.get(CameraParameters::KEY_GPS_STATUS);
    if (status) {
        mParameters.set(CameraParameters::KEY_GPS_STATUS, status);
    }

    const char *dateTime = params.get(CameraParameters::KEY_EXIF_DATETIME);
    if (dateTime) {
        mParameters.set(CameraParameters::KEY_EXIF_DATETIME, dateTime);
    } else {
        mParameters.remove(CameraParameters::KEY_EXIF_DATETIME);
    }

    const char *timestamp = params.get(CameraParameters::KEY_GPS_TIMESTAMP);
    if (timestamp) {
        mParameters.set(CameraParameters::KEY_GPS_TIMESTAMP, timestamp);
    } else {
        mParameters.remove(CameraParameters::KEY_GPS_TIMESTAMP);
    }

    return NO_ERROR;

}

status_t QualcommCameraHardware::setRotation(const CameraParameters& params)
{
    status_t rc = NO_ERROR;
    int sensor_mount_angle = HAL_cameraInfo[HAL_currentCameraId].sensor_mount_angle;
    int rotation = params.getInt(CameraParameters::KEY_ROTATION);
    if (rotation != NOT_FOUND) {
        if (rotation == 0 || rotation == 90 || rotation == 180
            || rotation == 270) {
            rotation = (rotation + sensor_mount_angle)%360;
            mParameters.set(CameraParameters::KEY_ROTATION, rotation);
            mRotation = rotation;
        } else {
            ALOGE("Invalid rotation value: %d", rotation);
            rc = BAD_VALUE;
        }
    }
    return rc;
}

status_t QualcommCameraHardware::setZoom(const CameraParameters& params)
{
    if (!mCfgControl.mm_camera_is_supported(CAMERA_PARM_ZOOM)) {
        ALOGE("Parameter setZoom is not supported for this sensor");
        return NO_ERROR;
    }
    status_t rc = NO_ERROR;
    // No matter how many different zoom values the driver can provide, HAL
    // provides applictations the same number of zoom levels. The maximum driver
    // zoom value depends on sensor output (VFE input) and preview size (VFE
    // output) because VFE can only crop and cannot upscale. If the preview size
    // is bigger, the maximum zoom ratio is smaller. However, we want the
    // zoom ratio of each zoom level is always the same whatever the preview
    // size is. Ex: zoom level 1 is always 1.2x, zoom level 2 is 1.44x, etc. So,
    // we need to have a fixed maximum zoom value and do read it from the
    // driver.
    static const int ZOOM_STEP = 1;
    int32_t zoom_level = params.getInt(CameraParameters::KEY_ZOOM);
    if (zoom_level >= 0 && zoom_level <= mMaxZoom-1) {
        mParameters.set(CameraParameters::KEY_ZOOM, zoom_level);
        int32_t zoom_value = ZOOM_STEP * zoom_level;
        bool ret = native_set_parms(CAMERA_PARM_ZOOM, sizeof(zoom_value), &zoom_value);
        rc = ret ? NO_ERROR : UNKNOWN_ERROR;
    } else {
        rc = BAD_VALUE;
    }

    return rc;
}

status_t QualcommCameraHardware::setDenoise(const CameraParameters& params)
{
    if (!mCfgControl.mm_camera_is_supported(CAMERA_PARM_WAVELET_DENOISE)) {
        ALOGE("Wavelet Denoise is not supported for this sensor");
        return NO_ERROR;
    }
    const char *str = params.get(CameraParameters::KEY_DENOISE);
    if (str != NULL) {
        int value = attr_lookup(denoise,
        sizeof(denoise) / sizeof(str_map), str);
        if ((value != NOT_FOUND) &&  (mDenoiseValue != value)) {
            mDenoiseValue =  value;
            mParameters.set(CameraParameters::KEY_DENOISE, str);
            bool ret = native_set_parms(CAMERA_PARM_WAVELET_DENOISE, sizeof(value), &value);
            return ret ? NO_ERROR : UNKNOWN_ERROR;
        }
        return NO_ERROR;
    }
    ALOGE("Invalid Denoise value: %s", (str == NULL) ? "NULL" : str);
    return BAD_VALUE;
}

status_t QualcommCameraHardware::setZslParam(const CameraParameters& params)
{
    if (!mZslEnable) {
        ALOGV("Zsl is not enabled");
        return NO_ERROR;
    }
    /* This ensures that restart of Preview doesnt happen when taking
     * Snapshot for continuous viewfinder */
    const char *str = params.get("continuous-temporal-bracketing");
    if (str !=NULL) {
        if (!strncmp(str, "enable", 8))
            mZslPanorama = true;
        else
            mZslPanorama = false;
        return NO_ERROR;
    }
    mZslPanorama = false;
    return NO_ERROR;

}

status_t QualcommCameraHardware::setSnapshotCount(const CameraParameters& params)
{
    int value;
    char snapshotCount[5];
    if (!mZslEnable) {
        value = numCapture;
    } else {
        /* ZSL case: Get value from App */
        const char *str = params.get("num-snaps-per-shutter");
        if (str != NULL) {
            value = atoi(str);
        } else
            value = 1;
    }
    /* Sanity check */
    if (value > MAX_SNAPSHOT_BUFFERS -2)
        value = MAX_SNAPSHOT_BUFFERS -2;
    else if (value < 1)
        value = 1;
    snprintf(snapshotCount, sizeof(snapshotCount),"%d",value);
    numCapture = value;
    mParameters.set("num-snaps-per-shutter", snapshotCount);
    ALOGI("%s setting num-snaps-per-shutter to %s", __FUNCTION__, snapshotCount);
    return NO_ERROR;

}

status_t QualcommCameraHardware::updateFocusDistances(const char *focusmode)
{
    ALOGV("%s: IN", __FUNCTION__);
    focus_distances_info_t focusDistances;
    if ( mCfgControl.mm_camera_get_parm(CAMERA_PARM_FOCUS_DISTANCES,
        &focusDistances) == MM_CAMERA_SUCCESS) {
        String8 str;
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%f", focusDistances.focus_distance[0]);
        str.append(buffer);
        snprintf(buffer, sizeof(buffer), ",%f", focusDistances.focus_distance[1]);
        str.append(buffer);
        if (strcmp(focusmode, CameraParameters::FOCUS_MODE_INFINITY) == 0)
            snprintf(buffer, sizeof(buffer), ",%s", "Infinity");
        else
            snprintf(buffer, sizeof(buffer), ",%f", focusDistances.focus_distance[2]);
        str.append(buffer);
        ALOGI("%s: setting KEY_FOCUS_DISTANCES as %s", __FUNCTION__, str.string());
        mParameters.set(CameraParameters::KEY_FOCUS_DISTANCES, str.string());
        return NO_ERROR;
    }
    ALOGE("%s: get CAMERA_PARM_FOCUS_DISTANCES failed!!!", __FUNCTION__);
    return BAD_VALUE;
}

status_t QualcommCameraHardware::setMeteringAreas(const CameraParameters& params)
{
    const char *str = params.get(CameraParameters::KEY_METERING_AREAS);
    if (str == NULL || (strcmp(str, "0") == 0)) {
        ALOGE("%s: Parameter string is null", __FUNCTION__);
    }
    else {
        // handling default string
        if ((strcmp("(-2000,-2000,-2000,-2000,0)", str) == 0) ||
            (strcmp("(0,0,0,0,0)", str) == 0)) {
            mParameters.set(CameraParameters::KEY_METERING_AREAS, "");
            return NO_ERROR;
        }
        if (checkAreaParameters(str) != 0) {
            ALOGE("%s: Failed to parse the input string '%s'", __FUNCTION__, str);
            return BAD_VALUE;
        }
        mParameters.set(CameraParameters::KEY_METERING_AREAS, str);
    }

    return NO_ERROR;
}

status_t QualcommCameraHardware::setFocusAreas(const CameraParameters& params)
{
    const char *str = params.get(CameraParameters::KEY_FOCUS_AREAS);

    if (str == NULL || strcmp(str, "0") == 0) {
        ALOGE("%s: Parameter string is null", __FUNCTION__);
    }
    else {
        // handling default string
        if (strcmp("(-2000,-2000,-2000,-2000,0)", str) == 0 ||
            strcmp("(0,0,0,0,0)", str) == 0) {
            mParameters.set(CameraParameters::KEY_FOCUS_AREAS, "");
            return NO_ERROR;
        }

        if (checkAreaParameters(str) != 0) {
            ALOGE("%s: Failed to parse the input string '%s'", __FUNCTION__, str);
            return BAD_VALUE;
        }

        mParameters.set(CameraParameters::KEY_FOCUS_AREAS, str);
    }

    return NO_ERROR;
}
status_t QualcommCameraHardware::setFocusMode(const CameraParameters& params)
{
    const char *str = params.get(CameraParameters::KEY_FOCUS_MODE);
    if (str != NULL) {
        ALOGE("FocusMode =%s", str);
        int32_t value = attr_lookup(focus_modes,
            sizeof(focus_modes) / sizeof(str_map), str);
        if (value != NOT_FOUND) {
            mParameters.set(CameraParameters::KEY_FOCUS_MODE, str);

            if (mHasAutoFocusSupport && updateFocusDistances(str) != NO_ERROR) {
                ALOGE("%s: updateFocusDistances failed for %s", __FUNCTION__, str);
                return UNKNOWN_ERROR;
            }

            if (mHasAutoFocusSupport) {
                int8_t cafSupport = FALSE;
                if (!strcmp(str, CameraParameters::FOCUS_MODE_CONTINUOUS_VIDEO) ||
                    !strcmp(str, CameraParameters::FOCUS_MODE_CONTINUOUS_PICTURE)) {
                    cafSupport = TRUE;
                }
                ALOGV("Continuous Auto Focus %d", cafSupport);
                native_set_parms(CAMERA_PARM_CONTINUOUS_AF, sizeof(cafSupport), &cafSupport);
            }
            // Focus step is reset to infinity when preview is started. We do
            // not need to do anything now.
            return NO_ERROR;
        }
    }
    ALOGE("Invalid focus mode value: %s", (str == NULL) ? "NULL" : str);
    return BAD_VALUE;

}

status_t QualcommCameraHardware::setOrientation(const CameraParameters& params)
{
    const char *str = params.get("orientation");

    if (str != NULL) {
        if (strcmp(str, "portrait") == 0 || strcmp(str, "landscape") == 0) {
            // Camera service needs this to decide if the preview frames and raw
            // pictures should be rotated.
            mParameters.set("orientation", str);
        } else {
            ALOGE("Invalid orientation value: %s", str);
            return BAD_VALUE;
        }
    }
    return NO_ERROR;
}

status_t QualcommCameraHardware::setPictureFormat(const CameraParameters& params)
{
    const char * str = params.get(CameraParameters::KEY_PICTURE_FORMAT);

    if (str != NULL) {
        int32_t value = attr_lookup(picture_formats,
            sizeof(picture_formats) / sizeof(str_map), str);
        if (value != NOT_FOUND) {
            mParameters.set(CameraParameters::KEY_PICTURE_FORMAT, str);
        } else {
            ALOGE("Invalid Picture Format value: %s", str);
            return BAD_VALUE;
        }
    }
    return NO_ERROR;
}

QualcommCameraHardware::MMCameraDL::MMCameraDL()
{
    ALOGV("MMCameraDL: E");
    libmmcamera = NULL;
#ifdef DLOPEN_LIBMMCAMERA
    libmmcamera = ::dlopen("liboemcamera.so", RTLD_NOW);
#endif
    ALOGV("Open MM camera DL libeomcamera loaded at %p ", libmmcamera);
    ALOGV("MMCameraDL: X");
}

void *QualcommCameraHardware::MMCameraDL::pointer()
{
    return libmmcamera;
}

QualcommCameraHardware::MMCameraDL::~MMCameraDL()
{
    ALOGV("~MMCameraDL: E");
    LINK_mm_camera_destroy();
    if (libmmcamera != NULL) {
        ::dlclose(libmmcamera);
        ALOGV("closed MM Camera DL ");
    }
    libmmcamera = NULL;
    ALOGV("~MMCameraDL: X");
}

wp<QualcommCameraHardware::MMCameraDL> QualcommCameraHardware::MMCameraDL::instance;
Mutex QualcommCameraHardware::MMCameraDL::singletonLock;

sp<QualcommCameraHardware::MMCameraDL> QualcommCameraHardware::MMCameraDL::getInstance()
{
    Mutex::Autolock instanceLock(singletonLock);
    sp<MMCameraDL> mmCamera = instance.promote();
    if (mmCamera == NULL) {
        mmCamera = new MMCameraDL();
        instance = mmCamera;
    }
    return mmCamera;
}

static void receive_camframe_callback(struct msm_frame *frame)
{
    QualcommCameraHardware *obj = QualcommCameraHardware::getInstance();
    if (obj != 0) {
        obj->receivePreviewFrame(frame);
    }
}

static void receive_camstats_callback(camstats_type stype, camera_preview_histogram_info *histinfo)
{
    QualcommCameraHardware *obj = QualcommCameraHardware::getInstance();
    if (obj != 0) {
        obj->receiveCameraStats(stype,histinfo);
    }
}

static void receive_liveshot_callback(liveshot_status status, uint32_t jpeg_size)
{
    if (status == LIVESHOT_SUCCESS) {
        QualcommCameraHardware *obj = QualcommCameraHardware::getInstance();
        if (obj != 0) {
            obj->receiveLiveSnapshot(jpeg_size);
        }
    }
    else
        ALOGE("Liveshot not succesful");
}

static int8_t receive_event_callback(mm_camera_event *event)
{
    ALOGV("%s: E", __FUNCTION__);
    if (event == NULL) {
        ALOGE("%s: event is NULL!", __FUNCTION__);
        return FALSE;
    }

    QualcommCameraHardware *obj = QualcommCameraHardware::getInstance();
    if (obj == NULL)
        return TRUE;

    switch(event->event_type) {
    case SNAPSHOT_DONE:
        /* postview buffer is received */
        obj->receiveRawPicture(NO_ERROR, event->event_data.yuv_frames[0], event->event_data.yuv_frames[0]);
        break;
    case SNAPSHOT_FAILED:
        /* postview buffer is received */
        obj->receiveRawPicture(UNKNOWN_ERROR, NULL, NULL);
        break;
    case JPEG_ENC_DONE:
        obj->receiveJpegPicture(NO_ERROR, event->event_data.encoded_frame);
        break;
    case JPEG_ENC_FAILED:
        obj->receiveJpegPicture(UNKNOWN_ERROR, 0);
        break;
    default:
        ALOGE("%s: ignore default case", __FUNCTION__);
    }
    ALOGV("%s: X", __FUNCTION__);
    return TRUE;
}

static void receive_camframe_video_callback(struct msm_frame *frame)
{
    ALOGV("receive_camframe_video_callback E");
    QualcommCameraHardware *obj = QualcommCameraHardware::getInstance();
    if (obj != 0) {
        obj->receiveRecordingFrame(frame);
    }
    ALOGV("receive_camframe_video_callback X");
}

int QualcommCameraHardware::storeMetaDataInBuffers(int enable)
{
    /* this is a dummy func now. fix me later */
    ALOGI("in storeMetaDataInBuffers : enable %d", enable);
    //mStoreMetaDataInFrame = enable;
    return INVALID_OPERATION;
}

void QualcommCameraHardware::setCallbacks(camera_notify_callback notify_cb,
    camera_data_callback data_cb,
    camera_data_timestamp_callback data_cb_timestamp,
    camera_request_memory get_memory,
    void *user)
{
    Mutex::Autolock lock(mLock);
    mNotifyCallback = notify_cb;
    mDataCallback = data_cb;
    mDataCallbackTimestamp = data_cb_timestamp;
    mGetMemory = get_memory;
    mCallbackCookie = user;
}

void QualcommCameraHardware::enableMsgType(int32_t msgType)
{
    Mutex::Autolock lock(mLock);
    mMsgEnabled |= msgType;
    if (mCurrentTarget != TARGET_MSM7630 &&
        mCurrentTarget != TARGET_QSD8250 &&
        mCurrentTarget != TARGET_MSM8660) {
        if (mMsgEnabled & CAMERA_MSG_VIDEO_FRAME) {
            native_start_ops(CAMERA_OPS_VIDEO_RECORDING, NULL);
            mRecordingState = 1;
        }
    }
}

void QualcommCameraHardware::disableMsgType(int32_t msgType)
{
    Mutex::Autolock lock(mLock);
    if (mCurrentTarget != TARGET_MSM7630 &&
        mCurrentTarget != TARGET_QSD8250 &&
        mCurrentTarget != TARGET_MSM8660) {
        if (mMsgEnabled & CAMERA_MSG_VIDEO_FRAME) {
            native_stop_ops(CAMERA_OPS_VIDEO_RECORDING, NULL);
            mRecordingState = 0;
        }
    }
    mMsgEnabled &= ~msgType;
}

bool QualcommCameraHardware::msgTypeEnabled(int32_t msgType)
{
    return (mMsgEnabled & msgType);
}

void QualcommCameraHardware::receive_camframe_error_timeout(void)
{
    ALOGI("receive_camframe_error_timeout: E");
    Mutex::Autolock l(&mCamframeTimeoutLock);
    ALOGE(" Camframe timed out. Not receiving any frames from camera driver ");
    camframe_timeout_flag = TRUE;
    mNotifyCallback(CAMERA_MSG_ERROR, CAMERA_ERROR_UNKNOWN, 0, mCallbackCookie);
    ALOGI("receive_camframe_error_timeout: X");
}

static void receive_camframe_error_callback(camera_error_type err)
{
    QualcommCameraHardware *obj = QualcommCameraHardware::getInstance();
    if (obj != 0) {
        if (err == CAMERA_ERROR_TIMEOUT || err == CAMERA_ERROR_ESD) {
            /* Handling different error types is dependent on the requirement.
             * Do the same action by default
             */
            obj->receive_camframe_error_timeout();
        }
    }
}

bool QualcommCameraHardware::isValidDimension(int width, int height)
{
    bool retVal = false;
    /* This function checks if a given resolution is valid or not.
     * A particular resolution is considered valid if it satisfies
     * the following conditions:
     * 1. width & height should be multiple of 16.
     * 2. width & height should be less than/equal to the dimensions
     *    supported by the camera sensor.
     * 3. the aspect ratio is a valid aspect ratio and is among the
     *    commonly used aspect ratio as determined by the thumbnail_sizes
     *    data structure.
     */

    if (width == CEILING16(width) && height == CEILING16(height) &&
        width <= maxSnapshotWidth && height <= maxSnapshotHeight) {
        uint32_t pictureAspectRatio = (width * Q12) / height;
        for (uint32_t i = 0; i < THUMBNAIL_SIZE_COUNT; i++) {
            if (thumbnail_sizes[i].aspect_ratio == pictureAspectRatio) {
                retVal = true;
                break;
            }
        }
    }
    return retVal;
}

void QualcommCameraHardware::getCameraInfo()
{
    ALOGI("getCameraInfo: IN");
    mm_camera_status_t status;

#ifdef DLOPEN_LIBMMCAMERA
    void *libhandle = dlopen("liboemcamera.so", RTLD_NOW);
    if (!libhandle) {
        ALOGE("FATAL ERROR: could not dlopen liboemcamera.so: %s", dlerror());
    }
    *(void **)&LINK_mm_camera_get_camera_info =
        ::dlsym(libhandle, "mm_camera_get_camera_info");
#endif
    storeTargetType();
    status = LINK_mm_camera_get_camera_info(HAL_cameraInfo, &HAL_numOfCameras);
    ALOGI("getCameraInfo: numOfCameras = %d", HAL_numOfCameras);
    for (int i = 0; i < HAL_numOfCameras; i++) {
        if (HAL_cameraInfo[i].position == BACK_CAMERA &&
            mCurrentTarget == TARGET_MSM8660) {
            HAL_cameraInfo[i].modes_supported |= CAMERA_ZSL_MODE;
        } else {
            HAL_cameraInfo[i].modes_supported |= CAMERA_NONZSL_MODE;
        }
        ALOGI("Camera sensor %d info:", i);
        ALOGI("camera_id: %d", HAL_cameraInfo[i].camera_id);
        ALOGI("modes_supported: %x", HAL_cameraInfo[i].modes_supported);
        ALOGI("position: %d", HAL_cameraInfo[i].position);
        ALOGI("sensor_mount_angle: %d", HAL_cameraInfo[i].sensor_mount_angle);
    }

    ALOGI("getCameraInfo: OUT");
}

extern "C" {
int HAL_getNumberOfCameras()
{
    QualcommCameraHardware::getCameraInfo();
    return HAL_numOfCameras;
}

void HAL_getCameraInfo(int cameraId, struct CameraInfo *cameraInfo)
{
    if (cameraInfo == NULL) {
        ALOGE("cameraInfo is NULL");
        return;
    }

    for (int i = 0; i < HAL_numOfCameras; i++) {
        if (i == cameraId) {
            ALOGI("Found a matching camera info for ID %d", cameraId);
            cameraInfo->facing = (HAL_cameraInfo[i].position == BACK_CAMERA) ?
                CAMERA_FACING_BACK : CAMERA_FACING_FRONT;
            cameraInfo->orientation =
                ((APP_ORIENTATION - HAL_cameraInfo[i].sensor_mount_angle) + 360)%360;

            ALOGI("%s: orientation = %d", __FUNCTION__, cameraInfo->orientation);
            sensor_rotation = HAL_cameraInfo[i].sensor_mount_angle;

            return;
        }
    }
    ALOGE("Unable to find matching camera info for ID %d", cameraId);
}
} /* extern "C" */
