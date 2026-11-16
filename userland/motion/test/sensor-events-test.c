/* Host test of sensor-events.c (libdlisa-motion: the kernel's sensor values turned into what Apple's HID events carry) */
#include <math.h>
#include <stdio.h>

#include "sensor-events.c"

static int failures;

static void
expect(int ok, const char *what)
{
	if (!ok) {
		printf("FAIL: %s\n", what);
		failures++;
	}
}

static int
near(double a, double b)
{
	return fabs(a - b) < 1e-6;
}

static void
testAcceleration(void)
{
	/* Android: +g on the axis that points up. Apple: the same axes, -1 g on the axis that points up. */
	struct sensorVector upright = appleAcceleration(0.0f, 9.80665f, 0.0f);
	struct sensorVector flat = appleAcceleration(0.0f, 0.0f, 9.80665f);
	struct sensorVector left = appleAcceleration(4.903325f, 0.0f, 0.0f);

	expect(near(upright.x, 0) && near(upright.y, -1) && near(upright.z, 0), "upright is y = -1 g");
	expect(near(flat.z, -1), "face up is z = -1 g");
	expect(near(left.x, -0.5), "half g on x turns sign and scale");
}

static void
testRotationAndField(void)
{
	struct sensorVector spin = appleRotation(0.5f, -0.25f, 1.0f);
	struct sensorVector field = appleField(20.0f, -5.0f, -40.0f);

	expect(near(spin.x, 0.5) && near(spin.y, -0.25) && near(spin.z, 1.0), "gyro keeps rad/s and signs");
	expect(near(field.x, 20) && near(field.y, -5) && near(field.z, -40), "compass keeps microtesla and signs");
}

static void
testLight(void)
{
	expect(luxLevel(0.0f) == 0, "0 lux");
	expect(luxLevel(123.4f) == 123, "rounds down below .5");
	expect(luxLevel(123.5f) == 124, "rounds up from .5");
	expect(luxLevel(-3.0f) == 0, "negative lux is 0");
	expect(luxLevel(NAN) == 0, "NaN lux is 0");
	expect(luxLevel(5e9f) == LUX_MAX, "huge lux is clamped");
	expect(near(luxIlluminance(12.25f), 12.25), "illuminance keeps the fraction");
	expect(near(luxIlluminance(-1.0f), 0) && near(luxIlluminance(NAN), 0), "illuminance below 0 or NaN is 0");
	expect(near(luxIlluminance(5e9f), LUX_MAX), "huge illuminance is clamped");
}

static void
testProximity(void)
{
	int last = PROXIMITY_UNKNOWN;

	expect(proximityMask(1) == 0x440, "near: receiver bits backboardd tests (0x400 | 0x40)");
	expect(proximityMask(0) == 0, "far: no bit");
	expect(proximityMask(-1) == 0, "not known: no bit");
	expect(!proximityChanged(-1, &last), "not known is never sent");
	expect(proximityChanged(0, &last) && last == 0, "first far is sent");
	expect(!proximityChanged(0, &last), "far again is not");
	expect(proximityChanged(1, &last) && last == 1, "near is sent");
	expect(!proximityChanged(-1, &last) && last == 1, "losing the value keeps the last one");
}

static void
testNewSamples(void)
{
	uint32_t seen[SENSORS] = { 0 };
	uint32_t count[SENSORS] = { 3, 0, 0, 1, 0 };

	expect(takeNewSamples(count, seen) == ((1u << SENSOR_ACCEL) | (1u << SENSOR_LIGHT)), "accel and light are new");
	expect(takeNewSamples(count, seen) == 0, "nothing new the second time");
	count[SENSOR_GYRO] = 7;
	count[SENSOR_ACCEL] = 2;        /* a counter that went back (kernel restarted the client) is new too */
	expect(takeNewSamples(count, seen) == ((1u << SENSOR_ACCEL) | (1u << SENSOR_GYRO)), "gyro and a reset accel");
}

static void
testBootArgs(void)
{
	const char *args = "debug=0x8 sensors=1 sensors_hid=0 xsensors_hid=5 sensors_hid_rate=50";

	expect(bootArgValue(args, "sensors_hid", 1) == 0, "sensors_hid=0 found");
	expect(bootArgValue(args, "sensors_hid_rate", 25) == 50, "a longer name is its own argument");
	expect(bootArgValue(args, "missing", 7) == 7, "missing gives the fallback");
	expect(bootArgValue("sensors_hid", "sensors_hid", 3) == 3, "a name without a value gives the fallback");
	expect(bootArgValue(NULL, "sensors_hid", 4) == 4, "no arguments give the fallback");
}

int
main(void)
{
	testAcceleration();
	testRotationAndField();
	testLight();
	testProximity();
	testNewSamples();
	testBootArgs();
	printf("%d failures\n", failures);
	return failures != 0;
}
