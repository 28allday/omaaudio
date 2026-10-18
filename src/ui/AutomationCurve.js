.pragma library

// Mirrors session::smoothAutomationFraction. Restricted phase windows preserve
// an inherited part of the original cubic, rather than easing it again.
function weights(point) {
    const a = point.smoothStart === undefined ? 0 : point.smoothStart
    const b = point.smoothEnd === undefined ? 1 : point.smoothEnd
    const q0 = a * (1 - a), q1 = a * (1 - b) + b * (1 - a), q2 = b * (1 - b)
    const scale = Math.max(q0, q1, q2)
    return scale > 0 ? [q0 / scale, q1 / scale, q2 / scale] : [0, 1, 0]
}
function smooth(t, point) {
    if (t <= 0) return 0
    if (t >= 1) return 1
    const a = point.smoothStart === undefined ? 0 : point.smoothStart
    const b = point.smoothEnd === undefined ? 1 : point.smoothEnd
    if (a === 0 && b === 1) return t * t * (3 - 2 * t)
    const q = weights(point)
    return Math.max(0, Math.min(1, t * (2 * q[0] * (3 - 3 * t + t * t) + q[1] * (3 * t - 2 * t * t) + 2 * q[2] * t * t)
        / (2 * q[0] + q[1] + 2 * q[2])))
}
function slope(t, point) {
    const q = weights(point), u = 1 - t
    return 6 * (q[0] * u * u + q[1] * t * u + q[2] * t * t) / (2 * q[0] + q[1] + 2 * q[2])
}
function mode(point, hold) { return point.interpolation || (hold ? 2 : 1) }
function between(left, right, frame, hold) {
    if (frame <= left.frame) return left.value
    if (frame >= right.frame) return right.value
    const shape = mode(left, hold)
    if (shape === 2) return left.value
    const t = (frame - left.frame) / (right.frame - left.frame)
    const amount = shape === 3 ? smooth(t, left) : t
    return left.value + (right.value - left.value) * amount
}
function sample(points, frame, hold, base) {
    if (!points.length) return base
    if (frame <= points[0].frame) return points[0].value
    let lo = 0, hi = points.length
    while (lo < hi) {
        const middle = Math.floor((lo + hi) / 2)
        if (points[middle].frame <= frame) lo = middle + 1
        else hi = middle
    }
    return lo === points.length ? points[lo - 1].value : between(points[lo - 1], points[lo], frame, hold)
}
// Trace only the visible portion. Cubic endpoints and derivatives give exact
// Bezier controls even when either point or part of its phase lies offscreen.
function trace(ctx, points, from, to, hold, base, px, py) {
    ctx.moveTo(px(from), py(sample(points, from, hold, base)))
    for (let i = 1; i < points.length; ++i) {
        const a = points[i - 1], b = points[i]
        if (b.frame <= from || a.frame >= to) continue
        const start = Math.max(from, a.frame), end = Math.min(to, b.frame)
        const shape = mode(a, hold), x0 = px(start), x1 = px(end)
        const v0 = between(a, b, start, hold)
        ctx.lineTo(x0, py(v0))
        if (shape === 2) {
            ctx.lineTo(x1, py(a.value))
            if (end === b.frame) ctx.lineTo(x1, py(b.value))
        } else if (shape === 3) {
            const u = (start - a.frame) / (b.frame - a.frame), v = (end - a.frame) / (b.frame - a.frame)
            const v1 = between(a, b, end, hold), delta = (b.value - a.value) * (v - u) / 3
            ctx.bezierCurveTo(x0 + (x1 - x0) / 3, py(v0 + delta * slope(u, a)),
                x1 - (x1 - x0) / 3, py(v1 - delta * slope(v, a)), x1, py(v1))
        } else ctx.lineTo(x1, py(between(a, b, end, hold)))
    }
    ctx.lineTo(px(to), py(sample(points, to, hold, base)))
}
function moved(points, index, frame, value) {
    const data = points.map(point => Object.assign({}, point))
    if (index >= 0 && index < data.length) { data[index].frame = frame; data[index].value = value }
    data.sort((a, b) => a.frame - b.frame)
    return data
}
