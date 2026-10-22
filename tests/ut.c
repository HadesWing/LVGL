/**
 * @file ut.c
 * @brief UT 框架实现 + 入口（注册表 / 断言 / 崩溃兜底 / 统计）
 *
 * 设计取舍见 ut.h 顶部注释。这里只强调两点实现细节：
 *
 * 1) 崩溃兜底：解析器、内存拷贝这类用例一旦踩野指针，默认行为是整个测试进程
 *    直接死掉 —— 后面的用例全白写了。这里用 signal + longjmp 把"当前用例"
 *    标成 CRASH 然后接着跑下一个。longjmp 出信号处理器严格来说不是可移植行为，
 *    但对测试进程足够了（它就活这一瞬间），换来的是"一次跑完全部用例"。
 *
 * 2) 每个用例最多打 10 条失败。循环里断言失败会刷出几百行，把真正有用的
 *    第一条挤没影；超过 10 条后只计数不再打印。
 */
#include "ut.h"

#include <signal.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#endif

#define UT_MAX_MSG_PER_CASE 10

typedef struct {
    const char *suite;
    const char *name;
    ut_fn_t     fn;
} ut_case_t;

static ut_case_t *s_cases;
static int        s_count;
static int        s_cap;

static int  s_verbose;
static int  s_fail_msgs;      /* 当前用例已打印的失败条数 */
static int  s_cur_fails;      /* 当前用例失败断言数 */
static int  s_cur_crashed;

static jmp_buf            s_jmp;
static volatile sig_atomic_t s_in_case;

/*---------------------------------------------------------------------------
 * 注册
 *---------------------------------------------------------------------------*/
void ut_register(const char *suite, const char *name, ut_fn_t fn)
{
    if (s_count == s_cap) {
        int cap = s_cap ? s_cap * 2 : 32;
        ut_case_t *p = (ut_case_t *)realloc(s_cases, (size_t)cap * sizeof(*p));
        if (!p) {
            fprintf(stderr, "ut: 注册表扩容失败\n");
            exit(2);
        }
        s_cases = p;
        s_cap   = cap;
    }
    s_cases[s_count].suite = suite;
    s_cases[s_count].name  = name;
    s_cases[s_count].fn    = fn;
    s_count++;
}

/*---------------------------------------------------------------------------
 * 断言失败上报
 *---------------------------------------------------------------------------*/
void ut_report_fail(const char *file, int line, const char *fmt, ...)
{
    s_cur_fails++;
    if (s_fail_msgs >= UT_MAX_MSG_PER_CASE) {
        if (s_fail_msgs == UT_MAX_MSG_PER_CASE) {
            printf("      … 该用例失败过多，后续只计数不再打印\n");
            s_fail_msgs++;
        }
        return;
    }
    s_fail_msgs++;

    /* 只保留文件名，不打印整条绝对路径 —— 输出短一行更好读 */
    const char *base = file;
    for (const char *p = file; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;

    printf("      %s:%d  ", base, line);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

void ut_note_elapsed(uint32_t us)
{
    (void)us;
}

/*---------------------------------------------------------------------------
 * 崩溃兜底
 *---------------------------------------------------------------------------*/
static void on_signal(int sig)
{
    if (s_in_case) {
        s_cur_crashed = sig;
        s_in_case     = 0;
        longjmp(s_jmp, 1);
    }
    /* 不在用例里崩溃 = 框架自身的问题，照常死 */
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_crash_handlers(void)
{
    signal(SIGSEGV, on_signal);
    signal(SIGFPE,  on_signal);
    signal(SIGILL,  on_signal);
    signal(SIGABRT, on_signal);
#ifdef SIGBUS
    signal(SIGBUS,  on_signal);
#endif
}

/*---------------------------------------------------------------------------
 * 运行
 *---------------------------------------------------------------------------*/
static int cmp_case(const void *a, const void *b)
{
    const ut_case_t *x = (const ut_case_t *)a, *y = (const ut_case_t *)b;
    int c = strcmp(x->suite, y->suite);
    return c ? c : strcmp(x->name, y->name);
}

int ut_run_all(int argc, char **argv)
{
    const char *filter = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) s_verbose = 1;
        else if (argv[i][0] != '-')  filter = argv[i];
    }

    /* 稳定输出顺序：按 suite/name 排，diff 两次运行结果才有意义 */
    if (s_count > 1) qsort(s_cases, (size_t)s_count, sizeof(*s_cases), cmp_case);

    install_crash_handlers();

    printf("\n");
    int run = 0, pass = 0, fail = 0, crash = 0;
    const char *last_suite = NULL;

    for (int i = 0; i < s_count; i++) {
        ut_case_t *c = &s_cases[i];
        if (filter && !strstr(c->suite, filter) && !strstr(c->name, filter))
            continue;

        if (!last_suite || strcmp(last_suite, c->suite)) {
            printf("  [%s]\n", c->suite);
            last_suite = c->suite;
        }

        s_cur_fails = 0;
        s_fail_msgs = 0;
        s_cur_crashed = 0;
        s_in_case = 1;

        clock_t t0 = clock();
        if (setjmp(s_jmp) == 0) {
            c->fn();
        }
        s_in_case = 0;
        clock_t t1 = clock();
        double ms = 1000.0 * (double)(t1 - t0) / (double)CLOCKS_PER_SEC;

        run++;
        if (s_cur_crashed) {
            crash++;
            printf("    ✗ %-38s 崩溃 signal=%d（后面的断言没跑）\n",
                   c->name, s_cur_crashed);
        } else if (s_cur_fails) {
            fail++;
            printf("    ✗ %-38s %d 处断言失败\n", c->name, s_cur_fails);
            if (s_verbose) printf("      耗时 %.2f ms\n", ms);
        } else {
            pass++;
            printf("    ✓ %-38s", c->name);
            if (s_verbose) printf(" %.2f ms", ms);
            printf("\n");
        }
    }

    printf("\n────────────────────────────────────────────────────────\n");
    printf("总计 %d 用例    通过 %d    失败 %d    崩溃 %d\n",
           run, pass, fail, crash);
    if (run == 0) {
        printf("⚠ 一个用例都没匹配到（过滤条件 \"%s\"）\n", filter ? filter : "");
        return 2;
    }
    if (!fail && !crash) {
        printf("✅ 全部通过\n\n");
        return 0;
    }
    printf("❌ 有未通过项\n\n");
    return 1;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);   /* 源码是 UTF-8，不改会打出乱码 */
#endif
    return ut_run_all(argc, argv);
}
