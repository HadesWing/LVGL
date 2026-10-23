# 第三方组件与许可（发布包必读）

本产品是**静态链接**的单一可执行文件。静态链接把库的目标代码复制进了我们的二进制，
所以许可证的义务不是"保留个 LICENSE 文件"就完了 —— 下面逐条写清。

## 1 组件清单

| 组件 | 版本 | 许可证 | 用途 | 链接方式 |
|---|---|---|---|---|
| LVGL | 9.x | MIT | GUI 框架（渲染、控件、输入） | 静态 |
| libosip2 | 5.3.2 | **LGPL-2.1** | SIP 报文编解码（REGISTER/INVITE/SDP） | **静态** |
| libavcodec / libavutil | 9.0.2 | LGPL-2.1+ | **仅 PC 调试构建**：H.264 软解 | 动态（PC）/ 板端用 Rockchip MPP 替代 |

板端发布版**不包含 FFmpeg**：解码走芯片自带的 MPP 硬解（`vdec_mpp.c`）。
所以真正需要履行义务的只有 libosip2 一项。

## 2 LVGL（MIT）

MIT 只要求保留版权声明与许可全文。已在 `LICENSES/MIT-lvgl.txt` 附上。

## 3 libosip2（LGPL-2.1）—— 唯一有实质义务的组件

LGPL-2.1 §6 对静态链接的要求是：**必须让接收方能够用修改过的 libosip2 重新链接出
同样的可执行文件**。我们做了三件事：

1. **不改库**：`third_party/libosip2-5.3.2.tar.gz` 是上游原始 tarball，我们的改动为零；
2. **附源码**：发布包 `LICENSES/libosip2-5.3.2.tar.gz`（或者给出可长期访问的下载地址）；
3. **给重链接能力**：`relink/` 目录下有
   - `relink/libosip2.a` —— 我们链接时用的那个静态库；
   - `relink/link.txt` —— 完整的链接命令（含所有目标文件与库的顺序）；
   - `relink/obj.tar.gz` —— 我们自己的目标文件（不含第三方）。
   拿到修改后的 libosip2 后：
   ```sh
   tar xzf relink/obj.tar.gz && cd obj
   # 用 relink/link.txt 里那条命令重新链接，把 libosip2.a 换成新编的
   ```

> 只提供"书面承诺可索取"是不够的 —— 那要求用户先联系你、再等回复，
> 实际操作中等于没有能力重链接。附上目标文件和链接命令才算真的给到。

## 4 其他

- 字体：`src/ui/fonts/ui_font_cjk_*.c` 由 `scripts/gen_font.py` 用 Noto Sans CJK 子集化生成，
  该字体为 SIL Open Font License 1.1，允许嵌入与再分发。
- 本仓库自研代码为闭源（随产品授权），不在本文件范围内。

## 5 检查方式

```sh
# 确认发布二进制里没有意外的动态依赖（板端应当只有 libc/libpthread 之类）
readelf -d bin/indoor-unit | grep NEEDED

# 确认 strip 之后没有残留符号
file bin/indoor-unit
```
