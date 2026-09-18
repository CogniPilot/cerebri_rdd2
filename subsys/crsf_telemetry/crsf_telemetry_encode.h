/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure C encoder for CRSF telemetry frames and for the Yaapu
 * passthrough words carried inside CRSF frame type 0x80.
 *
 * The ground truth for the passthrough bit layout is the Yaapu Lua widget
 * decoder (processTelemetry / crossfirePop); where other encoders of this
 * format disagree with it, the Lua layout is used.
 *
 * No Zephyr headers here: every multi-byte field is hand packed so this file
 * can be built and checked on the host.
 */

#ifndef CRSF_TELEMETRY_ENCODE_H
#define CRSF_TELEMETRY_ENCODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CRSF_YAAPU_AP_STATUS_APPID  0x5001
#define CRSF_YAAPU_GPS_STATUS_APPID 0x5002
#define CRSF_YAAPU_BATTERY_APPID    0x5003
#define CRSF_YAAPU_HOME_APPID       0x5004
#define CRSF_YAAPU_VELANDYAW_APPID  0x5005
#define CRSF_YAAPU_ROLLPITCH_APPID  0x5006
#define CRSF_YAAPU_PARAMS_APPID     0x5007
#define CRSF_AP_CUSTOM_STATUS_TEXT  0xF1
#define CRSF_AP_CUSTOM_MULTI_PACKET 0xF2
#define CRSF_AP_SEVERITY_CRITICAL   2
#define CRSF_AP_SEVERITY_INFO       6
#define CRSF_MAV_TYPE_QUADROTOR     2
#define CRSF_YAAPU_PARAM_FRAME_TYPE 1
#define CRSF_MULTI_PACKET_MAX       3  /* ELRS cap */
#define CRSF_STATUS_TEXT_MAX        49 /* chars, plus NUL */
#define CRSF_ENCODE_BUF_LEN         64
#define CRSF_BIND_COMMAND_LEN       5

/** One Yaapu passthrough word: application id plus its 32 bit payload. */
struct crsf_yaapu_packet {
	uint16_t appid;
	uint32_t data;
};

/**
 * Pack a number into the FrSky passthrough exponent+mantissa+sign field.
 *
 * Behavioural port of AP_Frsky_SPort::prep_number(). Supported (digits,power)
 * pairs are (2,0), (2,1), (2,2), (3,1) and (3,2); anything else returns 0.
 */
uint16_t crsf_prep_number(int32_t number, uint8_t digits, uint8_t power);

/** Hamilton quaternion (w,x,y,z) to ZYX (321) Euler angles, radians. */
void crsf_quat_to_euler321(float w, float x, float y, float z, float *roll_rad, float *pitch_rad,
			   float *yaw_rad);

/** 0x5006 attitude word, range field left at 0. */
uint32_t crsf_yaapu_rollpitch(float roll_rad, float pitch_rad);

/** 0x5005 velocity and yaw word, yaw wrapped to [0,360). */
uint32_t crsf_yaapu_velandyaw(float vspeed_m_s, float hspeed_m_s, float yaw_rad);

/**
 * 0x5001 autopilot status word.
 *
 * Mode field is cerebri_mode + 1 for modes 0..2, else 0. throttle_milli is
 * 0..1000 (per mille of full throttle).
 */
uint32_t crsf_yaapu_ap_status(uint8_t cerebri_mode, bool armed, bool failsafe,
			      int16_t throttle_milli);

/** 0x5002 GPS status word. fix_type is the synapse fix enum (0..7). */
uint32_t crsf_yaapu_gps_status(uint8_t sats, uint8_t fix_type, uint16_t hdop_centi,
			       int32_t alt_msl_mm);

/** 0x5003 battery word. */
uint32_t crsf_yaapu_battery(uint16_t voltage_cv, int16_t current_da, uint16_t consumed_mah);

/** 0x5004 home word, home taken to be the ENU frame origin. */
uint32_t crsf_yaapu_home(float east_m, float north_m, float up_m);

/** 0x5007 parameter word. */
uint32_t crsf_yaapu_param(uint8_t id, uint32_t value);

/**
 * Build the payload of a CRSF type 0x80 multi packet frame:
 * [0xF2][n][appid LE16, data LE32] * n.
 *
 * @return 2 + 6 * n, or 0 when n is 0, above CRSF_MULTI_PACKET_MAX, or the
 *         buffer is too small.
 */
size_t crsf_encode_multi_packet(uint8_t *buf, size_t buf_len, const struct crsf_yaapu_packet *pk,
				size_t n);

/**
 * Build the payload of a CRSF type 0x80 status text frame:
 * [0xF1][severity][text][NUL], text truncated to CRSF_STATUS_TEXT_MAX chars.
 *
 * @return length + 3, or 0 on error or when text is shorter than 2 chars.
 */
size_t crsf_encode_status_text(uint8_t *buf, size_t buf_len, uint8_t severity, const char *text);

/** Build the 15 byte big-endian payload of a CRSF type 0x02 GPS frame. */
size_t crsf_encode_gps(uint8_t *buf, size_t buf_len, int32_t lat_e7, int32_t lon_e7,
		       uint16_t ground_speed_cm_s, uint16_t course_cdeg, int32_t alt_msl_mm,
		       uint8_t sats);

/** Build the 8 byte big-endian payload of a CRSF type 0x08 battery frame. */
size_t crsf_encode_battery(uint8_t *buf, size_t buf_len, uint16_t voltage_cv, int16_t current_da,
			   uint32_t consumed_mah, uint8_t remaining_pct);

/** Build the payload of a CRSF type 0x21 flight mode frame: name plus NUL. */
size_t crsf_encode_flight_mode(uint8_t *buf, size_t buf_len, const char *name);

/**
 * Build the payload of the CRSF type 0x32 command frame that asks the
 * receiver to enter bind mode:
 * [dest 0xEC][origin 0xC8][command id 0x10 (receiver)][sub command 0x01 (bind)]
 * [command CRC8, poly 0xBA over the 0x32 type byte through the sub command].
 *
 * The frame that reaches the wire once the driver has added the sync byte,
 * the length and the 0xD5 frame CRC is
 * C8 07 32 EC C8 10 01 9E E8.
 *
 * @return CRSF_BIND_COMMAND_LEN, or 0 when the buffer is too small.
 */
size_t crsf_encode_bind_command(uint8_t *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* CRSF_TELEMETRY_ENCODE_H */
