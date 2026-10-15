#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""简谱 → Harmonica Player 曲谱（JSON）生成器。

它写的是给人看的简谱（数字），负责算出这个游戏口琴真正能弹的东西：

    口琴本音 8 个：  1 2 3 4 5 6 7 i        （i = 高音 1）
    对应键盘：       Z X C V B N M ,
    演奏方式：       正常按键 / 按住鼠标左键 / 中键 / 右键 / 组合键
                     （游戏里的说法：左键 = 低一个八度、右键 = 高一个八度、中键 = 升半音；
                       左键+中键 = 低八度又升半音、右键+中键 = 高八度又升半音）
    音域：           本音是 1 到 i（一个八度 + 高音 do）；再宽就往低/高八度借，
                     只要按住某个鼠标键，8 个本音就能覆盖三个多八度

用法：
    python tools/jianpu2score.py --list                列出内置曲目
    python tools/jianpu2score.py --out dist/songs      生成到曲谱目录（默认就是这个）
    python tools/jianpu2score.py --print 小星星        只打印一首的换算结果，不写文件

简谱写法（够用就行）：
    1 2 3 4 5 6 7       本音（do re mi fa sol ti）
    i                   高音 1（键盘最右边那个逗号键）
    #4  b7              半音（用没写出来时按本音 + 中键升半音，见下面的换算规则）
    +4  +i              高一个八度（按住鼠标右键）
    v4  v1              低一个八度（按住鼠标左键）
    +#4  +b7  v#4  vb7  高/低八度再升/降半音（两键同时按住 → RightMiddle / LeftMiddle）
    0                   休止符（不发音，但占时值）
    0/2  0/4            休止半拍 / 休止四分之一拍（休止符也能带时值记号）
    -                   延长前一个音一拍
    /2  /4              时值减半 / 四分之一（八分音符、十六分音符）
    4.  /2.  /4.        附点写在记号最后：时值 ×1.5
                        附点四分 = 1.5 拍、附点八分 = 0.75 拍、附点十六分 = 0.375 拍
                        附点只影响时值，不影响音高、八度或演奏方式
    | 空格 ,            只是给人看的，全部忽略

记号骨架（括号里是可选的，顺序固定）：
    [+ 或 v][# 或 b]<1~7 或 i>[/2 或 /4][.]
    例： +4  高八度的 4        v1   低八度的 1
         +#4 高八度再升半音（RightMiddle）   4/2.  附点八分音符（0.75 拍）
         i/4 高音 1 的十六分音符            0/2   半拍休止

音级 → 按键 → 演奏方式的换算规则（MODE_ASSUMPTION 那张表就是它）：
    · 八度记号（+/v）决定"八度分量"：+ 按住右键、v 按住左键；
    · 半音记号（#/b）决定"半音分量"：中键 = 升半音；
    · 两者叠加就是组合模式：# 或 b 遇上 + 是 RightMiddle，遇上 v 是 LeftMiddle；
    · 按键（音级）按这么选：
        #d  → 按 d 本身 + 中键（d 升半音正好是 #d）；
        bd  → 按低一级的 d-1，再看它到 d 差多少：
              差一个全音（1→2、2→3、4→5、5→6、6→7）→ d-1 再升半音，正好是 bd；
              本来就只差半音（3→4、7→i）→ d-1 本身就是 bd，不用加半音。
    · 3/mi 与 7/si 上方没有黑键，所以 #3、#7 这种写法里"再加半音"会落到 4、i 上；
      要写这两个音，直接写 b4、bi 更准。
    · 数字记号本身（不带 #/b）不影响演奏方式：+4 是"高八度的 4"，键还是 4 那把。

音域自适应：曲子超出 1~i 时会自动整体移调（移八度或移音级），把旋律塞进 8 个音里，
移调不改变旋律的听感（只是换了调）。真塞不进去（八度跨度太大）会如实报错，不硬凑。
"""

import argparse
import json
import os
import sys

# ---------------------------------------------------------------------------
# 口琴与键盘的对应
# ---------------------------------------------------------------------------
KEYS = ["Z", "X", "C", "V", "B", "N", "M", ","]     # 依次是 1 2 3 4 5 6 7 i

# 音级（度）编号：1→0, 2→1, ..., 7→6, i→7（高音 1）
LOW = 0      # 最低能弹的音 = 1
HIGH = 7     # 最高能弹的音 = i

# 八度记号的写法（写在音级/半音记号前面）
OCTAVE_UP = "+"      # +4 / +i：高一个八度 → 按住鼠标右键
OCTAVE_DOWN = "v"    # v4 / v1：低一个八度 → 按住鼠标左键

# 音级之间的半音数（1 2 3 4 5 6 7 i），用来判断"低一级到 d 差多少"：
# 差 2 个半音（全音）时低一级再加半音才正好是 bd；差 1 个半音（3→4、7→i）时低一级本身就是 bd。
DEGREE_SEMITONES = [0, 2, 4, 5, 7, 9, 11, 12]

# (八度记号, 半音记号) → 演奏方式
# 游戏里鼠标是"修饰键"（2026-10 用户核对）：
#   左键 = 低一个八度、右键 = 高一个八度、中键 = 升半音，
#   而且左键/右键能和中间同时按 → 低八度+升半音（LeftMiddle）/ 高八度+升半音（RightMiddle）。
MODE_ASSUMPTION = {
    (None, None): "Normal",
    (None, "sharp"): "Middle",                # #4  → 按 4 + 中键
    (None, "flat"): "Middle",                 # b7  → 按 6 + 中键（b7 = 6 升半音）
    (OCTAVE_UP, None): "Right",               # +4  → 按住右键（高八度）
    (OCTAVE_UP, "sharp"): "RightMiddle",      # +#4 → 右键 + 中键
    (OCTAVE_UP, "flat"): "RightMiddle",       # +b7 → 右键 + 中键
    (OCTAVE_DOWN, None): "Left",              # v4  → 按住左键（低八度）
    (OCTAVE_DOWN, "sharp"): "LeftMiddle",     # v#4 → 左键 + 中键
    (OCTAVE_DOWN, "flat"): "LeftMiddle",      # vb7 → 左键 + 中键
}


class ScoreError(Exception):
    pass


def degree_name(index):
    """音级编号 → 简谱写法（0..6 → 1..7，7 → i）。"""
    if index == 7:
        return "i"
    if 0 <= index <= 6:
        return str(index + 1)
    return "?" + str(index)


def key_for_index(index):
    """音级编号 → 键盘键（越界返回 None）。"""
    if LOW <= index <= HIGH:
        return KEYS[index]
    return None


def parse_token(token, previous, beats_so_far):
    """把一个简谱记号变成 (音级编号 or None, 半音记号, 八度记号, 时值)。

    半音记号：None / "sharp"（#）/ "flat"（b）
    八度记号：None / "+"（高一个八度）/ "v"（低一个八度）

    时值：默认 1 拍（四分音符）；
          "/2" 减半、"/4" 再减半；末尾的 "." 是附点，时值 ×1.5。
          休止符 "0" 走同一条路，所以 "0/2"、"0." 都成立。
    """
    if token == "-":
        if previous is None:
            raise ScoreError("开头出现「-」：前面没有音可以延长")
        return ("extend", None, None, 1.0)

    octave = None
    body = token
    if body and body[0] == OCTAVE_UP:
        octave = OCTAVE_UP
        body = body[1:]
    elif body and body[0] in ("v", "V"):
        octave = OCTAVE_DOWN
        body = body[1:]

    accidental = None
    if body and body[0] in "#♯":
        accidental = "sharp"
        body = body[1:]
    elif body and body[0] in "b♭":
        accidental = "flat"
        body = body[1:]

    # 附点：写在记号最末尾（"4." 附点四分、"4/2." 附点八分），
    # 先摘掉它，剩下的部分才按老规矩解析，这样不带附点的老记号行为完全不变。
    dotted = False
    if body.endswith("."):
        dotted = True
        body = body[:-1]

    duration = 1.0
    if "/" in body:
        body, _, tail = body.partition("/")
        try:
            duration = 1.0 / float(tail)
        except ValueError:
            raise ScoreError("时值写法看不懂：「%s」" % token)
    if dotted:
        duration *= 1.5

    if body == "0":
        # 休止符：不发音，但和音符一样占时值（也能带 /2、/4、附点）
        return (None, None, None, duration)

    if body == "i" or body == "I":
        index = 7
    elif body.isdigit():
        number = int(body)
        if not 1 <= number <= 7:
            raise ScoreError("简谱只认得 1~7 和 i，不认识「%s」" % token)
        index = number - 1
    else:
        raise ScoreError("记号看不懂：「%s」" % token)

    return (index, accidental, octave, duration)


def tokenize(text):
    out = []
    for raw in text.replace("|", " ").replace(",", " ").split():
        out.append(raw)
    return out


def to_events(text):
    """简谱文本 → [{index, mode, beat, duration}]（尚未移调）。

    "index" 是**真正要按的那个键**的音级编号；八度记号与半音记号合起来决定演奏方式，
    换算规则见 MODE_ASSUMPTION 上面的注释与文件头的说明。
    """
    events = []
    beats = 0.0
    last = None
    for token in tokenize(text):
        parsed = parse_token(token, last, beats)
        if parsed[0] == "extend":
            if last is None:
                raise ScoreError("「-」前面没有音")
            events[last_index_of(events, last)]["duration"] += 1.0
            beats += 1.0
            continue
        index, accidental, octave, duration = parsed
        mode = "Normal"
        if index is not None:
            if accidental == "sharp":
                # #d = 按 d 本身 + 中键（中键升半音）→ 正好是 #d
                mode = MODE_ASSUMPTION[(octave, "sharp")]
            elif accidental == "flat":
                # bd = 按低一级的 d-1，再按需加半音：
                # d-1 到 d 差一个全音 → 加半音（中键）；本来就只差半音（3→4、7→i）→ 不加
                if index == 0:
                    raise ScoreError("「b1」比口琴最低音（1）还低半音，弹不出来；"
                                     "可以把整段移高，或者用 v7（低八度的 7）代替")
                step = DEGREE_SEMITONES[index] - DEGREE_SEMITONES[index - 1]
                index -= 1
                mode = MODE_ASSUMPTION[(octave, "flat" if step == 2 else None)]
            else:
                # 不带 #/b：八度记号决定要不要按住左/右键
                mode = MODE_ASSUMPTION[(octave, None)]
        events.append({"index": index, "mode": mode, "beat": beats, "duration": duration})
        last = index
        beats += duration
    return events


def last_index_of(events, index):
    for i in range(len(events) - 1, -1, -1):
        if events[i]["index"] == index:
            return i
    raise ScoreError("找不到要延长的音")


def fit_shift(events):
    """找一个整体移调量（音级数），把所有音塞进 1~i；返回 (shift, 是否移动)。"""
    indices = [e["index"] for e in events if e["index"] is not None]
    if not indices:
        return 0
    span = max(indices) - min(indices)
    if span > HIGH - LOW:
        raise ScoreError("旋律跨度 %d 个音级，超过口琴的 %d 个（1~i），这个曲子塞不进来"
                         % (span + 1, HIGH - LOW + 1))
    for shift in range(-14, 15):
        moved = [i + shift for i in indices]
        if min(moved) >= LOW and max(moved) <= HIGH:
            return shift
    raise ScoreError("找不到能塞进 1~i 的移调量")


def build_song(title, jianpu, bpm=120.0, time_signature="4/4"):
    events = to_events(jianpu)
    shift = fit_shift(events)
    notes = []
    for event in events:
        if event["index"] is None:
            continue
        key = key_for_index(event["index"] + shift)
        if key is None:
            raise ScoreError("移调后仍然越界：%s" % degree_name(event["index"] + shift))
        notes.append({
            "key": key,
            "mode": event["mode"],
            "beat": round(event["beat"], 6),
            "duration": round(event["duration"], 6),
        })
    used = sorted({degree_name(n["index"] + shift) for n in [e for e in events if e["index"] is not None]},
                  key=lambda s: (s == "i", s))
    modes = sorted({n["mode"] for n in notes})
    return {
        "title": title,
        "bpm": bpm,
        "timeSignature": time_signature,
        "notes": notes,
        "shift": shift,
        "used": used,
        "modes": modes,
        "beats": max([e["beat"] + e["duration"] for e in events]) if events else 0.0,
    }


def to_json(song):
    return {
        "name": song["title"],
        "bpm": song["bpm"],
        "timeSignature": song["timeSignature"],
        "beatUnit": "Quarter",
        "version": 1,
        "notes": song["notes"],
    }


# ---------------------------------------------------------------------------
# 内置曲目（都是能塞进 8 个音的自然音阶旋律，不含半音）
# ---------------------------------------------------------------------------
SONGS = [
    ("小星星",
     "1 1 5 5 6 6 5 - | 4 4 3 3 2 2 1 - | "
     "5 5 4 4 3 3 2 - | 5 5 4 4 3 3 2 - | "
     "1 1 5 5 6 6 5 - | 4 4 3 3 2 2 1 -", 120.0),
    ("两只老虎",
     "1 2 3 1 | 1 2 3 1 | 3 4 5 - | 3 4 5 - | "
     "5 6 5 4 3 1 | 5 6 5 4 3 1 | 2 5 1 - | 2 5 1 -", 120.0),
    ("欢乐颂",
     "3 3 4 5 | 5 4 3 2 | 1 1 2 3 | 3 - 2 - | "
     "3 3 4 5 | 5 4 3 2 | 1 1 2 3 | 2 - 1 -", 110.0),
    ("铃儿响叮当",
     "3 3 3 - | 3 3 3 - | 3 5 1 2 | 3 - - - | "
     "4 4 4 4 | 4 3 3 3 | 3 2 2 3 | 2 - 5 - | "
     "3 3 3 - | 3 3 3 - | 3 5 1 2 | 3 - - - | "
     "4 4 4 4 | 4 3 3 3 | 5 5 4 2 | 1 - - -", 120.0),
    ("音阶练习",
     "1 2 3 4 5 6 7 i | i 7 6 5 4 3 2 1 | "
     "1 1 5 5 6 6 7 i | i 7 7 6 6 5 5 1", 100.0),
    ("玛丽有只小羊羔",
     "3 2 1 2 | 3 3 3 - | 2 2 2 - | 3 5 5 - | "
     "3 2 1 2 | 3 3 3 3 | 2 2 3 2 | 1 - - -", 100.0),
    ("小蜜蜂",
     "5 3 3 - | 4 2 2 - | 1 2 3 4 | 5 5 5 - | "
     "5 3 3 - | 4 2 2 - | 1 3 5 5 | 3 - - -", 100.0),
    ("伦敦铁桥",
     "5 6 5 4 | 3 4 5 - | 2 3 4 - | 3 4 5 - | "
     "5 6 5 4 | 3 4 5 - | 2 - 5 - | 3 1 1 -", 110.0),
    ("老麦克唐纳有个农场",
     "1 1 1 5 | 6 6 5 - | 3 3 2 2 | 1 - - - | "
     "1 1 1 5 | 6 6 5 - | 3 3 2 2 | 1 - - -", 120.0),
    ("划船歌",
     "1 1 1 2 | 3 - - - | 3 2 3 4 | 5 - - - | "
     "i i i 5 | 5 5 3 3 | 3 1 1 1 | 5 4 3 2 | 1 - - -", 100.0),
    ("洋基歌",
     "1 1 2 3 | 1 3 2 - | 1 1 2 3 | 1 - - - | "
     "3 3 4 5 | 3 5 4 - | 3 3 4 5 | 3 - - -", 120.0),
    # 父亲：2026-10 照 1=E / 4/4 原谱图逐小节重读（标准时值：四分 / 八分 / 十六分 /
    # 附点四分 / 附点八分 / 延长 / 休止），共 28 小节 × 4 拍 = 112 拍。
    # 记号约定：原谱数字上方带高八度点的音写成 "+n"（1 写成 i）；没有点的是本音；
    # "n/2" 八分、 "n/4" 十六分、 "n." 附点四分、 "n/2." 附点八分、 "-" 延长一拍。
    # 全曲只用自然音级，无半音，演奏方式只用到 Normal 与 Right。
    ("父亲",
     "i +3 +2 6/2 5/2 | 6 5 3 - | i +3 +2. +5/2 | +3 - - 6/2 7/2 | i +3 +2. i/4 7/4 | "
     "i 5/2 4/2 5 6/2 7/2 | i. +3/2 +2. 5/2 | 6 - - - | 1/2 5/2 1/4 3/2. 4/2 3/2 2/4 1/2. | 1/2 5/2 1/4 2/2. 3 - | "
     "1/2 5/2 1/4 3/2. 4/2 3/2 2/4 1/2. | 2/4 3/4 2/2 2/4 1/2. 1 - | 1/2 5/2 1/4 3/2. 4/2 3/2 2/4 1/2. | "
     "6/2 5/2 5/4 4/2. 3 - | 1/2 5/2 1/4 3/2. 4/2 3/2 2/2. 1/4 | 3/2 2/2 2/4 1/2. 1 1/2 1/2 | "
     "6 6/2. 6/4 5/2 3/4 3/4 3/2. 3/4 | 4/2 5/2 1/2 5/2 3 1/2 7/2 | 6 6/2. 6/4 7/2 5/4 5/4 5/2. 5/4 | "
     "5/4 6/4 5/2 4/2 3/2 3. 2/2 | 2 - 0 0/2 6/4 7/4 | i/2 i/4 i/4 i/2 7/4 i/4 7 7/2 6/4 7/4 | "
     "7/2 7/4 7/4 7/2 +2/4 +3/4 +2/2. i/4 i/2 6/4 7/4 | i/2 i/4 i/4 i/2 i/2 i/2 7/4 6/4 6/2 7/2 | +6/4 7/4 +6/4 5/4 5 - 0/2 6/4 7/4 | "
     "i/2 i/4 i/4 i/2 7/4 i/4 7 7/2 6/4 7/4 | 7/2 7/4 7/4 7/2 +3/2 7/2. i/4 0/2 6/4 7/4 | i/2 i/4 i/4 i/2 +3/2 +2 i/2 7/4 6/4", 66.0),
    # 世上只有妈妈好：2026-10 照 1=D / 4/4 原谱图逐小节重读，8 小节 x 4 拍 = 32 拍，
    # 末尾带反复记号（两段歌词共用这段旋律，故原谱 8 小节自成一遍）。
    # 音高记号：原谱数字正下方带小圆点的 6 写成 "v6"、带点的 5 写成 "v5"（低八度 → 按住鼠标左键），
    # 数字正上方带小圆点的 1 写成 "i"（高音 1）。附点、延长线照原谱，不改动。
    # 减时线（八分音符）：原谱印面上看不到——对 33 个音符逐个做像素检查，基线以下一行零墨迹
    # （阈值放到 200/225 也没有），所以图里没有减时线可读。八分位置按"每小节必须凑满 4 拍"这个
    # 硬约束定：第 4（四个八分）/ 6（全四分）/ 7（五个八分）/ 8（低音 5 撑满四拍）小节被约束唯一确定；
    # 第 1 / 2 / 3 / 5 小节算术上仍有多个候选（3 / 3 / 10 / 4 种），取与同版乐谱一致的读法。
    ("世上只有妈妈好",
     "6. 5/2 3 5 | i 6/2 5/2 6 - | 3 5/2 6/2 5 3 | 1/2 v6/2 5/2 3/2 2 - | "
     "2. 3/2 5 5/2 6/2 | 3 2 1 - | 5. 3/2 2/2 1/2 v6/2 1/2 | v5 - - -", 96.0),
]

MODE_TEST_NOTE = "口琴模式测试"


def mode_test_song():
    """单独用代码拼「本音 + 三种鼠标方式」，比在简谱里硬写清楚。"""
    lines = []
    plan = [("本音", "Normal"), ("左键", "Left"), ("中键", "Middle"), ("右键", "Right")]
    beat = 0.0
    notes = []
    for _, mode in plan:
        for repeat in range(2):
            notes.append({"key": "Z", "mode": mode, "beat": round(beat, 6), "duration": 1.0})
            beat += 1.0
            notes.append({"key": "Z", "mode": "Normal", "beat": round(beat, 6), "duration": 1.0})
            beat += 2.0
        beat += 1.0
    del lines
    return {
        "name": MODE_TEST_NOTE,
        "bpm": 90.0,
        "timeSignature": "4/4",
        "beatUnit": "Quarter",
        "version": 1,
        "notes": notes,
    }


# 与 App 里 ModePrefixText 逐字一致（Middle 就是 MIDDLE，单字母 M 只是解析器容忍的写法）
MODE_PREFIX = {"Normal": "", "Left": "L", "Middle": "MIDDLE", "Right": "R",
               "LeftMiddle": "LM", "RightMiddle": "RM"}
WHOLE_FRACTIONS = [(1, 1), (1, 2), (1, 4), (1, 8), (1, 16), (1, 32)]


def beats_to_fraction(beats, denominator):
    """拍的时值 → 全音符分数；返回 (分子, 分母, 是否精确)。一拍 = 1/denominator 个全音符。"""
    want = float(beats) / float(denominator)
    best = None
    for num, den in WHOLE_FRACTIONS:
        diff = abs(want - float(num) / den)
        if best is None or diff < best[2]:
            best = (num, den, diff)
    return best[0], best[1], best[2] < 1e-6


def to_text(song):
    """与 App 里 SongToTextScore 同格式：[演奏方式+]键名 分数，一行一个音符（顺序累加）。"""
    denom = int(str(song["timeSignature"]).split("/")[1])
    lines, inexact, cursor = [], [], 0.0
    for i, n in enumerate(song["notes"], start=1):
        if n["beat"] > cursor + 1e-6:
            inexact.append("第 %d 个音符前有 %.3f 拍空档（文本谱不表示休止符）" % (i, n["beat"] - cursor))
        elif n["beat"] + 1e-6 < cursor:
            inexact.append("第 %d 个音符与前一个重叠（文本谱是顺序累加）" % i)
        cursor = n["beat"] + n["duration"]
        prefix = MODE_PREFIX.get(n["mode"], "")
        num, den, exact = beats_to_fraction(n["duration"], denom)
        lines.append("%s%s %d/%d" % ((prefix + "+") if prefix else "", n["key"], num, den))
        if not exact:
            inexact.append("第 %d 个音符（%s）时值 %.3f 拍不是标准时值，按最接近的 %d/%d 写"
                           % (i, n["key"], n["duration"], num, den))
    return "\n".join(lines) + "\n", inexact


def main():
    parser = argparse.ArgumentParser(description="简谱 → Harmonica Player 曲谱")
    parser.add_argument("--out", default="dist/songs", help="输出目录（默认 dist/songs）")
    parser.add_argument("--list", action="store_true", help="列出内置曲目")
    parser.add_argument("--print", dest="show", metavar="曲名", help="打印某首的换算结果，不写文件")
    parser.add_argument("--text", metavar="曲名", help="打印某首的文本曲谱（一行一个音符，格式与 App 的\"文本曲谱\"窗口相同，可直接粘进去）")
    args = parser.parse_args()

    if args.text:
        for title, jianpu, bpm in SONGS:
            if title == args.text:
                song = build_song(title, jianpu, bpm)
                text, inexact = to_text(song)
                print("=== %s（%d 个音符，%g bpm，%s，移调 %+d）"
                      % (title, len(song["notes"]), bpm, song["timeSignature"], song["shift"]))
                print(text, end="")
                for line in inexact:
                    print("# 近似：" + line)
                print("# 拍数合计 %.3f（原曲 %.3f）" % (sum(n["duration"] for n in song["notes"]), song["beats"]))
                return 0
        print("没有这首：%s（用 --list 看曲目）" % args.text)
        return 1

    if args.list:
        for title, jianpu, bpm in SONGS:
            song = build_song(title, jianpu, bpm)
            print("%-12s %d 个音符  %s拍  %s bpm  移调 %+d  音域 %s"
                  % (title, len(song["notes"]), song["beats"], song["bpm"], song["shift"],
                     "".join(song["used"])))
        print("%-12s %d 个音符（本音 + 左/中/右 对照）" % (MODE_TEST_NOTE, len(mode_test_song()["notes"])))
        return 0

    built = []
    for title, jianpu, bpm in SONGS:
        built.append(to_json(build_song(title, jianpu, bpm)))
    built.append(mode_test_song())

    if args.show:
        for song in built:
            if song["name"] == args.show:
                print(json.dumps(song, ensure_ascii=False, indent=4))
                return 0
        print("没有这首：%s" % args.show, file=sys.stderr)
        return 1

    directory = args.out
    if not os.path.isdir(directory):
        os.makedirs(directory)
    for song in built:
        path = os.path.join(directory, song["name"] + ".json")
        with open(path, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(song, handle, ensure_ascii=False, indent=4)
            handle.write("\n")
        print("写出 %s（%d 个音符）" % (path, len(song["notes"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
