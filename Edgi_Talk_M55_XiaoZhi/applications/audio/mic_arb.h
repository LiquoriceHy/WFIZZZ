/*
 * mic_arb.h - mic0 的唯一仲裁者
 *
 * 需要仲裁的原因：drv_pdm 通过 rt_audio_register 只保存一个 rx_done 回调
 * （drv_pdm.c:430），后 open 的一方会覆盖前一方的接收通路。两个模块各自
 * 调用 rt_device_open("mic0") 的结果是：均返回成功，但只有最后一个能收到
 * 数据，另一个处于静默空读状态，比直接失败更难排查。
 *
 * 因此所有 mic0 使用者都必须先 acquire，退出时 release。仲裁器保证
 * 同一时刻只有一个持有者，并在切换持有者时真正 close/open 设备。
 *
 * 抢占策略：AI 聊天（交互式，用户正在等待）优先于鼾声检测（后台长期运行）。
 */
#ifndef MIC_ARB_H
#define MIC_ARB_H

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    MIC_ARB_NONE = 0,
    MIC_ARB_SNORE,          /* snore_rt 鼾声检测 */
    MIC_ARB_AUDIO_EVENT,    /* audio_event 电平测试 */
    MIC_ARB_XIAOZHI,        /* 小智语音对话 */
} mic_arb_owner_t;

/* 申请 mic0。成功返回 RT_EOK，owner 成为当前持有者。
 * 若已被别的 owner 占用：
 *   - 新请求方优先级更高 → 先让占地方释放（调其 stop 回调），等待退出后接管
 *   - 否则返回 -RT_EBUSY
 * 已持有者重复申请同一设备返回 RT_EOK（幂等）。 */
rt_err_t mic_arb_acquire(mic_arb_owner_t owner);

/* 释放 mic0。只有当前持有者调用才生效，其他 owner 调用被忽略
 * （避免抢占过程中旧线程的清理把新持有者的设备关掉）。 */
void mic_arb_release(mic_arb_owner_t owner);

/* 当前持有者；无人持有返回 MIC_ARB_NONE。 */
mic_arb_owner_t mic_arb_owner(void);

/* 由各使用者注册"被抢占时如何停下"。仲裁器在抢占前调用它，
 * 然后轮询 owner() 直到对方释放（最多 timeout_ms）。 */
void mic_arb_set_stop_cb(mic_arb_owner_t owner, void (*cb)(void));

#ifdef __cplusplus
}
#endif

#endif /* MIC_ARB_H */
