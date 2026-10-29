#pragma once

/* 命令分发：解析 {"cmd":...} 并执行，回包经 mqtt_link_publish_reply
 * 发出，原文同步记到状态屏（status_ui_set_cmd/set_reply）。 */

void dev_cmd_handle(const char *data, int len);
