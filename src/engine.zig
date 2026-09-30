fn magnitude(value: f32) f32 {
    return if (value < 0.0) -value else value;
}

export fn ds_engine_smooth(previous: f32, current: f32, alpha: f32) f32 {
    const bounded = if (alpha < 0.0) 0.0 else if (alpha > 1.0) 1.0 else alpha;
    return previous + ((current - previous) * bounded);
}

export fn ds_engine_signal(magnetic_field: f32, measured_baseline: f32) f32 {
    if (!@import("std").math.isFinite(magnetic_field) or !@import("std").math.isFinite(measured_baseline)) return 0;
    const distance = magnitude(magnetic_field - measured_baseline);
    const normalized = distance / 20.0;
    return if (normalized > 1.0) 1.0 else normalized;
}

fn goertzel(samples: [*]const i16, count: usize, sample_rate: u32, frequency: f64) f32 {
    if (count == 0 or sample_rate == 0) return 0.0;
    const omega = 6.283185307179586 * frequency / @as(f64, @floatFromInt(sample_rate));
    const coefficient: f32 = @floatCast(2.0 * @cos(omega));
    var previous: f32 = 0.0;
    var previous_two: f32 = 0.0;
    for (0..count) |i| {
        const sample: f32 = @as(f32, @floatFromInt(samples[i])) / 32768.0;
        const current = sample + coefficient * previous - previous_two;
        previous_two = previous;
        previous = current;
    }
    const power = previous * previous + previous_two * previous_two - coefficient * previous * previous_two;
    if (power <= 0.0) return 0.0;
    const amplitude: f32 = @floatCast(@sqrt(@as(f64, power)) / @as(f64, @floatFromInt(count)));
    return if (amplitude > 1.0) 1.0 else amplitude;
}

export fn ds_engine_spectrum(samples: [*]const i16, count: usize, sample_rate: u32, bands: [*]f32) void {
    bands[0] = goertzel(samples, count, sample_rate, 250.0);
    bands[1] = goertzel(samples, count, sample_rate, 1000.0);
    bands[2] = goertzel(samples, count, sample_rate, 3000.0);
}

// Log-spaced measurements of actual microphone samples, not a decorative EQ.
// Frequencies above Nyquist are deliberately left empty.
export fn ds_engine_spectrum_bins(samples: [*]const i16, count: usize, sample_rate: u32,
                                 bins: [*]f32, bin_count: usize) void {
    if (bin_count == 0 or bin_count > 64) return;
    for (0..bin_count) |i| {
        const t = if (bin_count == 1) 0.0 else @as(f64, @floatFromInt(i)) / @as(f64, @floatFromInt(bin_count - 1));
        const frequency = 125.0 * @exp(t * @log(@as(f64, 64.0)));
        bins[i] = if (frequency < @as(f64, @floatFromInt(sample_rate)) * 0.5)
            goertzel(samples, count, sample_rate, frequency) else 0.0;
    }
}

fn read_u16(bytes: [*]const u8, offset: usize) u16 {
    return @as(u16, bytes[offset]) | (@as(u16, bytes[offset + 1]) << 8);
}

fn read_u32(bytes: [*]const u8, offset: usize) u32 {
    return @as(u32, bytes[offset]) |
        (@as(u32, bytes[offset + 1]) << 8) |
        (@as(u32, bytes[offset + 2]) << 16) |
        (@as(u32, bytes[offset + 3]) << 24);
}

export fn ds_engine_next_word(magnetic_field: f32, sample: u32, dictionary: ?[*]const u8,
                              dictionary_size: usize, out: [*]u8, capacity: usize) usize {
    if (capacity == 0) return 0;
    out[0] = 0;
    const bytes = dictionary orelse return 0;
    if (dictionary_size < 12 or bytes[0] != 'D' or bytes[1] != 'S' or bytes[2] != 'W' or bytes[3] != 'B') return 0;
    if (read_u32(bytes, 4) != 1) return 0;
    const word_count = read_u32(bytes, 8);
    if (word_count == 0) return 0;

    // Reject invalid/out-of-range fields before float-to-integer conversion.
    // A malformed reading must never panic the native engine or select a word.
    if (!@import("std").math.isFinite(magnetic_field) or magnitude(magnetic_field) > 1000000.0) return 0;

    const quantized: u64 = @intFromFloat(magnitude(magnetic_field) * 10.0);
    const index: usize = @intCast((quantized + @as(u64, sample)) % @as(u64, word_count));
    var offset: usize = 12;
    for (0..index + 1) |word_index| {
        if (offset + 2 > dictionary_size) return 0;
        const word_length = @as(usize, read_u16(bytes, offset));
        offset += 2;
        if (offset + word_length > dictionary_size) return 0;
        if (word_index == index) {
            const length = if (word_length < capacity - 1) word_length else capacity - 1;
            for (0..length) |i| out[i] = bytes[offset + i];
            out[length] = 0;
            return length;
        }
        offset += word_length;
    }
    return 0;
}
