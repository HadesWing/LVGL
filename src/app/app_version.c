/**
 * @file app_version.c
 * @brief 版本与构建信息（见 app_version.h）
 */
#include "app_version.h"

const char *app_version(void)
{
    return APP_VERSION;
}

const char *app_build_info(void)
{
    /* 静态缓冲：只读一次，之后反复调用返回同一个指针。
       构建信息里 __DATE__/__TIME__ 是编译期常量，运行时不会变。 */
    static char s_buf[192];
    static int  s_ready;

    if (!s_ready) {
#if defined(__clang__)
        const char *cc = "clang " __clang_version__;
#elif defined(__GNUC__)
        const char *cc = "gcc " __VERSION__;
#elif defined(_MSC_VER)
        const char *cc = "msvc";
#else
        const char *cc = "unknown-cc";
#endif
        /* 手工拼，不引 snprintf 省一个 stdio 依赖（板子上也可能用到） */
        int n = 0;
        const char *parts[5] = { APP_VERSION, "  built ", __DATE__, " ", __TIME__ };
        for (int i = 0; i < 5; i++)
            for (const char *p = parts[i]; *p && n < (int)sizeof(s_buf) - 1; p++)
                s_buf[n++] = *p;

        s_buf[n++] = ' ';
        s_buf[n++] = '(';
        for (const char *p = cc; *p && n < (int)sizeof(s_buf) - 2; p++)
            s_buf[n++] = *p;
        s_buf[n++] = ')';
        s_buf[n]   = '\0';
        s_ready = 1;
    }
    return s_buf;
}
