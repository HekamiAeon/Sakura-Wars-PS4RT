/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef PS4RTGPU_H
#define PS4RTGPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} Ps4RtGpuConfig;
/* Registers kernel event queues (needed with or without graphics). */
void ps4rtgpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int ps4rtgpu_init(const Ps4RtGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t ps4rtgpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int ps4rtgpu_handle_fault(void *ucontext, void *address);
/* PS4RT_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void ps4rtgpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
int ps4rtgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8);
int ps4rtgpu_text_input_poll(char *out_utf8, uint64_t size);
/* A list for the player to choose from, drawn over the game (save list). images: a PNG path per
 * item drawn beside it (NULL array or entry: none). begin: 0 when no window exists.
 * poll: -1 while open, -2 cancelled, else the chosen index. */
int ps4rtgpu_choice_begin(const char *title, const char *const *items, const char *const *images, int count, int focus);
int ps4rtgpu_choice_poll(void);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int ps4rtgpu_overlay_captures_input(void);
/* Controller vibration strength set in the menu, 0 (off) to 100 (the game's own). */
int ps4rtgpu_vibration_percent(void);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned ps4rtgpu_symbol_count(void);
#ifdef __cplusplus
}
#endif
#endif
