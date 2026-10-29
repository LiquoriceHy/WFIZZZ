/*
 * xiaozhi_port.h - 小智客户端适配层对外接口（xiaozhi.cpp 调用的补充入口）
 */
#ifndef __XIAOZHI_PORT_H__
#define __XIAOZHI_PORT_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 用户语音识别结果（stt）上屏：以 role=用户 写入聊天通道。
 * xiaozhi.cpp 的 stt 分支调用（原版仅打日志）。 */
void xiaozhi_port_on_stt(const char *text);

/* 把 ES8388 音量设到对话可听的档位。原先这里是开机的
 * INIT_APP_EXPORT 静音，会连带掐掉 TTS 与其他音频，
 * 改为由 xz_app_start 显式调用。 */
void xz_port_audio_unmute(void);

#ifdef __cplusplus
}
#endif

#endif /* __XIAOZHI_PORT_H__ */
