/*
 * The kernel's sensor values (kern.hakios.sensors: Android's axes and units, kernel/drivers/adsp/dlisa.ssc.c) turned
 * into what Apple's HID sensor events carry. Pure: no IOKit, host-tested (test/sensor-events-test.c).
 *
 * Axes: Android and Apple both put x to the right, y to the top and z out of the screen. They differ in the
 * accelerometer's sign and unit: Android reports the reaction to gravity in m/s^2 (+9.8 on the axis that points up),
 * Apple's accelerometer events carry acceleration in g with gravity as it pulls (-1 on the axis that points up:
 * UIAccelerometer reads y = -1 with the phone upright). The gyroscope (rad/s, right-handed) and the magnetometer
 * (microtesla, the field's own direction) are the same in both.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef SENSOR_IDS_DEFINED
#define SENSOR_IDS_DEFINED
enum { SENSOR_ACCEL, SENSOR_GYRO, SENSOR_MAG, SENSOR_LIGHT, SENSOR_PROXIMITY, SENSORS };
#endif

#define STANDARD_GRAVITY        9.80665
#define LUX_MAX                 1000000L
#define PROXIMITY_UNKNOWN       (-1)

/*
 * backboardd's BKHIDProximityEventProcessor counts an object as near when the event's detection mask has 0x400, or
 * 0x40 on the older path (it picks one of the two; both are set here). 0x40 is kIOHIDProximityDetectionReceiverCrude.
 */
#define PROXIMITY_NEAR_MASK     0x440u

struct sensorVector {
	double x, y, z;
};

static struct sensorVector
appleAcceleration(float x, float y, float z)
{
	return (struct sensorVector){ -x / STANDARD_GRAVITY, -y / STANDARD_GRAVITY, -z / STANDARD_GRAVITY };
}

static struct sensorVector
appleRotation(float x, float y, float z)
{
	return (struct sensorVector){ x, y, z };
}

static struct sensorVector
appleField(float x, float y, float z)
{
	return (struct sensorVector){ x, y, z };
}

/* The ambient light event's integer level (kIOHIDEventFieldAmbientLightSensorLevel): whole lux, never negative. */
static long
luxLevel(float lux)
{
	if (!(lux > 0.0f)) {
		return 0;
	}
	if (lux >= (float)LUX_MAX) {
		return LUX_MAX;
	}
	return lround(lux);
}

/* The ambient light event's illuminance (0xc000b, a double): lux with its fraction, within 0..LUX_MAX. */
static double
luxIlluminance(float lux)
{
	if (!(lux > 0.0f)) {
		return 0;
	}
	return lux >= (float)LUX_MAX ? (double)LUX_MAX : (double)lux;
}

static uint32_t
proximityMask(int near)
{
	return near == 1 ? PROXIMITY_NEAR_MASK : 0;
}

/* True when `near` (1, 0, or -1 not known) is a known state different from the last one sent; then it is the last. */
static bool
proximityChanged(int near, int *last)
{
	if (near == PROXIMITY_UNKNOWN || near == *last) {
		return false;
	}
	*last = near;
	return true;
}

/* Bit n set: sensor n has an event count other than the one seen last; `seen` takes the new counts. */
static uint32_t
takeNewSamples(const uint32_t count[SENSORS], uint32_t seen[SENSORS])
{
	uint32_t fresh = 0;

	for (unsigned i = 0; i < SENSORS; i++) {
		if (count[i] != seen[i]) {
			fresh |= 1u << i;
			seen[i] = count[i];
		}
	}
	return fresh;
}

/* NAME=NUMBER among the space-separated boot arguments; `fallback` when it is not there or has no value. */
static long
bootArgValue(const char *args, const char *name, long fallback)
{
	size_t length = strlen(name);

	for (const char *at = args; at != NULL && (at = strstr(at, name)) != NULL; at += length) {
		if ((at == args || at[-1] == ' ') && at[length] == '=') {
			return strtol(at + length + 1, NULL, 0);
		}
	}
	return fallback;
}
