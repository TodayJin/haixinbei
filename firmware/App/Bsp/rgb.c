/* RGB 灯：新板不做 RGB，保留同名空实现让上层不用改 */
#include "rgb.h"

void RGB_on_r(GPIO_PinState x, GPIO_PinState y, GPIO_PinState z) { (void)x; (void)y; (void)z; }
void RGB_on_l(GPIO_PinState x, GPIO_PinState y, GPIO_PinState z) { (void)x; (void)y; (void)z; }
void RGB_off_r(void) { }
void RGB_off_l(void) { }
void RGB_blink(void) { }
void RGB_folg(void) { }
