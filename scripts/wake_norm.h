/**
 * wake_norm.h - 唤醒词同音字归一 + 命中判定（端侧可直接使用）
 *
 * 设计要点：
 *   1. 所有替换规则都是「等长字节替换」（2 个汉字 → 2 个汉字，UTF-8 下都是 6 字节），
 *      因此可以原地修改缓冲区，零额外内存、零分配。
 *   2. 规则分两级：
 *        HARD - 裸替换。这些串在日常语料里不会独立出现，替换安全。
 *        SOFT - 必须带「你好/你行」锚点。防止「我想吃饭」被替换成「我小智饭」造成误唤醒。
 *   3. 命中判定先去标点再比较，所以「你好，小智。」与「你好小智」等价。
 *
 * 实测（PC, sherpa-onnx）：
 *   ZipCTC 60MB   : 小智 25% -> 100%，兮兮 0% -> 33%
 *   SenseVoice 228MB: 小智 75% -> 75%，兮兮 0% -> 83%
 *   20 句无关指令误唤醒 0/20
 */
#ifndef WAKE_NORM_H
#define WAKE_NORM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 原地做同音字归一。返回 1 表示发生了替换，0 表示无变化。
 *  @param text 可修改的 UTF-8 缓冲区（NUL 结尾），长度不变
 *  @note 本函数【不处理标点】。文本里有「，。！？」时锚点规则（如"你好谢谢"）
 *        匹配不上，请改用 wake_is_wake / wake_is_wake_any。 */
int wake_norm_inplace(char *text);

/** 判断 text 中是否出现唤醒词（自动跳过标点与空白）。
 *  @param text 识别结果，UTF-8
 *  @param word 目标词，如 "你好小智"
 *  @return 1 命中，0 未命中 */
int wake_contains(const char *text, const char *word);

/** 便捷入口：先去标点 -> 归一 -> 判定 */
int wake_is_wake(const char *text, const char *word);

/** 多词表判定：任一命中即返回 1（与 Python 侧 keys 列表语义一致）
 *  典型用法：words = {"你好小智", "小智"} */
int wake_is_wake_any(const char *text, const char *const *words, size_t n_words);

#ifdef __cplusplus
}
#endif
#endif /* WAKE_NORM_H */
