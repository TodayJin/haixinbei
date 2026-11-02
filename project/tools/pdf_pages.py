"""docs/ 下参考 PDF 的通用小工具：导出文本 / 渲染页面 / 抽取内嵌图。

用法示例：
    python tools/pdf_pages.py --pdf docs/竞赛规则-第七届吉林省AI创新大赛.pdf --list
    python tools/pdf_pages.py --pdf docs/竞赛规则-第七届吉林省AI创新大赛.pdf --dump 46-55
    python tools/pdf_pages.py --pdf docs/竞赛规则-第七届吉林省AI创新大赛.pdf --render 49 46 --dpi 200
    python tools/pdf_pages.py --pdf docs/竞赛规则-第七届吉林省AI创新大赛.pdf --images 49-55

页面范围写法：46-55 或 49,51,53 或单页 49。
"""
import argparse
import os
import sys

import pymupdf

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _paths import out  # noqa: E402


def parse_pages(spec: str, n: int) -> list[int]:
    pages: list[int] = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            a, b = part.split("-", 1)
            pages.extend(range(int(a), int(b) + 1))
        else:
            pages.append(int(part))
    return [p for p in pages if 1 <= p <= n]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="PDF 文本/渲染/取图工具")
    ap.add_argument("--pdf", required=True)
    ap.add_argument("--list", action="store_true", help="列出每页首行与前缀")
    ap.add_argument("--dump", help="导出指定页全文")
    ap.add_argument("--render", nargs="*", type=int, help="渲染指定页为 PNG")
    ap.add_argument("--images", help="导出指定页范围的内嵌图片")
    ap.add_argument("--dpi", type=int, default=180)
    ap.add_argument("--outdir", default=None, help="默认 out/pdf")
    args = ap.parse_args(argv)

    doc = pymupdf.open(args.pdf)
    n = len(doc)
    outdir = args.outdir or out("pdf")
    os.makedirs(outdir, exist_ok=True)

    if args.list:
        for i, page in enumerate(doc):
            t = [l.strip() for l in page.get_text().split("\n") if l.strip()]
            print("p%-4d img=%-2d %s" % (i + 1, len(page.get_images(full=True)),
                                         " / ".join(t[:2])[:88]))

    if args.dump:
        for p in parse_pages(args.dump, n):
            print("=" * 70)
            print("### 第 %d 页" % p)
            print("=" * 70)
            print(doc[p - 1].get_text().strip())

    if args.render:
        for p in args.render:
            if not 1 <= p <= n:
                continue
            pix = doc[p - 1].get_pixmap(dpi=args.dpi)
            path = os.path.join(outdir, "p%d.png" % p)
            pix.save(path)
            print("渲染", path, "%dx%d" % (pix.width, pix.height))

    if args.images:
        for p in parse_pages(args.images, n):
            page = doc[p - 1]
            for k, info in enumerate(page.get_image_info(xrefs=True)):
                xref = info.get("xref")
                if not xref:
                    continue
                img = doc.extract_image(xref)
                ext = img.get("ext", "png")
                path = os.path.join(outdir, "p%d_%d.%s" % (p, k, ext))
                with open(path, "wb") as fh:
                    fh.write(img["image"])
                print("取图", path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
