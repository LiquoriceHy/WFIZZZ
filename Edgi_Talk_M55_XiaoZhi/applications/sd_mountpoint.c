/*
 * sd_mountpoint.c - 为 SD 卡提供 romfs 挂载点目录 /sdcard
 *
 * 功能：
 *   定义强符号 romfs_root 覆盖 RT-Thread romfs 的 rt_weak 默认根目录，
 *   使 "/" 下存在 "sdcard" 目录项，dfs_mount(..., "/sdcard", ...) 才能成功。
 *
 * 需要本文件的原因（2026-10-26 定位）：
 *   BSP 的 mnt.c 将 _romfs_root（含 flash 与 sdcard 两个目录项）整段包在
 *   #ifndef BSP_USING_XiaoZhi 中，而本工程定义了该宏（rtconfig.h:586），
 *   因此实际链入固件的是 rt-thread romfs.c 的 rt_weak romfs_root，
 *   内容仅有 dummy/dummy.txt。nm rt-thread.elf 可见 `V romfs_root`（弱符号）。
 *   而 dfs_mount 在 dfs_fs.c:265-282 会先 open(path, O_DIRECTORY) 校验
 *   挂载点是否存在，失败即返回 -ENOTDIR。两者叠加导致 SD 卡始终无法挂载
 *   （2026-10-23 真机已验证：/dev/sd 出现、sdcard_mkfs 能格式化，但挂载必失败）。
 *
 * 使用强符号覆盖而非修改 BSP 的原因：
 *   romfs_root 本就是 rt_weak（romfs.c:34），设计上即供应用覆盖。
 *   本文件仅位于 applications/，不修改 mnt.c，不改 rtconfig.h 与 Kconfig，
 *   不涉及非应用文件的修改。
 *
 * 空目录项的安全性：
 *   size=0 且 data=NULL 的目录项可正常通过 check_dirent（仅拒绝 size == ~0U），
 *   dfs_romfs_lookup 匹配到最后一级后直接返回 dirent 而不解引用 data
 *   （dfs_romfs.c:120-124），dfs_romfs_open 在 O_DIRECTORY 下只记录
 *   vnode->data/size（dfs_romfs.c:242-262）。因此此处传 RT_NULL 是安全的，
 *   与 BSP mnt.c 的原生写法一致。
 *
 * 挂载后自愈：
 *   mnt.c 的 sd_hotplug_thread 每 1s 轮询，未挂载时反复尝试；本文件使挂载点
 *   始终存在，插卡或热插拔后会自动挂载，无需手动干预。
 *
 * 约束与结论：
 *   - 若将来取消 mnt.c 中的 BSP_USING_XiaoZhi 屏蔽，该处也会定义
 *     romfs_root，与本文件构成重复定义（链接期报错）。届时删除本文件即可。
 *   - 有意不提供 "flash" 目录项：M55 侧 _fal_mount() 被跳过，/flash 归 M33
 *     独占，本核擦写共享 NOR 会打断 CM33 的 XIP 取指（2026-09-28 实测）。
 */

#include <rtthread.h>
#include <dfs_romfs.h>

/* 只暴露 SD 卡挂载点；内容由 elmfat 在挂载后接管 */
static const struct romfs_dirent _sd_romfs_root[] =
{
    {ROMFS_DIRENT_DIR, "sdcard", RT_NULL, 0},
};

/* 强符号，覆盖 rt-thread romfs.c 中的 rt_weak 同名符号 */
const struct romfs_dirent romfs_root =
{
    ROMFS_DIRENT_DIR, "/",
    (rt_uint8_t *)_sd_romfs_root,
    sizeof(_sd_romfs_root) / sizeof(_sd_romfs_root[0])
};
