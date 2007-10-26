#include <sys/stat.h>
#include <sys/types.h>
#include <sys/syslimits.h> // PATH_MAX

#include <pspthreadman.h>
#include <pspdisplay.h>
#include <psputils.h>
#include <pspgu.h>
#include <pspaudio.h>

#include "psp.h"
#include "menu.h"
#include "emu.h"
#include "../common/emu.h"
#include "../common/lprintf.h"
#include "../../Pico/PicoInt.h"

#ifdef BENCHMARK
#define OSD_FPS_X 380
#else
#define OSD_FPS_X 420
#endif

// additional pspaudio imports, credits to crazyc
int sceAudio_38553111(unsigned short samples, unsigned short freq, char unknown);  // play with conversion?
int sceAudio_5C37C0AE(void);				// end play?
int sceAudio_E0727056(int volume, void *buffer);	// blocking output
int sceAudioOutput2GetRestSample();


char romFileName[PATH_MAX];
unsigned char *PicoDraw2FB = (unsigned char *)VRAM_CACHED_STUFF + 8; // +8 to be able to skip border with 1 quadword..
int engineState;

static int combo_keys = 0, combo_acts = 0; // keys and actions which need button combos
static unsigned int noticeMsgTime = 0;
int reset_timing = 0; // do we need this?


static void sound_init(void);
static void sound_deinit(void);
static void blit2(const char *fps, const char *notice);
static void clearArea(int full);

void emu_noticeMsgUpdated(void)
{
	noticeMsgTime = sceKernelGetSystemTimeLow();
}

void emu_getMainDir(char *dst, int len)
{
	if (len > 0) *dst = 0;
}

static void osd_text(int x, const char *text, int is_active)
{
	unsigned short *screen = is_active ? psp_video_get_active_fb() : psp_screen;
	int len = strlen(text) * 8 / 2;
	int *p, h;
	void *tmp;
	for (h = 0; h < 8; h++) {
		p = (int *) (screen+x+512*(264+h));
		p = (int *) ((int)p & ~3); // align
		memset32(p, 0, len);
	}
	if (is_active) { tmp = psp_screen; psp_screen = screen; } // nasty pointer tricks
	emu_textOut16(x, 264, text);
	if (is_active) psp_screen = tmp;
}

void emu_msg_cb(const char *msg)
{
	osd_text(4, msg, 1);
	noticeMsgTime = sceKernelGetSystemTimeLow() - 2000000;

	/* assumption: emu_msg_cb gets called only when something slow is about to happen */
	reset_timing = 1;
}

static void emu_msg_tray_open(void)
{
	strcpy(noticeMsg, "CD tray opened");
	noticeMsgTime = sceKernelGetSystemTimeLow();
}


void emu_Init(void)
{
	// make dirs for saves, cfgs, etc.
	mkdir("mds", 0777);
	mkdir("srm", 0777);
	mkdir("brm", 0777);
	mkdir("cfg", 0777);

	sound_init();

	PicoInit();
	PicoMessage = emu_msg_cb;
	PicoMCDopenTray = emu_msg_tray_open;
	PicoMCDcloseTray = menu_loop_tray;
}

void emu_Deinit(void)
{
	// save SRAM
	if ((currentConfig.EmuOpt & 1) && SRam.changed) {
		emu_SaveLoadGame(0, 1);
		SRam.changed = 0;
	}

	if (!(currentConfig.EmuOpt & 0x20)) {
		FILE *f = fopen(PicoConfigFile, "r+b");
		if (!f) emu_WriteConfig(0);
		else {
			// if we already have config, reload it, except last ROM
			fseek(f, sizeof(currentConfig.lastRomFile), SEEK_SET);
			fread(&currentConfig.EmuOpt, 1, sizeof(currentConfig) - sizeof(currentConfig.lastRomFile), f);
			fseek(f, 0, SEEK_SET);
			fwrite(&currentConfig, 1, sizeof(currentConfig), f);
			fflush(f);
			fclose(f);
		}
	}

	PicoExit();
	sound_deinit();
}

void emu_setDefaultConfig(void)
{
	memset(&currentConfig, 0, sizeof(currentConfig));
	currentConfig.lastRomFile[0] = 0;
	currentConfig.EmuOpt  = 0x1f | 0x680; // | confirm_save, cd_leds, 16bit rend
	currentConfig.PicoOpt = 0x07 | 0xc00; // | cd_pcm, cd_cdda
	currentConfig.PsndRate = 22050;
	currentConfig.PicoRegion = 0; // auto
	currentConfig.PicoAutoRgnOrder = 0x184; // US, EU, JP
	currentConfig.Frameskip = -1; // auto
	currentConfig.volume = 50;
	currentConfig.CPUclock = 222;
	currentConfig.KeyBinds[ 4] = 1<<0; // SACB RLDU
	currentConfig.KeyBinds[ 6] = 1<<1;
	currentConfig.KeyBinds[ 7] = 1<<2;
	currentConfig.KeyBinds[ 5] = 1<<3;
	currentConfig.KeyBinds[14] = 1<<4;
	currentConfig.KeyBinds[13] = 1<<5;
	currentConfig.KeyBinds[15] = 1<<6;
	currentConfig.KeyBinds[ 3] = 1<<7;
	currentConfig.KeyBinds[ 8] = 1<<27; // save state
	currentConfig.KeyBinds[ 9] = 1<<28; // load state
	currentConfig.PicoCDBuffers = 0;
	currentConfig.scaling = 1; // bilinear filtering for psp
	currentConfig.scale = currentConfig.hscale32 = currentConfig.hscale40 = 1.0;
}


extern void amips_clut(unsigned short *dst, unsigned char *src, unsigned short *pal, int count);

struct Vertex
{
	short u,v;
	short x,y,z;
};

static struct Vertex __attribute__((aligned(4))) g_vertices[2];
static unsigned short __attribute__((aligned(16))) localPal[0x100];
static int dynamic_palette = 0, need_pal_upload = 0, blit_16bit_mode = 0;
static int fbimg_offs = 0;

static void set_scaling_params(void)
{
	int src_width, fbimg_width, fbimg_height, fbimg_xoffs, fbimg_yoffs;
	g_vertices[0].x = g_vertices[0].y =
	g_vertices[0].z = g_vertices[1].z = 0;

	fbimg_height = (int)(240.0 * currentConfig.scale + 0.5);
	if (Pico.video.reg[12] & 1) {
		fbimg_width = (int)(320.0 * currentConfig.scale * currentConfig.hscale40 + 0.5);
		src_width = 320;
	} else {
		fbimg_width = (int)(256.0 * currentConfig.scale * currentConfig.hscale32 + 0.5);
		src_width = 256;
	}

	if (fbimg_width >= 480) {
		g_vertices[0].u = (fbimg_width-480)/2;
		g_vertices[1].u = src_width - (fbimg_width-480)/2;
		fbimg_width = 480;
		fbimg_xoffs = 0;
	} else {
		g_vertices[0].u = 0;
		g_vertices[1].u = src_width;
		fbimg_xoffs = 240 - fbimg_width/2;
	}

	if (fbimg_height >= 272) {
		g_vertices[0].v = (fbimg_height-272)/2;
		g_vertices[1].v = 240 - (fbimg_height-272)/2;
		fbimg_height = 272;
		fbimg_yoffs = 0;
	} else {
		g_vertices[0].v = 0;
		g_vertices[1].v = 240;
		fbimg_yoffs = 136 - fbimg_height/2;
	}

	g_vertices[1].x = fbimg_width;
	g_vertices[1].y = fbimg_height;
	if (fbimg_xoffs < 0) fbimg_xoffs = 0;
	if (fbimg_yoffs < 0) fbimg_yoffs = 0;
	fbimg_offs = (fbimg_yoffs*512 + fbimg_xoffs) * 2; // dst is always 16bit

	lprintf("set_scaling_params:\n");
	lprintf("offs: %i, %i\n", fbimg_xoffs, fbimg_yoffs);
	lprintf("xy0, xy1: %i, %i; %i, %i\n", g_vertices[0].x, g_vertices[0].y, g_vertices[1].x, g_vertices[1].y);
	lprintf("uv0, uv1: %i, %i; %i, %i\n", g_vertices[0].u, g_vertices[0].v, g_vertices[1].u, g_vertices[1].v);
}

static void do_slowmode_pal(void)
{
	unsigned int *spal=(void *)Pico.cram;
	unsigned int *dpal=(void *)localPal;
	int i;

	for (i = 0x3f/2; i >= 0; i--)
		dpal[i] = ((spal[i]&0x000f000f)<< 1)|((spal[i]&0x00f000f0)<<3)|((spal[i]&0x0f000f00)<<4);

	if (Pico.video.reg[0xC]&8) // shadow/hilight?
	{
		// shadowed pixels
		for (i = 0x3f/2; i >= 0; i--)
			dpal[0x20|i] = dpal[0x60|i] = (spal[i]>>1)&0x738e738e;
		// hilighted pixels
		for (i = 0x3f; i >= 0; i--) {
			int t=localPal[i]&0xe71c;t+=0x4208;
			if (t&0x20) t|=0x1c;
			if (t&0x800) t|=0x700;
			if (t&0x10000) t|=0xe000;
			t&=0xe71c;
			localPal[0x80|i]=(unsigned short)t;
		}
		localPal[0xe0] = 0;
	}
	Pico.m.dirtyPal = 0;
	need_pal_upload = 1;
}

static void do_slowmode_lines(int line_to)
{
	int line = 0, line_len = (Pico.video.reg[12]&1) ? 320 : 256;
	unsigned short *dst = (unsigned short *)VRAM_STUFF + 512*240/2;
	unsigned char  *src = (unsigned char  *)VRAM_CACHED_STUFF + 16;
	if (!(Pico.video.reg[1]&8)) { line = 8; dst += 512*8; src += 512*8; }

	for (; line < line_to; line++, dst+=512, src+=512)
		amips_clut(dst, src, localPal, line_len);
}

static void EmuScanPrepare(void)
{
	HighCol = (unsigned char *)VRAM_CACHED_STUFF + 8;
	if (!(Pico.video.reg[1]&8)) HighCol += 8*512;

	dynamic_palette = 0;
	if (Pico.m.dirtyPal)
		do_slowmode_pal();
}

static int EmuScanSlow(unsigned int num, void *sdata)
{
	if (!(Pico.video.reg[1]&8)) num += 8;

	if (Pico.m.dirtyPal) {
		if (!dynamic_palette) {
			do_slowmode_lines(num);
			dynamic_palette = 1;
		}
		do_slowmode_pal();
	}

	if (dynamic_palette) {
		int line_len = (Pico.video.reg[12]&1) ? 320 : 256;
		void *dst = (char *)VRAM_STUFF + 512*240 + 512*2*num;
		amips_clut(dst, HighCol + 8, localPal, line_len);
	} else
		HighCol = (unsigned char *)VRAM_CACHED_STUFF + (num+1)*512 + 8;

	return 0;
}

static void blitscreen_clut(void)
{
	int offs = fbimg_offs;
	offs += (psp_screen == VRAM_FB0) ? VRAMOFFS_FB0 : VRAMOFFS_FB1;

	sceKernelDcacheWritebackAll();

	sceGuSync(0,0); // sync with prev
	sceGuStart(GU_DIRECT, guCmdList);
	sceGuDrawBuffer(GU_PSM_5650, (void *)offs, 512); // point to back buffer

	if (dynamic_palette)
	{
		if (!blit_16bit_mode) {
			sceGuTexMode(GU_PSM_5650, 0, 0, 0);
			sceGuTexImage(0,512,512,512,(char *)VRAM_STUFF + 512*240);

			blit_16bit_mode = 1;
		}
	}
	else
	{
		if (blit_16bit_mode) {
			sceGuClutMode(GU_PSM_5650,0,0xff,0);
			sceGuTexMode(GU_PSM_T8,0,0,0); // 8-bit image
			sceGuTexImage(0,512,512,512,(char *)VRAM_STUFF + 16);
			blit_16bit_mode = 0;
		}

		if ((PicoOpt&0x10) && Pico.m.dirtyPal)
		{
			int i, *dpal = (void *)localPal, *spal = (int *)Pico.cram;
			for (i = 0x3f/2; i >= 0; i--)
				dpal[i] = ((spal[i]&0x000f000f)<< 1)|((spal[i]&0x00f000f0)<<3)|((spal[i]&0x0f000f00)<<4);
			localPal[0xe0] = 0;
			Pico.m.dirtyPal = 0;
			need_pal_upload = 1;
		}

		if (need_pal_upload) {
			need_pal_upload = 0;
			sceGuClutLoad((256/8), localPal); // upload 32*8 entries (256)
		}
	}

#if 1
	if (g_vertices[0].u == 0 && g_vertices[1].u == g_vertices[1].x)
	{
		struct Vertex* vertices;
		int x;

		#define SLICE_WIDTH 32
		for (x = 0; x < g_vertices[1].x; x += SLICE_WIDTH)
		{
			// render sprite
			vertices = (struct Vertex*)sceGuGetMemory(2 * sizeof(struct Vertex));
			memcpy(vertices, g_vertices, 2 * sizeof(struct Vertex));
			vertices[0].u = vertices[0].x = x;
			vertices[1].u = vertices[1].x = x + SLICE_WIDTH;
			sceGuDrawArray(GU_SPRITES,GU_TEXTURE_16BIT|GU_VERTEX_16BIT|GU_TRANSFORM_2D,2,0,vertices);
		}
		// lprintf("listlen: %iB\n", sceGuCheckList()); // ~480 only
	}
	else
#endif
		sceGuDrawArray(GU_SPRITES,GU_TEXTURE_16BIT|GU_VERTEX_16BIT|GU_TRANSFORM_2D,2,0,g_vertices);

	sceGuFinish();
}


static void cd_leds(void)
{
	static int old_reg = 0;
	unsigned int col_g, col_r, *p;

	if (!((Pico_mcd->s68k_regs[0] ^ old_reg) & 3)) return; // no change
	old_reg = Pico_mcd->s68k_regs[0];

	p = (unsigned int *)((short *)psp_screen + 512*2+4+2);
	col_g = (old_reg & 2) ? 0x06000600 : 0;
	col_r = (old_reg & 1) ? 0xc000c000 : 0;
	*p++ = col_g; *p++ = col_g; p+=2; *p++ = col_r; *p++ = col_r; p += 512/2 - 12/2;
	*p++ = col_g; *p++ = col_g; p+=2; *p++ = col_r; *p++ = col_r; p += 512/2 - 12/2;
	*p++ = col_g; *p++ = col_g; p+=2; *p++ = col_r; *p++ = col_r;
}


static void dbg_text(void)
{
	int *p, h, len;
	char text[128];

	sprintf(text, "sl: %i, 16b: %i", g_vertices[0].u == 0 && g_vertices[1].u == g_vertices[1].x, blit_16bit_mode);
	len = strlen(text) * 8 / 2;
	for (h = 0; h < 8; h++) {
		p = (int *) ((unsigned short *) psp_screen+2+512*(256+h));
		p = (int *) ((int)p & ~3); // align
		memset32(p, 0, len);
	}
	emu_textOut16(2, 256, text);
}


/* called after rendering is done, but frame emulation is not finished */
void blit1(void)
{
	if (PicoOpt&0x10)
	{
		int i;
		unsigned char *pd;
		// clear top and bottom trash
		for (pd = PicoDraw2FB+8, i = 8; i > 0; i--, pd += 512)
			memset32((int *)pd, 0xe0e0e0e0, 320/4);
		for (pd = PicoDraw2FB+512*232+8, i = 8; i > 0; i--, pd += 512)
			memset32((int *)pd, 0xe0e0e0e0, 320/4);
	}

	blitscreen_clut();
}


static void blit2(const char *fps, const char *notice)
{
	int emu_opt = currentConfig.EmuOpt;

	sceGuSync(0,0);

	if (notice || (emu_opt & 2)) {
		if (notice)      osd_text(4, notice, 0);
		if (emu_opt & 2) osd_text(OSD_FPS_X, fps, 0);
	}

	dbg_text();

	if ((emu_opt & 0x400) && (PicoMCD & 1))
		cd_leds();

	psp_video_flip(0);
}

// clears whole screen or just the notice area (in all buffers)
static void clearArea(int full)
{
	if (full) {
		memset32(psp_screen, 0, 512*272*2/4);
		psp_video_flip(0);
		memset32(psp_screen, 0, 512*272*2/4);
		memset32(VRAM_CACHED_STUFF, 0xe0e0e0e0, 512*240/4);
		memset32((int *)VRAM_CACHED_STUFF+512*240/4, 0, 512*240*2/4);
	} else {
		void *fb = psp_video_get_active_fb();
		memset32((int *)((char *)psp_screen + 512*264*2), 0, 512*8*2/4);
		memset32((int *)((char *)fb         + 512*264*2), 0, 512*8*2/4);
	}
}

static void vidResetMode(void)
{
	// setup GU
	sceGuSync(0,0); // sync with prev
	sceGuStart(GU_DIRECT, guCmdList);

	sceGuClutMode(GU_PSM_5650,0,0xff,0);
	sceGuTexMode(GU_PSM_T8,0,0,0); // 8-bit image
	sceGuTexFunc(GU_TFX_REPLACE,GU_TCC_RGB);
	if (currentConfig.scaling)
	     sceGuTexFilter(GU_LINEAR, GU_LINEAR);
	else sceGuTexFilter(GU_NEAREST, GU_NEAREST);
	sceGuTexScale(1.0f,1.0f);
	sceGuTexOffset(0.0f,0.0f);

	sceGuTexImage(0,512,512,512,(char *)VRAM_STUFF + 16);

	// slow rend.
	PicoDrawSetColorFormat(-1);
	PicoScan = EmuScanSlow;

	localPal[0xe0] = 0;
	Pico.m.dirtyPal = 1;
	blit_16bit_mode = dynamic_palette = 0;

	sceGuFinish();
	set_scaling_params();
	sceGuSync(0,0);
}


/* sound stuff */
#define SOUND_DEF_BLOCK_SIZE 1024 // 1152 // 1024
#define SOUND_BLOCK_COUNT    4

static short __attribute__((aligned(4))) sndBuffer[SOUND_DEF_BLOCK_SIZE*SOUND_BLOCK_COUNT*2 + 44100/50*2];
static short *snd_playptr = NULL;
static int samples_made = 0, samples_done = 0, samples_block = SOUND_DEF_BLOCK_SIZE;
static int sound_thread_exit = 0;
static SceUID sound_sem = -1;

static void writeSound(int len);

static int sound_thread(SceSize args, void *argp)
{
	short *endptr = &sndBuffer[SOUND_DEF_BLOCK_SIZE*SOUND_BLOCK_COUNT*2];
	int ret;

	lprintf("sound_thread: started, priority %i\n", sceKernelGetThreadCurrentPriority());

	while (!sound_thread_exit)
	{
		if (samples_made - samples_done < samples_block) {
			// wait for data...
			//lprintf("sthr: wait... (%i/%i)\n", samples_done, samples_made);
			ret = sceKernelWaitSema(sound_sem, 1, 0);
			//lprintf("sthr: sceKernelWaitSema: %i\n", ret);
			continue;
		}

		//lprintf("sthr: got data: %i\n", samples_made - samples_done);

		ret = sceAudio_E0727056(PSP_AUDIO_VOLUME_MAX, snd_playptr);

		samples_done += samples_block;
		snd_playptr  += samples_block;
		if (snd_playptr >= endptr)
			snd_playptr = sndBuffer;
		if (ret)
			lprintf("sthr: outf: %i; pos %i/%i\n", ret, samples_done, samples_made);
	}

	lprintf("sthr: exit\n");
	sceKernelExitDeleteThread(0);
	return 0;
}

static void sound_init(void)
{
	SceUID thid;

	sound_sem = sceKernelCreateSema("sndsem", 0, 0, 1, NULL);
	if (sound_sem < 0) lprintf("sceKernelCreateSema() failed: %i\n", sound_sem);

	sound_thread_exit = 0;
	thid = sceKernelCreateThread("sndthread", sound_thread, 0x12, 0x10000, 0, NULL);
	if (thid >= 0)
	{
		sceKernelStartThread(thid, 0, 0);
	}
	else
		lprintf("sceKernelCreateThread failed: %i\n", thid);
}

static void sound_prepare(void)
{
	static int PsndRate_old = 0, PicoOpt_old = 0, pal_old = 0;
	int ret, stereo;

	samples_made = samples_done = 0;

	if (PsndRate != PsndRate_old || (PicoOpt&0x0b) != (PicoOpt_old&0x0b) || Pico.m.pal != pal_old) {
		sound_rerate(Pico.m.frame_count ? 1 : 0);
	}
	stereo=(PicoOpt&8)>>3;
	samples_block = SOUND_DEF_BLOCK_SIZE;
	if (PsndRate < 44100) samples_block = SOUND_DEF_BLOCK_SIZE / 2;
	if (PsndRate < 22050) samples_block = SOUND_DEF_BLOCK_SIZE / 4;

	lprintf("starting audio: %i, len: %i, stereo: %i, pal: %i, block samples: %i\n",
			PsndRate, PsndLen, stereo, Pico.m.pal, samples_block);

	while (sceAudioOutput2GetRestSample() > 0) psp_msleep(100);
	sceAudio_5C37C0AE();
	ret = sceAudio_38553111(samples_block/2, PsndRate, 2/*stereo ? 2 : 1*/);
		lprintf("sceAudio_38553111() ret: %i\n", ret);
	if (ret < 0) {
		lprintf("sceAudio_38553111() failed: %i\n", ret);
		sprintf(noticeMsg, "sound init failed (%i), snd disabled", ret);
		noticeMsgTime = sceKernelGetSystemTimeLow();
		currentConfig.EmuOpt &= ~4;
	} else {
//		int ret = sceAudioSetChannelDataLen(ret, PsndLen); // a try..
//		lprintf("sceAudioSetChannelDataLen: %i\n", ret);
		PicoWriteSound = writeSound;
		memset32((int *)(void *)sndBuffer, 0, sizeof(sndBuffer)/4);
		snd_playptr = sndBuffer;
		PsndOut = sndBuffer;
		PsndRate_old = PsndRate;
		PicoOpt_old  = PicoOpt;
		pal_old = Pico.m.pal;
	}
}

static void sound_end(void)
{
	int ret;
	while (sceAudioOutput2GetRestSample() > 0) psp_msleep(100);
	ret = sceAudio_5C37C0AE();
	lprintf("sound_end: sceAudio_5C37C0AE ret %i\n", ret);
}

static void sound_deinit(void)
{
	sound_thread_exit = 1;
	sceKernelSignalSema(sound_sem, 1);
}

static void writeSound(int len)
{
	int ret;
	short *endptr = &sndBuffer[SOUND_DEF_BLOCK_SIZE*SOUND_BLOCK_COUNT*2];
	if (PicoOpt&8) len<<=1;

	PsndOut += len;
	if (PsndOut > endptr) {
		memcpy32((int *)(void *)sndBuffer, (int *)endptr, (PsndOut - endptr + 1) / 2);
		PsndOut = &sndBuffer[PsndOut - endptr];
	}
	else if (PsndOut == endptr)
		PsndOut = sndBuffer; // happy case

	// signal the snd thread
	samples_made += len;
	if (samples_made - samples_done >= samples_block) {
		if (!Pico.m.scanline) lprintf("signal, %i/%i\n", samples_done, samples_made);
		ret = sceKernelSignalSema(sound_sem, 1);
		if (!Pico.m.scanline) lprintf("signal ret %i\n", ret);
	}
}


static void SkipFrame(void)
{
	PicoSkipFrame=1;
	PicoFrame();
	PicoSkipFrame=0;
}

void emu_forcedFrame(void)
{
	int po_old = PicoOpt;
	int eo_old = currentConfig.EmuOpt;

	PicoOpt &= ~0x0010;
	PicoOpt |=  0x4080; // soft_scale | acc_sprites
	currentConfig.EmuOpt |= 0x80;

	vidResetMode();
	memset32(VRAM_CACHED_STUFF, 0xe0e0e0e0, 512*8/4); // borders
	memset32((int *)VRAM_CACHED_STUFF + 512*232/4, 0xe0e0e0e0, 512*8/4);
	memset32((int *)psp_screen + 512*264*2/4, 0, 512*8*2/4);

	PicoDrawSetColorFormat(-1);
	PicoScan = EmuScanSlow;
	EmuScanPrepare();
	PicoFrameDrawOnly();
	blit1();
	sceGuSync(0,0);

	PicoOpt = po_old;
	currentConfig.EmuOpt = eo_old;
}


static void RunEvents(unsigned int which)
{
	if (which & 0x1800) // save or load (but not both)
	{
		int do_it = 1;

		if ( emu_checkSaveFile(state_slot) &&
				(( (which & 0x1000) && (currentConfig.EmuOpt & 0x800)) || // load
				 (!(which & 0x1000) && (currentConfig.EmuOpt & 0x200))) ) // save
		{
			int keys;
			blit2("", (which & 0x1000) ? "LOAD STATE? (X=yes, O=no)" : "OVERWRITE SAVE? (X=yes, O=no)");
			while( !((keys = psp_pad_read(1)) & (BTN_X|BTN_CIRCLE)) )
				psp_msleep(50);
			if (keys & BTN_CIRCLE) do_it = 0;
			while(  ((keys = psp_pad_read(1)) & (BTN_X|BTN_CIRCLE)) ) // wait for release
				psp_msleep(50);
			clearArea(0);
		}

		if (do_it)
		{
			osd_text(4, (which & 0x1000) ? "LOADING GAME" : "SAVING GAME", 1);
			PicoStateProgressCB = emu_msg_cb;
			emu_SaveLoadGame((which & 0x1000) >> 12, 0);
			PicoStateProgressCB = NULL;
			psp_msleep(0);
		}

		reset_timing = 1;
	}
	if (which & 0x0400) // switch renderer
	{
		if (PicoOpt&0x10) { PicoOpt&=~0x10; currentConfig.EmuOpt |=  0x80; }
		else              { PicoOpt|= 0x10; currentConfig.EmuOpt &= ~0x80; }

		vidResetMode();

		if (PicoOpt&0x10) {
			strcpy(noticeMsg, " 8bit fast renderer");
		} else if (currentConfig.EmuOpt&0x80) {
			strcpy(noticeMsg, "16bit accurate renderer");
		} else {
			strcpy(noticeMsg, " 8bit accurate renderer");
		}

		noticeMsgTime = sceKernelGetSystemTimeLow();
	}
	if (which & 0x0300)
	{
		if(which&0x0200) {
			state_slot -= 1;
			if(state_slot < 0) state_slot = 9;
		} else {
			state_slot += 1;
			if(state_slot > 9) state_slot = 0;
		}
		sprintf(noticeMsg, "SAVE SLOT %i [%s]", state_slot, emu_checkSaveFile(state_slot) ? "USED" : "FREE");
		noticeMsgTime = sceKernelGetSystemTimeLow();
	}
}

static void updateKeys(void)
{
	unsigned int keys, allActions[2] = { 0, 0 }, events;
	static unsigned int prevEvents = 0;
	int i;

	keys = psp_pad_read(0);
	if (keys & PSP_CTRL_HOME)
		sceDisplayWaitVblankStart();

	if (keys & BTN_SELECT)
		engineState = PGS_Menu;

	keys &= CONFIGURABLE_KEYS;

	for (i = 0; i < 32; i++)
	{
		if (keys & (1 << i))
		{
			int pl, acts = currentConfig.KeyBinds[i];
			if (!acts) continue;
			pl = (acts >> 16) & 1;
			if (combo_keys & (1 << i))
			{
				int u = i+1, acts_c = acts & combo_acts;
				// let's try to find the other one
				if (acts_c)
					for (; u < 32; u++)
						if ( (currentConfig.KeyBinds[u] & acts_c) && (keys & (1 << u)) ) {
							allActions[pl] |= acts_c;
							keys &= ~((1 << i) | (1 << u));
							break;
						}
				// add non-combo actions if combo ones were not found
				if (!acts_c || u == 32)
					allActions[pl] |= acts & ~combo_acts;
			} else {
				allActions[pl] |= acts;
			}
		}
	}

	PicoPad[0] = (unsigned short) allActions[0];
	PicoPad[1] = (unsigned short) allActions[1];

	events = (allActions[0] | allActions[1]) >> 16;

	// volume is treated in special way and triggered every frame
	if ((events & 0x6000) && PsndOut != NULL)
	{
		int vol = currentConfig.volume;
		if (events & 0x2000) {
			if (vol < 100) vol++;
		} else {
			if (vol >   0) vol--;
		}
		// FrameworkAudio_SetVolume(vol, vol); // TODO
		sprintf(noticeMsg, "VOL: %02i ", vol);
		noticeMsgTime = sceKernelGetSystemTimeLow();
		currentConfig.volume = vol;
	}

	events &= ~prevEvents;
	if (events) RunEvents(events);
	if (movie_data) emu_updateMovie();

	prevEvents = (allActions[0] | allActions[1]) >> 16;
}

static void find_combos(void)
{
	int act, u;

	// find out which keys and actions are combos
	combo_keys = combo_acts = 0;
	for (act = 0; act < 32; act++)
	{
		int keyc = 0;
		if (act == 16 || act == 17) continue; // player2 flag
		for (u = 0; u < 32; u++)
		{
			if (currentConfig.KeyBinds[u] & (1 << act)) keyc++;
		}
		if (keyc > 1)
		{
			// loop again and mark those keys and actions as combo
			for (u = 0; u < 32; u++)
			{
				if (currentConfig.KeyBinds[u] & (1 << act)) {
					combo_keys |= 1 << u;
					combo_acts |= 1 << act;
				}
			}
		}
	}
}


static void simpleWait(unsigned int until)
{
	unsigned int tval;
	int diff;

	tval = sceKernelGetSystemTimeLow();
	diff = (int)until - (int)tval;
	if (diff >= 512 && diff < 100*1024)
		sceKernelDelayThread(diff);
}

void emu_Loop(void)
{
	char fpsbuff[24]; // fps count c string
	unsigned int tval, tval_prev = 0, tval_thissec = 0; // timing
	int frames_done = 0, frames_shown = 0, oldmodes = 0;
	int target_fps, target_frametime, lim_time, tval_diff, i;
	char *notice = NULL;

	lprintf("entered emu_Loop()\n");

	fpsbuff[0] = 0;

	if (currentConfig.CPUclock != psp_get_cpu_clock()) {
		lprintf("setting cpu clock to %iMHz... ", currentConfig.CPUclock);
		i = psp_set_cpu_clock(currentConfig.CPUclock);
		lprintf(i ? "failed\n" : "done\n");
		currentConfig.CPUclock = psp_get_cpu_clock();
	}

	// make sure we are in correct mode
	vidResetMode();
	clearArea(1);
	Pico.m.dirtyPal = 1;
	oldmodes = ((Pico.video.reg[12]&1)<<2) ^ 0xc;
	find_combos();

	// pal/ntsc might have changed, reset related stuff
	target_fps = Pico.m.pal ? 50 : 60;
	target_frametime = Pico.m.pal ? (1000000<<8)/50 : (1000000<<8)/60+1;
	reset_timing = 1;

	// prepare CD buffer
	if (PicoMCD & 1) PicoCDBufferInit();

	// prepare sound stuff
	PsndOut = NULL;
	if (currentConfig.EmuOpt & 4)
	{
		sound_prepare();
	}

	// loop?
	while (engineState == PGS_Running)
	{
		int modes;

		tval = sceKernelGetSystemTimeLow();
		if (reset_timing || tval < tval_prev) {
			//stdbg("timing reset");
			reset_timing = 0;
			tval_thissec = tval;
			frames_shown = frames_done = 0;
		}

		// show notice message?
		if (noticeMsgTime) {
			static int noticeMsgSum;
			if (tval - noticeMsgTime > 2000000) { // > 2.0 sec
				noticeMsgTime = 0;
				clearArea(0);
				notice = 0;
			} else {
				int sum = noticeMsg[0]+noticeMsg[1]+noticeMsg[2];
				if (sum != noticeMsgSum) { clearArea(0); noticeMsgSum = sum; }
				notice = noticeMsg;
			}
		}

		// check for mode changes
		modes = ((Pico.video.reg[12]&1)<<2)|(Pico.video.reg[1]&8);
		if (modes != oldmodes) {
			oldmodes = modes;
			clearArea(1);
			set_scaling_params();
		}

		// second passed?
		if (tval - tval_thissec >= 1000000)
		{
#ifdef BENCHMARK
			static int bench = 0, bench_fps = 0, bench_fps_s = 0, bfp = 0, bf[4];
			if(++bench == 10) {
				bench = 0;
				bench_fps_s = bench_fps;
				bf[bfp++ & 3] = bench_fps;
				bench_fps = 0;
			}
			bench_fps += frames_shown;
			sprintf(fpsbuff, "%02i/%02i/%02i", frames_shown, bench_fps_s, (bf[0]+bf[1]+bf[2]+bf[3])>>2);
#else
			if(currentConfig.EmuOpt & 2)
				sprintf(fpsbuff, "%02i/%02i", frames_shown, frames_done);
#endif
			tval_thissec += 1000000;

			if (currentConfig.Frameskip < 0) {
				frames_done  -= target_fps; if (frames_done  < 0) frames_done  = 0;
				frames_shown -= target_fps; if (frames_shown < 0) frames_shown = 0;
				if (frames_shown > frames_done) frames_shown = frames_done;
			} else {
				frames_done = frames_shown = 0;
			}
		}
#ifdef PFRAMES
		sprintf(fpsbuff, "%i", Pico.m.frame_count);
#endif

		tval_prev = tval;
		lim_time = (frames_done+1) * target_frametime;
		if (currentConfig.Frameskip >= 0) // frameskip enabled
		{
			for (i = 0; i < currentConfig.Frameskip; i++) {
				updateKeys();
				SkipFrame(); frames_done++;
				if (PsndOut) { // do framelimitting if sound is enabled
					int tval_diff;
					tval = sceKernelGetSystemTimeLow();
					tval_diff = (int)(tval - tval_thissec) << 8;
					if (tval_diff < lim_time) // we are too fast
						simpleWait(tval + ((lim_time - tval_diff)>>8));
				}
				lim_time += target_frametime;
			}
		}
		else // auto frameskip
		{
			int tval_diff;
			tval = sceKernelGetSystemTimeLow();
			tval_diff = (int)(tval - tval_thissec) << 8;
			if (tval_diff > lim_time)
			{
				// no time left for this frame - skip
				if (tval_diff - lim_time >= (300000<<8)) {
					/* something caused a slowdown for us (disk access? cache flush?)
					 * try to recover by resetting timing... */
					reset_timing = 1;
					continue;
				}
				updateKeys();
				SkipFrame(); frames_done++;
				continue;
			}
		}

		updateKeys();

		if (!(PicoOpt&0x10))
			EmuScanPrepare();

		PicoFrame();

		blit2(fpsbuff, notice);

		// check time
		tval = sceKernelGetSystemTimeLow();
		tval_diff = (int)(tval - tval_thissec) << 8;

		if (currentConfig.Frameskip < 0 && tval_diff - lim_time >= (300000<<8)) // slowdown detection
			reset_timing = 1;
		else if (PsndOut != NULL || currentConfig.Frameskip < 0)
		{
			// sleep if we are still too fast
			if (tval_diff < lim_time)
			{
				// we are too fast
				simpleWait(tval + ((lim_time - tval_diff) >> 8));
			}
		}

		frames_done++; frames_shown++;
	}


	if (PicoMCD & 1) PicoCDBufferFree();

	if (PsndOut != NULL) {
		PsndOut = NULL;
		sound_end();
	}

	// save SRAM
	if ((currentConfig.EmuOpt & 1) && SRam.changed) {
		emu_msg_cb("Writing SRAM/BRAM..");
		emu_SaveLoadGame(0, 1);
		SRam.changed = 0;
	}

	// draw a frame for bg..
	emu_forcedFrame();
}


void emu_ResetGame(void)
{
	PicoReset(0);
	reset_timing = 1;
}

