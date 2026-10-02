#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
renwu_states.py —— 比赛任务流程（renwu.c 的 A()）状态可达性检查

为什么要有这个：A() 是一个手写的"状态机"，状态存在全局变量 black_num 里，
每个 `else if (... && black_num == K)` 分支末尾写 `black_num = N` 跳到下一步。
这种写法最典型的坑就是**跳号**：某个分支直接 K -> K+2，中间那个状态
（连同它那一段动作：抬舵机、转 90°、延时）就永远不会执行 —— 车少做一个动作，
而编译器和静态分析都不会报任何错。

这个脚本不编译，只读源码文本：
  1) 把每个分支的 (进入状态 K -> 写出的状态 N, 行号) 列出来；
  2) 从 0 出发做可达性分析，报出"作为分支条件出现过、但走不到"的状态；
  3) 顺带列出"没有出边的状态"（走到就停）与"不在任何分支里的赋值"。

用法：
    python tools/renwu_states.py                 # 打印报告
    python tools/renwu_states.py -Strict         # 有不可达状态就以退出码 1 结束
    python tools/renwu_states.py -Out out/renwu_states.txt
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'firmware', 'App', 'Svc', 'renwu.c')

RE_FUNC = re.compile(r'^\s*void\s+([A-Za-z_]\w*)\s*\(')
RE_GUARD = re.compile(r'black_num\s*==\s*(\d+)')
RE_ASSIGN = re.compile(r'black_num\s*=\s*(\d+)')
RE_IF = re.compile(r'(^|\s)(if|else\s+if)\s*\(')
RE_CALL = re.compile(r'\b([A-Za-z_]\w*)\s*\(')
KEYWORDS = {
    'if', 'else', 'while', 'for', 'switch', 'return', 'sizeof', 'do', 'case',
    'break', 'continue', 'void', 'int', 'uint8_t', 'uint16_t', 'uint32_t',
    'float', 'double', 'char', 'static', 'const', 'unsigned', 'signed',
}


def strip_comments(lines):
    """去掉 // 与 /* */ 注释，保留行号（注释字符换成空格）。"""
    out = []
    in_block = False
    for line in lines:
        res = []
        i = 0
        while i < len(line):
            if in_block:
                if line.startswith('*/', i):
                    in_block = False
                    res.append('  ')
                    i += 2
                else:
                    res.append(' ')
                    i += 1
                continue
            if line.startswith('/*', i):
                in_block = True
                res.append('  ')
                i += 2
                continue
            if line.startswith('//', i):
                res.append(' ' * (len(line) - i))
                break
            res.append(line[i])
            i += 1
        out.append(''.join(res))
    return out


def depth_profile(lines):
    """depth_after[i] = 处理完第 i 行后的花括号深度。"""
    depth = 0
    prof = []
    for line in lines:
        depth += line.count('{') - line.count('}')
        prof.append(depth)
    return prof


def find_function(lines, prof, name):
    for i, line in enumerate(lines):
        m = RE_FUNC.match(line)
        if m and m.group(1) == name:
            for j in range(i, len(lines)):
                if '{' in lines[j]:
                    return j, prof[j]          # 函数体的 '{' 行、进入后的深度
            return None, None
    return None, None


def scan_branches(lines, prof, body_start, body_depth):
    """在函数体里找所有 `if/else if (... black_num == K ...) { ... }` 分支。"""
    edges = []
    guards = []
    bodies = []
    i = body_start
    n = len(lines)
    while i < n:
        if prof[i] < body_depth:
            break
        if prof[i] == body_depth and RE_IF.search(lines[i]):
            g = RE_GUARD.search(lines[i])
            if g:
                k = int(g.group(1))
                # 找这个分支的 '{'
                j = i
                while j < n and prof[j] < body_depth + 1:
                    j += 1
                if j < n:
                    # 找匹配的 '}'
                    m = j
                    while m < n and not (m > j and prof[m] == body_depth):
                        m += 1
                    body = lines[j + 1:m]
                    assigns = []
                    for off, bl in enumerate(body):
                        a = RE_ASSIGN.search(bl)
                        if a and 'uint8_t' not in bl and 'extern' not in bl:
                            assigns.append((j + 2 + off, int(a.group(1))))
                    guards.append((k, i + 1))
                    bodies.append((k, i + 1, body))
                    for ln, v in assigns:
                        edges.append((k, v, i + 1, ln))
                    # `}else if (...)` 把上一个分支的 '}' 和下一个分支的条件写在
                    # 同一行：回退一行，让下面的 i+=1 正好落在那行上重新识别。
                    i = m - 1
        i += 1
    return edges, guards, bodies


def called_names(body_lines):
    """分支体里调用到的函数名集合（用于判断某个动作是否在别处也做了）。"""
    names = set()
    for bl in body_lines:
        for m in RE_CALL.finditer(bl):
            nm = m.group(1)
            if nm not in KEYWORDS:
                names.add(nm)
    return names


def main(argv):
    argv = [a for a in argv]
    strict = '-Strict' in argv
    out_path = None
    if '-Out' in argv:
        idx = argv.index('-Out')
        if idx + 1 < len(argv):
            out_path = argv[idx + 1]

    try:
        sys.stdout.reconfigure(encoding='utf-8')
    except Exception:
        pass

    if not os.path.exists(SRC):
        print('找不到 %s' % SRC)
        return 2

    with open(SRC, 'r', encoding='utf-8') as fp:
        raw = fp.read().splitlines()
    lines = strip_comments(raw)
    prof = depth_profile(lines)

    body_start, body_depth = find_function(lines, prof, 'A')
    if body_start is None:
        print('没找到 void A()')
        return 2

    edges, guards, bodies = scan_branches(lines, prof, body_start, body_depth)

    # 去重（同一分支同一对只留第一次）
    seen = set()
    uniq = []
    for e in edges:
        key = (e[0], e[1])
        if key in seen:
            continue
        seen.add(key)
        uniq.append(e)

    succ = {}
    for f, t, gl, al in uniq:
        succ.setdefault(f, []).append((t, gl, al))

    # 可达性：起点 0（renwu() 的 case 0 复位值）
    reach = set()
    work = [0]
    while work:
        s = work.pop()
        if s in reach:
            continue
        reach.add(s)
        for t, _, _ in succ.get(s, []):
            if t not in reach:
                work.append(t)

    guard_states = sorted({k for k, _ in guards})
    unreachable = [s for s in guard_states if s not in reach]
    dead_end = [s for s in guard_states if s in reach and not succ.get(s)]

    # 不在任何分支里的赋值（列出来人工确认，一般是 renwu() 的 case 0）
    branch_assign_lines = {al for _, _, _, al in uniq}
    unguarded = []
    for i, line in enumerate(lines):
        if RE_ASSIGN.search(line) and (i + 1) not in branch_assign_lines:
            if 'uint8_t' in line or 'extern' in line:
                continue
            unguarded.append((i + 1, int(RE_ASSIGN.search(line).group(1))))

    out = []
    out.append('renwu.c 任务流程状态图（%s）' % os.path.relpath(SRC, ROOT).replace('\\', '/'))
    out.append('')
    out.append('  状态转移（进入状态的判断行 -> 分支末尾写出的下一个状态）：')
    for f, t, gl, al in sorted(uniq):
        mark = '' if f in reach else '   <== 起点 0 走不到'
        out.append('    %3d -> %-3d   (条件 %4d 行, 赋值 %4d 行)%s' % (f, t, gl, al, mark))
    out.append('')
    out.append('  分支条件里出现过的状态：%s' % ', '.join(str(s) for s in guard_states))
    out.append('  从 0 出发可达的状态：  %s' % ', '.join(str(s) for s in sorted(reach)))
    out.append('')
    if unreachable:
        out.append('  !! 不可达状态 %d 个：%s' % (len(unreachable), ', '.join(str(s) for s in unreachable)))
        out.append('     这些分支永远不会进。逐个核对“跳过它的那一支里是不是也做了同样的动作”：')
        body_of = {k: body for k, _gl, body in bodies}
        for s in unreachable:
            owners = [(f, t) for f, t, _gl, _al in uniq if f < s < t and f in reach]
            owner_calls = set()
            for f, _t in owners:
                owner_calls |= called_names(body_of.get(f, []))
            dead_calls = sorted(called_names(body_of.get(s, [])))
            only_dead = [c for c in dead_calls if c not in owner_calls]
            ow = ', '.join('%d->%d' % (f, t) for f, t in owners) or '（没有单一跨过它的分支）'
            out.append('      状态 %-2d 被 %s 跳过；本支调用 %s' %
                       (s, ow, ('、'.join(dead_calls) if dead_calls else '（没有函数调用）')))
            if only_dead:
                out.append('            !! 这些调用在跳过它的分支里找不到：%s —— 动作很可能真的丢了' %
                           '、'.join(only_dead))
            elif dead_calls:
                out.append('            OK：本支的调用在跳过它的分支里都有 ⇒ 动作已合并进大分支，'
                           '死掉的只是分支条件')
    else:
        out.append('  可达性 OK：所有分支条件里的状态都能走到。')
    if dead_end:
        out.append('  !! 没有出边的状态（走到就停在那一支，靠分支条件反复进）：%s'
                   % ', '.join(str(s) for s in dead_end))
    if unguarded:
        out.append('  不在任何 black_num==K 分支里的赋值（人工确认）：')
        for ln, v in unguarded:
            out.append('    %4d 行: black_num = %d' % (ln, v))
    out.append('')
    out.append('（状态 5 从未被使用；注释掉的跳转不参与分析。跳号是这份流程最容易犯的错。）')

    text = '\n'.join(out)
    print(text)
    if out_path:
        with open(out_path, 'w', encoding='utf-8') as fp:
            fp.write(text + '\n')
        print('\n已写入 %s' % out_path)

    return 1 if (strict and unreachable) else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
