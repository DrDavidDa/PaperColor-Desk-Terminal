# -*- coding: utf-8 -*-
# 解析 Anki 卡片 md → JSON（第1、2章），生成 /cards/ch01.json / cards/ch02.json
# 卡片格式：
#   ### 【...】问题{{c1::答案}}   (填空卡)
#   ### 【...】问题                (问答卡)
#   背面: xxx                      (答案/口诀)
# 丢弃：牌组/类型/标签 行
import json, re, os, sys

SRC = r"E:\ClaudeCode\Work\projects\中级经济师备考2026\卡库\经济基础"
OUT = "cards"
os.makedirs(OUT, exist_ok=True)

def clean_text(s):
    """去掉 {{c1::...}} 占位符，保留内容"""
    s = re.sub(r"\{\{c\d+::(.*?)\}\}", r"\1", s)
    s = s.replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">")
    s = s.replace("<br>", "；").replace("<br/>", "；")
    return s.strip()

def hide_cloze(s):
    """Anki 填空：把 {{c1::答案}} 中的答案隐藏成下划线（问题显示为空白，让用户回忆）
    用 3 个半角下划线 ___（efont 字体一定有该字形；全角＿＿可能缺失）"""
    return re.sub(r"\{\{c\d+::(.*?)\}\}", "___", s)

def extract_answers(s):
    """提取所有 {{c1::答案}} 的答案内容，供背面显示"""
    ans = re.findall(r"\{\{c\d+::(.*?)\}\}", s)
    return ans

def parse_file(path):
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    cards = []
    cur = None  # {raw_question, back}
    for ln in lines:
        ln = ln.strip()
        if ln.startswith("### "):
            if cur and cur["raw"]:
                cards.append(cur)
            cur = {"raw": ln[4:], "back": ""}
        elif ln.startswith("背面:"):
            if cur:
                cur["back"] = clean_text(ln[3:])
        elif ln.startswith(("牌组:", "类型:", "标签:", "#", ">")):
            continue
    if cur and cur["raw"]:
        cards.append(cur)
    return cards

def to_kp(cards):
    """转 KnowledgePoint：question/answer/mnemonic
    Anki 填空卡：问题隐藏 {{c1::答案}} 为下划线，答案=填空答案汇总+背面内容"""
    result = []
    for c in cards:
        raw = c["raw"]
        # 去掉前缀【...】章节标识（冗余）
        raw = re.sub(r"^【[^】]*】\s*", "", raw).strip()
        back = c["back"]

        # 填空答案汇总（若有 {{c1:: }} 则本卡是填空卡）
        cloze_ans = extract_answers(raw)
        has_cloze = len(cloze_ans) > 0

        # 问题：填空卡隐藏答案 → 下划线；问答卡原样
        if has_cloze:
            q = hide_cloze(raw).strip()
            # 收集答案：填空答案 + 背面核心
            answer = "；".join(cloze_ans)
            # 背面如果还有补充内容（非口诀部分），追加
            back_core = re.sub(r"(口诀|速记)[:：]\s*.+$", "", back).strip()
            if back_core and not back_core.startswith(("【多选", "【费曼", "【")):
                answer = answer + "。" + back_core
        else:
            q = raw.strip()
            answer = back

        # 拆口诀
        mnemonic = ""
        m = re.search(r"(口诀|速记)[:：]\s*(.+)", back)
        if m:
            mnemonic = m.group(2).strip()
        # 答案里截掉【多选】【费曼】扩充
        cut = re.search(r"(【多选】|【费曼】)", answer)
        if cut:
            answer = answer[:cut.start()].strip()

        result.append({
            "category": "经济基础",
            "question": q,
            "answer": answer if answer else "(见口诀)",
            "mnemonic": mnemonic,
        })
    return result

for name in ["第01章_社会主义经济制度.md", "第02章_需求供给与均衡价格.md"]:
    src_path = os.path.join(SRC, name)
    cards = parse_file(src_path)
    kps = to_kp(cards)
    m = re.match(r"第(\d+)章", name)
    out_name = f"ch{m.group(1)}.json"
    with open(os.path.join(OUT, out_name), "w", encoding="utf-8") as f:
        json.dump(kps, f, ensure_ascii=False, indent=1)
    print(f"{name}: {len(cards)} 卡 → {out_name}")
    for i, k in enumerate(kps[:3]):
        print(f"  [{i}] Q:{k['question'][:40]}... A:{k['answer'][:30]}... 口诀:{k['mnemonic'][:20]}")
