/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Self-check for the CRSF telemetry encoder. Every builder output is decoded
 * again with the formulas of the Yaapu Lua widget (bit32.extract based) and
 * compared against the inputs.
 *
 * Builds both as a Zephyr test (native_sim) and standalone:
 *   cc -std=c99 -Wall -Wextra -I subsys/crsf_telemetry \
 *      tests/crsf_telemetry_encode/src/main.c \
 *      subsys/crsf_telemetry/crsf_telemetry_encode.c -lm
 *
 * Output format, kept stable so a Lua harness can replay the frames:
 *   - a line starting with '#' comments the inputs of the frame that follows
 *   - a frame line is "TYPE <hh> PAYLOAD <hh> <hh> ...": the CRSF frame type
 *     byte, then the payload bytes, lowercase hex, space separated
 *   - the last line is "PASS" once every check succeeded
 */

#include "crsf_telemetry_encode.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                                                \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                     \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

#define CHECK_NEAR(got, want, tol)                                                                 \
	do {                                                                                       \
		double g_ = (double)(got), w_ = (double)(want);                                    \
		if (fabs(g_ - w_) > (double)(tol)) {                                               \
			printf("FAIL %s:%d: %s = %g, want %g +/- %g\n", __FILE__, __LINE__, #got,  \
			       g_, w_, (double)(tol));                                             \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

#define DEG_TO_RAD 0.01745329251994329577f

/* bit32.extract(value, offset, length) */
static uint32_t ex(uint32_t v, int off, int len)
{
	return (v >> off) & (len >= 32 ? 0xFFFFFFFFu : ((1u << len) - 1u));
}

/* decode a prep_number(x, 2, 1) field: 7 digit bits, 1 exponent bit, 1 sign bit */
static int32_t dec21(uint32_t field)
{
	int32_t mag = (int32_t)(ex(field, 1, 7) * (ex(field, 0, 1) ? 10u : 1u));

	return ex(field, 8, 1) ? -mag : mag;
}

/* decode a prep_number(x, 3, 2) field: 10 digit bits, 2 exponent bits, 1 sign bit */
static int32_t dec32(uint32_t field)
{
	static const int32_t pow10[4] = {1, 10, 100, 1000};
	int32_t mag = (int32_t)ex(field, 2, 10) * pow10[ex(field, 0, 2)];

	return ex(field, 12, 1) ? -mag : mag;
}

static void check_prep_number(void)
{
	/* (2,1): exponent boundaries and saturation at 0x7F x 10 */
	CHECK(crsf_prep_number(99, 2, 1) == (99u << 1));
	CHECK(crsf_prep_number(100, 2, 1) == ((10u << 1) | 1u));
	CHECK(dec21(crsf_prep_number(127, 2, 1)) == 130);
	CHECK(dec21(crsf_prep_number(128, 2, 1)) == 130);
	CHECK(crsf_prep_number(1270, 2, 1) == 0xFF);
	CHECK(dec21(crsf_prep_number(1270, 2, 1)) == 1270);
	CHECK(dec21(crsf_prep_number(1280, 2, 1)) == 1270);
	CHECK(dec21(crsf_prep_number(-99, 2, 1)) == -99);
	CHECK(dec21(crsf_prep_number(-127, 2, 1)) == -130);
	CHECK(dec21(crsf_prep_number(-1280, 2, 1)) == -1270);

	/* (2,0): 7 bits unsigned, 6 bits plus sign at bit 6 when negative */
	CHECK(crsf_prep_number(63, 2, 0) == 63u);
	CHECK(crsf_prep_number(127, 2, 0) == 127u);
	CHECK(crsf_prep_number(200, 2, 0) == 127u);
	CHECK(crsf_prep_number(-63, 2, 0) == (63u | (1u << 6)));
	CHECK(crsf_prep_number(-100, 2, 0) == (63u | (1u << 6)));

	/* (3,2) */
	CHECK(dec32(crsf_prep_number(999, 3, 2)) == 999);
	CHECK(dec32(crsf_prep_number(1000, 3, 2)) == 1000);
	CHECK(dec32(crsf_prep_number(-1000, 3, 2)) == -1000);
	CHECK(dec32(crsf_prep_number(12345, 3, 2)) == 12300);

	/* unsupported combination */
	CHECK(crsf_prep_number(5, 4, 0) == 0);
}

static void check_rollpitch(void)
{
	static const float roll_deg[] = {-179.9f, -90.0f, 0.0f, 45.0f, 179.9f};
	static const float pitch_deg[] = {-89.9f, -45.0f, 0.0f, 12.5f, 89.9f};

	for (size_t i = 0; i < sizeof(roll_deg) / sizeof(roll_deg[0]); i++) {
		for (size_t j = 0; j < sizeof(pitch_deg) / sizeof(pitch_deg[0]); j++) {
			uint32_t v = crsf_yaapu_rollpitch(roll_deg[i] * DEG_TO_RAD,
							  pitch_deg[j] * DEG_TO_RAD);
			uint32_t r = ex(v, 0, 11);
			uint32_t p = ex(v, 11, 10);

			CHECK(r <= 1800u && p <= 900u);
			CHECK_NEAR(((double)r - 900.0) * 0.2, roll_deg[i], 0.2);
			CHECK_NEAR(((double)p - 450.0) * 0.2, pitch_deg[j], 0.2);
			CHECK(ex(v, 21, 11) == 0u); /* no rangefinder */
		}
	}
}

static void check_velandyaw(void)
{
	uint32_t v = crsf_yaapu_velandyaw(-3.2f, 9.5f, -0.1f * DEG_TO_RAD);
	double yaw;

	CHECK(dec21(ex(v, 0, 9)) == -32);              /* dm/s down */
	CHECK(dec21(ex(v, 9, 8)) == 95);               /* dm/s ground speed, sign bit never set */
	yaw = (double)ex(v, 17, 11) * 0.2;
	CHECK_NEAR(yaw, 359.9, 0.2);                   /* -0.1 deg wraps to 359.9 */
	CHECK(ex(v, 28, 1) == 0u);                     /* ground speed, not airspeed */

	v = crsf_yaapu_velandyaw(0.0f, -4.0f, 90.0f * DEG_TO_RAD);
	CHECK(dec21(ex(v, 9, 8)) == 0);                /* negative ground speed clamped */
	CHECK_NEAR((double)ex(v, 17, 11) * 0.2, 90.0, 0.2);

	v = crsf_yaapu_velandyaw(12.0f, 25.0f, 359.95f * DEG_TO_RAD);
	CHECK(dec21(ex(v, 0, 9)) == 120);
	CHECK(dec21(ex(v, 9, 8)) == 250);
	CHECK(ex(v, 17, 11) <= 1800u);
}

static void check_ap_status(void)
{
	static const int16_t throttle[] = {0, 500, 1000};
	static const double expect[] = {0.0, 50.0, 100.0};
	uint32_t v;

	for (uint8_t mode = 0; mode < 5; mode++) {
		v = crsf_yaapu_ap_status(mode, false, false, 0);
		CHECK(ex(v, 0, 5) == (mode <= 2 ? (uint32_t)mode + 1u : 0u));
	}

	v = crsf_yaapu_ap_status(1, true, false, 0);
	CHECK(ex(v, 8, 1) == 1u && ex(v, 12, 1) == 0u);
	v = crsf_yaapu_ap_status(1, false, true, 0);
	CHECK(ex(v, 8, 1) == 0u && ex(v, 12, 1) == 1u);

	for (size_t i = 0; i < 3; i++) {
		double dec;

		v = crsf_yaapu_ap_status(2, true, false, throttle[i]);
		/* Lua: floor(0.5 + extract(19,6) * sign(extract(25,1)) * 1.58) */
		dec = floor(0.5 + (double)ex(v, 19, 6) * (ex(v, 25, 1) ? -1.0 : 1.0) * 1.58);
		CHECK_NEAR(dec, expect[i], 2.0);
		CHECK(ex(v, 26, 6) == 0u); /* no IMU temperature */
	}

	/* out of range throttle is clamped, not wrapped */
	CHECK(ex(crsf_yaapu_ap_status(0, false, false, -100), 19, 7) == 0u);
	CHECK(ex(crsf_yaapu_ap_status(0, false, false, 2000), 19, 7) == 63u);
}

static void check_gps_status(void)
{
	uint32_t v = crsf_yaapu_gps_status(20, 7, 150, 123456);

	CHECK(ex(v, 0, 4) == 15u);                  /* sats clamped to 15 */
	CHECK(ex(v, 4, 2) == 3u);                   /* fix 7 clamped to 3 */
	CHECK(ex(v, 14, 2) == 3u);                  /* advanced fix clamped to 3 */
	CHECK(dec21(ex(v, 6, 8)) == 15);            /* 1.50 -> 15 dm, 8 bit field */
	CHECK_NEAR((double)(int32_t)(ex(v, 24, 7) * (uint32_t)pow(10.0, ex(v, 22, 2))) *
			   (ex(v, 31, 1) ? -1.0 : 1.0),
		   1234.0, 100.0); /* dm */

	v = crsf_yaapu_gps_status(9, 2, 90, -5000);
	CHECK(ex(v, 0, 4) == 9u);
	CHECK(ex(v, 4, 2) == 2u && ex(v, 14, 2) == 0u);
	CHECK(dec21(ex(v, 6, 8)) == 9);
	CHECK(ex(v, 31, 1) == 1u); /* negative altitude */
	CHECK(ex(v, 24, 7) * (uint32_t)pow(10.0, ex(v, 22, 2)) == 50u);

	v = crsf_yaapu_gps_status(4, 4, 200, 0);
	CHECK(ex(v, 4, 2) == 3u && ex(v, 14, 2) == 1u); /* DGNSS */
}

static void check_battery(void)
{
	uint32_t v = crsf_yaapu_battery(1680, 125, 40000);

	CHECK(ex(v, 0, 9) == 168u);                 /* dV */
	CHECK(dec21(ex(v, 9, 8)) == 130);           /* 12.5 A -> 13 x 10 dA, 8 bit field */
	CHECK(ex(v, 17, 15) == 0x7FFFu);            /* mAh clamped */

	v = crsf_yaapu_battery(6000, -50, 1200);
	CHECK(ex(v, 0, 9) == 511u);                 /* dV clamped */
	CHECK(dec21(ex(v, 9, 8)) == 0);             /* negative current is not representable */
	CHECK(ex(v, 17, 15) == 1200u);
}

static void check_home(void)
{
	uint32_t v = crsf_yaapu_home(3.0f, 4.0f, 10.0f);
	double bearing;

	CHECK(dec32(ex(v, 0, 12)) == 5);            /* hypot(3,4) m, 12 bit field */
	CHECK_NEAR((double)dec32(ex(v, 12, 13)) * 0.1, 10.0, 0.1);
	bearing = (double)ex(v, 25, 7) * 3.0;
	CHECK_NEAR(bearing, 216.87, 3.0);           /* bearing back to the origin */

	v = crsf_yaapu_home(0.0f, 0.0f, -2.5f);
	CHECK(dec32(ex(v, 0, 12)) == 0);
	CHECK_NEAR((double)dec32(ex(v, 12, 13)) * 0.1, -2.5, 0.1);
	CHECK(ex(v, 24, 1) == 1u);                  /* negative altitude sign */

	v = crsf_yaapu_home(-10.0f, 0.0f, 0.0f);
	CHECK(dec32(ex(v, 0, 12)) == 10);
	CHECK_NEAR((double)ex(v, 25, 7) * 3.0, 90.0, 3.0); /* home is east of us */
}

static void check_param(void)
{
	uint32_t v = crsf_yaapu_param(CRSF_YAAPU_PARAM_FRAME_TYPE, CRSF_MAV_TYPE_QUADROTOR);

	CHECK(ex(v, 24, 4) == CRSF_YAAPU_PARAM_FRAME_TYPE);
	CHECK(ex(v, 0, 24) == CRSF_MAV_TYPE_QUADROTOR);
	CHECK(ex(crsf_yaapu_param(4, 0xFFFFFFFFu), 0, 24) == 0xFFFFFFu);
}

static void check_multi_packet(void)
{
	struct crsf_yaapu_packet pk[4] = {
		{CRSF_YAAPU_AP_STATUS_APPID, 0x12345678u},
		{CRSF_YAAPU_GPS_STATUS_APPID, 0x000000FFu},
		{CRSF_YAAPU_BATTERY_APPID, 0xDEADBEEFu},
		{CRSF_YAAPU_HOME_APPID, 0u},
	};
	uint8_t buf[CRSF_ENCODE_BUF_LEN];
	size_t len;

	for (size_t n = 1; n <= CRSF_MULTI_PACKET_MAX; n++) {
		len = crsf_encode_multi_packet(buf, sizeof(buf), pk, n);
		CHECK(len == 2u + 6u * n);
		CHECK(buf[0] == CRSF_AP_CUSTOM_MULTI_PACKET && buf[1] == n);
		for (size_t i = 0; i < n; i++) {
			const uint8_t *p = &buf[2 + 6 * i];
			uint32_t appid = ((uint32_t)p[1] << 8) | p[0];
			uint32_t data = ((uint32_t)p[5] << 24) | ((uint32_t)p[4] << 16) |
					((uint32_t)p[3] << 8) | p[2];

			CHECK(appid == pk[i].appid);
			CHECK(data == pk[i].data);
		}
	}

	CHECK(crsf_encode_multi_packet(buf, sizeof(buf), pk, 4) == 0);
	CHECK(crsf_encode_multi_packet(buf, sizeof(buf), pk, 0) == 0);
	CHECK(crsf_encode_multi_packet(buf, 7, pk, 1) == 0);
	CHECK(crsf_encode_multi_packet(buf, 8, pk, 1) == 8);
}

static void check_status_text(void)
{
	static const char long_text[] = "0123456789012345678901234567890123456789"
					"0123456789ABCDEF"; /* 56 chars */
	uint8_t buf[CRSF_ENCODE_BUF_LEN];
	size_t len;

	CHECK(crsf_encode_status_text(buf, sizeof(buf), CRSF_AP_SEVERITY_INFO, "") == 0);
	CHECK(crsf_encode_status_text(buf, sizeof(buf), CRSF_AP_SEVERITY_INFO, "a") == 0);
	CHECK(crsf_encode_status_text(buf, sizeof(buf), CRSF_AP_SEVERITY_INFO, NULL) == 0);
	CHECK(crsf_encode_status_text(buf, 4, CRSF_AP_SEVERITY_INFO, "ok") == 0);

	len = crsf_encode_status_text(buf, sizeof(buf), CRSF_AP_SEVERITY_CRITICAL, "ok");
	CHECK(len == 5u);
	CHECK(buf[0] == CRSF_AP_CUSTOM_STATUS_TEXT && buf[1] == CRSF_AP_SEVERITY_CRITICAL);
	CHECK(buf[2] == 'o' && buf[3] == 'k' && buf[4] == '\0');

	len = crsf_encode_status_text(buf, sizeof(buf), CRSF_AP_SEVERITY_INFO, long_text);
	CHECK(len == CRSF_STATUS_TEXT_MAX + 3u);
	CHECK(buf[len - 1] == '\0');
	CHECK(memcmp(&buf[2], long_text, CRSF_STATUS_TEXT_MAX) == 0);
}

static void check_gps_frame(void)
{
	uint8_t buf[CRSF_ENCODE_BUF_LEN];
	int32_t lat = -123456789;
	uint32_t ulat = (uint32_t)lat;

	CHECK(crsf_encode_gps(buf, 14, lat, 0, 0, 0, 0, 0) == 0);
	CHECK(crsf_encode_gps(buf, sizeof(buf), lat, 987654321, 1000, 27000, 123456, 11) == 15u);
	CHECK(buf[0] == (uint8_t)(ulat >> 24) && buf[1] == (uint8_t)(ulat >> 16) &&
	      buf[2] == (uint8_t)(ulat >> 8) && buf[3] == (uint8_t)ulat);
	CHECK(((uint32_t)buf[4] << 24 | (uint32_t)buf[5] << 16 | (uint32_t)buf[6] << 8 | buf[7]) ==
	      987654321u);
	CHECK((((uint32_t)buf[8] << 8) | buf[9]) == 360u);    /* 10 m/s -> 36.0 km/h */
	CHECK((((uint32_t)buf[10] << 8) | buf[11]) == 27000u); /* 270.00 deg */
	CHECK((((uint32_t)buf[12] << 8) | buf[13]) == 1123u);  /* 123 m + 1000 offset */
	CHECK(buf[14] == 11u);

	/* below the offset floor: negative altitude clamps to the 0 m code */
	CHECK(crsf_encode_gps(buf, sizeof(buf), 0, 0, 0, 0, -50000, 0) == 15u);
	CHECK((((uint32_t)buf[12] << 8) | buf[13]) == 1000u);
	/* above the ceiling */
	CHECK(crsf_encode_gps(buf, sizeof(buf), 0, 0, 0, 0, 9000000, 0) == 15u);
	CHECK((((uint32_t)buf[12] << 8) | buf[13]) == 6000u);
}

static void check_battery_frame(void)
{
	uint8_t buf[CRSF_ENCODE_BUF_LEN];

	CHECK(crsf_encode_battery(buf, 7, 0, 0, 0, 0) == 0);
	CHECK(crsf_encode_battery(buf, sizeof(buf), 1680, -125, 0x123456, 77) == 8u);
	CHECK((((uint32_t)buf[0] << 8) | buf[1]) == 168u);
	CHECK((int16_t)(((uint16_t)buf[2] << 8) | buf[3]) == -125);
	CHECK(buf[4] == 0x12 && buf[5] == 0x34 && buf[6] == 0x56);
	CHECK(buf[7] == 77u);

	CHECK(crsf_encode_battery(buf, sizeof(buf), 0, 0, 0x1000000, 0) == 8u);
	CHECK(buf[4] == 0xFF && buf[5] == 0xFF && buf[6] == 0xFF);
}

static void check_flight_mode(void)
{
	uint8_t buf[CRSF_ENCODE_BUF_LEN];

	CHECK(crsf_encode_flight_mode(buf, sizeof(buf), "Manual") == 7u);
	CHECK(memcmp(buf, "Manual", 7) == 0);
	CHECK(crsf_encode_flight_mode(buf, 6, "Manual") == 0);
	CHECK(crsf_encode_flight_mode(buf, 7, "Manual") == 7u);
	CHECK(crsf_encode_flight_mode(buf, sizeof(buf), NULL) == 0);
}

static void check_bind_command(void)
{
	/* Wire frame: sync, length, type, payload, frame CRC (poly 0xD5). */
	static const uint8_t expect[] = {0xC8, 0x07, 0x32, 0xEC, 0xC8, 0x10, 0x01, 0x9E, 0xE8};
	uint8_t frame[sizeof(expect)];
	uint8_t crc = 0;
	size_t i;
	int bit;

	CHECK(crsf_encode_bind_command(&frame[3], 4) == 0);
	CHECK(crsf_encode_bind_command(&frame[3], sizeof(frame) - 3u) == CRSF_BIND_COMMAND_LEN);

	frame[0] = 0xC8;
	frame[1] = CRSF_BIND_COMMAND_LEN + 2u;
	frame[2] = 0x32;
	for (i = 2; i < sizeof(frame) - 1u; i++) {
		crc ^= frame[i];
		for (bit = 0; bit < 8; bit++) {
			crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0xD5u) : (uint8_t)(crc << 1);
		}
	}
	frame[sizeof(frame) - 1u] = crc;

	CHECK(memcmp(frame, expect, sizeof(expect)) == 0);
}

static void check_euler(void)
{
	float roll, pitch, yaw;
	const float s = 0.70710678f;

	crsf_quat_to_euler321(1.0f, 0.0f, 0.0f, 0.0f, &roll, &pitch, &yaw);
	CHECK_NEAR(roll, 0.0, 1e-5);
	CHECK_NEAR(pitch, 0.0, 1e-5);
	CHECK_NEAR(yaw, 0.0, 1e-5);

	crsf_quat_to_euler321(s, s, 0.0f, 0.0f, &roll, &pitch, &yaw); /* roll +90 */
	CHECK_NEAR(roll * (1.0 / DEG_TO_RAD), 90.0, 0.01);
	CHECK_NEAR(pitch, 0.0, 1e-4);
	CHECK_NEAR(yaw, 0.0, 1e-4);

	crsf_quat_to_euler321(s, 0.0f, s, 0.0f, &roll, &pitch, &yaw); /* pitch +90 */
	/* asinf loses precision next to the gimbal lock point, hence the wider bound */
	CHECK_NEAR(pitch * (1.0 / DEG_TO_RAD), 90.0, 0.05);

	crsf_quat_to_euler321(s, 0.0f, 0.0f, s, &roll, &pitch, &yaw); /* yaw +90 */
	CHECK_NEAR(yaw * (1.0 / DEG_TO_RAD), 90.0, 0.01);
	CHECK_NEAR(roll, 0.0, 1e-4);
	CHECK_NEAR(pitch, 0.0, 1e-4);

	/* nose down 30 deg, decoded through the 0x5006 word */
	{
		float half = -15.0f * DEG_TO_RAD;
		uint32_t v;

		crsf_quat_to_euler321(cosf(half), 0.0f, sinf(half), 0.0f, &roll, &pitch, &yaw);
		v = crsf_yaapu_rollpitch(roll, pitch);
		CHECK_NEAR(((double)ex(v, 11, 10) - 450.0) * 0.2, -30.0, 0.2);
	}
}

static void print_frame(const char *comment, uint8_t type, const uint8_t *payload, size_t len)
{
	printf("# %s\n", comment);
	printf("TYPE %02x PAYLOAD", type);
	for (size_t i = 0; i < len; i++) {
		printf(" %02x", payload[i]);
	}
	printf("\n");
}

/* Build one frame of every kind with known inputs, for replay by a Lua harness. */
static void print_frames(void)
{
	uint8_t buf[CRSF_ENCODE_BUF_LEN];
	struct crsf_yaapu_packet pk[CRSF_MULTI_PACKET_MAX];
	size_t len;

	pk[0].appid = CRSF_YAAPU_ROLLPITCH_APPID;
	pk[0].data = crsf_yaapu_rollpitch(-12.5f * DEG_TO_RAD, 7.5f * DEG_TO_RAD);
	pk[1].appid = CRSF_YAAPU_VELANDYAW_APPID;
	pk[1].data = crsf_yaapu_velandyaw(-3.2f, 9.5f, -0.1f * DEG_TO_RAD);
	pk[2].appid = CRSF_YAAPU_AP_STATUS_APPID;
	pk[2].data = crsf_yaapu_ap_status(1, true, false, 500);
	len = crsf_encode_multi_packet(buf, sizeof(buf), pk, 3);
	print_frame("0x5006 roll=-12.5 pitch=7.5 deg; 0x5005 vspeed=-3.2 hspeed=9.5 m/s yaw=-0.1 deg; "
		    "0x5001 mode=1 armed=1 failsafe=0 throttle=500 milli",
		    0x80, buf, len);

	pk[0].appid = CRSF_YAAPU_GPS_STATUS_APPID;
	pk[0].data = crsf_yaapu_gps_status(11, 3, 150, 123456);
	pk[1].appid = CRSF_YAAPU_BATTERY_APPID;
	pk[1].data = crsf_yaapu_battery(1680, 125, 1200);
	pk[2].appid = CRSF_YAAPU_HOME_APPID;
	pk[2].data = crsf_yaapu_home(3.0f, 4.0f, 10.0f);
	len = crsf_encode_multi_packet(buf, sizeof(buf), pk, 3);
	print_frame("0x5002 sats=11 fix=3 hdop=1.50 alt=123456 mm; 0x5003 16.80 V 12.5 A 1200 mAh; "
		    "0x5004 east=3 north=4 up=10 m",
		    0x80, buf, len);

	pk[0].appid = CRSF_YAAPU_PARAMS_APPID;
	pk[0].data = crsf_yaapu_param(CRSF_YAAPU_PARAM_FRAME_TYPE, CRSF_MAV_TYPE_QUADROTOR);
	len = crsf_encode_multi_packet(buf, sizeof(buf), pk, 1);
	print_frame("0x5007 param id=1 (frame type) value=2 (quadrotor)", 0x80, buf, len);

	len = crsf_encode_status_text(buf, sizeof(buf), CRSF_AP_SEVERITY_INFO, "cerebri ready");
	print_frame("status text severity=6 text=\"cerebri ready\"", 0x80, buf, len);

	len = crsf_encode_gps(buf, sizeof(buf), -123456789, 987654321, 1000, 27000, 123456, 11);
	print_frame("gps lat=-12.3456789 lon=98.7654321 deg speed=1000 cm/s course=270.00 deg "
		    "alt=123456 mm sats=11",
		    0x02, buf, len);

	len = crsf_encode_battery(buf, sizeof(buf), 1680, 125, 1200, 77);
	print_frame("battery 16.80 V 12.5 A 1200 mAh 77 percent", 0x08, buf, len);

	len = crsf_encode_flight_mode(buf, sizeof(buf), "MANUAL");
	print_frame("flight mode \"MANUAL\"", 0x21, buf, len);
}

int main(void)
{
	check_prep_number();
	check_rollpitch();
	check_velandyaw();
	check_ap_status();
	check_gps_status();
	check_battery();
	check_home();
	check_param();
	check_multi_packet();
	check_status_text();
	check_gps_frame();
	check_battery_frame();
	check_flight_mode();
	check_bind_command();
	check_euler();

	print_frames();
	printf("PASS\n");
	return 0;
}
