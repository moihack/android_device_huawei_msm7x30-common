/*
** Copyright (c) 2011 The Linux Foundation. All rights reserved.
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

#ifndef __QUALCOMM_CAMERA_H__
#define __QUALCOMM_CAMERA_H__

extern "C" {

#include <hardware/camera.h>

int qcamera_get_number_of_cameras(void);
int qcamera_get_camera_info(int camera_id, struct camera_info *info);
int qcamera_open(const struct hw_module_t* module, const char* id,
	struct hw_device_t** device);
int qcamera_close(struct hw_device_t* device);

int set_preview_window(struct camera_device *,
	struct preview_stream_ops *window);
void set_callbacks(struct camera_device *,
	camera_notify_callback notify_cb,
	camera_data_callback data_cb,
	camera_data_timestamp_callback data_cb_timestamp,
	camera_request_memory get_memory,
	void *user);
void enable_msg_type(struct camera_device *, int32_t msg_type);
void disable_msg_type(struct camera_device *, int32_t msg_type);
int msg_type_enabled(struct camera_device *, int32_t msg_type);
int start_preview(struct camera_device *);
void stop_preview(struct camera_device *);
int preview_enabled(struct camera_device *);
int store_meta_data_in_buffers(struct camera_device *, int enable);
int start_recording(struct camera_device *);
void stop_recording(struct camera_device *);
int recording_enabled(struct camera_device *);
void release_recording_frame(struct camera_device *, const void *opaque);
int auto_focus(struct camera_device *);
int cancel_auto_focus(struct camera_device *);
int take_picture(struct camera_device *);
int cancel_picture(struct camera_device *);
int set_parameters(struct camera_device *, const char *parms);
char *get_parameters(struct camera_device *);
void put_parameters(struct camera_device *, char *);
int send_command(struct camera_device *,
int32_t cmd, int32_t arg1, int32_t arg2);
void release(struct camera_device *);
int dump(struct camera_device *, int fd);

} // extern "C"

#endif /* __QUALCOMM_CAMERA_H__ */