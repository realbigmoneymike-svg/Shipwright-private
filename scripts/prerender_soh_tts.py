"""Pre-render Ship of Harkinian's text-to-speech with a VoiceStudio voice.

Put this file in your SoH game folder (next to soh.exe) and run, with VoiceStudio open:

    python prerender_soh_tts.py --voice "Cosmo Wright" --name Link

It reads every dialogue line from oot.o2r / oot-mq.o2r and the menu/scene texts from
soh.o2r, turns each one into the exact text SoH speaks, and asks VoiceStudio for a WAV
of each line. The WAVs go into tts_cache/<voice>/ (one folder per voice), which the game
checks before asking VoiceStudio, so anything rendered here plays instantly in game.
Render each voice once; then switch voices in SoH: Settings > General > Accessibility.

- Safe to stop (Ctrl+C) and re-run: lines that are already rendered are skipped.
- --dry-run only counts the lines (and writes tts_lines.txt so you can look at them).
- Lines that say your name are rendered for every name in your Save folder. Use --name to
  pick names yourself instead (once per name, e.g. --name Link --name Mike).
- Not covered (these are still made live, then cached): randomizer item and hint text,
  numbers that change (rupees, timers, scores, tokens, the clock), and menu lines with a
  number in them.

Matches VoiceStudioSpeechSynthesizer.cpp: file name = FNV-1a 64 of b"<voice>|<text>".
"""
import argparse
import concurrent.futures as cf
import json
import os
import struct
import sys
import time
import urllib.error
import urllib.request
import zipfile

# ---- message control codes (include/message_data_fmt.h) ----
NEWLINE, END, BOX_BREAK, COLOR, SHIFT, TEXTID = 0x01, 0x02, 0x04, 0x05, 0x06, 0x07
QUICK_ON, QUICK_OFF, PERSISTENT, EVENT, BOX_BREAK_DELAYED = 0x08, 0x09, 0x0A, 0x0B, 0x0C
AWAIT, FADE, NAME, OCARINA, FADE2, SFX, ITEM_ICON = 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13
TEXT_SPEED, BACKGROUND, MARATHON_TIME, RACE_TIME, POINTS, TOKENS = 0x14, 0x15, 0x16, 0x17, 0x18, 0x19
UNSKIPPABLE, TWO_CHOICE, THREE_CHOICE, FISH_INFO, HIGHSCORE, TIME = 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F

BOX_ENDS = {BOX_BREAK, TEXTID, BOX_BREAK_DELAYED, EVENT, END}
DYNAMIC = {MARATHON_TIME, RACE_TIME, POINTS, TOKENS, FISH_INFO, HIGHSCORE, TIME}

MESSAGE_TABLES = [
    "text/nes_message_data_static/ntsc_nes_message_data_static",
    "text/nes_message_data_static/nes_message_data_static",
    "text/staff_message_data_static/staff_message_data_static",
]

# remap() in soh/Enhancements/tts/tts.cpp (0x9F+ are button names from misc_eng.json)
REMAP = dict(zip(range(0x80, 0x9F), "ÀîÂÄÇÈÉÊËÏÔÖÙÛÜßàáâäçèéêëïôöùûü"))
BUTTONS = {0x9F: "input_button_a", 0xA0: "input_button_b", 0xA1: "input_button_c", 0xA2: "input_button_l",
           0xA3: "input_button_r", 0xA4: "input_button_z", 0xA5: "input_button_c_up",
           0xA6: "input_button_c_down", 0xA7: "input_button_c_left", 0xA8: "input_button_c_right",
           0xAA: "input_analog_stick", 0xAB: "input_d_pad"}


def folder_name(voice: str) -> str:
    """Same as VoiceStudioSpeechSynthesizer::FolderName."""
    out = "".join("_" if ord(c) < 0x20 or c in '<>:"/\\|?*' else c for c in voice).rstrip(". ")
    return out or "_"


def fnv1a64(data: bytes) -> int:
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


# ---------------------------------------------------------------- reading o2r files
def read_message_table(blob: bytes):
    """Binary Text resource: 0x40-byte header, u32 count, then {u16 id, u8, u8, u32 len, bytes}."""
    little = blob[0] == 0
    e = "<" if little else ">"
    pos = 0x40
    (count,) = struct.unpack_from(e + "I", blob, pos)
    pos += 4
    out = []
    for _ in range(count):
        mid, _t, _y, ln = struct.unpack_from(e + "HBBI", blob, pos)
        pos += 8
        out.append((mid, blob[pos:pos + ln]))
        pos += ln
    return out


def load_json_bank(z, name):
    raw = z.read(f"accessibility/texts/{name}_eng.json")
    i = raw.find(b"{")  # tolerate a resource header in front of the JSON
    return json.loads(raw[i:].decode("utf-8"))


def save_file_names(game_dir):
    """Player names from Save/file*.sav, decoded like Message_DecodeName (z_message_PAL.c)."""
    def find(o):
        if isinstance(o, dict):
            if "playerName" in o:
                yield o.get("filenameLanguage", 0), o["playerName"]
            for v in o.values():
                yield from find(v)
        elif isinstance(o, list):
            for v in o:
                yield from find(v)

    names = set()
    folder = os.path.join(game_dir, "Save")
    for f in sorted(os.listdir(folder)) if os.path.isdir(folder) else []:
        if not (f.startswith("file") and f.endswith(".sav")):
            continue
        try:
            with open(os.path.join(folder, f), encoding="utf-8") as fh:
                found = list(find(json.load(fh)))
        except Exception:
            continue
        for lang, raw in found[:1]:
            pal = lang == 0
            empty = 0x3E if pal else 0xDF
            raw = list(raw)
            while raw and raw[-1] == empty:
                raw.pop()
            out = ""
            for c in raw:
                if pal:
                    c = {0x3E: 32, 0x40: 46, 0x3F: 45}.get(c, c + 48 if c < 0xA else c + 55 if c < 0x24
                                                       else c + 61 if c < 0x3E else c)
                else:
                    c = {0xDF: 32, 0xEA: 46, 0xE4: 45}.get(c, c + 48 if c < 0xA else c - 0x6A if c < 0xC5
                                                       else c - 0x64 if c < 0xDF else c)
                out += chr(c) if 32 <= c < 127 else "?"
            if out.strip():
                names.add(out)
    return sorted(names)


# ---------------------------------------------------------------- copy of SoH's decoding
def decode_box(buf, p, name_bytes):
    """Message_Decode (z_message_PAL.c) for one text box.
    Returns (decoded bytes, start of next box or None, has changing numbers, says the player's name)."""
    d = bytearray()
    dynamic = has_name = False
    n = len(buf)
    while p < n:
        c = buf[p]
        if c in BOX_ENDS:
            if c == BOX_BREAK:
                return d, p + 1, dynamic, has_name
            if c == BOX_BREAK_DELAYED:
                return d, p + 2, dynamic, has_name
            return d, None, dynamic, has_name  # TEXTID / EVENT / END: next message is its own entry
        if c == NAME:
            has_name = True
            d += name_bytes
            p += 1
            continue
        if c in DYNAMIC:
            dynamic = True
            d.append(c)
            p += 2 if c == HIGHSCORE else 1
            continue
        d.append(c)
        if c in (COLOR, FADE, SHIFT, TEXT_SPEED, ITEM_ICON):
            d.append(buf[p + 1] if p + 1 < n else 0)
            p += 2
        elif c in (FADE2, SFX):
            d += buf[p + 1:p + 3]
            p += 3
        elif c == BACKGROUND:
            p += 4
        else:
            p += 1
    return d, None, dynamic, has_name


def tts_decode(d, start, size, misc):
    """Message_TTS_Decode (soh/Enhancements/tts/tts.cpp), byte for byte."""
    out = bytearray()
    listing = False
    i = 0
    while i < size:
        c = d[start + i]
        if c < 0x20:
            if c == NEWLINE:
                out += b"\n" if listing else b" "
            elif c in (THREE_CHOICE, TWO_CHOICE):
                out += b"\n"
                listing = True
            elif c in (COLOR, SHIFT, TEXT_SPEED, BOX_BREAK_DELAYED, FADE, ITEM_ICON):
                i += 1
            elif c in (FADE2, SFX, TEXTID):
                i += 2
        elif c <= 0x80:
            out.append(c)  # SoH copies 0x80 as a raw byte too
        elif c in REMAP:
            out += REMAP[c].encode("utf-8")
        elif c in BUTTONS:
            out += misc.get(BUTTONS[c], "unknown").encode("utf-8")
        i += 1
    return bytes(out)


def choice_lines(d, misc):
    """The single choice line SoH speaks when you move the cursor over it."""
    start = 0
    while start < len(d):
        if d[start] in (TWO_CHOICE, THREE_CHOICE):
            start += 1
            break
        start += 1
    else:
        return []
    lines = []
    while start < len(d):
        end = start
        while end < len(d) and d[end] != NEWLINE:
            end += 1
        if end != start:
            lines.append(tts_decode(d, start, end - start, misc))
        start = end + 1
    return lines


# ---------------------------------------------------------------- collecting lines
def collect_lines(game_dir, names):
    lines, skipped = set(), 0
    soh = zipfile.ZipFile(os.path.join(game_dir, "soh.o2r"))
    misc = load_json_bank(soh, "misc")

    name_bytes = [n.encode("ascii", "replace") for n in names]
    for archive in ("oot.o2r", "oot-mq.o2r"):
        path = os.path.join(game_dir, archive)
        if not os.path.exists(path):
            continue
        z = zipfile.ZipFile(path)
        for table in MESSAGE_TABLES:
            if table not in z.namelist():
                continue
            for _mid, msg in read_message_table(z.read(table)):
                p = 0
                while p is not None:
                    d, nxt, dynamic, has_name = decode_box(msg, p, b"")
                    if dynamic or (has_name and not name_bytes):
                        skipped += 1
                    else:
                        boxes = [decode_box(msg, p, nb)[0] for nb in name_bytes] if has_name else [d]
                        for box in boxes:
                            if box:
                                lines.add(tts_decode(box, 0, len(box), misc))
                            lines.update(choice_lines(box, misc))
                    p = nxt

    # Menus, scene names (title cards and map), file select: every text with no number in it
    banks = {b: load_json_bank(soh, b) for b in ("misc", "kaleidoscope", "filechoose", "scenes")}
    for bank in banks.values():
        for v in bank.values():
            if "$" not in v:
                lines.add(v.encode("utf-8"))
    kal = banks["kaleidoscope"]
    equipped = kal.get("equipped") or next((v for v in kal.values() if v.startswith("$0 - ")), None)
    if equipped:
        for v in kal.values():
            if "$" not in v:
                lines.add(equipped.replace("$0", v, 1).encode("utf-8"))
    for key in ("yes", "no"):
        if key in banks["misc"] and "save_prompt" in kal:
            lines.add((kal["save_prompt"] + " - " + banks["misc"][key]).encode("utf-8"))

    lines.discard(b"")
    return sorted(lines), skipped


# ---------------------------------------------------------------- rendering
def render(text: bytes, voice, base, path):
    body = json.dumps({"model": "tts-1", "voice": voice, "input": text.decode("utf-8", "replace"),
                       "response_format": "wav"}).encode()
    req = urllib.request.Request(base + "/v1/audio/speech", data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=300) as r:
        data = r.read()
    if not data.startswith(b"RIFF"):
        raise RuntimeError("VoiceStudio didn't send back a WAV")
    tmp = path + ".part"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)  # the game never sees a half-written file


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--voice", required=True, help="the voice name exactly as in VoiceStudio")
    ap.add_argument("--name", action="append", default=[], help="player name for lines that say it (repeatable; default: names in your Save folder)")
    ap.add_argument("--game-dir", default=here)
    ap.add_argument("--base", default="http://127.0.0.1:3900")
    ap.add_argument("--workers", type=int, default=1, help="lines rendered at once (1-2 for one GPU)")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    names = a.name or save_file_names(a.game_dir)
    print("Player names:", ", ".join(names) if names else "(none found - lines with your name stay live)")
    lines, skipped = collect_lines(a.game_dir, names)
    root = os.path.join(a.game_dir, "tts_cache")
    cache = os.path.join(root, folder_name(a.voice))
    os.makedirs(cache, exist_ok=True)
    vb = a.voice.encode("utf-8") + b"|"
    jobs = [(t, os.path.join(cache, f"{fnv1a64(vb + t)}.wav")) for t in lines]
    moved = 0
    for _t, path in jobs:  # files from before voices had their own folders
        old = os.path.join(root, os.path.basename(path))
        if os.path.exists(old) and not os.path.exists(path):
            os.replace(old, path)
            moved += 1
    if moved:
        print(f"Moved {moved} {a.voice} files into tts_cache/{folder_name(a.voice)}/")
    todo = [j for j in jobs if not os.path.exists(j[1])]
    print(f"{len(lines)} lines found, {len(jobs) - len(todo)} already rendered, {len(todo)} to go "
          f"({skipped} text boxes with changing numbers are left to render live).")

    if a.dry_run:
        with open(os.path.join(a.game_dir, "tts_lines.txt"), "w", encoding="utf-8") as f:
            for t in lines:
                f.write(t.decode("utf-8", "replace").replace("\n", " / ") + "\n")
        print("Wrote tts_lines.txt")
        return

    try:
        urllib.request.urlopen(a.base + "/health", timeout=5).read()
    except urllib.error.HTTPError:
        pass  # the server answered, just not on /health
    except Exception:
        sys.exit(f"Can't reach VoiceStudio at {a.base}. Open VoiceStudio first.")

    done = failed = 0
    t0 = time.time()
    with cf.ThreadPoolExecutor(max_workers=max(1, a.workers)) as pool:
        futs = {pool.submit(render, t, a.voice, a.base, p): t for t, p in todo}
        try:
            for fut in cf.as_completed(futs):
                text = futs[fut].decode("utf-8", "replace").replace("\n", " / ")
                try:
                    fut.result()
                    done += 1
                except Exception as e:
                    failed += 1
                    print(f"  FAILED: {text[:60]} ({e})")
                n = done + failed
                if n % 10 == 0 or n == len(todo):
                    rate = n / max(time.time() - t0, 1e-6)
                    left = (len(todo) - n) / rate if rate else 0
                    print(f"[{n}/{len(todo)}] about {left / 60:.0f} min left - {text[:50]}")
        except KeyboardInterrupt:
            print("\nStopping... run it again later to carry on where it left off.")
            pool.shutdown(wait=False, cancel_futures=True)
            return
    print(f"Done: {done} rendered, {failed} failed.")
    if failed and done == 0:
        print(f'Nothing rendered - check the voice name "{a.voice}" matches VoiceStudio exactly.')


if __name__ == "__main__":
    main()
