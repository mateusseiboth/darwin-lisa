/*
 * fbtest: draws a picture through iOS 26's own display stack -- IOSurface.framework for the pixels, CoreGraphics
 * (and CoreText, when it can) to draw them, IOMobileFramebuffer.framework to put the surface on the screen -- into
 * the kernel's stand-ins (darwin-lisa kernel/drivers/display: MiniIOSurface, MiniIOMobileFramebuffer).
 *
 * It is what the window server does, without the window server: a check of the display path on its own, and a
 * picture on the panel while the rest of the system is still being brought up. Run it before launchd with
 * init26's boot-arg init_run=/usr/local/bin/fbtest (or by hand). It keeps the picture on screen for FBTEST_SECS
 * seconds (default 8, a swap a second), then exits; see main() for the arguments.
 *
 * arm64 (not arm64e), like any App Store program: dyld runs it against the arm64e shared cache. IOMobileFramebuffer
 * is a private framework the SDK has no stub for, so it is reached with dlopen/dlsym.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dlfcn.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

typedef void *IOMobileFramebufferRef;

static int (*GetMainDisplay)(IOMobileFramebufferRef *);
static int (*GetDisplaySize)(IOMobileFramebufferRef, CGSize *);
static int (*SwapBegin)(IOMobileFramebufferRef, int *);
static int (*SwapSetLayer)(IOMobileFramebufferRef, int, IOSurfaceRef, CGRect, CGRect, int);
static int (*SwapEnd)(IOMobileFramebufferRef);

static void
number(CFMutableDictionaryRef d, CFStringRef key, int32_t value)
{
	CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt32Type, &value);

	CFDictionarySetValue(d, key, n);
	CFRelease(n);
}

static void
text(CGContextRef cg, const char *string, CGFloat size, CGFloat x, CGFloat y)
{
	CFStringRef s = CFStringCreateWithCString(NULL, string, kCFStringEncodingUTF8);
	CTFontRef font = CTFontCreateWithName(CFSTR("Helvetica-Bold"), size, NULL);
	CGColorRef white;
	CFStringRef keys[2];
	CFTypeRef values[2];
	CFDictionaryRef attributes;
	CFAttributedStringRef attributed;
	CTLineRef line;
	CGFloat components[4] = { 1, 1, 1, 1 };
	CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();

	if (font == NULL) {
		printf("fbtest: no font, no text\n");
		CFRelease(s);
		return;
	}
	white = CGColorCreate(space, components);
	keys[0] = kCTFontAttributeName;
	values[0] = font;
	keys[1] = kCTForegroundColorAttributeName;
	values[1] = white;
	attributes = CFDictionaryCreate(NULL, (const void **)keys, (const void **)values, 2, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	attributed = CFAttributedStringCreate(NULL, s, attributes);
	line = CTLineCreateWithAttributedString(attributed);
	CGContextSetTextPosition(cg, x, y);
	CTLineDraw(line, cg);
	CFRelease(line);
	CFRelease(attributed);
	CFRelease(attributes);
	CFRelease(white);
	CFRelease(font);
	CFRelease(s);
	CGColorSpaceRelease(space);
}

/* the picture, drawn into plain memory: rows of BGRA, rowBytes apart */
static void
draw(uint8_t *pixels, int width, int height, int rowBytes)
{
	CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
	CGContextRef cg = CGBitmapContextCreate(pixels, width, height, 8, rowBytes, space,
	    kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
	static const CGFloat bands[6][3] = { { 1, 0.23, 0.19 }, { 1, 0.58, 0 }, { 1, 0.8, 0 }, { 0.2, 0.78, 0.35 },
		                             { 0, 0.48, 1 }, { 0.69, 0.32, 0.87 } };

	if (cg == NULL) {
		printf("fbtest: CGBitmapContextCreate failed\n");
		CGColorSpaceRelease(space);
		return;
	}
	/* a dark blue to violet gradient, a band per hue, and the words */
	for (int y = 0; y < height; y += 8) {
		CGFloat t = (CGFloat)y / height;

		CGContextSetRGBFillColor(cg, 0.10 + 0.35 * t, 0.12, 0.45 - 0.15 * t, 1);
		CGContextFillRect(cg, CGRectMake(0, y, width, 8));
	}
	for (int i = 0; i < 6; i++) {
		CGContextSetRGBFillColor(cg, bands[i][0], bands[i][1], bands[i][2], 1);
		CGContextFillRect(cg, CGRectMake(0, height / 2 - 300 + i * 60, width, 60));
	}
	CGContextSetRGBFillColor(cg, 1, 1, 1, 1);
	CGContextFillEllipseInRect(cg, CGRectMake(width / 2 - 150, height - 700, 300, 300));
	text(cg, "iOS 26.5.2", 120, 120, height / 2 + 300);
	text(cg, "on Xiaomi lisa (SM7325)", 64, 120, height / 2 + 160);
	text(cg, "IOSurface + IOMobileFramebuffer", 52, 120, 400);
	CGContextFlush(cg);
	CGContextRelease(cg);
	CGColorSpaceRelease(space);
}

/*
 * fbtest [SECONDS [bg]]: keep the picture up so long, a swap a second (default 8, or FBTEST_SECS). With "bg" the
 * program starts itself again without "bg" and returns at once (init_run=/usr/local/bin/fbtest:1200:bg), so the
 * picture is put back over whatever else draws on the panel memory (the loader's own text, while the host polls it).
 * The size is the display's as the kernel reports it by default (1125x2436).
 */
int
main(int argc, char **argv)
{
	IOMobileFramebufferRef fb = NULL;
	CGSize size = { 0, 0 };
	int ret, token = 0, width = 1125, height = 2436, rowBytes, secs = 8;
	const char *env = getenv("FBTEST_SECS");
	uint8_t *picture;
	void *iomfb;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (env != NULL) {
		secs = atoi(env);
	}
	if (argc > 1) {
		secs = atoi(argv[1]);
	}
	/* bg: the same program again, in the foreground of a process of its own, and this one returns (a fork without exec
	 * of a program that has CoreGraphics' threads hangs in the child's first dlopen) */
	if (argc > 2 && strcmp(argv[2], "bg") == 0) {
		char *child_argv[] = { argv[0], argv[1], NULL };
		pid_t child;
		int err = posix_spawn(&child, argv[0], NULL, NULL, child_argv, environ);

		printf("fbtest: swapping for %d s in pid %d (%d)\n", secs, err ? -1 : child, err);
		return err ? 1 : 0;
	}
	rowBytes = (width * 4 + 63) & ~63;
	picture = calloc((size_t)rowBytes, (size_t)height);
	if (picture == NULL) {
		return 1;
	}
	draw(picture, width, height, rowBytes);
	iomfb = dlopen("/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/IOMobileFramebuffer", RTLD_NOW);
	if (iomfb == NULL) {
		printf("fbtest: %s\n", dlerror());
		return 1;
	}
	GetMainDisplay = dlsym(iomfb, "IOMobileFramebufferGetMainDisplay");
	GetDisplaySize = dlsym(iomfb, "IOMobileFramebufferGetDisplaySize");
	SwapBegin = dlsym(iomfb, "IOMobileFramebufferSwapBegin");
	SwapSetLayer = dlsym(iomfb, "IOMobileFramebufferSwapSetLayer");
	SwapEnd = dlsym(iomfb, "IOMobileFramebufferSwapEnd");
	if (!GetMainDisplay || !GetDisplaySize || !SwapBegin || !SwapSetLayer || !SwapEnd) {
		printf("fbtest: IOMobileFramebuffer lacks a function\n");
		return 1;
	}
	ret = GetMainDisplay(&fb);
	printf("fbtest: IOMobileFramebufferGetMainDisplay -> 0x%x, %p\n", ret, fb);
	if (ret != 0 || fb == NULL) {
		return 1;
	}
	ret = GetDisplaySize(fb, &size);
	printf("fbtest: display size -> 0x%x, %.0fx%.0f\n", ret, size.width, size.height);

	CFMutableDictionaryRef props = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	number(props, kIOSurfaceWidth, width);
	number(props, kIOSurfaceHeight, height);
	number(props, kIOSurfaceBytesPerElement, 4);
	number(props, kIOSurfaceBytesPerRow, rowBytes);
	number(props, kIOSurfaceAllocSize, rowBytes * height);
	number(props, kIOSurfacePixelFormat, 'BGRA');
	IOSurfaceRef surface = IOSurfaceCreate(props);
	CFRelease(props);
	if (surface == NULL) {
		printf("fbtest: IOSurfaceCreate failed\n");
		return 1;
	}
	printf("fbtest: surface %u: %zux%zu, %zu bytes per row, base %p\n", IOSurfaceGetID(surface), IOSurfaceGetWidth(surface),
	    IOSurfaceGetHeight(surface), IOSurfaceGetBytesPerRow(surface), IOSurfaceGetBaseAddress(surface));
	IOSurfaceLock(surface, 0, NULL);
	for (int y = 0; y < height; y++) {
		memcpy((uint8_t *)IOSurfaceGetBaseAddress(surface) + (size_t)y * IOSurfaceGetBytesPerRow(surface),
		    picture + (size_t)y * rowBytes, (size_t)width * 4);
	}
	IOSurfaceUnlock(surface, 0, NULL);

	for (int i = 0; i < secs; i++) {
		ret = SwapBegin(fb, &token);
		if (ret == 0) {
			ret = SwapSetLayer(fb, 0, surface, CGRectMake(0, 0, width, height), CGRectMake(0, 0, width, height), 0);
		}
		if (ret == 0) {
			ret = SwapEnd(fb);
		}
		if (i < 8 || ret != 0) {
			printf("fbtest: swap %d (token %d) -> 0x%x\n", i, token, ret);
		}
		sleep(1);
	}
	printf("fbtest: done\n");
	return 0;
}
