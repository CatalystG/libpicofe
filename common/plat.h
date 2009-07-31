#ifdef __cplusplus
extern "C" {
#endif

/* stuff to be implemented by platform code */
extern char cpu_clk_name[];

void pemu_prep_defconfig(void);
void pemu_loop_prep(void);
void pemu_loop_end(void);
void pemu_forced_frame(int opts);
void pemu_video_mode_change(int is_32col, int is_240_lines);
void pemu_update_display(const char *fps, const char *notice_msg);

void pemu_sound_start(void);
void pemu_sound_stop(void);
void pemu_sound_wait(void);

void menu_romload_prepare(const char *rom_name);
void menu_romload_end(void);

void plat_early_init(void);
void plat_init(void);
void plat_finish(void);

/* return the dir/ where configs, saves, bios, etc. are found */
int  plat_get_root_dir(char *dst, int len);

/* used before things blocking for a while (these funcs redraw on return) */
void plat_status_msg_busy_first(const char *msg);
void plat_status_msg_busy_next(const char *msg);
void plat_status_msg_clear(void);

/* menu: enter (switch bpp, etc), begin/end drawing */
void plat_video_menu_enter(int is_rom_loaded);
void plat_video_menu_begin(void);
void plat_video_menu_end(void);

void plat_video_wait_vsync(void);
void plat_video_toggle_renderer(int is_next, int is_menu);
void plat_validate_config(void);
void plat_update_volume(int has_changed, int is_up);

int  plat_is_dir(const char *path);
int  plat_wait_event(int *fds_hnds, int count, int timeout_ms);
void plat_sleep_ms(int ms);

/* timers, to be used for time diff and must refet to the same clock */
unsigned int plat_get_ticks_ms(void);
unsigned int plat_get_ticks_us(void);
void plat_wait_till_us(unsigned int us);

const char  *plat_get_credits(void);
void plat_debug_cat(char *str);

#ifdef __cplusplus
} // extern "C"
#endif

