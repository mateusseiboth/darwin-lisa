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
