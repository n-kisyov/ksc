#ifndef KEYHOOK_H
#define KEYHOOK_H

int keyhook_start(void);
void keyhook_stop(void);
const char *keyhook_get_name(unsigned int vk_code);

#endif
