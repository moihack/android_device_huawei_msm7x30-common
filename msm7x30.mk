#
# Copyright (C) 2014  Rudolf Tammekivi <rtammekivi@gmail.com>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# The gps config appropriate for this device
$(call inherit-product, device/common/gps/gps_eu_supl.mk)

# Use standard dalvik heap sizes
$(call inherit-product, frameworks/native/build/phone-hdpi-512-dalvik-heap.mk)

DEVICE_PACKAGE_OVERLAYS += device/huawei/msm7x30-common/overlay

PRODUCT_AAPT_CONFIG := normal hdpi
PRODUCT_AAPT_PREF_CONFIG := hdpi

# Common hardware-specific features
PRODUCT_COPY_FILES += \
	frameworks/native/data/etc/handheld_core_hardware.xml:system/etc/permissions/handheld_core_hardware.xml \
	frameworks/native/data/etc/android.hardware.location.gps.xml:system/etc/permissions/android.hardware.location.gps.xml \
	frameworks/native/data/etc/android.hardware.touchscreen.multitouch.jazzhand.xml:system/etc/permissions/android.hardware.touchscreen.multitouch.jazzhand.xml \
	frameworks/native/data/etc/android.hardware.sensor.light.xml:system/etc/permissions/android.hardware.sensor.light.xml \
	frameworks/native/data/etc/android.hardware.sensor.proximity.xml:system/etc/permissions/android.hardware.sensor.proximity.xml \
	frameworks/native/data/etc/android.hardware.telephony.gsm.xml:system/etc/permissions/android.hardware.telephony.gsm.xml \
	frameworks/native/data/etc/android.hardware.camera.flash-autofocus.xml:system/etc/permissions/android.hardware.camera.flash-autofocus.xml \
	frameworks/native/data/etc/android.hardware.wifi.xml:system/etc/permissions/android.hardware.wifi.xml \
	frameworks/native/data/etc/android.hardware.usb.accessory.xml:system/etc/permissions/android.hardware.usb.accessory.xml \
	frameworks/native/data/etc/android.hardware.usb.host.xml:system/etc/permissions/android.hardware.usb.host.xml

# Graphics libraries
PRODUCT_PACKAGES += \
	libc2dcolorconvert \
	libgenlock \
	libmemalloc \
	liboverlay \
	libqdutils \
	libqservice \
	libtilerenderer

# OMX libraries
PRODUCT_PACKAGES += \
	libstagefrighthw \
	libOmxCore \
	libOmxVdec \
	libOmxVenc

# Audio libraries
PRODUCT_PACKAGES += \
	libaudio-resampler \
	libaudioutils \
	libdashplayer

# HAL
PRODUCT_PACKAGES += \
	audio.a2dp.default \
	audio.primary.msm7x30 \
	audio_policy.msm7x30 \
	camera.msm7x30 \
	copybit.msm7x30 \
	gralloc.msm7x30 \
	gps.msm7x30 \
	hwcomposer.msm7x30 \
	lights.msm7x30 \
	memtrack.msm7x30 \
	power.msm7x30

# Common packages
PRODUCT_PACKAGES += \
	hwprops \
	fstab.qcom \
	init.qcom.rc \
	init.qcom.usb.rc \
	ueventd.qcom.rc \
	audio_policy.conf \
	media_codecs.xml \
	wpa_supplicant_overlay.conf

# Common apps
PRODUCT_PACKAGES += \
	Torch

# Recovery init script
PRODUCT_COPY_FILES += \
	$(LOCAL_PATH)/recovery/init.recovery.qcom.rc:root/init.recovery.qcom.rc

# Common properties
PRODUCT_PROPERTY_OVERRIDES += \
	ro.sf.lcd_density=240 \
	ro.config.low_ram=true \
	ro.opengles.version=131072 \
	persist.sys.usb.config=mtp \
	wifi.interface=wlan0 \
	ro.bt.bdaddr_path=/sys/hwprops/btmac \
	ro.hwro=1

# Audio properties
PRODUCT_PROPERTY_OVERRIDES += \
	lpa.decode=false \
	ro.hs_intmic.supported=1 \
	af.resampler.quality=4 \
	audio.gapless.playback.disable=true

# Graphics properties
PRODUCT_PROPERTY_OVERRIDES += \
	debug.sf.hw=1 \
	debug.egl.hw=1 \
	debug.composition.type=dyn \
	persist.hwc.mdpcomp.enable=false \
	debug.mdpcomp.maxlayer=3 \
	debug.mdpcomp.logs=0

# Include proprietary stuff
$(call inherit-product, vendor/huawei/msm7x30-common/msm7x30-common-vendor.mk)
