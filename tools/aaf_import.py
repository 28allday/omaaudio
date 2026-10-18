#!/usr/bin/env python3
"""Read an AAF into a bounded interchange manifest; audio decoding stays native.

Private worker protocol, version 1. stdout is JSON only. The parent owns the
scratch directory, enforces cancellation/time/output limits and copies all audio
before deleting scratch. No project or original media is modified here.
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
import sys
import wave

sys.path.insert(0, str(Path(__file__).resolve().parent / "aaf_vendor"))
import aaf2
from aaf2 import components as C, mobs as M, essence as E

MAX_FRAMES = 10**12
MAX_CLIPS = 100000
MAX_SOURCES = 10000
MAX_TRACKS = 256
GAIN_IDS = {'9d2ea894-0968-11d3-8a38-0050040ef7d2','9d2ea895-0968-11d3-8a38-0050040ef7d2'}
# Level parameters: the SMPTE amplitude multiplier and Avid's clip-gain ratio (both linear).
AMPLITUDE_IDS = {'e4962321-2267-11d3-8a4c-0050040ef7d2', '47c00f7d-fb55-11d3-9ff7-0004ac969f50'}
GAIN_FLAG_IDS = {'922d458d-8f22-11d4-a03c-0004ac969f50'}   # Avid PanVol_IsTrimGainEffect
# Avid Media Composer audio-dissolve controls: Curve Type, AFX_TRANS_LEN,
# AvidParameterByteOrder, AvidEffectID and MC AVX2 EFFECT VERSION.
AVID_CURVE_TYPE = '8134fc48-3bd6-4904-9520-7a6360cc7127'
AVID_DISSOLVE_METADATA = {AVID_CURVE_TYPE, '8ba0916a-7c0b-41e4-b2da-c817048cbf5d', 'c0038672-a8cf-11d3-a05b-006094eb75cb',
                          '93994bd6-a81d-11d3-a05b-006094eb75cb', '8af7a5dc-367b-4919-afc7-5ca00a5bd558'}
DISSOLVE_IDS = {'0c3bea41-fc05-11d2-8a29-0050040ef7d2','0c3bea42-fc05-11d2-8a29-0050040ef7d2'}
COMBINER_ID = '6b46dd7a-132d-4856-ab21-8b751d8462ec'
LINEAR_ID = '5b6c85a4-0ede-11d3-80a9-006008143e6f'


class ImportFailure(Exception):
    pass


def prop(obj, key, default=None):
    p = obj.get(key, None)
    return default if p is None or p.value is None else p.value


def fraction(value):
    try:
        result = Fraction(value)
    except (ValueError, TypeError, ZeroDivisionError, OverflowError) as exc:
        raise ImportFailure("The AAF contains an invalid time or sample rate.") from exc
    if abs(result.numerator) > 2**63 or result.denominator > 10**9:
        raise ImportFailure("An AAF time value exceeds the supported range.")
    return result


def rate(slot):
    value = fraction(prop(slot, "EditRate", 0))
    if not 0 < value <= 768000:
        raise ImportFailure("An AAF track has an invalid edit rate.")
    return value


def references_audio(segment, depth=0):
    """True when a segment (searched a few levels deep) points at a real source."""
    if segment is None or depth > 12:
        return False
    if isinstance(segment, C.SourceClip):
        try:
            return segment.mob is not None
        except Exception:
            return False
    children = []
    if isinstance(segment, C.Sequence): children = list(segment.components)
    elif isinstance(segment, C.OperationGroup): children = list(segment.segments)
    elif isinstance(segment, C.NestedScope): children = [s.segment for s in segment.slots]
    elif isinstance(segment, C.Selector): children = [prop(segment, 'Selected', None)]
    return any(references_audio(child, depth + 1) for child in children)


def picture(obj):
    return str(getattr(obj, "media_kind", "")).lower() == "picture"


def sound(obj):
    return str(getattr(obj, "media_kind", "")).lower() in ("sound", "legacysound")


class Reader:
    def __init__(self, path, scratch, sample_rate, media_folder=None):
        self.path = Path(path).resolve(strict=True)
        self.scratch = Path(scratch).resolve(strict=True)
        self.sample_rate = sample_rate
        self.roots = [self.path.parent]
        if media_folder:
            selected = Path(media_folder).resolve(strict=True)
            if not selected.is_dir():
                raise ImportFailure("Choose the folder containing the AAF's audio files.")
            self.roots.insert(0, selected)
        self.sources = []
        self.source_ids = {}
        self.extracted = {}
        self.combined = {}
        self.warnings = []
        self.count = 0
        self.visits = 0
        self.index = None

    def warn(self, message):
        if message not in self.warnings:
            if len(self.warnings) >= 200:
                raise ImportFailure("The AAF has too many unsupported elements to import reliably.")
            self.warnings.append(message)

    def clip_envelope(self, curve, length):
        """Keyframes (clip-local seconds, linear gain) as clip frames 0..length, cut
        at the clip's edges by interpolation; strictly increasing frames."""
        points = sorted((self.frame(t), float(v)) for t, v in curve)
        def at(frame):
            if frame <= points[0][0]: return points[0][1]
            if frame >= points[-1][0]: return points[-1][1]
            for (f0, v0), (f1, v1) in zip(points, points[1:]):
                if f0 <= frame <= f1:
                    return v0 if f1 == f0 else v0 + (v1 - v0) * (frame - f0) / (f1 - f0)
            return points[-1][1]
        inside = [(f, v) for f, v in points if 0 < f < length]
        result, last = [], None
        for f, v in [(0, at(0))] + inside + [(length, at(length))]:
            if v > 4:
                self.warn("AAF volume keyframes above +12 dB were limited to +12 dB.")
                v = 4.0
            if last is not None and f <= last: continue
            result.append([f, v]); last = f
        return result

    def frame(self, seconds):
        f = fraction(seconds) * self.sample_rate
        value = (2*f.numerator + f.denominator) // (2*f.denominator)
        if abs(value) > MAX_FRAMES:
            raise ImportFailure("The AAF timeline exceeds the supported length.")
        return value

    def locate(self, descriptor, mob):
        candidates = []
        names = []
        for locator in prop(descriptor, "Locator", []):
            raw = str(prop(locator, "URLString", prop(locator, "Path", "")))
            if not raw:
                continue
            decoded = unquote(raw).replace("\\", "/")
            parts = urlsplit(raw)
            if parts.scheme and parts.scheme.lower() != "file" and not re.match(r"^[A-Za-z]:[/\\]", raw):
                self.warn("Network media locations are not opened; choose a local audio folder.")
                continue
            if parts.scheme.lower() == "file":
                decoded = unquote(parts.path)
            name = Path(decoded).name
            if name:
                names.append(name)
            # Prefer the delivered media folder over stale source-machine paths.
            for root in self.roots:
                if not Path(decoded).is_absolute() and not re.match(r"^[A-Za-z]:", decoded):
                    candidates.append(root / decoded)
                candidates.append(root / name)
            if Path(decoded).is_absolute() and (not parts.netloc or parts.netloc == "localhost"):
                candidates.append(Path(decoded))
        for candidate in candidates:
            if candidate.is_file():
                return str(candidate.resolve())
        # Resolve exports are commonly moved with a nested media folder. Index
        # once, bounded, without following symlink directories or guessing by stem.
        if self.index is None:
            self.index = {}
            seen = 0
            for root in self.roots:
                for directory, dirs, files in os.walk(root, followlinks=False):
                    relative = Path(directory).relative_to(root)
                    if len(relative.parts) >= 6:
                        dirs[:] = []
                    dirs[:] = [d for d in dirs if not d.startswith('.')]
                    for filename in files:
                        seen += 1
                        if seen > 50000:
                            raise ImportFailure("The media folder is too large to search. Choose the specific AAF audio folder.")
                        self.index.setdefault(filename.casefold(), set()).add(str((Path(directory)/filename).resolve()))
        found = set()
        for name in names:
            found.update(self.index.get(name.casefold(), set()))
        if len(found) == 1:
            return found.pop()
        label = names[0] if names else mob.name or "unnamed source"
        if len(found) > 1:
            raise ImportFailure(f"More than one audio file matches {label}. Choose its exact media folder.")
        raise ImportFailure(f"Missing audio: {label}. Keep the exported audio beside the AAF or choose its media folder.")

    def media(self, mob, slot):
        descriptor = mob.descriptor
        if isinstance(descriptor, E.MultipleDescriptor):
            descriptors = list(prop(descriptor, "FileDescriptors", []))
            matches = [d for d in descriptors if prop(d, "LinkedSlotID") == slot.slot_id]
            if not matches and not any(prop(d, "LinkedSlotID") is not None for d in descriptors):
                # Unlinked (Avid camera media: one picture and one multichannel
                # sound descriptor): the only sound descriptor serves every sound
                # track, which picks its channel by physical track number.
                matches = [d for d in descriptors if isinstance(d, (E.SoundDescriptor, E.WAVEDescriptor, E.AIFCDescriptor))]
            if len(matches) != 1:
                raise ImportFailure("An AAF multichannel source has an ambiguous descriptor. Export one track per channel from the editing application.")
            descriptor = matches[0]
        if not isinstance(descriptor, (E.SoundDescriptor, E.WAVEDescriptor, E.AIFCDescriptor)):
            raise ImportFailure(f"Unsupported AAF audio container: {descriptor.__class__.__name__}. Export WAV audio from the editing application.")
        channels = int(prop(descriptor, "Channels", 0))
        physical = prop(slot, "PhysicalTrackNumber", None)
        channel = int(physical)-1 if physical is not None and channels != 1 else -1
        if channel < -1 or (channels and channel >= channels):
            raise ImportFailure("An AAF source refers to an invalid audio channel.")
        key = (str(mob.mob_id), channel)
        if key in self.source_ids:
            return self.source_ids[key]
        if len(self.sources) >= MAX_SOURCES:
            raise ImportFailure("The AAF contains too many audio sources.")
        essence = mob.essence
        if essence is not None:
            mob_key = str(mob.mob_id)
            if mob_key not in self.extracted:
                output = self.scratch / f"essence-{len(self.extracted)}.wav"
                stream = essence.open('r')
                if isinstance(descriptor, E.PCMDescriptor):
                    sample_rate = fraction(prop(descriptor, "AudioSamplingRate", prop(descriptor, "SampleRate", 0)))
                    bits = int(prop(descriptor, "QuantizationBits", 0))
                    align = int(prop(descriptor, "BlockAlign", 0))
                    if not 1 <= channels <= 32 or sample_rate.denominator != 1 or not 8000 <= sample_rate <= 768000 or bits not in (8,16,24,32) or align != channels*(bits//8):
                        raise ImportFailure("The embedded AAF PCM layout is unsupported. Export separate WAV media.")
                    # Streams may be padded to the container's block size; the
                    # descriptor's Length (frames) says how much is audio.
                    declared = int(prop(descriptor, "Length", 0) or 0)
                    remaining = declared * align if declared > 0 else None
                    with wave.open(str(output), 'wb') as wav:
                        wav.setparams((channels,bits//8,int(sample_rate),0,'NONE','not compressed'))
                        while remaining is None or remaining > 0:
                            want = align * 16384 if remaining is None else min(align * 16384, remaining)
                            block = stream.read(want)
                            if not block: break
                            if remaining is not None: remaining -= len(block)
                            elif len(block) % align:
                                block = block[:len(block) - len(block) % align]   # trailing padding
                            wav.writeframesraw(block)
                        if remaining:
                            raise ImportFailure("The embedded AAF audio is truncated.")
                else:
                    with output.open('xb') as target:
                        while True:
                            block = stream.read(1024*1024)
                            if not block: break
                            target.write(block)
                self.extracted[mob_key] = str(output)
            path = self.extracted[mob_key]
            original = str(self.path)
        else:
            path = self.locate(descriptor,mob)
            original = path
        entry = {"path":path,"original":original,"name":mob.name or Path(path).name,"channel":channel}
        index = len(self.sources)
        self.sources.append(entry)
        self.source_ids[key] = index
        return index

    def fade(self, clip, direction, length, offset):
        if length <= 0 or offset >= length:
            return
        key = 'fade_' + direction
        if clip[key]:
            raise ImportFailure("Overlapping AAF fade envelopes must be rendered in the editing application before export.")
        clip[key] = length
        clip[key+'_offset'] = offset

    def source_fades(self, segment, clips, edit_rate, begin, duration, timeline):
        length = fraction(segment.length)/edit_rate
        for direction, label in (('in','In'),('out','Out')):
            fade = fraction(prop(segment,'Fade'+label+'Length',0))/edit_rate
            if not fade:
                continue
            kind = prop(segment,'Fade'+label+'Type',None)
            if kind == 'FadeNone':
                continue
            if kind != 'FadeLinearAmp' or not 0 < fade <= length:
                raise ImportFailure("A non-linear or invalid AAF source fade must be rendered in the editing application before export.")
            for clip in clips:
                offset = (begin+clip['time']-timeline if direction=='in' else
                          length-begin-(clip['time']+clip['duration']-timeline))
                if offset < 0:
                    raise ImportFailure("An AAF fade extends outside its source clip.")
                self.fade(clip,direction,fade,offset)
        return clips

    def walk(self, segment, edit_rate, begin, duration, timeline, gain=1.0, depth=0, chain=()):
        """Flatten a selected interval, with all positions kept as exact seconds."""
        self.visits += 1
        if depth > 64 or self.visits > MAX_CLIPS*10:
            raise ImportFailure("The AAF contains a cyclic or excessively complex timeline.")
        if duration <= 0:
            return []
        if begin < 0:
            raise ImportFailure("The AAF requests audio before the beginning of a source.")
        if isinstance(segment,C.Filler):
            return []
        if isinstance(segment,C.Sequence):
            if begin+duration > fraction(segment.length)/edit_rate:
                raise ImportFailure("An AAF source reference lies outside its sequence.")
            output = []
            groups = []
            transitions = []
            cursor = Fraction(0)
            for component in segment.components:
                length = fraction(component.length)/edit_rate
                if length < 0:
                    raise ImportFailure("An AAF segment has a negative length.")
                if isinstance(component,C.Transition):
                    cursor -= length
                    transitions.append((cursor,cursor+length,component,len(groups)))
                    continue
                left,right = max(cursor,begin),min(cursor+length,begin+duration)
                clips = self.walk(component,edit_rate,left-cursor,right-left,timeline+left-begin,gain,depth+1,chain) if right > left else []
                groups.append((cursor,cursor+length,clips))
                output.extend(clips)
                cursor += length
            for start,end,transition,index in transitions:
                if index==0 or index>=len(groups) or groups[index-1][1]!=end or groups[index][0]!=start:
                    raise ImportFailure("An AAF transition has no matching adjacent clips.")
                if end <= begin or start >= begin+duration: continue
                operation = prop(transition,"OperationGroup",None)
                name = str(operation.operation.name if operation else "")
                # Where it is, from the timeline's start, so it can be found in the editor.
                seconds = int(max(Fraction(0), timeline+start-begin))
                at = f"{seconds//3600:02d}:{seconds//60%60:02d}:{seconds%60:02d}"
                if operation is None or str(operation.operation.auid) not in DISSOLVE_IDS:
                    raise ImportFailure(f"Unsupported AAF transition: {name or 'unnamed'}, {at} into the timeline. Render the transition in the editing application before export.")
                for parameter in operation.parameters:
                    parameter_id = str(parameter.parameterdef.auid)
                    if parameter_id in AVID_DISSOLVE_METADATA:
                        # Avid Media Composer's dissolve carries its own descriptive
                        # controls instead of a Level curve.
                        if parameter_id == AVID_CURVE_TYPE:
                            self.warn("Avid audio dissolves were imported as linear crossfades; listen to each join.")
                        continue
                    points = list(prop(parameter,"PointList",[]))
                    interpolation = prop(parameter,'Interpolation',None)
                    def near(value, expected):
                        try: return abs(float(value) - expected) < 1e-6
                        except (TypeError, ValueError, OverflowError): return False
                    # A straight ramp from silence to full: Level 0 to 1 as the AAF
                    # standard has it, or 0 to 100 (per cent) as Resolve writes it.
                    full = 100 if len(points)==2 and near(prop(points[1],'Value',-1), 100) else 1
                    if (parameter_id != 'e4962320-2267-11d3-8a4c-0050040ef7d2'
                        or len(points)!=2 or interpolation is None or str(interpolation.auid)!=LINEAR_ID
                        or not all(near(prop(pt,'Time',-1), t) and near(prop(pt,'Value',-1), v)
                                   for pt, (t, v) in zip(points, ((0, 0), (1, full))))):
                        raise ImportFailure(f"A non-standard AAF transition ({name or 'unnamed'}, {at} into the timeline) must be rendered in the editing application before export.")
                a,b = timeline+start-begin,timeline+end-begin
                for clip in groups[index-1][2]:
                    self.fade(clip,'out',end-start,b-clip['time']-clip['duration'])
                for clip in groups[index][2]:
                    self.fade(clip,'in',end-start,clip['time']-a)
            return output
        if isinstance(segment,C.SourceClip):
            # Resolve can round a reference length at each edit rate. Validate
            # the final sample interval against decoded media in the native reader.
            mob = segment.mob
            if mob is None:
                # An all-zero SourceID is AAF's "no further source": silence, like a filler.
                source_id = prop(segment,'SourceID',None)
                if source_id is None or set(str(source_id).replace('urn:smpte:umid:','').replace('.','')) <= {'0'}:
                    return []
                raise ImportFailure("An audio clip refers to missing AAF source metadata.")
            slot = segment.slot
            if slot is None:
                raise ImportFailure("An audio clip refers to a missing AAF source slot.")
            key = (str(mob.mob_id),int(slot.slot_id))
            if key in chain:
                raise ImportFailure("The AAF source references contain a cycle.")
            source_rate = rate(slot)
            start = fraction(segment.start)/source_rate + begin
            origin = fraction(prop(slot,'Origin',0))/source_rate
            if isinstance(mob,M.SourceMob) and mob.descriptor is not None and (mob.essence is not None or isinstance(mob.descriptor,(E.SoundDescriptor,E.WAVEDescriptor,E.AIFCDescriptor,E.MultipleDescriptor))):
                self.count += 1
                if self.count > MAX_CLIPS: raise ImportFailure("The AAF contains too many clips.")
                clip = {'source':self.media(mob,slot),'time':timeline,'offset':start+origin,'duration':duration,'gain':gain,
                        'fade_in':Fraction(0),'fade_out':Fraction(0),
                        'fade_in_offset':Fraction(0),'fade_out_offset':Fraction(0)}
                clips = [clip]
            else:
                clips = self.walk(slot.segment,source_rate,start+origin,duration,timeline,gain,depth+1,chain+(key,))
            return self.source_fades(segment,clips,edit_rate,begin,duration,timeline)
        if isinstance(segment,C.Selector):
            selected = prop(segment,'Selected',None)
            if selected is None: raise ImportFailure("An AAF selector has no chosen audio.")
            return self.walk(selected,edit_rate,begin,duration,timeline,gain,depth+1,chain)
        if isinstance(segment,C.OperationGroup):
            name = str(segment.operation.name)
            if bool(prop(segment.operation,'IsTimeWarp',False)):
                raise ImportFailure(f"AAF time effect {name} must be rendered in the editing application before export.")
            rendering = prop(segment,'Rendering',None)
            if rendering is not None:
                self.warn(f"Using rendered audio for {name}.")
                return self.walk(rendering,edit_rate,begin,duration,timeline,gain,depth+1,chain)
            inputs = list(segment.segments)
            if str(segment.operation.auid) == COMBINER_ID:
                if not 1 <= len(inputs) <= 32:
                    raise ImportFailure("The AAF channel combiner has an invalid channel count.")
                channels = [self.walk(part,edit_rate,begin,duration,timeline,gain,depth+1,chain) for part in inputs]
                if any(len(c)!=1 for c in channels):
                    raise ImportFailure("An AAF channel combiner spans multiple edits. Export one track per channel from the editing application.")
                first = channels[0][0]
                fields = ('time','offset','duration','gain','fade_in','fade_out','fade_in_offset','fade_out_offset')
                if any(any(c[0][field]!=first[field] for field in fields) for c in channels[1:]):
                    raise ImportFailure("The AAF's combined channels have different timing or gain. Export one track per channel from the editing application.")
                key = tuple(c[0]['source'] for c in channels)
                if key not in self.combined:
                    if len(self.sources)>=MAX_SOURCES: raise ImportFailure("The AAF contains too many audio sources.")
                    self.combined[key] = len(self.sources)
                    self.sources.append({'channels':list(key),'name':self.sources[key[0]]['name']})
                return [{**first,'source':self.combined[key]}]
            if len(inputs)!=1:
                raise ImportFailure(f"AAF effect {name} has an unsupported channel layout. Export one track per channel from the editing application.")
            envelope = None
            if str(segment.operation.auid) in GAIN_IDS:
                for parameter in segment.parameters:
                    parameter_id = str(parameter.parameterdef.auid)
                    if parameter_id in GAIN_FLAG_IDS:
                        continue   # Avid's "is trim gain" marker: not a level
                    if parameter_id not in AMPLITUDE_IDS:
                        raise ImportFailure("An AAF gain parameter is unsupported. Render the effect in the editing application before export.")
                    if parameter.__class__.__name__ == 'VaryingValue':
                        points = list(prop(parameter,'PointList',[]))
                        try:
                            curve = [(float(prop(p,'Time',0)), float(prop(p,'Value',0))) for p in points]
                        except (TypeError, ValueError, OverflowError) as exc:
                            raise ImportFailure("An AAF gain keyframe is invalid.") from exc
                        if not curve or any(not (math.isfinite(t) and math.isfinite(v)) or v < 0 for t, v in curve):
                            raise ImportFailure("An AAF gain keyframe is invalid.")
                        if all(abs(v - curve[0][1]) < 1e-9 for _, v in curve):
                            value = curve[0][1]
                        else:
                            # Clip volume keyframes (Avid's clip gain automation) become
                            # the clip's volume envelope, times relative to this effect.
                            if envelope is not None:
                                raise ImportFailure("An AAF clip has more than one gain automation curve. Render its audio before export.")
                            interpolation = prop(parameter,'Interpolation',None)
                            if interpolation is not None and str(interpolation.auid) != LINEAR_ID:
                                self.warn("Stepped or curved AAF volume keyframes were imported as straight lines between points.")
                            envelope = sorted(curve)
                            value = 1.0
                    elif parameter.__class__.__name__ == 'ConstantValue':
                        value = float(prop(parameter,'Value',1))
                    else:
                        raise ImportFailure("An AAF gain parameter is unsupported.")
                    if not math.isfinite(value) or value < 0 or value > 64:
                        raise ImportFailure("An AAF clip gain is invalid.")
                    gain *= value
            else:
                self.warn(f"Effect omitted: {name}. Render this effect in the editing application if it must be preserved.")
            clips = self.walk(inputs[0],edit_rate,begin,duration,timeline,gain,depth+1,chain)
            if envelope:
                # VaryingValue times run 0..1 across the whole effect; place them on the timeline.
                length = fraction(segment.length)/edit_rate
                origin = timeline - begin
                for clip in clips:
                    if clip.get('envelope'):
                        raise ImportFailure("An AAF clip has nested gain automation. Render its audio before export.")
                    clip['envelope'] = [(origin + Fraction(t).limit_denominator(10**6)*length - clip['time'], v) for t, v in envelope]
            return clips
        if isinstance(segment,C.NestedScope) and len(segment.slots)==1:
            return self.walk(segment.slots[0],edit_rate,begin,duration,timeline,gain,depth+1,chain)
        raise ImportFailure(f"Unsupported AAF audio structure: {segment.__class__.__name__}. Render its audio in the editing application before export.")

    def picture_clips(self, segment, edit_rate, begin, duration, timeline, depth=0, chain=()):
        """The picture track as shots in timeline order, positions in exact seconds:
        [{'time','offset','duration','path','name'}], 'path' None for a shot that
        shows black (its file is missing or stored inside the AAF). A dissolve
        becomes a cut at its cut point and a video effect shows its source shot;
        both are noted."""
        self.visits += 1
        if depth > 64 or self.visits > MAX_CLIPS*10:
            raise ImportFailure("The AAF contains a cyclic or excessively complex timeline.")
        if duration <= 0 or isinstance(segment, C.Filler):
            return []
        if isinstance(segment, C.Sequence):
            groups, cursor = [], Fraction(0)
            cuts = []
            for component in segment.components:
                length = fraction(component.length)/edit_rate
                if isinstance(component, C.Transition):
                    # It overlaps the shots either side; the picture cuts at its cut point.
                    cuts.append(cursor - length + fraction(prop(component, 'CutPoint', 0))/edit_rate)
                    cursor -= length
                    self.warn("Video dissolves are shown as cuts in the reference video.")
                    continue
                groups.append((cursor, cursor+length, component))
                cursor += length
            output = []
            for index, (start, end, component) in enumerate(groups):
                # Shots that meet at a dissolve end and begin at its cut.
                for cut in cuts:
                    if index > 0 and groups[index-1][1] > cut >= start: start = max(start, cut)
                    if index+1 < len(groups) and groups[index+1][0] < cut <= end: end = min(end, cut)
                left, right = max(start, begin), min(end, begin+duration)
                if right > left:
                    output.extend(self.picture_clips(component, edit_rate, left-groups[index][0], right-left,
                                                     timeline+left-begin, depth+1, chain))
            return output
        if isinstance(segment, C.SourceClip):
            mob = segment.mob
            if mob is None:
                return []
            slot = segment.slot
            if slot is None:
                return []
            key = (str(mob.mob_id), int(slot.slot_id))
            if key in chain:
                raise ImportFailure("The AAF source references contain a cycle.")
            source_rate = rate(slot)
            start = fraction(segment.start)/source_rate + begin
            origin = fraction(prop(slot, 'Origin', 0))/source_rate
            if isinstance(mob, M.SourceMob) and mob.descriptor is not None:
                path = None
                if mob.essence is not None:
                    self.warn(f"The picture {mob.name or 'unnamed'} is stored inside the AAF and shows as black.")
                else:
                    descriptors = [mob.descriptor]
                    if isinstance(mob.descriptor, E.MultipleDescriptor):
                        descriptors += list(prop(mob.descriptor, "FileDescriptors", []))
                    for descriptor in descriptors:
                        if not list(prop(descriptor, "Locator", [])):
                            continue
                        try:
                            path = self.locate(descriptor, mob)
                            break
                        except ImportFailure:
                            continue
                    if path is None:
                        self.warn(f"The video file {mob.name or 'unnamed'} was not found beside the AAF; it shows as black.")
                return [{'time': timeline, 'offset': start+origin, 'duration': duration, 'path': path,
                         'name': mob.name or (Path(path).name if path else 'unnamed')}]
            return self.picture_clips(slot.segment, source_rate, start+origin, duration, timeline, depth+1, chain+(key,))
        if isinstance(segment, C.Selector):
            selected = prop(segment, 'Selected', None)
            return [] if selected is None else self.picture_clips(selected, edit_rate, begin, duration, timeline, depth+1, chain)
        if isinstance(segment, C.NestedScope) and len(segment.slots) == 1:
            return self.picture_clips(segment.slots[0], edit_rate, begin, duration, timeline, depth+1, chain)
        if isinstance(segment, C.OperationGroup):
            inputs = list(segment.segments)
            self.warn(f"Video effect {segment.operation.name or 'unnamed'} is not shown; the reference video shows its source.")
            if bool(prop(segment.operation, 'IsTimeWarp', False)) or not inputs:
                return [{'time': timeline, 'offset': Fraction(0), 'duration': duration, 'path': None, 'name': str(segment.operation.name)}]
            return self.picture_clips(inputs[0], edit_rate, begin, duration, timeline, depth+1, chain)
        return [{'time': timeline, 'offset': Fraction(0), 'duration': duration, 'path': None, 'name': segment.__class__.__name__}]

    def reference_video(self, composition):
        """The picture to play beside the session. One clip of one file is that
        file, placed so its own zero sits where the timeline puts it. An edited
        picture (cuts, several files) is a list of shots the application builds
        into one video. None when there is no picture."""
        found = []
        for slot in composition.slots:
            if not picture(slot):
                continue
            edit_rate = rate(slot)
            origin = fraction(prop(slot, 'Origin', 0))/edit_rate
            length = fraction(slot.segment.length)/edit_rate
            clips = self.picture_clips(slot.segment, edit_rate, Fraction(0), length, -origin)
            if clips:
                found.append((edit_rate, clips))
        if not found:
            return None
        if len(found) > 1:
            self.warn("The timeline has more than one video track; the reference video shows the first.")
        edit_rate, clips = found[0]
        clips = [c for c in clips if c['time'] + c['duration'] > 0]
        if not clips:
            return None
        zero = clips[0]['time'] - clips[0]['offset']
        if clips[0]['path'] and all(c['path'] == clips[0]['path'] and c['time'] - c['offset'] == zero for c in clips[1:]):
            # Pieces of one file that keep its timing play as that file.
            return {'path': clips[0]['path'], 'name': clips[0]['name'], 'offset': self.frame(zero)}
        if len(clips) > MAX_CLIPS:
            raise ImportFailure("The AAF's picture track has too many shots.")
        return {'name': str(composition.name or 'Timeline'), 'rate': str(edit_rate),
                'shots': [{'time': float(c['time']), 'offset': float(c['offset']), 'duration': float(c['duration']),
                           'path': c['path'], 'name': c['name']} for c in clips]}

    def read(self):
        with aaf2.open(str(self.path),'r') as f:
            toplevel = list(f.content.toplevel())
            everything = list(f.content.compositionmobs())
            choices = [m for m in (toplevel or everything) if any(sound(s) for s in m.slots)]
            if not choices and not toplevel:
                choices = [m for m in everything if any(sound(s) for s in m.slots)]
            clip_only = False
            if not choices and not (toplevel or everything):
                # An exported clip rather than a sequence: its master clip becomes a
                # track starting at the session start.
                masters = list(f.content.mastermobs())
                choices = [m for m in masters if any(sound(s) for s in m.slots)]
                clip_only = bool(choices)
                if not choices and masters:
                    raise ImportFailure("This AAF has no audio tracks, only video, markers or other data. "
                                        "Include the audio tracks when exporting it.")
            if not choices:
                if toplevel or everything:
                    raise ImportFailure("This AAF has no audio tracks, only video, markers or other data. "
                                        "Include the audio tracks when exporting it.")
                raise ImportFailure("This AAF contains no timeline. Export the sequence itself, not only its media.")
            if clip_only:
                self.warn("This AAF holds a clip rather than a sequence; it was placed at the start of new tracks.")
            # Prefer timelines whose audio tracks actually reference audio (a nested or
            # placeholder sequence can carry empty sound tracks).
            choices.sort(key=lambda m: not any(references_audio(slot.segment) for slot in m.slots if sound(slot)))
            composition = choices[0]
            if len(choices) > 1:
                # Several sequences (Avid can export a bin of them): take the first, say which.
                others = ", ".join(repr(str(m.name or "Untitled")) for m in choices[1:4]) + (" …" if len(choices) > 4 else "")
                self.warn(f"This AAF holds {len(choices)} timelines; imported {str(composition.name or 'the first')!r}. "
                          f"Export {others} separately to import them.")
            tracks = []
            # Editors keep a primary timecode track and often auxiliary ones (Avid's TC2–TC4,
            # pulldown and edge code); the primary one (physical track 1, else the first) places the timeline.
            timecodes = [slot for slot in composition.slots if isinstance(slot.segment,C.Timecode)]
            primary = next((slot for slot in timecodes if prop(slot,'PhysicalTrackNumber',None)==1), timecodes[0] if timecodes else None)
            time_reference = self.frame(fraction(primary.segment.start)/rate(primary)) if primary is not None else None
            for slot in composition.slots:
                if isinstance(slot.segment,C.Timecode):
                    continue
                if not sound(slot):
                    if not isinstance(slot.segment,C.Timecode) and not picture(slot):
                        self.warn("Markers and other non-audio tracks are not imported.")
                    continue
                if len(tracks)>=MAX_TRACKS: raise ImportFailure("The AAF contains more than 256 audio tracks.")
                edit_rate = rate(slot)
                # The slot origin locates sample zero within the sequence.
                origin = fraction(prop(slot,'Origin',0))/edit_rate
                length = fraction(slot.segment.length)/edit_rate
                raw = self.walk(slot.segment,edit_rate,Fraction(0),length,-origin)
                clips=[]
                for clip in raw:
                    start=self.frame(clip['time']);end=self.frame(clip['time']+clip['duration'])
                    offset=self.frame(clip['offset'])
                    if start < 0 or offset < 0 or end <= start:
                        raise ImportFailure("An AAF clip lies outside the supported timeline or source range.")
                    entry={'source':clip['source'],'start':start,'offset':offset,'length':end-start,'gain':clip['gain'],
                           **{k:self.frame(clip[k]) for k in ('fade_in','fade_out','fade_in_offset','fade_out_offset')}}
                    if clip.get('envelope'):
                        entry['envelope']=self.clip_envelope(clip['envelope'], end-start)
                    clips.append(entry)
                tracks.append({'name':slot.name or f'Audio {len(tracks)+1}','clips':clips})
            if not any(t['clips'] for t in tracks): raise ImportFailure("The AAF's audio tracks are empty: they hold no audio clips.")
            video = self.reference_video(composition)
            return {'version':1,'name':composition.name or self.path.stem,'sample_rate':self.sample_rate,'time_reference':time_reference,
                    'sources':self.sources,'tracks':tracks,'warnings':self.warnings,'video':video}


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('path');parser.add_argument('scratch');parser.add_argument('--sample-rate',type=int,required=True);parser.add_argument('--media-folder')
    args=parser.parse_args()
    if not 8000<=args.sample_rate<=192000: raise ImportFailure("Unsupported project sample rate.")
    # Parsing runs outside the application with an explicit memory/CPU budget.
    resource.setrlimit(resource.RLIMIT_AS,(2*1024**3,2*1024**3))
    resource.setrlimit(resource.RLIMIT_CPU,(120,120))
    result=Reader(args.path,args.scratch,args.sample_rate,args.media_folder).read()
    payload=json.dumps(result,ensure_ascii=False,allow_nan=False)
    if len(payload.encode('utf-8'))>16*1024*1024:raise ImportFailure("The AAF import description is too large.")
    print(payload)


if __name__=='__main__':
    try:
        main()
    except ImportFailure as exc:
        print(json.dumps({'version':1,'error':str(exc) or 'The AAF could not be read.'},ensure_ascii=False))
        sys.exit(1)
    except Exception as exc:
        # A parser fault inside the file's structure: say what that means, keep the detail.
        detail=str(exc) or type(exc).__name__
        print(json.dumps({'version':1,'error':'This file could not be read as an AAF session. It may be damaged, incomplete '
                          'or from an unsupported application; try exporting it again. (Detail: %s)'%detail},ensure_ascii=False))
        sys.exit(1)
