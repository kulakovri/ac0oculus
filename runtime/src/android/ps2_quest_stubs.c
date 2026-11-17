/* ps2_quest_stubs.c -- the entry points of the Dear ImGui files (ps2_ui.cpp: the F4 settings page; ps2_launcher.cpp:
   the launcher window) for the Quest standalone build, which has neither (no window toolkit, NOTES_QUEST.md stage 2).
   What is lost (BUGS_QUEST.md): the F4 page (settings come from files/ac5_settings.ini and commandline.txt; the
   game's own ENHANCEMENTS page in the pause menu still works, it is drawn by the game), the launcher (the disc path
   comes from the data root), the FPS/menu overlay of the desktop window. */
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include "ps2_ui.h"

int  ps2_ui_init(const ps2_ui_init_info *info) { (void)info; return -1; }
void ps2_ui_shutdown(void) {}
int  ps2_ui_event(const SDL_Event *e) { (void)e; return 0; }
int  ps2_ui_visible(void) { return 0; }
void ps2_ui_set_visible(int on) { (void)on; }
int  ps2_ui_blocks_game_input(void) { return 0; }
void ps2_ui_draw(VkCommandBuffer cmd) { (void)cmd; }
void ps2_ui_swapchain_changed(uint32_t image_count) { (void)image_count; }
void ps2_ui_hotas_test(int row, int slot) { (void)row; (void)slot; }

int  ps2_launcher_run(int hidden) { (void)hidden; return 0; }
SDL_Window *ps2_launcher_take_window(const char *title) { (void)title; return NULL; }
int  ps2_launcher_take_thread(void *(*fn)(void *), void *arg, pthread_t *out) { (void)fn; (void)arg; (void)out; return 0; }
