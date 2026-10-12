#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""简谱 → Harmonica Player 曲谱（JSON）生成器。

它写的是给人看的简谱（数字），负责算出这个游戏口琴真正能弹的东西：

    口琴本音 8 个：  1 2 3 4 5 6 7 i        （i = 高音 1）
    对应键盘：       Z X C V B N M ,
    演奏方式：       正常按键 / 按住鼠标左键 / 中键 / 右键
                     （按游戏里的说法：左键=降调、中键=半音、右键=升调）
    音域：           1 到 i 共 8 个音（一个八度 + 高音 do）；再宽就得靠鼠标三种变音

用法：
    python tools/jianpu2score.py --list                列出内置曲目
    python tools/jianpu2score.py --out dist/songs      生成到曲谱目录（默认就是这个）
    python tools/jianpu2score.py --print 小星星        只打印一首的换算结果，不写文件

简谱写法（够用就行）：
    1 2 3 4 5 6 7       本音（do re mi fa sol la ti）
    i                   高音 1（键盘最右边那个逗号键）
    #4  b7              半音（用鼠标左键/右键变音，见下面的 MODE_ASSUMPTION）
    0                   休止符（不发音，但占时值）
    -                   延长前一个音一拍
    /2  /4              时值减半 / 四分之一（八分音符、十六分音符）
    | 空格 ,            只是给人看的，全部忽略

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

# 半音的写法 → (音级偏移, 演奏方式)
# 说明：升 d 用「比它高一级的音降半音」来弹（等价），所以 #4 = 降5 = 5 按左键。
# 「左键=降半音、右键=升半音」是按游戏里的说法定的；中键（"半音"）暂不参与换算，
# 等你在游戏里核对过音高再改这张表即可。
MODE_ASSUMPTION = {
    "flat": "Left",      # 降半音
    "sharp": "Right",    # 升半音
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
    """把一个简谱记号变成 (音级编号 or None, 演奏方式, 时值, 半音标记)。"""
    if token == "-":
        if previous is None:
            raise ScoreError("开头出现「-」：前面没有音可以延长")
        return ("extend", None, 1.0)
    if token == "0":
        return (None, None, 1.0)

    accidental = None
    body = token
    if body and body[0] in "#♯":
        accidental = "sharp"
        body = body[1:]
    elif body and body[0] in "b♭":
        accidental = "flat"
        body = body[1:]

    duration = 1.0
    if "/" in body:
        body, _, tail = body.partition("/")
        try:
            duration = 1.0 / float(tail)
        except ValueError:
            raise ScoreError("时值写法看不懂：「%s」" % token)

    if body == "i" or body == "I":
        index = 7
    elif body.isdigit():
        number = int(body)
        if not 1 <= number <= 7:
            raise ScoreError("简谱只认得 1~7 和 i，不认识「%s」" % token)
        index = number - 1
    else:
        raise ScoreError("记号看不懂：「%s」" % token)

    return (index, accidental, duration)


def tokenize(text):
    out = []
    for raw in text.replace("|", " ").replace(",", " ").split():
        out.append(raw)
    return out


def to_events(text):
    """简谱文本 → [(音级编号 or None, 演奏方式, 起始拍, 时值)]（尚未移调）。"""
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
        index, accidental, duration = parsed
        mode = "Normal"
        if accidental is not None and index is not None:
            # 升 d = 高一级的音降半音（等价），降 d = 本音降半音
            if accidental == "sharp":
                index += 1
                mode = MODE_ASSUMPTION["flat"]
                if index > HIGH:
                    index -= 1
                    mode = MODE_ASSUMPTION["sharp"]
            else:
                mode = MODE_ASSUMPTION["flat"]
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
    ("父亲",
     "i 3 2 6/2 5/2 | 6 5 3 - | i 3 2 5 | 3 - - 6/2 7/2 | i 3 2/2 i/2 7 | "
     "i/2 5/2 4 5/2 6/2 7 | i 3 2 5 | 6 - - - | 1/2 5/2 1/2 3/2 4/2 3/2 2/2 i/2 | 1/2 5/2 1/2 2/2 3 - | "
     "1/2 5/2 1/2 3/2 4/2 3/2 2/2 i/2 | 2/4 3/4 2/4 2/4 i 1 - | 1/2 5/2 1/2 3/2 4/2 3/2 2/2 i/2 | "
     "6/2 5/2 5/2 4/2 3 - | 1/2 5/2 1/2 3/2 4/2 3/2 2/2 1/2 | 3 2/2 2/2 i/2 1/2 1/2 1/2 | "
     "6/2 6/2 6/2 5/2 3/2 3/2 3/2 3/2 | 4 5/2 1/2 5/2 3/2 1/2 7/2 | 6/2 6/2 6/2 7/2 5/2 5/2 5/2 5/2 | "
     "5 6/2 5/2 4/2 3/2 3/2 2/2 | 2 - 0 6/2 7/2 | i/2 i/2 i/2 i/2 7/2 i/2 7/2 7/2 6/2 7/2 | "
     "7/2 7/2 7/2 7/2 2/2 3/2 2/2 i/2 i/2 6/2 7/2 | i/2 i/2 i/2 i/2 i/2 7/2 6/2 6/2 7/2 | 6/2 7/2 6/2 5/2 5 - 0 6/2 7/2 | "
     "i/2 i/2 i/2 i/2 7/2 i/2 7/2 7/2 6/2 7/2 | 7/2 7/2 7/2 7/2 3/2 7/2 i 0 6/2 7/2 | i/2 i/2 i/2 i/2 3/2 2/2 i/2 7/2 6", 66.0),
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


def main():
    parser = argparse.ArgumentParser(description="简谱 → Harmonica Player 曲谱")
    parser.add_argument("--out", default="dist/songs", help="输出目录（默认 dist/songs）")
    parser.add_argument("--list", action="store_true", help="列出内置曲目")
    parser.add_argument("--print", dest="show", metavar="曲名", help="打印某首的换算结果，不写文件")
    args = parser.parse_args()

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
