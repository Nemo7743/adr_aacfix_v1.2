/*
 * adr_aacfix 1.2 - AAC (sceAudiocodec 0x1003) fix for the PS Vita PSP emulator (ScePspemu) under Adrenaline
 *
 * Supports: Vita firmware 3.65 - 3.70 (ScePspemu module NID 0x3F75D4D3).  Verified on 3.65 only.
 *
 * The problem
 * -----------
 * PSP homebrew that plays PMP movies (NGE / AVG MAKER PORTABLE engine, and any program calling
 * sceAudiocodec with type 0x1003 directly) crashes ScePspemu's Media-Engine thread
 * ("ScePspemuRemoteMe", error C2-12828-1): ME command 0x93 of the AAC group (handler text+0xECDC) does
 *
 *     r4 = ConvertArg(arg2, mode=1, size=8)          text+0x6404, returns NULL when it rejects the address
 *     text+0x1AEDC(value=arg1, ptr=r4)               -> writes through r4 without a NULL check
 *
 * and arg2 is an address in the Media Engine's own EDRAM (0x0018B8C0), not in PSP main RAM.
 * ConvertArg only understands main RAM.  AT3+/MP3 treat their EDRAM address as an opaque handle and never
 * convert it, so only AAC is affected.
 *
 * The fix
 * -------
 *   1. EDRAM shadow: the ME EDRAM window [0x00100000,0x00200000) is backed by a 1 MiB host buffer and
 *      ConvertArg returns a pointer into it instead of NULL.  One ME address maps to one fixed host
 *      address, so the handlers' pointer-keyed slot table stays consistent and the values cmd 0x93/0x95/0x97
 *      write there are read back by cmd 0x90 (decode).
 *   2. NULL guard: whatever the shadow cannot cover (or if the shadow could not be allocated) makes the
 *      handler return -1 -- the value it already returns for "arg == 0" -- instead of writing through NULL.
 *
 * Only three functions are hooked.  Normal operation writes no per-frame log (logging every decode call
 * starves the decoder and makes the first seconds of audio crackle).
 *
 * Log     : ux0:data/adr_aacfix.log  (fallback ur0:tai/adr_aacfix.log) -- start line + errors only
 * Debug   : create an empty file ux0:data/adr_aacfix_debug to also log the first AAC requests
 *
 * Offsets are text-segment (segment 0) offsets of the 3.65 ScePspemu ELF.  The plugin verifies module NID
 * and the code bytes of each target before hooking and never patches blindly.
 */

#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <taihen.h>

#include <stdarg.h>
#include <stdint.h>

#define VERSION_STR     "1.2"
#define LOG_PATH        "ux0:data/adr_aacfix.log"
#define LOG_PATH2       "ur0:tai/adr_aacfix.log"
#define DEBUG_PATH      "ux0:data/adr_aacfix_debug"
#define LOG_MAX_BYTES   32768            /* the log is recreated at start if it grew beyond this */
#define NID_PSPEMU_365  0x3F75D4D3u

/* ME EDRAM window: values seen on real runs are 0x00180040, 0x0018B8C0, 0x0019AE00 */
#define EDRAM_LO        0x00100000u
#define EDRAM_HI        0x00200000u
#define SHADOW_BYTES    ((EDRAM_HI - EDRAM_LO) + 0x1000)

/* text-segment offsets (3.65) */
#define OFF_CONVARG     0x6404   /* void *ConvertArg(uint32_t psp_addr, int mode, uint32_t size) */
#define OFF_AAC_GROUP   0xECDC   /* int group(cmd, a1, a2, a3, a4, a5)   ME cmds 0x90..0x97      */
#define OFF_AAC_SET     0x1AEDC  /* int f(uint32_t value, void *ptr)     the crashing function   */

#define MAX_HOOKS 4

static uintptr_t g_base;
static int       g_debug;
static uint8_t  *g_shadow;
static SceUID    g_shadow_uid = -1;
static SceKernelLwMutexWork g_mtx;
static int       g_mtx_ok;

static SceUID         g_hook_uid[MAX_HOOKS];
static tai_hook_ref_t g_hook_ref[MAX_HOOKS];
static int            g_nhooks;
static tai_hook_ref_t g_ref_conv, g_ref_aac, g_ref_set;

/* ------------------------------------------------------------------------- logging */

static void logf_(const char *fmt, ...)
{
	char buf[320];
	va_list ap;
	va_start(ap, fmt);
	int n = sceClibVsnprintf(buf, sizeof(buf) - 1, fmt, ap);
	va_end(ap);
	if (n < 0) return;
	if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
	buf[n++] = '\n';

	if (g_mtx_ok) sceKernelLockLwMutex(&g_mtx, 1, NULL);
	SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
	if (fd < 0) fd = sceIoOpen(LOG_PATH2, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
	if (fd >= 0) {
		sceIoWrite(fd, buf, n);
		sceIoClose(fd);
	}
	if (g_mtx_ok) sceKernelUnlockLwMutex(&g_mtx, 1);
}
#define LOG(fmt, ...)  logf_("[%10u] " fmt, (unsigned)sceKernelGetProcessTimeLow(), ##__VA_ARGS__)
#define LOGD(fmt, ...) do { if (g_debug) LOG(fmt, ##__VA_ARGS__); } while (0)

/* ------------------------------------------------------------------------- shadow / probing */

static void *shadow_ptr(uint32_t a, uint32_t size)
{
	uint32_t m = a & 0x1FFFFFFF;
	if (!g_shadow) return NULL;
	if (m < EDRAM_LO || m >= EDRAM_HI) return NULL;
	if (size > EDRAM_HI - m) return NULL;
	return g_shadow + (m - EDRAM_LO);
}

/* Original conversion (bypasses our own hook), mode=0/size=0: no cache side effects; falls back to the shadow. */
static void *conv_probe(uint32_t a)
{
	void *r = TAI_CONTINUE(void *, g_ref_conv, a, 0, 0u);
	if (r == NULL && a != 0) r = shadow_ptr(a, 8);
	return r;
}

static int bad_ptr(uint32_t a)
{
	return a != 0 && conv_probe(a) == NULL;
}

/* ------------------------------------------------------------------------- hooks */

static void *hook_conv(uint32_t addr, int mode, uint32_t size)
{
	void *r = TAI_CONTINUE(void *, g_ref_conv, addr, mode, size);
	if (r == NULL && addr != 0) {
		void *sh = shadow_ptr(addr, size ? size : 8);
		if (sh) return sh;
		static int n;
		if (n < 20) {
			n++;
			uintptr_t lr = (uintptr_t)__builtin_return_address(0);
			LOG("CONV-NULL addr=%08x mode=%d size=%u caller=text+0x%x", addr, mode, size, (unsigned)((lr & ~1u) - g_base));
		}
	}
	return r;
}

static int g_dbg_calls;

static int hook_aac_group(uint32_t cmd, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
	if (g_debug && cmd != 0x90 && g_dbg_calls < 60) {
		g_dbg_calls++;
		LOG("AAC cmd=0x%02x a1=%08x a2=%08x a3=%08x a4=%08x a5=%08x", cmd, a1, a2, a3, a4, a5);
	}

	/* Slots the handler converts and dereferences without a NULL check (from disassembly):
	 *   0x90 decode: a1..a5   0x91: a1,a2   0x93: a2   0x95: a2   0x97: a4.
	 * A zero slot is already handled by the handler (returns -1). */
	int bad = 0;
	switch (cmd) {
	case 0x90: bad = bad_ptr(a1) || bad_ptr(a2) || bad_ptr(a3) || bad_ptr(a4) || bad_ptr(a5); break;
	case 0x91: bad = bad_ptr(a1) || bad_ptr(a2); break;
	case 0x93: bad = bad_ptr(a2); break;
	case 0x95: bad = bad_ptr(a2); break;
	case 0x97: bad = bad_ptr(a4); break;
	default: break;
	}
	if (bad) {
		static int n;
		if (n < 20) {
			n++;
			LOG("GUARD cmd=0x%02x a1=%08x a2=%08x a3=%08x a4=%08x a5=%08x -> return -1 (pointer not convertible)",
			    cmd, a1, a2, a3, a4, a5);
		}
		return -1;
	}
	return TAI_CONTINUE(int, g_ref_aac, cmd, a1, a2, a3, a4, a5);
}

/* Second line of defence: the crashing function itself. */
static int hook_aac_set(uint32_t value, void *ptr)
{
	if (ptr == NULL) {
		static int n;
		if (n < 20) { n++; LOG("GUARD AAC-SET ptr==NULL value=%u -> return -1", value); }
		return -1;
	}
	return TAI_CONTINUE(int, g_ref_set, value, ptr);
}

/* ------------------------------------------------------------------------- install */

static int install(SceUID modid, const char *name, uint32_t off, const uint8_t *sig, int siglen,
                   tai_hook_ref_t *ref, const void *func)
{
	if (g_nhooks >= MAX_HOOKS) return -1;
	if (sceClibMemcmp((const void *)(g_base + off), sig, siglen) != 0) {
		uint8_t got[8];
		sceClibMemcpy(got, (const void *)(g_base + off), 8);
		LOG("SKIP %s text+0x%05x : code bytes differ (%02x%02x%02x%02x%02x%02x%02x%02x)", name, off,
		    got[0], got[1], got[2], got[3], got[4], got[5], got[6], got[7]);
		return -1;
	}
	SceUID id = taiHookFunctionOffset(ref, modid, 0, off, 1 /*thumb*/, func);
	if (id < 0) {
		LOG("FAIL %s text+0x%05x : taiHookFunctionOffset = 0x%08x", name, off, id);
		return -1;
	}
	g_hook_uid[g_nhooks] = id;
	g_hook_ref[g_nhooks] = *ref;
	g_nhooks++;
	return 0;
}

static void release_all(void)
{
	for (int i = g_nhooks - 1; i >= 0; i--) taiHookRelease(g_hook_uid[i], g_hook_ref[i]);
	g_nhooks = 0;
}

void _start() __attribute__((weak, alias("module_start")));
int module_start(SceSize args, const void *argp)
{
	(void)args; (void)argp;

	if (sceKernelCreateLwMutex(&g_mtx, "adr_aacfix_mtx", 0, 0, NULL) >= 0) g_mtx_ok = 1;

	SceIoStat st;
	if (sceIoGetstat(DEBUG_PATH, &st) >= 0) g_debug = 1;
	if (sceIoGetstat(LOG_PATH, &st) >= 0 && st.st_size > LOG_MAX_BYTES) sceIoRemove(LOG_PATH);

	tai_module_info_t ti;
	sceClibMemset(&ti, 0, sizeof(ti));
	ti.size = sizeof(ti);
	int res = taiGetModuleInfo("ScePspemu", &ti);
	if (res < 0) return SCE_KERNEL_START_SUCCESS;              /* not the emulator process (e.g. Adrenaline's installer mode) */

	if (ti.module_nid != NID_PSPEMU_365) {
		LOG("adr_aacfix " VERSION_STR ": ScePspemu NID 0x%08x is not supported (3.65-3.70 only) -> disabled", ti.module_nid);
		return SCE_KERNEL_START_SUCCESS;
	}

	SceKernelModuleInfo mi;
	sceClibMemset(&mi, 0, sizeof(mi));
	mi.size = sizeof(mi);
	res = sceKernelGetModuleInfo(ti.modid, &mi);
	if (res < 0) { LOG("sceKernelGetModuleInfo failed 0x%08x -> disabled", res); return SCE_KERNEL_START_SUCCESS; }
	g_base = (uintptr_t)mi.segments[0].vaddr;

	/* EDRAM shadow; if this fails the plugin still hooks and simply uses the NULL guard. */
	g_shadow_uid = sceKernelAllocMemBlock("adr_aacfix_edram", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, SHADOW_BYTES, NULL);
	if (g_shadow_uid >= 0 && sceKernelGetMemBlockBase(g_shadow_uid, (void **)&g_shadow) >= 0 && g_shadow) {
		sceClibMemset(g_shadow, 0, SHADOW_BYTES);
	} else {
		LOG("EDRAM shadow allocation failed (0x%08x) -> falling back to the NULL guard only (AAC will be refused, no crash)", g_shadow_uid);
		if (g_shadow_uid >= 0) sceKernelFreeMemBlock(g_shadow_uid);
		g_shadow_uid = -1;
		g_shadow = NULL;
	}

	static const uint8_t sig_conv[] = {0x70,0xb5,0x16,0x46,0x1a,0xb1,0x42,0xea,0x00,0x03};
	static const uint8_t sig_aac[]  = {0x90,0x38,0x2d,0xe9,0xf0,0x41,0x0e,0x46,0x82,0xb0};
	static const uint8_t sig_set[]  = {0x38,0xb5,0x0c,0x46,0x05,0x46,0x02,0x21,0x20,0x46};

	/* All three or nothing: a half-installed fix (e.g. shadow hook without guard) is worse than none. */
	int ok = install(ti.modid, "ConvArg",  OFF_CONVARG,   sig_conv, sizeof(sig_conv), &g_ref_conv, hook_conv) == 0
	      && install(ti.modid, "AACgroup", OFF_AAC_GROUP, sig_aac,  sizeof(sig_aac),  &g_ref_aac,  hook_aac_group) == 0
	      && install(ti.modid, "AACset",   OFF_AAC_SET,   sig_set,  sizeof(sig_set),  &g_ref_set,  hook_aac_set) == 0;
	if (!ok) {
		release_all();
		if (g_shadow_uid >= 0) { sceKernelFreeMemBlock(g_shadow_uid); g_shadow_uid = -1; g_shadow = NULL; }
		LOG("adr_aacfix " VERSION_STR ": could not install all hooks -> disabled");
		return SCE_KERNEL_START_SUCCESS;
	}

	LOG("adr_aacfix " VERSION_STR " active: ScePspemu text=%08x, shadow=%s%s", (unsigned)g_base,
	    g_shadow ? "on" : "OFF(guard only)", g_debug ? ", debug" : "");
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, const void *argp)
{
	(void)args; (void)argp;
	release_all();
	if (g_shadow_uid >= 0) { sceKernelFreeMemBlock(g_shadow_uid); g_shadow_uid = -1; g_shadow = NULL; }
	return SCE_KERNEL_STOP_SUCCESS;
}
