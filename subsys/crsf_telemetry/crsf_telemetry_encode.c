/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "crsf_telemetry_encode.h"

#include <math.h>
#include <string.h>

#define RAD_TO_DEG 57.2957795130823208768f

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/** Wrap an angle in degrees to [0,360). */
static float wrap360(float deg)
{
	deg = fmodf(deg, 360.0f);
	return deg < 0.0f ? deg + 360.0f : deg;
}

static void store_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void store_be32(uint8_t *p, uint32_t v)
{
	store_be16(p, (uint16_t)(v >> 16));
	store_be16(p + 2, (uint16_t)v);
}

/** Length of a NUL terminated string, capped at max. */
static size_t str_len_max(const char *s, size_t max)
{
	size_t len = 0;

	while (len < max && s[len] != '\0') {
		len++;
	}
	return len;
}

uint16_t crsf_prep_number(int32_t number, uint8_t digits, uint8_t power)
{
	uint16_t res = 0;
	uint32_t abs_number = number < 0 ? (uint32_t)(-(int64_t)number) : (uint32_t)number;

	if (digits == 2 && power == 0) {
		/* 7 bits: the client has to know if the range is 0,127 or -63,63 */
		uint32_t max_value = number < 0 ? 63u : 127u;

		res = (uint16_t)(abs_number > max_value ? max_value : abs_number);
		if (number < 0) {
			res |= 1u << 6;
		}
	} else if (digits == 2 && power == 1) {
		/* 8 bits: 7 for the digits, 1 for 10^power */
		if (abs_number < 100) {
			res = (uint16_t)(abs_number << 1);
		} else if (abs_number < 1270) {
			res = (uint16_t)(((uint8_t)lroundf(abs_number * 0.1f) << 1) | 0x1);
		} else {
			res = 0xFF; /* max is 0x7F x 10^1 = 1270 */
		}
		if (number < 0) {
			res |= 0x1 << 8;
		}
	} else if (digits == 2 && power == 2) {
		/* 9 bits: 7 for the digits, 2 for 10^power */
		if (abs_number < 100) {
			res = (uint16_t)(abs_number << 2);
		} else if (abs_number < 1000) {
			res = (uint16_t)(((uint8_t)lroundf(abs_number * 0.1f) << 2) | 0x1);
		} else if (abs_number < 10000) {
			res = (uint16_t)(((uint8_t)lroundf(abs_number * 0.01f) << 2) | 0x2);
		} else if (abs_number < 127000) {
			res = (uint16_t)(((uint8_t)lroundf(abs_number * 0.001f) << 2) | 0x3);
		} else {
			res = 0x1FF; /* max is 0x7F x 10^3 = 127000 */
		}
		if (number < 0) {
			res |= 0x1 << 9;
		}
	} else if (digits == 3 && power == 1) {
		/* 11 bits: 10 for the digits, 1 for 10^power */
		if (abs_number < 1000) {
			res = (uint16_t)(abs_number << 1);
		} else if (abs_number < 10240) {
			res = (uint16_t)(((uint16_t)lroundf(abs_number * 0.1f) << 1) | 0x1);
		} else {
			res = 0x7FF; /* max is 0x3FF x 10^1 = 10230 */
		}
		if (number < 0) {
			res |= 0x1 << 11;
		}
	} else if (digits == 3 && power == 2) {
		/* 12 bits: 10 for the digits, 2 for 10^power */
		if (abs_number < 1000) {
			res = (uint16_t)(abs_number << 2);
		} else if (abs_number < 10000) {
			res = (uint16_t)(((uint16_t)lroundf(abs_number * 0.1f) << 2) | 0x1);
		} else if (abs_number < 100000) {
			res = (uint16_t)(((uint16_t)lroundf(abs_number * 0.01f) << 2) | 0x2);
		} else if (abs_number < 1024000) {
			res = (uint16_t)(((uint16_t)lroundf(abs_number * 0.001f) << 2) | 0x3);
		} else {
			res = 0xFFF; /* max is 0x3FF x 10^3 = 1023000 */
		}
		if (number < 0) {
			res |= 0x1 << 12;
		}
	}
	return res;
}

void crsf_quat_to_euler321(float w, float x, float y, float z, float *roll_rad, float *pitch_rad,
			   float *yaw_rad)
{
	float sin_pitch = 2.0f * (w * y - z * x);

	if (sin_pitch > 1.0f) {
		sin_pitch = 1.0f;
	} else if (sin_pitch < -1.0f) {
		sin_pitch = -1.0f;
	}

	*roll_rad = atan2f(2.0f * (w * x + y * z), 1.0f - 2.0f * (x * x + y * y));
	*pitch_rad = asinf(sin_pitch);
	*yaw_rad = atan2f(2.0f * (w * z + x * y), 1.0f - 2.0f * (y * y + z * z));
}

uint32_t crsf_yaapu_rollpitch(float roll_rad, float pitch_rad)
{
	/* Lua: roll = (min(extract(0,11),1800) - 900) * 0.2, pitch at offset 11 */
	int32_t roll = clamp_i32((int32_t)lroundf((roll_rad * RAD_TO_DEG + 180.0f) / 0.2f), 0, 1800);
	int32_t pitch = clamp_i32((int32_t)lroundf((pitch_rad * RAD_TO_DEG + 90.0f) / 0.2f), 0, 900);

	/* bits 21-31 hold the rangefinder distance, which this build does not have */
	return ((uint32_t)roll & 0x7FF) | (((uint32_t)pitch & 0x3FF) << 11);
}

uint32_t crsf_yaapu_velandyaw(float vspeed_m_s, float hspeed_m_s, float yaw_rad)
{
	int32_t yaw = (int32_t)lroundf(wrap360(yaw_rad * RAD_TO_DEG) / 0.2f);
	uint32_t value = crsf_prep_number((int32_t)lroundf(vspeed_m_s * 10.0f), 2, 1);

	/*
	 * The Lua decoder reads the horizontal speed as 8 bits (exponent at 9,
	 * mantissa at 10) and the yaw at offset 17, so the prep_number sign bit
	 * would land inside the yaw field: negative speeds are clamped to 0 and
	 * the field is masked to 8 bits.
	 */
	if (hspeed_m_s < 0.0f) {
		hspeed_m_s = 0.0f;
	}
	value |= (uint32_t)(crsf_prep_number((int32_t)lroundf(hspeed_m_s * 10.0f), 2, 1) & 0xFF) << 9;
	value |= ((uint32_t)yaw & 0x7FF) << 17;

	/* bit 28 flags the horizontal speed as an airspeed, we only send ground speed */
	return value;
}

uint32_t crsf_yaapu_ap_status(uint8_t cerebri_mode, bool armed, bool failsafe, int16_t throttle_milli)
{
	int32_t throttle = clamp_i32(throttle_milli, 0, 1000);
	uint32_t value = (cerebri_mode <= 2 ? (uint32_t)cerebri_mode + 1u : 0u) & 0x1F;

	value |= (uint32_t)(armed ? 1 : 0) << 8;
	value |= (uint32_t)(failsafe ? 1 : 0) << 12;
	/* throttle percent scaled to [-63,63]: 6 bits of magnitude plus sign at 25 */
	value |= (uint32_t)crsf_prep_number((int32_t)lroundf(throttle * 0.1f * 0.63f), 2, 0) << 19;

	/* bits 26-31 hold the IMU temperature, which this build does not send */
	return value;
}

uint32_t crsf_yaapu_gps_status(uint8_t sats, uint8_t fix_type, uint16_t hdop_centi,
			       int32_t alt_msl_mm)
{
	uint32_t value = sats > 15u ? 15u : sats;

	value |= (uint32_t)(fix_type > 3u ? 3u : fix_type) << 4;
	/* the advanced fix sits at bit 14, so the hdop field is 8 bits and unsigned */
	value |= (uint32_t)(crsf_prep_number(hdop_centi / 10, 2, 1) & 0xFF) << 6; /* dm */
	value |= (uint32_t)(fix_type > 3u ? (fix_type - 3u > 3u ? 3u : fix_type - 3u) : 0u) << 14;
	value |= (uint32_t)crsf_prep_number(alt_msl_mm / 100, 2, 2) << 22; /* dm */
	return value;
}

uint32_t crsf_yaapu_battery(uint16_t voltage_cv, int16_t current_da, uint16_t consumed_mah)
{
	uint32_t volt_dv = voltage_cv / 10u;
	uint32_t value = volt_dv > 0x1FFu ? 0x1FFu : volt_dv;

	/*
	 * The consumed capacity starts at bit 17, so the current field is 8 bits
	 * and unsigned: a negative (charging) current is reported as zero rather
	 * than as its magnitude.
	 */
	if (current_da < 0) {
		current_da = 0;
	}
	value |= (uint32_t)(crsf_prep_number(current_da, 2, 1) & 0xFF) << 9;
	value |= (uint32_t)(consumed_mah > 0x7FFFu ? 0x7FFFu : consumed_mah) << 17;
	return value;
}

uint32_t crsf_yaapu_home(float east_m, float north_m, float up_m)
{
	/*
	 * Home is the ENU frame origin, so the bearing to home is the direction
	 * of the inverted position vector, in 3 degree steps from north.
	 */
	int32_t bearing = (int32_t)lroundf(wrap360(atan2f(-east_m, -north_m) * RAD_TO_DEG) / 3.0f);
	/* the altitude starts at bit 12, so the distance field is 12 bits and unsigned */
	uint32_t value = crsf_prep_number((int32_t)lroundf(hypotf(east_m, north_m)), 3, 2) & 0xFFF;

	/*
	 * The Lua decoder reads the altitude mantissa as 10 bits at offset 14
	 * with the exponent at 12 and the sign at 24, so this field is
	 * prep_number(dm, 3, 2), as the Yaapu home decoder expects.
	 */
	value |= (uint32_t)crsf_prep_number((int32_t)lroundf(up_m * 10.0f), 3, 2) << 12;
	value |= ((uint32_t)bearing & 0x7F) << 25;
	return value;
}

uint32_t crsf_yaapu_param(uint8_t id, uint32_t value)
{
	/* the Lua decoder reads the id as 4 bits at offset 24 */
	return ((uint32_t)(id & 0xF) << 24) | (value & 0xFFFFFF);
}

size_t crsf_encode_multi_packet(uint8_t *buf, size_t buf_len, const struct crsf_yaapu_packet *pk,
				size_t n)
{
	size_t len = 2u + 6u * n;

	if (buf == NULL || pk == NULL || n == 0u || n > CRSF_MULTI_PACKET_MAX || buf_len < len) {
		return 0;
	}

	buf[0] = CRSF_AP_CUSTOM_MULTI_PACKET;
	buf[1] = (uint8_t)n;
	for (size_t i = 0; i < n; i++) {
		uint8_t *p = &buf[2u + 6u * i];

		p[0] = (uint8_t)pk[i].appid;
		p[1] = (uint8_t)(pk[i].appid >> 8);
		p[2] = (uint8_t)pk[i].data;
		p[3] = (uint8_t)(pk[i].data >> 8);
		p[4] = (uint8_t)(pk[i].data >> 16);
		p[5] = (uint8_t)(pk[i].data >> 24);
	}
	return len;
}

size_t crsf_encode_status_text(uint8_t *buf, size_t buf_len, uint8_t severity, const char *text)
{
	size_t len;

	if (buf == NULL || text == NULL) {
		return 0;
	}

	len = str_len_max(text, CRSF_STATUS_TEXT_MAX);
	/* the Lua decoder requires at least 5 payload bytes, so 2 chars plus NUL */
	if (len < 2u || buf_len < len + 3u) {
		return 0;
	}

	buf[0] = CRSF_AP_CUSTOM_STATUS_TEXT;
	buf[1] = severity;
	memcpy(&buf[2], text, len);
	buf[2 + len] = '\0';
	return len + 3u;
}

size_t crsf_encode_gps(uint8_t *buf, size_t buf_len, int32_t lat_e7, int32_t lon_e7,
		       uint16_t ground_speed_cm_s, uint16_t course_cdeg, int32_t alt_msl_mm,
		       uint8_t sats)
{
	if (buf == NULL || buf_len < 15u) {
		return 0;
	}

	store_be32(&buf[0], (uint32_t)lat_e7);
	store_be32(&buf[4], (uint32_t)lon_e7);
	store_be16(&buf[8], (uint16_t)((uint32_t)ground_speed_cm_s * 36u / 100u)); /* km/h x 10 */
	store_be16(&buf[10], course_cdeg);
	store_be16(&buf[12], (uint16_t)(clamp_i32(alt_msl_mm / 1000, 0, 5000) + 1000));
	buf[14] = sats;
	return 15u;
}

size_t crsf_encode_battery(uint8_t *buf, size_t buf_len, uint16_t voltage_cv, int16_t current_da,
			   uint32_t consumed_mah, uint8_t remaining_pct)
{
	if (buf == NULL || buf_len < 8u) {
		return 0;
	}

	store_be16(&buf[0], (uint16_t)(voltage_cv / 10u));
	store_be16(&buf[2], (uint16_t)current_da);
	if (consumed_mah > 0xFFFFFFu) {
		consumed_mah = 0xFFFFFFu;
	}
	buf[4] = (uint8_t)(consumed_mah >> 16);
	buf[5] = (uint8_t)(consumed_mah >> 8);
	buf[6] = (uint8_t)consumed_mah;
	buf[7] = remaining_pct;
	return 8u;
}

size_t crsf_encode_rpm(uint8_t *buf, size_t buf_len, uint8_t source_id, const int32_t *rpm,
		       size_t count)
{
	size_t i;

	if (buf == NULL || rpm == NULL || count == 0u || buf_len < 1u + 3u * count) {
		return 0;
	}

	buf[0] = source_id;
	for (i = 0; i < count; i++) {
		uint32_t v = (uint32_t)clamp_i32(rpm[i], -8388608, 8388607);

		buf[1u + i * 3u] = (uint8_t)(v >> 16);
		buf[2u + i * 3u] = (uint8_t)(v >> 8);
		buf[3u + i * 3u] = (uint8_t)v;
	}
	return 1u + 3u * count;
}

size_t crsf_encode_flight_mode(uint8_t *buf, size_t buf_len, const char *name)
{
	size_t len;

	if (buf == NULL || name == NULL || buf_len == 0u) {
		return 0;
	}

	len = str_len_max(name, buf_len);
	if (len + 1u > buf_len) {
		return 0;
	}
	memcpy(buf, name, len);
	buf[len] = '\0';
	return len + 1u;
}

size_t crsf_encode_bind_command(uint8_t *buf, size_t buf_len)
{
	/* Type byte included in the command CRC but not in the payload. */
	static const uint8_t crc_input[] = {0x32, 0xEC, 0xC8, 0x10, 0x01};
	uint8_t crc = 0;
	size_t i;
	int bit;

	if (buf == NULL || buf_len < CRSF_BIND_COMMAND_LEN) {
		return 0;
	}

	for (i = 0; i < sizeof(crc_input); i++) {
		crc ^= crc_input[i];
		for (bit = 0; bit < 8; bit++) {
			crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0xBAu) : (uint8_t)(crc << 1);
		}
	}

	memcpy(buf, &crc_input[1], sizeof(crc_input) - 1u);
	buf[4] = crc;
	return CRSF_BIND_COMMAND_LEN;
}
