/**
 * @file sip_md5.h
 * @brief SIP Digest 认证用的 MD5（RFC 1321 紧凑实现，仅本模块使用）
 *
 * 注：MD5 在 TLS 证书等安全场景已不安全，但 RFC 3261 Digest 摘要规定的
 *     就是 MD5/AKA-MD5，属于协议要求，这里仅用于 SIP REGISTER 鉴权。
 */
#ifndef SIP_MD5_H
#define SIP_MD5_H

#include <stddef.h>

/* 计算 32 字符小写十六进制摘要（out 需 33 字节） */
void md5_hex(const void *data, size_t len, char out[33]);

/* 便捷版：对 NUL 结尾字符串 */
void md5_hex_str(const char *str, char out[33]);

#endif /* SIP_MD5_H */
