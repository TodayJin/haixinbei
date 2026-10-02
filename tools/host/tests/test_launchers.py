"""启动脚本（*.bat）的格式防护。

Windows 的 cmd.exe **要求 .bat 是 CRLF 换行**；换成 LF 会让它把整个文件当成
一行来解析，报出 'errorlevel' is not recognized 这类莫名其妙的错。
另外 .bat 里的中文若按 GBK 读会乱码（cmd 用 OEM 代码页读脚本文件），
所以这里一并要求 .bat 只用 ASCII。

这个坑很隐蔽：写文件的工具默认给 LF，写完看起来完全正常，双击才炸。
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))))   # 仓库根


def find_bats():
    out = []
    for base, dirs, files in os.walk(ROOT):
        dirs[:] = [d for d in dirs
                   if d not in (".git", "__pycache__", "build", "out", ".agents")]
        for f in files:
            if f.lower().endswith(".bat"):
                out.append(os.path.join(base, f))
    return out


class TestLaunchers(unittest.TestCase):
    def test_bat_files_exist(self):
        bats = find_bats()
        self.assertGreaterEqual(len(bats), 2, "找不到启动脚本")
        names = {os.path.basename(b) for b in bats}
        self.assertIn("run_host.bat", names)
        self.assertIn("selftest.bat", names)

    def test_bat_uses_crlf(self):
        for path in find_bats():
            with open(path, "rb") as fh:
                data = fh.read()
            bare_lf = data.count(b"\n") - data.count(b"\r\n")
            self.assertEqual(bare_lf, 0,
                             "%s 有 %d 个裸 LF 换行；cmd.exe 会解析失败，"
                             "必须全部用 CRLF" % (os.path.basename(path), bare_lf))
            self.assertFalse(data.startswith(b"\xef\xbb\xbf"),
                             "%s 带 UTF-8 BOM，cmd.exe 会报错" % os.path.basename(path))

    def test_bat_is_ascii(self):
        for path in find_bats():
            with open(path, "rb") as fh:
                data = fh.read()
            bad = [(i, b) for i, b in enumerate(data) if b > 127]
            self.assertEqual(bad, [],
                             "%s 含非 ASCII 字节（cmd 用 OEM 代码页读脚本，会乱码）："
                             "偏移 %s" % (os.path.basename(path), bad[:5]))

    def test_bat_cds_to_own_dir(self):
        for path in find_bats():
            with open(path, "r", encoding="ascii") as fh:
                text = fh.read()
            self.assertIn("cd /d \"%~dp0\"", text,
                          "%s 必须 cd 到自身目录，否则双击时工作目录不对"
                          % os.path.basename(path))


if __name__ == "__main__":
    unittest.main(verbosity=2)
