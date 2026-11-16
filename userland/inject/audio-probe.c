/*
 * darwin-lisa: what audiomxd's virtual audio device decides from, said on the console (libdlisa in audiomxd; nothing is
 * changed in it). VirtualAudio picks its device settings and routing handlers from MobileGestalt's product type
 * (PlatformUtilities_iOS::GetProductType, then IsAcousticIDHandled) and its tunings from
 * /Library/Audio/Tunings/AID<AcousticID>; DeviceSupportsClosedLoopHaptics decides whether it wants an "Actuator".
 * Two seconds after audiomxd starts (MobileGestalt answers from its cache by then). The rest of its reasoning comes
 * with boot-arg dlisa_oslog=audiomxd (VirtualAudio and the HAL log through os_log).
 */
static void
sayAudioIdentity(void)
{
	void *mg = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_NOW | RTLD_NOLOAD);
	uint64_t (*productType)(void) = mg != NULL ? (uint64_t (*)(void))dlsym(mg, "MGGetProductType") : NULL;
	int32_t (*sint32)(CFStringRef, int32_t) = mg != NULL ? (int32_t (*)(CFStringRef, int32_t))dlsym(mg, "MGGetSInt32Answer") :
	    NULL;
	bool (*boolean)(CFStringRef) = mg != NULL ? (bool (*)(CFStringRef))dlsym(mg, "MGGetBoolAnswer") : NULL;
	CFTypeRef (*copyAnswer)(CFStringRef, void *) = mg != NULL ? (CFTypeRef (*)(CFStringRef, void *))dlsym(mg, "MGCopyAnswer") :
	    NULL;
	char model[64] = "?";

	if (productType == NULL || sint32 == NULL || boolean == NULL || copyAnswer == NULL) {
		say("dlisa[%s]: audio: libMobileGestalt not loaded here (%s)\n", getprogname(), dlerror());
		return;
	}
	{
		CFTypeRef answer = copyAnswer(CFSTR("ProductType"), NULL);

		if (answer != NULL && CFGetTypeID(answer) == CFStringGetTypeID()) {
			CFStringGetCString(answer, model, sizeof(model), kCFStringEncodingUTF8);
		}
		if (answer != NULL) {
			CFRelease(answer);
		}
	}
	say("dlisa[%s]: audio: MobileGestalt product type %#llx (%s), AcousticID %d, closed-loop haptics %d; HAL plug-ins: "
	    "VirtualAudio %s, DLisaAudio %s\n", getprogname(), (unsigned long long)productType(), model,
	    sint32(CFSTR("AcousticID"), -1), boolean(CFSTR("DeviceSupportsClosedLoopHaptics")),
	    access("/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin/VirtualAudio", R_OK) == 0 ? "present" : "MISSING",
	    access("/Library/Audio/Plug-Ins/HAL/DLisaAudio.driver/DLisaAudio", R_OK) == 0 ? "present" : "MISSING");
}

static void
fixAudiomxd(void)
{
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), ^{
		sayAudioIdentity();
	});
}

/* ---- dlisa_sndtest (sound-test.c), in SpringBoard ---------------------------------------------------------------- */

typedef uint32_t SystemSoundIDForTest;

static void
playSoundTestStep(const struct soundTestStep *step)
{
	void *toolbox = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_NOW);
	int32_t (*create)(CFURLRef, SystemSoundIDForTest *) = toolbox != NULL ?
	    (int32_t (*)(CFURLRef, SystemSoundIDForTest *))dlsym(toolbox, "AudioServicesCreateSystemSoundID") : NULL;
	void (*play)(SystemSoundIDForTest, void (^)(void)) = toolbox != NULL ?
	    (void (*)(SystemSoundIDForTest, void (^)(void)))dlsym(toolbox, "AudioServicesPlaySystemSoundWithCompletion") : NULL;
	char path[128];
	CFURLRef url;
	SystemSoundIDForTest sound = 0;
	int32_t status;
	uint64_t began = mach_absolute_time();

	soundTestPath(step->sound, path, sizeof(path));
	if (create == NULL || play == NULL) {
		say("dlisa[%s]: sndtest: AudioToolbox not usable here (%s)\n", getprogname(), dlerror());
		return;
	}
	url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
	status = url != NULL ? create(url, &sound) : -1;
	if (url != NULL) {
		CFRelease(url);
	}
	if (status != 0) {
		say("dlisa[%s]: sndtest: %s: AudioServicesCreateSystemSoundID: %d%s\n", getprogname(), path, status,
		    access(path, R_OK) == 0 ? "" : " (no such file)");
		return;
	}
	say("dlisa[%s]: sndtest: playing %s (system sound %u)\n", getprogname(), path, sound);
	play(sound, ^{
		mach_timebase_info_data_t base;

		mach_timebase_info(&base);
		say("dlisa[%s]: sndtest: %s done after %llu ms (the system sound server's completion)\n", getprogname(), step->sound,
		    (mach_absolute_time() - began) * base.numer / base.denom / 1000000ull);
	});
}

static void
startSoundTest(void)
{
	static struct soundTestStep steps[SOUND_TEST_STEPS];
	char arguments[1024];
	size_t size = sizeof(arguments) - 1;
	unsigned n;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	arguments[size] = 0;
	n = parseSoundTest(arguments, steps);
	for (unsigned i = 0; i < n; i++) {
		const struct soundTestStep *step = &steps[i];

		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)step->seconds * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
			playSoundTestStep(step);
		});
	}
	if (n > 0) {
		say("dlisa[%s]: sndtest: %u sound(s) to play, the first %u s from now\n", getprogname(), n, steps[0].seconds);
	}
}
