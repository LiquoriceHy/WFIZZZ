/*
 * maimemo_cfg.h - 【已停用】墨墨背单词配置
 *
 * 2026-10-26 起凭据不再编译进固件：
 *   地址与 token 改为运行时从 SD 卡的 /sdcard/wfizzz/cfg.json 读取
 *   （配网页 http://<板子IP>/ 填写），字段名 memo_host / memo_token。
 *   读取点见 maimemo.c 的 mm_post_collect() 与 mm_entry()。
 *
 * 为什么必须这么做：
 *   此处原本硬编码的 token 具备"读写云词本内容"的权限（scope 含
 *   open.memo.content），而本工作区为 git 仓库。凭据一旦进入版本历史，
 *   即使删除文件也无法消除，该 token 必须视为已泄露并轮换。
 *
 * 本文件已不再被 maimemo.c 包含。保留为空占位，用于明确告知后来者
 * "凭据不在此处"，而不是让该文件名静默消失。 */
#ifndef __MAIMEMO_CFG_H__
#define __MAIMEMO_CFG_H__

/* 空值：maimemo.c 会判为"未配置"并静默等待，不会发无效请求 */
#define MEMO_HOST       "open.maimemo.com"
#define MEMO_TOKEN      ""

#endif /* __MAIMEMO_CFG_H__ */
