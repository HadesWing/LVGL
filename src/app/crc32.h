/**
 * @file crc32.h
 * @brief CRC-32（IEEE 802.3，和 zlib.crc32 同一个多项式）
 *
 * 用途只有一个：给"上屏/落盘的每一帧"打指纹，写进 sidecar 日志，
 * 事后和参考码流逐帧比对，证明 **丢包恢复之后写出去的那些帧，没有一帧是脏的**。
 *
 * 为什么用 CRC 而不是直接比像素：
 *   丢包恢复会跳帧，落盘文件的第 N 帧已经不对应参考文件的第 N 帧了，
 *   逐帧对齐比会整体错位。而 CRC 是"内容指纹"，天然免疫对齐问题：
 *   只要文件里的每一帧都能在参考集合里找到同样指纹的帧，就说明
 *   每个写出去的帧都是完好的；反过来，被污染的解码结果 CRC 一定落在集合外。
 *
 * 为什么不用 zlib：目标机不想为了一个校验再挂一个库，而且这个多项式
 *   是公开标准，四十行就写完了。
 *
 * 为什么多项式用 0xEDB88320（反射版）：选这个是为了和 zlib/Python 的
 *   zlib.crc32 结果一致 —— 验证脚本是 Python 写的，两边必须对得上，
 *   否则天天怀疑"是不是 CRC 算错了"而不是查真正的 bug。
 */
#ifndef CRC32_H
#define CRC32_H

#include <stddef.h>
#include <stdint.h>

/* 一次性算完（内部会惰性建表；建表是幂等的，见 crc32.c 的说明） */
uint32_t crc32_buf(const void *data, size_t len);

/**
 * 分段累加（按平台分行拷帧的时候用：三个平面 3×h 行，逐行 update）。
 *
 * 用法：crc = crc32_update(0, p1, n1); crc = crc32_update(crc, p2, n2); …
 * 等价于把两段拼起来算一次的**返回值**（内部处理了首尾取反，所以不能
 * 直接把 crc32_buf 的返回值当中间状态 —— 这是最容易写错的地方）。
 */
uint32_t crc32_update(uint32_t crc, const void *data, size_t len);

#endif /* CRC32_H */
