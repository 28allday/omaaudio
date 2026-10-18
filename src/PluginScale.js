.pragma library

// Numbers for a plugin control: where a value sits on its slider or knob, the
// numbered marks over it, and how the value reads. A port is the descriptor
// Backend.pluginPorts gives: {min, max, defaultValue, logarithmic, integer, unit}.

function words(port) { return (port.name || "") + " " + (port.symbol || "") }
function plainNumber(port) {
    return !port.toggled && !port.integer && !(port.choices || []).length && (port.unit || "") === ""
}

// A level set as a plain factor (1 = unchanged): Calf's "Input Gain" runs
// 0.0156 to 64 with no unit, and some plugins mark such a factor with the unit
// "G" (a gain, 0 to 10 G). It reads in decibels on a decibel scale, so 1 sits as
// 0 dB instead of hard against the left end; a factor that can reach 0 starts
// at −∞ and runs from −60 dB.
function isGainUnit(port) { return (port.unit || "") === "G" }
function isGainFactor(port) {
    if (port.output || port.toggled || port.integer || (port.choices || []).length) return false
    if (isGainUnit(port)) return port.min >= 0 && port.max > 0 && port.max > port.min
    if (!plainNumber(port)) return false
    // A span of 24 dB or more (Calf's Makeup is 1 to 64, 0 to +36 dB).
    if (!(port.min > 0) || !(port.max <= 100) || !(port.max / port.min >= 16)) return false
    // "Reduction" and "Range" are a factor only up to 1 (a gate's floor);
    // "Bit Reduction" from 1 to 16 is a count of bits.
    const w = words(port)
    const factorUnderOne = /reduction|\brange\b/i.test(w) && !/\bbits?\b/i.test(w) && port.max <= 1.0001
    if (!factorUnderOne && !/gain|level|volume|\bvol\b|makeup|trim|threshold|ceiling|amount|\bdry\b|\bwet\b|\bmix\b/i.test(w)) return false
    const d = port.defaultValue
    return d === undefined || (d >= port.min && d <= port.max)
}
// The decibel span a gain factor's control covers.
const gainFloorDb = -60
function gainLowDb(port) { return port.min > 0 ? toDb(port.min) : Math.min(gainFloorDb, toDb(port.max) - 24) }

// A log scale where the plugin asks for one, for a gain factor, and for any
// plain number spanning three decades or more (a frequency or a time given
// without its unit): linear, those leave nearly the whole range in the first
// few pixels.
function isLog(port) {
    if (isGainFactor(port)) return true
    if (!(port.min > 0 && port.max > port.min)) return false
    if (port.logarithmic === true || isGainFactor(port)) return true
    if (!plainNumber(port)) return false
    // A frequency named as one ("High Frq Damp", "Cutoff") is heard by octaves.
    if (/fr(e)?q|cutoff|\bhz\b|damp|treble|bass cut/i.test(words(port)) && port.max / port.min >= 4) return true
    return port.max / port.min >= 1000
}
function toDb(v) { return v > 0 ? 20 * Math.log(v) / Math.LN10 : -Infinity }
function fromDb(db) { return Math.pow(10, db / 20) }
function dbText(db, signed) {
    if (!isFinite(db)) return "−∞"
    const r = Math.abs(db) < 0.05 ? 0 : db
    return minus((signed && r > 0 ? "+" : "") + r.toFixed(1))
}

// 0..1 along the control for a value, and back.
function fraction(port, value) {
    const span = port.max - port.min
    if (!(span > 0)) return 0
    const v = Math.max(port.min, Math.min(port.max, value))
    if (isGainFactor(port)) {
        if (!(v > 0)) return 0
        const lo = gainLowDb(port), hi = toDb(port.max)
        return Math.max(0, Math.min(1, (toDb(v) - lo) / (hi - lo)))
    }
    return isLog(port) ? Math.log(v / port.min) / Math.log(port.max / port.min) : (v - port.min) / span
}
function valueAt(port, f) {
    f = Math.max(0, Math.min(1, f))
    if (isGainFactor(port)) {
        if (f <= 0) return port.min
        const lo = gainLowDb(port), hi = toDb(port.max)
        return Math.max(port.min, Math.min(port.max, fromDb(lo + f * (hi - lo))))
    }
    return isLog(port) ? port.min * Math.exp(f * Math.log(port.max / port.min)) : port.min + f * (port.max - port.min)
}

// Where the fill starts: from zero for a control that runs either side of it
// (a gain of −18 to +18 dB), otherwise from the left end.
function origin(port) {
    if (isGainFactor(port)) return port.min < 1 && port.max > 1 ? fraction(port, 1) : 0
    return port.min < 0 && port.max > 0 && !isLog(port) ? fraction(port, 0) : 0
}

function minus(text) { return String(text).replace(/^-/, "−") }

function trim(n) {
    // 2.50 → 2.5, 3.00 → 3
    return String(n).indexOf(".") >= 0 ? String(n).replace(/0+$/, "").replace(/\.$/, "") : String(n)
}

// A tick label: short, the unit left out (it is on the value).
function tickText(port, v) {
    const unit = (port.unit || "").toLowerCase()
    let text
    if ((unit === "hz" || (unit === "" && isLog(port))) && Math.abs(v) >= 1000) text = trim((v / 1000).toFixed(1)) + "k"
    else if (Math.abs(v) >= 10000) text = trim((v / 1000).toFixed(0)) + "k"
    else text = trim(Number(v.toPrecision(3)))
    if (port.min < 0 && v > 0) text = "+" + text
    return minus(text)
}

function niceStep(span, count) {
    const raw = span / Math.max(1, count)
    const power = Math.pow(10, Math.floor(Math.log(raw) / Math.LN10))
    const unit = raw / power
    return (unit <= 1 ? 1 : unit <= 2 ? 2 : unit <= 2.5 ? 2.5 : unit <= 5 ? 5 : 10) * power
}

// The marks over a control, at most `count` of them: [{p (0..1), t (label)}].
// A log scale gets 1-2-5 steps (or whole decades when it spans many); a linear
// one even steps that include zero when the range crosses it.
function ticks(port, count) {
    count = Math.max(2, Math.floor(count))
    const out = []
    const span = port.max - port.min
    if (!(span > 0)) return out
    if (isGainFactor(port)) {
        // Marked in decibels, at even steps that include 0 dB.
        const lo = gainLowDb(port), hi = toDb(port.max)
        const step = niceStep(hi - lo, count - 1)
        const label = function (db) { return dbText(Math.round(db), true).replace(/\.0$/, "") }
        for (let db = Math.ceil(lo / step - 1e-9) * step; db <= hi + 1e-9; db += step)
            out.push({ p: fraction(port, fromDb(db)), t: label(db) })
        // A factor that reaches 0 starts at silence.
        if (!(port.min > 0)) { if (out.length && out[0].p < 0.12) out.shift(); out.unshift({ p: 0, t: "−∞" }) }
        else if (!out.length || out[0].p > 0.12) out.unshift({ p: 0, t: label(lo) })
        if (out[out.length - 1].p < 0.88) out.push({ p: 1, t: label(hi) })
    } else if (isLog(port)) {
        const decades = Math.log(port.max / port.min) / Math.LN10
        let mantissas = decades > count / 2 ? [1] : decades > count / 3 ? [1, 3] : [1, 2, 5]
        let values = []
        const fill = function () {
            values = []
            for (let e = Math.floor(Math.log(port.min) / Math.LN10) - 1; e <= Math.ceil(Math.log(port.max) / Math.LN10); ++e)
                for (const m of mantissas) {
                    const v = Number((m * Math.pow(10, e)).toPrecision(6))
                    if (v >= port.min * 0.999 && v <= port.max * 1.001) values.push(v)
                }
        }
        fill()
        if (values.length > count && mantissas.length > 1) { mantissas = [1]; fill() }
        // The ends are marked too when no mark is near them.
        if (!values.length || fraction(port, values[0]) > 0.06) values.unshift(port.min)
        if (fraction(port, values[values.length - 1]) < 0.94) values.push(port.max)
        for (const v of values) out.push({ p: fraction(port, v), t: tickText(port, v) })
    } else {
        const step = port.integer && span <= count ? 1 : niceStep(span, count - 1)
        const first = Math.ceil(port.min / step - 1e-9) * step
        const values = []
        for (let v = first; v <= port.max + step * 1e-6 && values.length < count + 2; v += step)
            values.push(Number(v.toPrecision(10)))
        if (!values.length || fraction(port, values[0]) > 0.06) values.unshift(port.min)
        if (fraction(port, values[values.length - 1]) < 0.94) values.push(port.max)
        for (const v of values) out.push({ p: fraction(port, v), t: tickText(port, v) })
    }
    // Labels that would touch: keep every other one, but always both ends.
    while (out.length > count) {
        const kept = out.filter(function (k, i) { return i === 0 || i === out.length - 1 || i % 2 === 0 })
        if (kept.length === out.length) break
        out.length = 0
        for (const k of kept) out.push(k)
    }
    return out
}

// How a value reads: {text, unit}. kHz past a thousand hertz; a sign where
// the range runs either side of zero; fewer decimals the bigger it is.
function valueText(port, v) {
    if (isGainFactor(port)) return { text: dbText(toDb(v), true), unit: "dB" }
    let unit = port.unit || ""
    let n = v
    if (unit.toLowerCase() === "hz" && Math.abs(v) >= 1000) { n = v / 1000; unit = "kHz" }
    // A compressor's ratio, given as a plain number: "4.0 : 1".
    if (unit === "" && !port.integer && port.min >= 1 && /\bratio\b/i.test(words(port).replace(/_/g, " ")))
        return { text: minus(n.toFixed(n >= 100 ? 0 : 1)), unit: ": 1" }
    let text
    if (port.integer) text = String(Math.round(n))
    else {
        const a = Math.abs(n)
        // Decibels to a tenth, as everywhere else in the app.
        text = unit.toLowerCase() === "db" ? n.toFixed(a >= 100 ? 0 : 1) : n.toFixed(a >= 100 ? 0 : a >= 10 ? 1 : 2)
    }
    if (port.min < 0 && v > 0) text = "+" + text
    return { text: minus(text), unit: unit }
}

// What a person typed, in the control's own unit ("1.2k" and "−3" work).
function parse(port, text) {
    let t = String(text).trim().replace(/−/g, "-").replace(",", ".")
    if (isGainFactor(port)) {
        // Typed in decibels, like it reads; "-inf" is silence where it can be.
        if (/^-?(inf|∞)/i.test(t)) return port.min
        const db = Number(t.replace(/\s*db$/i, ""))
        if (t === "" || !isFinite(db)) return NaN
        return Math.max(port.min, Math.min(port.max, fromDb(db)))
    }
    let scale = 1
    const lower = t.toLowerCase()
    if (/k\s*(hz)?$/.test(lower)) { scale = 1000; t = t.replace(/k\s*(hz)?$/i, "") }
    t = t.replace(/[a-z%:\s]+$/i, "")
    const n = Number(t)
    if (t === "" || !isFinite(n)) return NaN
    let v = Math.max(port.min, Math.min(port.max, n * scale))
    if (port.integer) v = Math.round(v)
    return v
}

// For the box you type into: the plain number, no unit, no rounding to the
// display's few decimals.
function editText(port, v) {
    if (isGainFactor(port)) return dbText(toDb(v), false)
    if (port.integer) return String(Math.round(v))
    return minus(trim(Number(v.toPrecision(6))))
}

function changed(port, v) {
    return Math.abs(v - port.defaultValue) > Math.max(1e-9, (port.max - port.min) * 1e-6)
}

// ── meters ─────────────────────────────────────────────────────────────────
// An effect's output controls, sorted into what they are and paired by channel:
// [{kind, name, ports, clips}]. kind is "level" (a plain 0..1 amplitude, shown
// in dB), "levelDb" (already in dB), "reduction" (how far the effect turns the
// sound down: a factor under 1, or dB), "led" (an on/off light) or "value".
// A left/right pair is one meter with two bars; a clip light whose name matches
// a meter's ("0dB-InL" with "Meter-InL") rides at that meter's end.

function channelOf(port) {
    // Symbol first ("meter_inL", "out_l"); the name when there is none.
    const s = port.symbol || port.name || ""
    const m = /^(.+?)(?:[_-](l|r|left|right)|(L|R)|(Left|Right))$/.exec(s)
    if (!m) return { base: s, channel: "" }
    const ch = (m[2] || m[3] || m[4] || "").toLowerCase()
    return { base: m[1], channel: ch.charAt(0) }
}
function baseKey(base) {
    return base.replace(/^(meter|clip|level|vu|peak|0db|led|over)[_\s-]*/i, "").toLowerCase() || base.toLowerCase()
}
function meterName(port) {
    let n = (port.name || port.symbol || "").replace(/[\s_-]*(?:L|R|Left|Right)$/, "")
    n = n.replace(/^(meter|level|vu|peak|0\s?db)[\s_-]+/i, "")
    // "Input level meter" is the Input meter.
    n = n.replace(/[\s_-]+(level[\s_-]+)?meter$/i, "")
    return n.length ? n.charAt(0).toUpperCase() + n.slice(1) : (port.name || "")
}
// Outputs that only feed a plugin's own drawing (its graph, scale or zoom):
// not something to read, so not shown.
function isDisplayOutput(port) {
    // A compressor's curve and envelope readings are its inner workings, drawn in
    // its own graph; its input, output and reduction are what a person reads.
    return /graph|mesh|\bscale\b|zoom|display|\bfft\b|spectrum|visual|indicator|\bphase\b|position|\bcurve level|envelope level/i.test(words(port).replace(/_/g, " "))
}
function meterKind(port) {
    const w = words(port)
    const unit = (port.unit || "").toLowerCase()
    if (port.toggled || /clip|0 ?db|overload|\bover\b|led\b|_led/i.test(w)) return "led"
    if (/reduc|compress|gating|atten|\bgr\b|(^|[_\s])att($|[_\s])/i.test(w)) return "reduction"
    if (unit === "db") return "levelDb"
    if ((unit === "" || unit === "g") && port.min >= 0 && port.max > 0 && port.max <= 4) return "level"
    // A gain-factor meter that can read above full scale (+36 dB) is still a level.
    if (unit === "g" && port.min >= 0 && port.max > 0 && /meter/i.test(w)) return "level"
    return "value"
}
// More plain readouts than this (and they are a plugin's inner workings, such
// as every band's state) are left out; a few (a tuner's pitch) are shown.
const plainReadouts = 4
function meterModel(outputs) {
    const items = [], levels = ({}), leds = []
    const shown = Array.prototype.slice.call(outputs || []).filter(function (o) { return !isDisplayOutput(o) })
    const plain = shown.filter(function (o) { return meterKind(o) === "value" }).length
    for (const o of shown) {
        const kind = meterKind(o)
        if (kind === "value" && plain > plainReadouts) continue
        const c = channelOf(o)
        if (kind === "level" || kind === "levelDb") {
            const key = kind + ":" + baseKey(c.base)
            if (levels[key] && c.channel !== "") { levels[key].ports.push(o); continue }
            levels[key] = { kind: kind, name: meterName(o), ports: [o], clips: [], key: baseKey(c.base) }
            items.push(levels[key])
        } else if (kind === "led") {
            leds.push({ port: o, key: baseKey(c.base), clip: /clip|0 ?db|over/i.test(words(o)) })
        } else if (kind === "reduction" && c.channel !== "") {
            // A left/right pair of reductions is one meter with two bars.
            const key = "reduction:" + baseKey(c.base)
            if (levels[key]) { levels[key].ports.push(o); continue }
            levels[key] = { kind: kind, name: meterName(o), ports: [o], clips: [], key: baseKey(c.base) }
            items.push(levels[key])
        } else {
            items.push({ kind: kind, name: o.name || o.symbol || "", ports: [o], clips: [] })
        }
    }
    for (const l of leds) {
        const meter = l.clip ? items.find(function (i) { return (i.kind === "level" || i.kind === "levelDb") && i.key === l.key }) : null
        if (meter) meter.clips.push(l.port)
        else items.push({ kind: "led", name: meterName(l.port), ports: [l.port], clips: [], clip: l.clip })
    }
    // Left before right in a pair.
    for (const i of items) i.ports.sort(function (a, b) { return channelOf(a).channel === "r" ? 1 : channelOf(b).channel === "r" ? -1 : 0 })
    // Input, then output, then how much it turns down, then the rest, each kept in
    // the effect's own order.
    const rank = function (i) {
        return /^(in|input)\b/i.test(i.name) ? 0 : /^(out|output)\b/i.test(i.name) ? 1 : i.kind === "reduction" ? 2 : 3
    }
    return items.map(function (i, at) { return { i: i, at: at } })
        .sort(function (a, b) { return rank(a.i) - rank(b.i) || a.at - b.at })
        .map(function (x) { return x.i })
}
// Where a reading sits on its bar (0..1), and how it reads.
function meterFraction(item, port, v) {
    if (item.kind === "level") return Math.max(0, Math.min(1, (toDb(v) + 60) / 60))
    if (item.kind === "levelDb") return Math.max(0, Math.min(1, (v + 60) / 60))
    if (item.kind === "reduction") {
        const r = reductionDb(port, v), most = reductionFull(port)
        return most > 0 ? Math.max(0, Math.min(1, r / most)) : 0
    }
    if (item.kind === "led") return v >= 0.5 ? 1 : 0
    return fraction(port, v)
}
function reductionFactor(port) {
    const unit = (port.unit || "").toLowerCase()
    return unit === "g" || (unit === "" && port.max <= 1.0001 && port.min >= 0)
}
function reductionDb(port, v) {
    if ((port.unit || "").toLowerCase() === "db") return Math.abs(v)
    // A factor: 1 leaves the sound alone, 0.5 halves it (6 dB down).
    if (reductionFactor(port)) return v > 0 ? Math.max(0, -toDb(v)) : 60
    return Math.abs(v)
}
// How much reduction fills the bar: what the effect can do, but no more than
// 24 dB, so the few dB of everyday use show plainly.
function reductionFull(port) {
    const most = reductionFactor(port) ? (port.min > 0 ? -toDb(port.min) : 60)
        : reductionDb(port, Math.abs(port.min) > Math.abs(port.max) ? port.min : port.max)
    return Math.min(24, most)
}
function meterText(item, port, v) {
    if (item.kind === "level") return dbText(toDb(v), false) + (isFinite(toDb(v)) ? " dB" : "")
    if (item.kind === "levelDb") return dbText(v, false) + " dB"
    if (item.kind === "reduction") return dbText(reductionDb(port, v), false) + " dB"
    if (item.kind === "led") return ""
    if ((port.unit || "").toLowerCase() === "g") return dbText(toDb(v), false) + (isFinite(toDb(v)) ? " dB" : "")
    const t = valueText(port, v)
    return t.text + (t.unit ? " " + t.unit : "")
}
// What a meter shows when nothing plays.
function meterRest(item, port) {
    if (item.kind === "level") return 0
    if (item.kind === "reduction") return reductionFactor(port) ? 1 : 0
    if (item.kind === "levelDb") return port.min
    return port.min
}
