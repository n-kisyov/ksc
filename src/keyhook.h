#ifndef KEYHOOK_H
#define KEYHOOK_H

[[nodiscard]] bool keyhook_start(void);
void keyhook_stop(void);
const char *keyhook_get_name(unsigned int vk_code);
void keyhook_set_keylogger_enabled(bool enabled);
void keyhook_get_wpm(int *out_wpm, int *out_peak_wpm);
void keyhook_reset_wpm(void);

#endif
