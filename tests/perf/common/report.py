#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""最小 Markdown 报告生成：表格加备注，无第三方依赖。"""


def write_summary(path, title, meta=None, table_header=None, table_rows=None, notes=None):
    lines = ["# " + title, ""]
    if meta:
        for k, v in meta.items():
            lines.append("- {}: {}".format(k, v))
        lines.append("")
    if table_header and table_rows:
        lines.append("| " + " | ".join(table_header) + " |")
        lines.append("| " + " | ".join(["---"] * len(table_header)) + " |")
        for row in table_rows:
            lines.append("| " + " | ".join(str(c) for c in row) + " |")
        lines.append("")
    if notes:
        lines.append(notes)
        lines.append("")
    with open(path, "w") as f:
        f.write("\n".join(lines))
