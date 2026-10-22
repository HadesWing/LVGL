/**
 * @file ui_category.h
 * @brief 二级页面：设备分类（灯光/窗帘/…）→ 房间筛选 + 设备卡片
 */
#ifndef UI_CATEGORY_H
#define UI_CATEGORY_H

#include <stdint.h>

/* 打开某分类页（type = device_type_t），内部压栈 */
void ui_category_open(uint8_t type);

#endif /* UI_CATEGORY_H */
