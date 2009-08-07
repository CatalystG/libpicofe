// (c) Copyright 2006-2007 notaz, All rights reserved.
// Free for non-commercial use.

// For commercial use, separate licencing terms must be obtained.

#include "port_config.h"

#ifdef __cplusplus
extern "C" {
#endif

extern void *g_screen_ptr;

#if SCREEN_SIZE_FIXED
#define g_screen_width  SCREEN_WIDTH
#define g_screen_height SCREEN_HEIGHT
#else
extern int g_screen_width;
extern int g_screen_height;
#endif


#define EOPT_EN_SRAM      (1<<0)
#define EOPT_SHOW_FPS     (1<<1)
#define EOPT_EN_SOUND     (1<<2)
#define EOPT_GZIP_SAVES   (1<<3)
#define EOPT_MMUHACK      (1<<4)
#define EOPT_NO_AUTOSVCFG (1<<5)
#define EOPT_16BPP        (1<<7)
#define EOPT_RAM_TIMINGS  (1<<8)
#define EOPT_CONFIRM_SAVE (1<<9)
#define EOPT_EN_CD_LEDS   (1<<10)
#define EOPT_CONFIRM_LOAD (1<<11)
#define EOPT_A_SN_GAMMA   (1<<12)
#define EOPT_VSYNC        (1<<13)
#define EOPT_GIZ_SCANLN   (1<<14)
#define EOPT_GIZ_DBLBUF   (1<<15)
#define EOPT_VSYNC_MODE   (1<<16)
#define EOPT_SHOW_RTC     (1<<17)
#define EOPT_NO_FRMLIMIT  (1<<18)
#define EOPT_WIZ_TEAR_FIX (1<<19)

enum {
	EOPT_SCALE_NONE = 0,
	EOPT_SCALE_SW_H,
	EOPT_SCALE_HW_H,
	EOPT_SCALE_HW_HV,
};

typedef struct _currentConfig_t {
	int EmuOpt;
	int s_PicoOpt;
	int s_PsndRate;
	int s_PicoRegion;
	int s_PicoAutoRgnOrder;
	int s_PicoCDBuffers;
	int Frameskip;
	int CPUclock;
	int volume;
	int gamma;
	int scaling;  // gp2x: 0=center, 1=hscale, 2=hvscale, 3=hsoftscale; psp: bilinear filtering
	int rotation; // for UIQ
	float scale; // psp: screen scale
	float hscale32, hscale40; // psp: horizontal scale
	int gamma2;  // psp: black level
	int turbo_rate;
} currentConfig_t;

extern currentConfig_t currentConfig, defaultConfig;
extern char *PicoConfigFile;
extern int rom_loaded;
extern int state_slot;
extern int config_slot, config_slot_current;
extern unsigned char *movie_data;
extern int reset_timing;

#define PICO_PEN_ADJUST_X 4
#define PICO_PEN_ADJUST_Y 2
extern int pico_pen_x, pico_pen_y;
extern int pico_inp_mode;

extern char rom_fname_reload[512];		// ROM to try loading on next PGS_ReloadRom
extern char rom_fname_loaded[512];		// currently loaded ROM filename

// engine states
extern int engineState;
enum TPicoGameState {
	PGS_Paused = 1,
	PGS_Running,
	PGS_Quit,
	PGS_KeyConfig,
	PGS_ReloadRom,
	PGS_Menu,
	PGS_RestartRun,
	PGS_Suspending,		/* PSP */
	PGS_SuspendWake,	/* PSP */
};


void  emu_init(void);
void  emu_finish(void);
void  emu_loop(void);

int   emu_reload_rom(char *rom_fname);
int   emu_swap_cd(const char *fname);
int   emu_save_load_game(int load, int sram);
void  emu_reset_game(void);

void  emu_set_defconfig(void);
int   emu_read_config(int game, int no_defaults);
int   emu_write_config(int game);

char *emu_get_save_fname(int load, int is_sram, int slot);
int   emu_check_save_file(int slot);
void  emu_setSaveStateCbs(int gz);

void  emu_text_out8 (int x, int y, const char *text);
void  emu_text_out16(int x, int y, const char *text);
void  emu_text_out8_rot (int x, int y, const char *text);
void  emu_text_out16_rot(int x, int y, const char *text);

void  emu_make_path(char *buff, const char *end, int size);
void  emu_update_input(void);
void  emu_get_game_name(char *str150);
void  emu_set_fastforward(int set_on);
void  emu_status_msg(const char *format, ...);

#ifdef __cplusplus
} // extern "C"
#endif

