.pragma library

// Mirrors the session fade shape: apply the optional smooth basis first,
// then bend its interior while keeping the endpoints exactly at 0 and 1.
function value(position, shape, bend) {
    let x = Math.max(0, Math.min(1, position))
    if (x === 0 || x === 1) return x
    if (shape === 1) x = x * x * (3 - 2 * x)
    const amount = Math.max(-1, Math.min(1, bend))
    const k = amount >= 0 ? 1 - 0.875 * amount : 1 - 7 * amount
    return x / (k + (1 - k) * x)
}
