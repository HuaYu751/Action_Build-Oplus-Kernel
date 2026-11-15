#!/usr/bin/env python3
# p5_patch_kallsyms.py — 任务书 9-B / B2：锚点插入 kallsyms 输出过滤
# 用法: p5_patch_kallsyms.py <kernel/kallsyms.c 路径>
# 幂等：已打过的文件直接跳过。
#
# 原理（已对照 Pad2Pro OKI 源码 kernel/kallsyms.c 验证）：
#   s_show() 开头有 "if (!iter->name[0]) return 0;"
#   在其后插入 "if (kallsyms_is_hidden(iter->name)) return 0;"
#   只在输出路径跳过我们的符号，内核内部查找不受影响。
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else 'kernel/kallsyms.c'
with open(path, 'r', encoding='utf-8') as f:
    s = f.read()

if 'kallsyms_is_hidden' in s:
    print('already patched, skip')
    sys.exit(0)

# 1. 插 include（放在文件第一个 #include 之前，保持与内核风格一致）
inc = '#include <generated/hidden_syms.h>\n'
idx = s.find('#include')
if idx < 0:
    print('FATAL: no #include found', file=sys.stderr)
    sys.exit(1)
s = s[:idx] + inc + s[idx:]

# 2. 锚点插入过滤（s_show 的 iter->name[0] 检查之后）
anchor = re.compile(r'(if \(!iter->name\[0\]\)\s*\n\s*return 0;\n)')
m = anchor.search(s)
if not m:
    print('FATAL: anchor not found (iter->name[0] check)', file=sys.stderr)
    sys.exit(1)
s = anchor.sub(r'\1\n\tif (kallsyms_is_hidden(iter->name))\n\t\treturn 0;\n', s, count=1)

with open(path, 'w', encoding='utf-8', newline='\n') as f:
    f.write(s)
print('patched:', path)
