/*
 * glfill: what a full-screen draw costs the Adreno 642L under Mesa's own GL driver (freedreno), on pmOS, for the same
 * cases hakios-metal's probe timed under iOS 26 (docs/gpu-device-test.md, Session 9: opaque 0.93 ms, blended 0.91,
 * blended + one sample of a 4x4 texture 1.80, framebuffer fetch 1.78, per 1125x2436 screen). If freedreno pays the same
 * ~0.85 ms a screen for a sample, that cost is the chip's; if it pays much less, hakios-metal's draw state differs
 * from Mesa's somewhere that matters, and the case that differs says where.
 *
 * Darwin-lisa session 13 (2026-11-16). Runs on pmOS as any user that can open /dev/dri/renderD128 (EGL surfaceless,
 * an FBO, no display). Every case draws a full-screen quad N times into a 1080x2400 target and is timed by the GPU
 * (GL_EXT_disjoint_timer_query) and by the wall clock around glFinish; the best of three runs is printed, per draw.
 * Run it twice: FD_MESA_DEBUG=sysmem (what hakios-metal does: the target in memory) and without (freedreno's GMEM).
 *
 *   glfill [DRAWS]          (default 40 draws a case)
 *
 * SPDX-License-Identifier: MIT
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W 1080
#define H 2400

static PFNGLGENQUERIESEXTPROC genQueries;
static PFNGLBEGINQUERYEXTPROC beginQuery;
static PFNGLENDQUERYEXTPROC endQuery;
static PFNGLGETQUERYOBJECTUI64VEXTPROC queryResult;

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static GLuint shader(GLenum kind, const char *src)
{
	GLuint s = glCreateShader(kind);
	GLint ok = 0;
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[2048];
		glGetShaderInfoLog(s, sizeof log, NULL, log);
		fprintf(stderr, "glfill: shader: %s\n%s\n", log, src);
		exit(1);
	}
	return s;
}

static GLuint program(const char *fs)
{
	/* the quad's texcoord goes straight to the sample in the plain cases (freedreno prefetches such a sample) */
	static const char vs[] = "#version 300 es\n"
				 "layout(location = 0) in vec2 pos;\n"
				 "out vec2 uv;\n"
				 "void main() { uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }\n";
	GLuint p = glCreateProgram();
	GLint ok = 0;
	glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
	glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(p, 0, "pos");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		fprintf(stderr, "glfill: link failed\n");
		exit(1);
	}
	return p;
}

static GLuint texture(GLenum internal, GLenum format, GLenum type, int w, int h, GLenum filter)
{
	GLuint t;
	size_t bytes = (size_t)w * h * 8;
	unsigned char *data = malloc(bytes);
	for (size_t i = 0; i < bytes; i++) data[i] = (unsigned char)(i * 37 + (i >> 9));
	if (type == GL_HALF_FLOAT) {
		for (size_t i = 0; i < bytes / 2; i++) ((unsigned short *)data)[i] = 0x3800 + (unsigned short)(i % 512);   /* 0.5 .. 0.75 */
	}
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, data);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	free(data);
	return t;
}

struct kase {
	const char *name;
	const char *fs;
	int blend;
	int clear;     /* glClear alone, no draw */
	GLuint tex[2];
};

static double runOnce(const struct kase *k, GLuint p, int draws, double *wall)
{
	GLuint q = 0;
	GLuint64 ns = 0;

	glUseProgram(p);
	for (int i = 0; i < 2; i++) {
		glActiveTexture(GL_TEXTURE0 + i);
		glBindTexture(GL_TEXTURE_2D, k->tex[i]);
	}
	if (k->blend) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	} else {
		glDisable(GL_BLEND);
	}
	glFinish();
	if (genQueries) {
		genQueries(1, &q);
		beginQuery(GL_TIME_ELAPSED_EXT, q);
	}
	double t0 = now();
	for (int i = 0; i < draws; i++) {
		if (k->clear) {
			glClearColor((i & 1) * 0.5f, 0.25f, 0.75f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);
		} else {
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}
	}
	if (genQueries) endQuery(GL_TIME_ELAPSED_EXT);
	glFinish();
	*wall = (now() - t0) * 1e6 / draws;
	if (genQueries) queryResult(q, GL_QUERY_RESULT_EXT, &ns);
	return genQueries ? ns / 1e3 / draws : *wall;
}

int main(int argc, char **argv)
{
	const int draws = argc > 1 ? atoi(argv[1]) : 40;
	EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	EGLint major, minor;
	if (d == EGL_NO_DISPLAY || !eglInitialize(d, &major, &minor)) {
		fprintf(stderr, "glfill: no surfaceless EGL display\n");
		return 1;
	}
	eglBindAPI(EGL_OPENGL_ES_API);
	static const EGLint cattr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
	EGLContext c = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, cattr);
	if (c == EGL_NO_CONTEXT || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) {
		fprintf(stderr, "glfill: no GLES 3 context\n");
		return 1;
	}
	const char *ext = (const char *)glGetString(GL_EXTENSIONS);
	if (strstr(ext, "GL_EXT_disjoint_timer_query")) {
		genQueries = (PFNGLGENQUERIESEXTPROC)eglGetProcAddress("glGenQueriesEXT");
		beginQuery = (PFNGLBEGINQUERYEXTPROC)eglGetProcAddress("glBeginQueryEXT");
		endQuery = (PFNGLENDQUERYEXTPROC)eglGetProcAddress("glEndQueryEXT");
		queryResult = (PFNGLGETQUERYOBJECTUI64VEXTPROC)eglGetProcAddress("glGetQueryObjectui64vEXT");
	}
	const int fetch = strstr(ext, "GL_EXT_shader_framebuffer_fetch") != NULL && strstr(ext, "GL_EXT_shader_framebuffer_fetch_non_coherent") == NULL;
	printf("glfill: %s, %s; %d draws a case, %dx%d RGBA8 target; GPU timer %s; FD_MESA_DEBUG=%s\n", glGetString(GL_RENDERER),
	       glGetString(GL_VERSION), draws, W, H, genQueries ? "yes" : "no (wall clock)", getenv("FD_MESA_DEBUG") ? getenv("FD_MESA_DEBUG") : "");

	GLuint target, fbo, vbo;
	glGenTextures(1, &target);
	glBindTexture(GL_TEXTURE_2D, target);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, W, H);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "glfill: the target is not complete\n");
		return 1;
	}
	glViewport(0, 0, W, H);
	static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

	const GLuint t4 = texture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, 4, GL_NEAREST);
	const GLuint t4l = texture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, 4, GL_LINEAR);
	const GLuint tf = texture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, W, H, GL_LINEAR);
	const GLuint th = texture(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, W, H, GL_LINEAR);
	const GLuint tq = texture(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, W / 2, H / 2, GL_LINEAR);

#define HEAD "#version 300 es\nprecision highp float;\nin vec2 uv;\nout vec4 o;\n"
#define HEADM "#version 300 es\nprecision mediump float;\nin highp vec2 uv;\nout mediump vec4 o;\n"
	const struct kase cases[] = {
		{"clear (glClear, no draw)", HEAD "void main() { o = vec4(1.0); }\n", 0, 1, {0, 0}},
		{"opaque, a constant", HEAD "void main() { o = vec4(0.25, 0.5, 0.75, 1.0); }\n", 0, 0, {0, 0}},
		{"blended, a constant", HEAD "void main() { o = vec4(0.1, 0.2, 0.3, 0.5); }\n", 1, 0, {0, 0}},
		{"blended, 4x4 nearest, highp (probe's tri_tex)", HEAD "uniform sampler2D s;\nvoid main() { o = texture(s, uv); }\n", 1, 0, {t4, 0}},
		{"blended, 4x4 nearest, mediump", HEADM "uniform mediump sampler2D s;\nvoid main() { o = texture(s, uv); }\n", 1, 0, {t4, 0}},
		{"blended, 4x4 linear, mediump", HEADM "uniform mediump sampler2D s;\nvoid main() { o = texture(s, uv); }\n", 1, 0, {t4l, 0}},
		{"blended, 4x4 linear, mediump, lod bias (CA's samb)", HEADM "uniform mediump sampler2D s;\nuniform float b;\nvoid main() { o = texture(s, uv, b); }\n", 1, 0, {t4l, 0}},
		{"blended, 4x4 linear, mediump, coord computed (no prefetch)", HEADM "uniform mediump sampler2D s;\nvoid main() { o = texture(s, uv * 0.999 + 0.0005); }\n", 1, 0, {t4l, 0}},
		{"opaque, 1080x2400 RGBA8 linear 1:1 (a copy)", HEADM "uniform mediump sampler2D s;\nvoid main() { o = texture(s, uv); }\n", 0, 0, {tf, 0}},
		{"blended, 1080x2400 RGBA8 linear 1:1", HEADM "uniform mediump sampler2D s;\nvoid main() { o = texture(s, uv); }\n", 1, 0, {tf, 0}},
		{"blended, 1080x2400 RGBA16F linear 1:1 (CA's backdrop)", HEADM "uniform mediump sampler2D s;\nvoid main() { o = texture(s, uv); }\n", 1, 0, {th, 0}},
		{"blended, 540x1200 RGBA16F linear, stretched", HEADM "uniform mediump sampler2D s;\nvoid main() { o = texture(s, uv); }\n", 1, 0, {tq, 0}},
		{"blended, two samples (1080x2400 RGBA8 + 4x4)", HEADM "uniform mediump sampler2D s;\nuniform mediump sampler2D t;\nvoid main() { o = texture(s, uv) * texture(t, uv); }\n", 1, 0, {tf, t4l}},
		{"framebuffer fetch (EXT_shader_framebuffer_fetch), opaque", "#version 300 es\n#extension GL_EXT_shader_framebuffer_fetch : require\nprecision mediump float;\nin highp vec2 uv;\ninout mediump vec4 o;\nvoid main() { o = o.gbra; }\n", 0, 0, {0, 0}},
	};
	for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		const struct kase *k = &cases[i];
		if (strstr(k->fs, "framebuffer_fetch") && !fetch) {
			printf("glfill: %-60s not offered\n", k->name);
			continue;
		}
		GLuint p = program(k->fs);
		glUseProgram(p);
		GLint u = glGetUniformLocation(p, "t");
		if (u >= 0) glUniform1i(u, 1);
		u = glGetUniformLocation(p, "b");
		if (u >= 0) glUniform1f(u, 0.0f);
		double best = 1e30, bestWall = 1e30, wall;
		runOnce(k, p, 4, &wall);   /* warm: compile, upload, caches */
		for (int r = 0; r < 3; r++) {
			double us = runOnce(k, p, draws, &wall);
			if (us < best) best = us;
			if (wall < bestWall) bestWall = wall;
		}
		printf("glfill: %-60s %7.0f us a draw (GPU timer), %7.0f wall\n", k->name, best, bestWall);
	}
	const GLenum err = glGetError();
	printf("glfill: done, GL error %#x\n", err);
	return err != GL_NO_ERROR;
}
