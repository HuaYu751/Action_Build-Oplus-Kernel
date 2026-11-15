# memkernel（方案三 built-in 形态）内核集成说明

> 2026-11-16 改造完成。对应《内核编译总任务书》第四节 C1~C11、P3 阶段。

## 改动清单（相对旧版）

| 文件 | 改动 |
|---|---|
| `comm.h` | 协议换新：幻数区间 `0xC0DE6000~0xC0DE60FF`、`OPLUS_DFR_AUTH_KEY`、`OP_AUTHENTICATE/OP_DEAUTH` |
| `memory.c` | 全部函数改 `oplus_dfr_` 前缀；**保留** `pte_present()` 预检（C8，原本就有）；维持 `phys_to_virt`（C9） |
| `entry.c` | **重写**：删 misc 注册（无 /dev 节点）、删 module 样板、零 dmesg；新增认证握手 + `oplus_dfr_dispatch()` 分发（C10/C11） |
| `Makefile` | `obj-$(CONFIG_OPLUS_DFR)`，built-in 默认；LKM 仅调试备用 |
| `Kconfig` | `config OPLUS_DFR` 改 **bool**（禁止 =m，模块形态违背隐蔽设计） |
| `patches/0001-proc-version-ioctl.patch` | **新增**：给 `fs/proc/version.c` 挂 `proc_ops.proc_ioctl` 通道 |

## 集成步骤（内核源码树内）

```bash
# 1. 复制驱动
cp -r memkernel <内核源码树>/drivers/oplus_dfr/

# 2. 挂进构建
echo 'obj-$(CONFIG_OPLUS_DFR) += oplus_dfr/' >> <内核源码树>/drivers/Makefile
echo 'source "drivers/oplus_dfr/Kconfig"'      >> <内核源码树>/drivers/Kconfig

# 3. 打通道补丁（上下文不匹配时按补丁头部说明手工对齐，只有 ~30 行）
cd <内核源码树> && git apply drivers/oplus_dfr/patches/0001-proc-version-ioctl.patch

# 4. defconfig 追加
CONFIG_OPLUS_DFR=y
```

## 验收（对应任务书第八节第 10 项 / P3）

| # | 检查 | 期望 |
|---|---|---|
| 1 | `grep oplus_bsp_dfr_state /proc/modules` | 无输出（built-in 不进模块列表） |
| 2 | `ls /sys/module | grep oplus` | 无新增条目（零 module_param） |
| 3 | `ls -l /dev | grep oplus_bsp_state` | 无输出（不再建节点） |
| 4 | `cat /proc/version` | 输出正常，与原来一致（通道不影响 read） |
| 5 | 设备上跑 `esp_read4` | 自检0~3 PASS；认证后读数正确；自检4 min_flt 增量 = 0 |
| 6 | `su 10389 sh -c "ls /dev/..."` | 对游戏身份不可见（PathMask 规则仍可保留） |

## 已知注意事项

1. **CFI**：`version_proc_ioctl` 的签名必须与 `proc_ops.proc_ioctl` 完全一致
   `long (*)(struct file *, unsigned int, unsigned long)`——补丁里已按此写，
   `CONFIG_CFI_CLANG` 下不要改签名。
2. **符号过滤**：`oplus_dfr_dispatch` 是 non-static（要 export 给 version.c），
   必进 kallsyms；B1 过滤表正则 `^oplus_dfr` 已覆盖，无需额外处理。
3. **幻数更换**：换 cmd 区间或密钥时，`comm.h`（内核）与 `esp_read4.c`（用户态）
   必须同步改，否则认证永远 -ENOTTY。
4. **lk 调试形态**：`make M=... modules` 出的 .ko 只是验证编译用，
   正式刷机必须用 built-in（obj-y）。
