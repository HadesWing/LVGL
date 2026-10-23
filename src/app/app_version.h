/**
 * @file app_version.h
 * @brief 版本与构建信息
 *
 * 【为什么单独有 VERSION 文件而不是写在代码里】
 *   版本号有三个消费者：编译（-DAPP_VERSION）、打包脚本（目录名/文件名）、
 *   现场排障（`--version` 输出）。三处各写一份必然漂移 —— 现场报过来的
 *   版本号对不上代码，是最浪费时间的一类问题。所以只有仓库根目录的 VERSION
 *   是唯一事实来源：CMake 读它、注入成宏，脚本也读它。
 *
 * 【为什么 --version 要打编译器版本和构建时间】
 *   嵌入式现场最常见的一句反馈是"这版就是不行"，而同一版本号下可能重编过
 *   很多次（改了个开关、换了工具链）。把 build id 打出来，才能确定跑的是
 *   不是那一份二进制。
 */
#ifndef APP_VERSION_H
#define APP_VERSION_H

/* CMake 从 VERSION 文件注入；IDE/临时编译没注入时给个显眼的兜底值，
   这样万一漏了，--version 一眼能看出来，而不是显示一个像模像样的假版本号。 */
#ifndef APP_VERSION
#define APP_VERSION "0.0.0-nobuild"
#endif

/** 语义化版本，如 "0.9.0" */
const char *app_version(void);

/** 一行构建信息：版本 + 编译器 + 构建日期，用于现场排查 */
const char *app_build_info(void);

#endif /* APP_VERSION_H */
