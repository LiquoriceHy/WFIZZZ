#pragma once

/* LED：受控对象是 GPIO3 外接 LED（高电平点亮），
 * 板载 WS2812（GPIO8）同状态镜像亮绿作板内可视反馈。 */

void led_ctrl_init(void);
void led_ctrl_set(int on);
int  led_ctrl_is_on(void);
