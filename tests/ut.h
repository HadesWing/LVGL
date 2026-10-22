/**
 * @file ut.h
 * @brief 极简单元测试框架 —— 零依赖，PC 和板子上跑的是同一份
 *
 * 为什么不用 Unity / CMocka / cmocka：
 *   1) 目标机（RK3308）上要能跑同一套用例。凡是要 cmake 拉包、要 libc 之外的
 *      依赖，交叉编译时都是一堆新问题；这里的框架只有一个 .h + 一个 .c。
 *   2) 断言失败**不终止**进程，跑完所有用例再汇总。C 里最烦的就是"第一处失败
 *      就 abort，后面 20 个用例根本不知道有没有问题"。
 *   3) 解析器类用例很容易踩野指针，框架顺手接了 SIGSEGV：崩掉的用例会被标记
 *      CRASH 并继续跑下一个，而不是让整个测试进程死掉。
 *
 * 用法：
 *   在 ut_xxx.c 里：
 *       #include "ut.h"
 *       UT_TEST(g711, encode_decode_roundtrip)
 *       {
 *           int16_t pcm[160] = {0};
 *           uint8_t enc[160];
 *           g711_encode_alaw(pcm, enc, 160);
 *           UT_ASSERT_EQ_INT(0xA8, enc[0]);
 *       }
 *   框架靠 GCC 的 constructor 属性自动注册，不需要维护一张用例表 ——
 *   漏注册是最常见的"测试写了但没跑"。
 *
 *   编译：见 tests/CMakeLists.txt  →  make ut
 *   运行：bin/ut.exe            全部
 *         bin/ut.exe g711       只跑 suite 名含 "g711" 的
 *         bin/ut.exe -v         打印每个用例的耗时
 */
#ifndef UT_H
#define UT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ut_fn_t)(void);

/* 由框架实现，用例代码只用宏 */
void ut_register(const char *suite, const char *name, ut_fn_t fn);
void ut_report_fail(const char *file, int line, const char *fmt, ...);
int  ut_run_all(int argc, char **argv);

/*---------------------------------------------------------------------------
 * 用例声明
 *
 * constructor 属性在进程启动（main 之前）就会执行，所以注册永远先于 main，
 * 也就不存在"忘了在 main 里调用 xxx_register()"这种漏测。
 *---------------------------------------------------------------------------*/
#if defined(__GNUC__) || defined(__clang__)
#  define UT_TEST(suite, name)                                                  \
      static void ut_case_##suite##_##name(void);                               \
      static void ut_reg_##suite##_##name(void) __attribute__((constructor));    \
      static void ut_reg_##suite##_##name(void) {                               \
          ut_register(#suite, #name, ut_case_##suite##_##name); }                \
      static void ut_case_##suite##_##name(void)
#else
#  error "UT_TEST 依赖 __attribute__((constructor))；换编译器请改成显式注册表"
#endif

/*---------------------------------------------------------------------------
 * 断言
 *
 * 全部"记录失败并继续"，不 abort —— 一次跑完能看到所有问题。
 *---------------------------------------------------------------------------*/
#define UT_FAIL(fmt, ...) \
    ut_report_fail(__FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define UT_ASSERT(cond)                                                        \
    do {                                                                       \
        if (!(cond)) ut_report_fail(__FILE__, __LINE__, "断言不成立: %s", #cond); \
    } while (0)

#define UT_ASSERT_MSG(cond, msg)                                               \
    do {                                                                       \
        if (!(cond)) ut_report_fail(__FILE__, __LINE__, "%s（表达式 %s）", msg, #cond); \
    } while (0)

/* 整数相等：故意用 long long 收口，避免 int/uint16 比较时的符号提升坑 */
#define UT_EQ_INT(exp, act)                                                    \
    do {                                                                       \
        long long _e = (long long)(exp), _a = (long long)(act);                 \
        if (_e != _a)                                                          \
            ut_report_fail(__FILE__, __LINE__,                                 \
                           "期望 %s == %lld，实际 %lld", #act, _e, _a);          \
    } while (0)

#define UT_NE_INT(exp, act)                                                    \
    do {                                                                       \
        long long _e = (long long)(exp), _a = (long long)(act);                 \
        if (_e == _a)                                                          \
            ut_report_fail(__FILE__, __LINE__,                                 \
                           "%s 不应等于 %lld", #act, _e);                       \
    } while (0)

/* 区间：闭区间 [lo, hi]，专治"查表结果差一个量化级"这类边界 */
#define UT_IN_RANGE(act, lo, hi)                                               \
    do {                                                                       \
        long long _a = (long long)(act), _lo = (long long)(lo), _hi = (long long)(hi); \
        if (_a < _lo || _a > _hi)                                              \
            ut_report_fail(__FILE__, __LINE__,                                 \
                           "%s = %lld，不在 [%lld, %lld] 内", #act, _a, _lo, _hi); \
    } while (0)

#define UT_EQ_STR(exp, act)                                                    \
    do {                                                                       \
        const char *_e = (exp), *_a = (act);                                    \
        if (!_e || !_a || strcmp(_e, _a) != 0)                                  \
            ut_report_fail(__FILE__, __LINE__,                                 \
                           "期望 \"%s\"，实际 \"%s\"",                          \
                           _e ? _e : "(null)", _a ? _a : "(null)");             \
    } while (0)

#define UT_MEM_EQ(exp, act, n)                                                 \
    do {                                                                       \
        const void *_e = (exp), *_a = (act);                                    \
        size_t _n = (size_t)(n);                                                \
        if (!_e || !_a || memcmp(_e, _a, _n) != 0)                              \
            ut_report_fail(__FILE__, __LINE__,                                 \
                           "%s 与 %s 前 %zu 字节不一致", #exp, #act, _n);        \
    } while (0)

/* 浮点近似（PSNR、增益这类量用得上） */
#define UT_NEAR(exp, act, tol)                                                 \
    do {                                                                       \
        double _e = (double)(exp), _a = (double)(act), _t = (double)(tol);       \
        double _d = (_e > _a) ? (_e - _a) : (_a - _e);                           \
        if (_d > _t)                                                           \
            ut_report_fail(__FILE__, __LINE__,                                 \
                           "期望 %s ≈ %.6f（容差 %.6f），实际 %.6f",            \
                           #act, _e, _t, _a);                                  \
    } while (0)

/* 一行一个用例的耗时（-v 时才打） */
void ut_note_elapsed(uint32_t us);

#ifdef __cplusplus
}
#endif

#endif /* UT_H */
