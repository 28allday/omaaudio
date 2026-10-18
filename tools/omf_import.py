#!/usr/bin/env python3
"""Read an OMF 2.x interchange file into the session-import manifest.

Private worker protocol, version 1 -- the same manifest as aaf_import.py, so the
application installs either with one transaction. stdout is JSON only. The parent
owns the scratch directory, enforces cancellation/time/output limits and copies
all audio before deleting scratch. No project or original media is modified here.

Written from the OMF Interchange Specification 2.1 (Avid, 1997) and the Bento
container layout (Apple, Bento 1.0d5): a label at the end of the file locates a
table of contents that lists every object's properties and where each value's
bytes are. Object references are 4-byte keys resolved through a per-value
reference list. Only audio is read; anything that cannot be reproduced exactly
is refused with a reason rather than approximated.
"""
from fractions import Fraction
from pathlib import Path
from urllib.parse import unquote, urlsplit
import argparse
import json
import math
import os
import re
import resource
import struct
import sys
import wave

MAX_FRAMES = 10**12
MAX_CLIPS = 100000
MAX_SOURCES = 10000
MAX_TRACKS = 256
MAX_OBJECTS = 2_000_000
MAX_TOC = 256 * 1024 * 1024
MAX_VALUE = 4 * 1024**3
MAGIC = b"\xa4CM\xa5Hdr\xd7"
LABEL_SIZE = 24

# Bento TOC codes and standard ids.
NEW_OBJECT, NEW_PROPERTY, NEW_TYPE, EXPLICIT_GEN = 1, 2, 3, 4
OFFSET4_LEN4, CONT_OFFSET4_LEN4, OFFSET8_LEN4, CONT_OFFSET8_LEN4 = 5, 6, 7, 8
IMMEDIATE0, IMMEDIATE4, CONT_IMMEDIATE4 = 9, 13, 14
REFERENCE_LIST_ID, END_OF_BUFFER, NOOP = 15, 24, 0xFF
PROP_GLOBAL_TYPE_NAME, PROP_GLOBAL_PROPERTY_NAME, PROP_OBJ_REFERENCES = 23, 24, 31

DISSOLVES = {"omfi:effect:SimpleMonoAudioDissolve", "omfi:effect:SimpleStereoAudioDissolve",
             "omfi:effect:MonoAudioDissolve", "omfi:effect:StereoAudioDissolve"}
GAINS = {"omfi:effect:MonoAudioGain", "omfi:effect:StereoAudioGain"}
SOUND = {"omfi:data:Sound", "omfi:data:StereoSound"}


class ImportFailure(Exception):
    pass


class Value:
    __slots__ = ("type", "segments", "references")

    def __init__(self, type_id, references):
        self.type = type_id
        self.segments = []          # (offset, length) or bytes for immediates
        self.references = references


class Bento:
    """The container: objects -> property id -> values, and the name tables."""

    def __init__(self, path):
        self.file = open(path, "rb")
        self.size = os.fstat(self.file.fileno()).st_size
        self.objects = {}           # object id -> {property id: Value}
        self.names = {}             # property/type object id -> name
        self.order = "<"
        try:
            self._read()
        except BaseException:
            self.file.close()
            raise

    def close(self):
        self.file.close()

    def _label(self):
        if self.size < LABEL_SIZE:
            raise ImportFailure("This is not an OMF file: it is too small.")
        self.file.seek(self.size - LABEL_SIZE)
        label = self.file.read(LABEL_SIZE)
        if label[:8] != MAGIC:
            # Some writers pad after the label; look back a bounded distance.
            span = min(self.size, 1024 * 1024)
            self.file.seek(self.size - span)
            tail = self.file.read(span)
            at = tail.rfind(MAGIC)
            if at < 0 or at + LABEL_SIZE > len(tail):
                raise ImportFailure("This is not an OMF file: no Bento container label was found.")
            label = tail[at:at + LABEL_SIZE]
        for order in ("<", ">"):
            flags, block, major, _minor, offset, size = struct.unpack(order + "HHHHII", label[8:])
            if major == 2 and block and flags in (0, 0x0101):
                self.order = order
                return block * 1024, offset, size
        raise ImportFailure("This OMF file uses an unsupported container version.")

    def _read(self):
        block, offset, size = self._label()
        if size > MAX_TOC or offset > self.size or size > self.size - offset:
            raise ImportFailure("The OMF table of contents is outside the file.")
        self.file.seek(offset)
        toc = self.file.read(size)
        if len(toc) != size:
            raise ImportFailure("The OMF table of contents is truncated.")
        at = 0

        def code():
            nonlocal at
            while True:
                if at >= len(toc):
                    return None
                value = toc[at]
                at += 1
                if value == END_OF_BUFFER:
                    at = ((at + block - 1) // block) * block
                    continue
                if value == NOOP:
                    continue
                return value

        def dword():
            nonlocal at
            if at + 4 > len(toc):
                raise ImportFailure("The OMF table of contents is truncated.")
            value = struct.unpack_from(self.order + "I", toc, at)[0]
            at += 4
            return value

        def qword():
            nonlocal at
            if at + 8 > len(toc):
                raise ImportFailure("The OMF table of contents is truncated.")
            value = struct.unpack_from(self.order + "Q", toc, at)[0]
            at += 8
            return value

        look = code()
        while look == NEW_OBJECT:
            obj = dword()
            if len(self.objects) >= MAX_OBJECTS:
                raise ImportFailure("The OMF file contains too many objects.")
            properties = self.objects.setdefault(obj, {})
            look = NEW_PROPERTY
            while look == NEW_PROPERTY:
                prop = dword()
                look = NEW_TYPE
                while look == NEW_TYPE:
                    type_id = dword()
                    look = code()
                    references = 0
                    if look == EXPLICIT_GEN:
                        dword()
                        look = code()
                    if look == REFERENCE_LIST_ID:
                        references = dword()
                        look = code()
                    value = Value(type_id, references)
                    while look is not None and OFFSET4_LEN4 <= look <= CONT_IMMEDIATE4:
                        if look in (OFFSET4_LEN4, CONT_OFFSET4_LEN4):
                            start, length = dword(), dword()
                            value.segments.append((start, length))
                        elif look in (OFFSET8_LEN4, CONT_OFFSET8_LEN4):
                            start = qword()
                            length = dword()
                            value.segments.append((start, length))
                        else:
                            length = 0 if look == IMMEDIATE0 else (4 if look in (IMMEDIATE4, CONT_IMMEDIATE4) else look - IMMEDIATE0)
                            if length:
                                if at + 4 > len(toc):
                                    raise ImportFailure("The OMF table of contents is truncated.")
                                value.segments.append(bytes(toc[at:at + length]))
                                at += 4
                        look = code()
                    if prop in (PROP_GLOBAL_PROPERTY_NAME, PROP_GLOBAL_TYPE_NAME):
                        self.names[obj] = self.raw(value)[:4096].split(b"\0", 1)[0].decode("ascii", "replace")
                    else:
                        # A property with several values keeps the first (OMF uses one).
                        properties.setdefault(prop, value)
        if look is not None:
            raise ImportFailure("The OMF table of contents is malformed.")
        self.by_name = {}
        for obj, name in self.names.items():
            self.by_name.setdefault(name, obj)

    def raw(self, value, limit=MAX_VALUE):
        total = sum(len(s) if isinstance(s, bytes) else s[1] for s in value.segments)
        if total > limit:
            raise ImportFailure("An OMF value exceeds the supported size.")
        parts = []
        for segment in value.segments:
            if isinstance(segment, bytes):
                parts.append(segment)
                continue
            start, length = segment
            if start > self.size or length > self.size - start:
                raise ImportFailure("An OMF value lies outside the file.")
            self.file.seek(start)
            data = self.file.read(length)
            if len(data) != length:
                raise ImportFailure("An OMF value is truncated.")
            parts.append(data)
        return b"".join(parts)

    def stream(self, value, target):
        """Copy a large value (embedded audio) without holding it in memory."""
        for segment in value.segments:
            if isinstance(segment, bytes):
                target.write(segment)
                continue
            start, length = segment
            if start > self.size or length > self.size - start:
                raise ImportFailure("Embedded OMF audio lies outside the file.")
            self.file.seek(start)
            while length:
                chunk = self.file.read(min(length, 1024 * 1024))
                if not chunk:
                    raise ImportFailure("Embedded OMF audio is truncated.")
                target.write(chunk)
                length -= len(chunk)

    def reference_map(self, value):
        if not value.references:
            return {}
        holder = self.objects.get(value.references, {})
        listing = holder.get(PROP_OBJ_REFERENCES)
        if listing is None:
            return {}
        data = self.raw(listing, 64 * 1024 * 1024)
        pairs = {}
        for at in range(0, len(data) - 7, 8):
            key, target = struct.unpack_from(self.order + "II", data, at)
            pairs[key] = target
        return pairs


class Omf:
    """OMF objects addressed by property name, with byte order from the header."""

    def __init__(self, bento):
        self.b = bento
        self.order = bento.order
        self.ids = bento.by_name
        header = self.find_class("HEAD")
        if header is None:
            if "OMFI:ObjID" in self.ids:
                raise ImportFailure("This is an OMF 1.0 file. Export OMF 2.0 from the editing application.")
            raise ImportFailure("This file has no OMF header.")
        self.header = header
        order = self.data(header, "OMFI:HEAD:ByteOrder")
        if order and order[:2] in (b"II", b"MM"):
            self.order = "<" if order[:2] == b"II" else ">"

    def value(self, obj, name):
        prop = self.ids.get(name)
        return None if prop is None else self.b.objects.get(obj, {}).get(prop)

    def data(self, obj, name, limit=16 * 1024 * 1024):
        value = self.value(obj, name)
        return None if value is None else self.b.raw(value, limit)

    def int(self, obj, name, default=None, signed=True):
        data = self.data(obj, name, 16)
        if data is None:
            return default
        if len(data) == 8:
            return struct.unpack(self.order + ("q" if signed else "Q"), data)[0]
        if len(data) == 4:
            return struct.unpack(self.order + ("i" if signed else "I"), data)[0]
        if len(data) == 2:
            return struct.unpack(self.order + ("h" if signed else "H"), data)[0]
        if len(data) == 1:
            return data[0]
        raise ImportFailure(f"OMF property {name} has an unexpected size.")

    def string(self, obj, name, default=""):
        data = self.data(obj, name, 64 * 1024)
        if data is None:
            return default
        return data.split(b"\0", 1)[0].decode("utf-8", "replace") or default

    def rational(self, obj, name):
        data = self.data(obj, name, 16)
        if data is None or len(data) != 8:
            raise ImportFailure(f"OMF {name} is missing or invalid.")
        num, den = struct.unpack(self.order + "ii", data)
        if den <= 0:
            raise ImportFailure(f"OMF {name} has an invalid denominator.")
        return Fraction(num, den)

    def uid(self, obj, name):
        data = self.data(obj, name, 16)
        if data is None or len(data) != 12:
            return None
        return struct.unpack(self.order + "iii", data)

    def cls(self, obj):
        data = self.data(obj, "OMFI:OOBJ:ObjClass", 8)
        return data[:4].decode("ascii", "replace") if data else ""

    def ref(self, obj, name):
        value = self.value(obj, name)
        if value is None:
            return None
        data = self.b.raw(value, 16)
        if len(data) < 4:
            return None
        key = struct.unpack(self.b.order + "I", data[:4])[0]
        target = self.b.reference_map(value).get(key, key)
        return target if target in self.b.objects else None

    def refs(self, obj, name):
        value = self.value(obj, name)
        if value is None:
            return []
        data = self.b.raw(value, 64 * 1024 * 1024)
        mapping = self.b.reference_map(value)
        # Writers may prefix the array with a 4- or 2-byte count, or not at all.
        # Prefer the reading whose keys are exactly the value's reference list.
        candidates = []
        for width, fmt in ((4, "I"), (2, "H")):
            if len(data) >= width and (len(data) - width) % 4 == 0:
                count = struct.unpack_from(self.b.order + fmt, data, 0)[0]
                if count == (len(data) - width) // 4:
                    candidates.append(struct.unpack_from(self.b.order + "%dI" % count, data, width))
        if len(data) % 4 == 0:
            candidates.append(struct.unpack(self.b.order + "%dI" % (len(data) // 4), data))
        if not candidates:
            raise ImportFailure(f"OMF {name} is not a valid list of objects.")
        keys = candidates[0]
        if mapping:
            exact = [c for c in candidates if len(c) == len(mapping) and all(k in mapping for k in c)]
            if exact:
                keys = exact[0]
        result = []
        for key in keys:
            target = mapping.get(key, key)
            if target not in self.b.objects:
                raise ImportFailure(f"OMF {name} refers to a missing object.")
            result.append(target)
        return result

    def find_class(self, name):
        for obj in self.b.objects:
            if self.cls(obj) == name:
                return obj
        return None


class Reader:
    def __init__(self, path, scratch, sample_rate, media_folder=None):
        self.path = Path(path)
        self.scratch = Path(scratch)
        self.sample_rate = sample_rate
        self.roots = [Path(media_folder)] if media_folder else []
        self.roots.append(self.path.parent)
        self.sources, self.source_ids, self.extracted = [], {}, {}
        self.warnings, self.visits, self.count, self.index = [], 0, 0, None

    def warn(self, text):
        if text not in self.warnings and len(self.warnings) < 50:
            self.warnings.append(text)

    def frame(self, seconds):
        value = seconds * self.sample_rate
        frames = math.floor(value + Fraction(1, 2))
        if abs(frames) > MAX_FRAMES:
            raise ImportFailure("An OMF time lies outside the supported range.")
        return int(frames)

    # --- mobs ------------------------------------------------------------------
    def kind(self, obj):
        definition = self.o.ref(obj, "OMFI:CPNT:DataKind")
        return self.o.string(definition, "OMFI:DDEF:DataKindID") if definition else ""

    def slot_named(self, mob, track):
        for slot in self.o.refs(mob, "OMFI:MOBJ:Slots"):
            desc = self.o.ref(slot, "OMFI:MSLT:TrackDesc")
            if desc is not None and self.o.int(desc, "OMFI:TRKD:TrackID", -1, False) == track:
                return slot
        return None

    def media(self, mob, slot):
        key = (mob, slot)
        if key in self.source_ids:
            return self.source_ids[key]
        if len(self.sources) >= MAX_SOURCES:
            raise ImportFailure("The OMF contains too many audio sources.")
        descriptor = self.o.ref(mob, "OMFI:SMOB:MediaDescription")
        kind = self.o.cls(descriptor) if descriptor else ""
        if kind not in ("WAVD", "AIFD"):
            raise ImportFailure(f"Unsupported OMF audio media ({kind or 'none'}). Export WAVE or AIFC audio.")
        uid = self.o.uid(mob, "OMFI:MOBJ:MobID")
        embedded = self.mobdata.get(uid)
        name = self.o.string(mob, "OMFI:MOBJ:Name")
        if embedded is not None and self.o.int(descriptor, "OMFI:MDFL:IsOMFI", 1, False):
            path = self.extract(embedded, descriptor, kind)
            original = str(self.path)
        else:
            path = self.locate(descriptor)
            original = path
        entry = {"path": path, "original": original, "name": name or Path(path).name, "channel": -1}
        self.sources.append(entry)
        self.source_ids[key] = len(self.sources) - 1
        return self.source_ids[key]

    def extract(self, data_obj, descriptor, kind):
        if data_obj in self.extracted:
            return self.extracted[data_obj]
        output = self.scratch / f"essence-{len(self.extracted)}.wav"
        audio = self.o.value(data_obj, "OMFI:WAVE:AudioData" if kind == "WAVD" else "OMFI:AIFC:AudioData")
        if audio is None:
            raise ImportFailure("Embedded OMF audio is missing its data.")
        prefix = self.peek(audio, 12)
        summary = self.o.data(descriptor, "OMFI:WAVD:Summary" if kind == "WAVD" else "OMFI:AIFD:Summary", 1024 * 1024) or b""
        if kind == "WAVD":
            if prefix[:4] == b"RIFF":
                with output.open("xb") as target:
                    self.o.b.stream(audio, target)
            else:
                channels, rate, bits = self.wave_format(summary)
                with output.open("xb") as target:
                    self.write_wave_header(target, channels, rate, bits, self.length(audio))
                    self.o.b.stream(audio, target)
        else:
            raw = self.o.b.raw(audio, 1024**3)
            if raw[:4] == b"FORM":
                channels, rate, bits, offset, frames = self.aifc_layout(raw, raw)
                data = raw[offset:offset + frames * channels * (bits // 8)]
            else:
                channels, rate, bits, _offset, _frames = self.aifc_layout(summary, None)
                data = raw
            width = bits // 8
            if width == 1:
                converted = bytes((b + 128) & 0xFF for b in data)  # signed AIFF to unsigned WAVE
            else:
                swapped = bytearray(len(data) - len(data) % width)
                for i in range(width):
                    swapped[i::width] = data[width - 1 - i:len(swapped):width]
                converted = bytes(swapped)
            with output.open("xb") as target:
                self.write_wave_header(target, channels, rate, bits, len(converted))
                target.write(converted)
        self.extracted[data_obj] = str(output)
        return str(output)

    def peek(self, value, count):
        out = b""
        for segment in value.segments:
            if isinstance(segment, bytes):
                out += segment
            else:
                start, length = segment
                self.o.b.file.seek(start)
                out += self.o.b.file.read(min(length, count - len(out)))
            if len(out) >= count:
                break
        return out[:count]

    @staticmethod
    def length(value):
        return sum(len(s) if isinstance(s, bytes) else s[1] for s in value.segments)

    def wave_format(self, summary):
        at = 12 if summary[:4] == b"RIFF" else 0
        while at + 8 <= len(summary):
            chunk, size = summary[at:at + 4], struct.unpack_from("<I", summary, at + 4)[0]
            if chunk == b"fmt " and size >= 16:
                tag, channels, rate, _avg, _align, bits = struct.unpack_from("<HHIIHH", summary, at + 8)
                if tag != 1 or not 1 <= channels <= 32 or bits not in (8, 16, 24, 32):
                    raise ImportFailure("Embedded OMF WAVE audio is not uncompressed PCM.")
                return channels, rate, bits
            at += 8 + size + (size & 1)
        raise ImportFailure("Embedded OMF WAVE audio has no format description.")

    def aifc_layout(self, data, whole):
        if data[:4] != b"FORM":
            raise ImportFailure("Embedded OMF AIFC audio has no format description.")
        at, channels, frames, bits, rate, offset = 12, None, 0, 0, 0, None
        while at + 8 <= len(data):
            chunk, size = data[at:at + 4], struct.unpack_from(">I", data, at + 4)[0]
            if chunk == b"COMM" and size >= 18:
                channels, frames, bits = struct.unpack_from(">hIh", data, at + 8)
                exponent, mantissa = struct.unpack_from(">HQ", data, at + 16)
                rate = round(mantissa * 2.0 ** ((exponent & 0x7FFF) - 16383 - 63))
                if size >= 22 and data[8:12] == b"AIFC" and data[at + 26:at + 30] not in (b"NONE", b"twos", b""):
                    raise ImportFailure("Embedded OMF AIFC audio is compressed.")
            elif chunk == b"SSND":
                offset = at + 16 + struct.unpack_from(">I", data, at + 8)[0]
            at += 8 + size + (size & 1)
        if not channels or bits not in (8, 16, 24, 32) or not 8000 <= rate <= 768000:
            raise ImportFailure("Embedded OMF AIFC audio has an unsupported format.")
        if whole is not None and offset is None:
            raise ImportFailure("Embedded OMF AIFC audio has no sample data.")
        return channels, rate, bits, offset or 0, frames

    @staticmethod
    def write_wave_header(target, channels, rate, bits, length):
        align = channels * (bits // 8)
        target.write(b"RIFF" + struct.pack("<I", 36 + length) + b"WAVEfmt " + struct.pack(
            "<IHHIIHH", 16, 1, channels, rate, rate * align, align, bits) + b"data" + struct.pack("<I", length))

    def locate(self, descriptor):
        candidates, names = [], []
        for locator in self.o.refs(descriptor, "OMFI:MDES:Locator"):
            kind = self.o.cls(locator)
            raw = {"UNXL": "OMFI:UNXL:PathName", "DOSL": "OMFI:DOSL:PathName", "WINL": "OMFI:WINL:PathName",
                   "TXTL": "OMFI:TXTL:Name", "NETL": "OMFI:NETL:URLString"}.get(kind)
            if kind == "MACL":
                text = self.o.string(locator, "OMFI:MACL:PathName") or self.o.string(locator, "OMFI:MACL:FileName")
                text = text.replace(":", "/")
            elif raw:
                text = self.o.string(locator, raw)
            else:
                continue
            if not text:
                continue
            parts = urlsplit(text)
            if parts.scheme and parts.scheme.lower() not in ("file",) and not re.match(r"^[A-Za-z]:[/\\]", text):
                self.warn("Network media locations are not opened; choose a local audio folder.")
                continue
            decoded = unquote(parts.path) if parts.scheme.lower() == "file" else text.replace("\\", "/")
            name = Path(decoded).name
            if name:
                names.append(name)
            for root in self.roots:
                if not Path(decoded).is_absolute() and not re.match(r"^[A-Za-z]:", decoded):
                    candidates.append(root / decoded)
                candidates.append(root / name)
            if Path(decoded).is_absolute():
                candidates.append(Path(decoded))
        for candidate in candidates:
            if candidate.is_file():
                return str(candidate.resolve())
        if self.index is None:
            self.index = {}
            seen = 0
            for root in self.roots:
                for folder, dirs, files in os.walk(root, followlinks=False):
                    for file in files:
                        seen += 1
                        if seen > 100000:
                            break
                        self.index.setdefault(file, []).append(Path(folder) / file)
        for name in names:
            matches = self.index.get(name, [])
            if len(matches) == 1:
                return str(matches[0].resolve())
            if len(matches) > 1:
                raise ImportFailure(f"Several files are named {name}; choose the exact media folder.")
        label = names[0] if names else "the OMF's external audio"
        raise ImportFailure(f"Missing media: {label}. Choose the folder that holds the exported audio.")

    # --- timeline ------------------------------------------------------------------
    def fade(self, clip, direction, length, offset):
        if length <= 0 or offset >= length:
            return
        key = "fade_" + direction
        if clip[key]:
            raise ImportFailure("Overlapping OMF fades must be rendered before export.")
        clip[key] = length
        clip[key + "_offset"] = offset

    def effect_id(self, effect):
        definition = self.o.ref(effect, "OMFI:EFFE:EffectKind")
        return self.o.string(definition, "OMFI:EDEF:EffectID") if definition else ""

    def walk(self, segment, edit_rate, begin, duration, timeline, gain=1.0, depth=0, chain=()):
        self.visits += 1
        if depth > 64 or self.visits > MAX_CLIPS * 10:
            raise ImportFailure("The OMF contains a cyclic or excessively complex timeline.")
        if duration <= 0:
            return []
        if begin < 0:
            raise ImportFailure("The OMF requests audio before the beginning of a source.")
        o = self.o
        kind = o.cls(segment)
        length = Fraction(o.int(segment, "OMFI:CPNT:Length", 0)) / edit_rate
        if kind == "FILL":
            return []
        if kind == "SEQU":
            if begin + duration > length:
                raise ImportFailure("An OMF reference lies outside its sequence.")
            output, groups, transitions, cursor = [], [], [], Fraction(0)
            for component in o.refs(segment, "OMFI:SEQU:Components"):
                part = Fraction(o.int(component, "OMFI:CPNT:Length", 0)) / edit_rate
                if part < 0:
                    raise ImportFailure("An OMF segment has a negative length.")
                if o.cls(component) == "TRAN":
                    cursor -= part
                    transitions.append((cursor, cursor + part, component, len(groups)))
                    continue
                left, right = max(cursor, begin), min(cursor + part, begin + duration)
                clips = self.walk(component, edit_rate, left - cursor, right - left, timeline + left - begin,
                                  gain, depth + 1, chain) if right > left else []
                groups.append((cursor, cursor + part, clips))
                output.extend(clips)
                cursor += part
            for start, end, transition, index in transitions:
                if index == 0 or index >= len(groups) or groups[index - 1][1] != end or groups[index][0] != start:
                    raise ImportFailure("An OMF transition has no matching adjacent clips.")
                if end <= begin or start >= begin + duration:
                    continue
                effect = o.ref(transition, "OMFI:TRAN:Effect")
                name = self.effect_id(effect) if effect else ""
                if name not in DISSOLVES:
                    raise ImportFailure(f"Unsupported OMF transition: {name or 'unnamed'}. Render it before export.")
                for slot in o.refs(effect, "OMFI:EFFE:EffectSlots"):
                    if o.int(slot, "OMFI:ESLT:ArgID", 0) == -3:
                        level = o.ref(slot, "OMFI:ESLT:ArgValue")
                        if level is not None and o.cls(level) != "VVAL":
                            raise ImportFailure("A non-standard OMF dissolve must be rendered before export.")
                a, b = timeline + start - begin, timeline + end - begin
                for clip in groups[index - 1][2]:
                    self.fade(clip, "out", end - start, b - clip["time"] - clip["duration"])
                for clip in groups[index][2]:
                    self.fade(clip, "in", end - start, clip["time"] - a)
            return output
        if kind == "SCLP":
            uid = o.uid(segment, "OMFI:SCLP:SourceID")
            if uid is None or uid == (0, 0, 0):
                return []  # original source: nothing further to read
            mob = self.mobs.get(uid)
            if mob is None:
                raise ImportFailure("An OMF clip refers to a missing source description.")
            track = o.int(segment, "OMFI:SCLP:SourceTrackID", -1, False)
            slot = self.slot_named(mob, track)
            if slot is None:
                raise ImportFailure("An OMF clip refers to a missing source track.")
            key = (uid, track)
            if key in chain:
                raise ImportFailure("The OMF source references contain a cycle.")
            # StartTime is in this clip's own edit units.
            start = Fraction(o.int(segment, "OMFI:SCLP:StartTime", 0)) / edit_rate + begin
            source_rate = o.rational(slot, "OMFI:MSLT:EditRate")
            desc = o.ref(slot, "OMFI:MSLT:TrackDesc")
            origin = Fraction(o.int(desc, "OMFI:TRKD:Origin", 0)) / source_rate if desc is not None else Fraction(0)
            if o.cls(mob) == "SMOB" and o.ref(mob, "OMFI:SMOB:MediaDescription") is not None \
                    and o.cls(o.ref(mob, "OMFI:SMOB:MediaDescription")) in ("WAVD", "AIFD"):
                self.count += 1
                if self.count > MAX_CLIPS:
                    raise ImportFailure("The OMF contains too many clips.")
                clips = [{"source": self.media(mob, slot), "time": timeline, "offset": start + origin,
                          "duration": duration, "gain": gain, "fade_in": Fraction(0), "fade_out": Fraction(0),
                          "fade_in_offset": Fraction(0), "fade_out_offset": Fraction(0)}]
            else:
                inner = o.ref(slot, "OMFI:MSLT:Segment")
                if inner is None:
                    raise ImportFailure("An OMF source track has no content.")
                clips = self.walk(inner, source_rate, start + origin, duration, timeline, gain, depth + 1, chain + (key,))
            for direction, label in (("in", "In"), ("out", "Out")):
                fade = Fraction(o.int(segment, f"OMFI:SCLP:Fade{label}Length", 0)) / edit_rate
                kind_value = o.int(segment, f"OMFI:SCLP:Fade{label}Type", 0)
                if not fade or not kind_value:
                    continue
                if kind_value != 1 or not 0 < fade <= length:
                    raise ImportFailure("A non-linear or invalid OMF clip fade must be rendered before export.")
                for clip in clips:
                    offset = (begin + clip["time"] - timeline if direction == "in"
                              else length - begin - (clip["time"] + clip["duration"] - timeline))
                    if offset < 0:
                        raise ImportFailure("An OMF fade extends outside its clip.")
                    self.fade(clip, direction, fade, offset)
            return clips
        if kind == "SLCT":
            selected = o.ref(segment, "OMFI:SLCT:Selected")
            if selected is None:
                raise ImportFailure("An OMF selector has no chosen audio.")
            return self.walk(selected, edit_rate, begin, duration, timeline, gain, depth + 1, chain)
        if kind == "NEST":
            slots = o.refs(segment, "OMFI:NEST:Slots")
            if not slots:
                raise ImportFailure("An OMF nested scope is empty.")
            return self.walk(slots[-1], edit_rate, begin, duration, timeline, gain, depth + 1, chain)
        if kind == "EFFE":
            name = self.effect_id(segment)
            rendering = o.ref(segment, "OMFI:EFFE:FinalRendering")
            slots = o.refs(segment, "OMFI:EFFE:EffectSlots")
            inputs = [o.ref(s, "OMFI:ESLT:ArgValue") for s in slots if o.int(s, "OMFI:ESLT:ArgID", 0) == -1]
            if name in GAINS and len(inputs) == 1:
                for s in slots:
                    if o.int(s, "OMFI:ESLT:ArgID", 0) != 1:
                        continue
                    level = o.ref(s, "OMFI:ESLT:ArgValue")
                    if level is None or o.cls(level) != "CVAL":
                        raise ImportFailure("OMF gain automation must be rendered before export; constant clip gain is supported.")
                    raw = o.data(level, "OMFI:CVAL:Value", 16) or b""
                    if len(raw) != 8:
                        raise ImportFailure("An OMF clip gain is invalid.")
                    num, den = struct.unpack(o.order + "ii", raw)
                    value = num / den if den else -1
                    if not math.isfinite(value) or not 0 <= value <= 64:
                        raise ImportFailure("An OMF clip gain is invalid.")
                    gain *= value
                return self.walk(inputs[0], edit_rate, begin, duration, timeline, gain, depth + 1, chain)
            if rendering is not None:
                self.warn(f"Using rendered audio for {name or 'an effect'}.")
                return self.walk(rendering, edit_rate, begin, duration, timeline, gain, depth + 1, chain)
            if len(inputs) == 1:
                self.warn(f"Effect omitted: {name or 'unnamed'}. Render it before export if it must be preserved.")
                return self.walk(inputs[0], edit_rate, begin, duration, timeline, gain, depth + 1, chain)
            raise ImportFailure(f"Unsupported OMF effect: {name or 'unnamed'}. Render it before export.")
        raise ImportFailure(f"Unsupported OMF audio structure: {kind or 'unknown'}. Render its audio before export.")

    def read(self):
        bento = Bento(self.path)
        try:
            self.o = Omf(bento)
            o = self.o
            self.mobs, self.mobdata = {}, {}
            for mob in o.refs(o.header, "OMFI:HEAD:Mobs"):
                uid = o.uid(mob, "OMFI:MOBJ:MobID")
                if uid is not None:
                    self.mobs[uid] = mob
            for data in o.refs(o.header, "OMFI:HEAD:MediaData"):
                uid = o.uid(data, "OMFI:MDAT:MobID")
                if uid is not None:
                    self.mobdata[uid] = data
            compositions = [m for m in self.mobs.values() if o.cls(m) == "CMOB"]
            primary = [m for m in o.refs(o.header, "OMFI:HEAD:PrimaryMobs") if o.cls(m) == "CMOB"]
            referenced = set()
            for mob in compositions:
                stack = [o.ref(s, "OMFI:MSLT:Segment") for s in o.refs(mob, "OMFI:MOBJ:Slots")]
                seen = 0
                while stack and seen < 100000:
                    item = stack.pop(); seen += 1
                    if item is None:
                        continue
                    if o.cls(item) == "SCLP":
                        uid = o.uid(item, "OMFI:SCLP:SourceID")
                        if uid in self.mobs:
                            referenced.add(self.mobs[uid])
                    for name in ("OMFI:SEQU:Components", "OMFI:NEST:Slots", "OMFI:EFFE:EffectSlots"):
                        stack.extend(o.refs(item, name))
                    for name in ("OMFI:SLCT:Selected", "OMFI:ESLT:ArgValue", "OMFI:TRAN:Effect"):
                        stack.append(o.ref(item, name))
            def audible(mob):
                return any(self.kind(s) in SOUND or self.kind(o.ref(s, "OMFI:MSLT:Segment") or s) in SOUND
                           for s in o.refs(mob, "OMFI:MOBJ:Slots"))
            choices = [m for m in (primary or [m for m in compositions if m not in referenced]) if audible(m)]
            if len(choices) != 1:
                raise ImportFailure("Export one audio timeline per OMF; no unique audio composition was found.")
            composition = choices[0]
            tracks = []
            for slot in o.refs(composition, "OMFI:MOBJ:Slots"):
                segment = o.ref(slot, "OMFI:MSLT:Segment")
                if segment is None or self.kind(segment) not in SOUND:
                    if segment is not None and o.cls(segment) not in ("TCCP",):
                        self.warn("Video, timecode and other non-audio tracks are not imported.")
                    continue
                if len(tracks) >= MAX_TRACKS:
                    raise ImportFailure("The OMF contains more than 256 audio tracks.")
                edit_rate = o.rational(slot, "OMFI:MSLT:EditRate")
                if not 0 < edit_rate <= 768000:
                    raise ImportFailure("An OMF track has an invalid edit rate.")
                desc = o.ref(slot, "OMFI:MSLT:TrackDesc")
                origin = Fraction(o.int(desc, "OMFI:TRKD:Origin", 0)) / edit_rate if desc is not None else Fraction(0)
                length = Fraction(o.int(segment, "OMFI:CPNT:Length", 0)) / edit_rate
                if self.kind(segment) == "omfi:data:StereoSound":
                    raise ImportFailure("OMF stereo sound tracks are not supported. Export one track per channel.")
                raw = self.walk(segment, edit_rate, Fraction(0), length, -origin)
                clips = []
                for clip in raw:
                    start = self.frame(clip["time"]); end = self.frame(clip["time"] + clip["duration"])
                    offset = self.frame(clip["offset"])
                    if start < 0 or offset < 0 or end <= start:
                        raise ImportFailure("An OMF clip lies outside the supported timeline or source range.")
                    clips.append({"source": clip["source"], "start": start, "offset": offset, "length": end - start,
                                  "gain": clip["gain"], **{k: self.frame(clip[k]) for k in
                                  ("fade_in", "fade_out", "fade_in_offset", "fade_out_offset")}})
                name = o.string(desc, "OMFI:TRKD:TrackName") if desc is not None else ""
                tracks.append({"name": name or f"Audio {len(tracks) + 1}", "clips": clips})
            if not any(t["clips"] for t in tracks):
                raise ImportFailure("The OMF contains no usable audio clips.")
            return {"version": 1, "name": o.string(composition, "OMFI:MOBJ:Name") or self.path.stem,
                    "sample_rate": self.sample_rate, "time_reference": None, "sources": self.sources,
                    "tracks": tracks, "warnings": self.warnings}
        finally:
            bento.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("path"); parser.add_argument("scratch")
    parser.add_argument("--sample-rate", type=int, required=True); parser.add_argument("--media-folder")
    args = parser.parse_args()
    if not 8000 <= args.sample_rate <= 192000:
        raise ImportFailure("Unsupported project sample rate.")
    resource.setrlimit(resource.RLIMIT_AS, (2 * 1024**3, 2 * 1024**3))
    resource.setrlimit(resource.RLIMIT_CPU, (120, 120))
    result = Reader(args.path, args.scratch, args.sample_rate, args.media_folder).read()
    payload = json.dumps(result, ensure_ascii=False, allow_nan=False)
    if len(payload.encode("utf-8")) > 16 * 1024 * 1024:
        raise ImportFailure("The OMF import description is too large.")
    print(payload)


if __name__ == "__main__":
    try:
        main()
    except ImportFailure as exc:
        print(json.dumps({"version": 1, "error": str(exc) or "The OMF could not be read."}, ensure_ascii=False))
        sys.exit(1)
    except Exception as exc:
        # A parser fault inside the file's structure: say what that means, keep the detail.
        detail = str(exc) or type(exc).__name__
        print(json.dumps({"version": 1, "error": "This file could not be read as an OMF session. It may be damaged, incomplete "
                          "or from an unsupported application; try exporting it again. (Detail: %s)" % detail}, ensure_ascii=False))
        sys.exit(1)
