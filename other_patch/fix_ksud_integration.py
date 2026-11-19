#!/usr/bin/env python3
# 修复 10_enable 补丁对 SukiSU Nova v3.0.0 的失败 hunk #5：
# 把旧签名 ksu_handle_execveat_ksud(const char*, user_arg_ptr*) 替换为补丁意图的新签名版本
# （int ksu_handle_execveat_ksud(int*, struct filename**, ...)，static_branch 机制）
# 幂等：已是新签名则跳过

import sys

NEW_FUNC = '''extern int ksu_handle_execveat_init(struct filename *filename, struct user_arg_ptr *argv_user, struct user_arg_ptr *envp_user);

// IMPORTANT NOTE: the call from execve_handler_pre WON'T provided correct value for envp and flags in GKI version
int ksu_handle_execveat_ksud(int *fd, struct filename **filename_ptr,
                             struct user_arg_ptr *argv,
                             struct user_arg_ptr *envp, int *flags)
{
    struct filename *filename;
    static const char app_process[] = "/system/bin/app_process";

    /* This applies to versions Android 10+ */
    static const char system_bin_init[] = "/system/bin/init";

    if (!filename_ptr)
        return 0;

    filename = *filename_ptr;
    if (IS_ERR(filename)) {
        return 0;
    }

    if (static_branch_unlikely(&is_init_second_stage_not_executed)) {
        // https://cs.android.com/android/platform/superproject/+/android-16.0.0_r2:system/core/init/main.cpp;l=77
        if (unlikely(!memcmp(filename->name, system_bin_init, sizeof(system_bin_init) - 1) && argv)) {
            char buf[16];
            if (check_argv(*argv, 1, "second_stage", buf, sizeof(buf))) {
                pr_info("/system/bin/init second_stage executed\\n");
                ksu_preinit_release();
                ksu_selinux_hide_handle_second_stage();
                apply_kernelsu_rules();
                cache_sid();
                setup_ksu_cred();
                // after apply_kernelsu_rules, so that selinux_hide has its sepolicy backup
                ksu_feature_apply_forced();
                static_branch_disable(&is_init_second_stage_not_executed);
            }
        }
    }

    if (static_branch_unlikely(&is_first_zygote)) {
        if (unlikely(!memcmp(filename->name, app_process, sizeof(app_process) - 1) && argv)) {
            char buf[16];
            if (check_argv(*argv, 1, "-Xzygote", buf, sizeof(buf))) {
                pr_info("exec zygote, /data prepared, second_stage: %d\\n", !static_key_enabled(&is_init_second_stage_not_executed));
                ksu_preinit_release();
                on_post_fs_data();
                static_branch_disable(&is_first_zygote);
            }
        }
    }

    return 0;
}'''

OLD_START = 'void ksu_handle_execveat_ksud(const char *path, struct user_arg_ptr *argv)'

def main(path):
    with open(path, encoding='utf-8') as f:
        lines = f.readlines()

    if 'int ksu_handle_execveat_ksud(int *fd' in ''.join(lines):
        print('[SKIP] 已是新签名')
        return 0

    try:
        start = next(i for i, l in enumerate(lines) if l.startswith(OLD_START))
    except StopIteration:
        print('[FATAL] 找不到旧签名函数起点')
        return 1

    # 函数体：从 { 开始做花括号配平找结束行
    depth = 0
    end = None
    for i in range(start, len(lines)):
        depth += lines[i].count('{') - lines[i].count('}')
        if depth == 0 and i > start and '{' in ''.join(lines[start:i + 1]):
            end = i
            break
    if end is None:
        print('[FATAL] 找不到函数结束')
        return 1

    new_lines = lines[:start] + [NEW_FUNC + '\n'] + lines[end + 1:]
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.writelines(new_lines)
    print(f'[OK] 替换 {start + 1}~{end + 1} 行（{end - start + 1} 行旧函数 -> 新签名版本）')
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
