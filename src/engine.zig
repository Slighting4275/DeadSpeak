pub fn panic(msg: []const u8, error_return_trace: ?*anyopaque, return_address: ?usize) noreturn { _ = msg; _ = error_return_trace; _ = return_address; while (true) {} }

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
    const denom: f64 = if (count > 1) @as(f64, @floatFromInt(count - 1)) else 1.0;
    var previous: f32 = 0.0;
    var previous_two: f32 = 0.0;
    for (0..count) |i| {
        const hann: f32 = if (count > 1)
            @floatCast(1.0 - @cos(6.283185307179586 * @as(f64, @floatFromInt(i)) / denom))
        else
            1.0;
        const sample: f32 = (@as(f32, @floatFromInt(samples[i])) / 32768.0) * hann;
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

export fn ds_engine_spectrum_hann_db(samples: ?[*]const i16, count: usize, sample_rate: u32,
                                    bins: ?[*]f32, bin_count: usize) void {
    if (bin_count == 0 or bin_count > 64) return;
    const out = bins orelse return;
    const pcm = samples orelse {
        for (0..bin_count) |i| out[i] = 0.0;
        return;
    };
    if (count == 0 or sample_rate == 0) {
        for (0..bin_count) |i| out[i] = 0.0;
        return;
    }
    const nyquist = @as(f64, @floatFromInt(sample_rate)) * 0.5;
    for (0..bin_count) |i| {
        const t = if (bin_count == 1) 0.0 else @as(f64, @floatFromInt(i)) / @as(f64, @floatFromInt(bin_count - 1));
        const frequency = 125.0 * @exp(t * @log(@as(f64, 64.0)));
        if (frequency >= nyquist) {
            out[i] = 0.0;
            continue;
        }
        const amp = goertzel(pcm, count, sample_rate, frequency);
        if (amp <= 0.0005) {
            out[i] = 0.0;
            continue;
        }
        const db = 20.0 * @log10(@as(f64, amp));
        const norm: f32 = @floatCast((db + 60.0) / 60.0);
        out[i] = if (norm < 0.0) 0.0 else if (norm > 1.0) 1.0 else norm;
    }
}

export fn ds_engine_sub_bass_energy(samples: ?[*]const i16, count: usize, sample_rate: u32) f32 {
    const pcm = samples orelse return 0.0;
    if (count == 0 or sample_rate == 0) return 0.0;
    const freqs = [_]f64{ 80.0, 120.0, 160.0, 200.0, 250.0, 300.0 };
    const nyquist = @as(f64, @floatFromInt(sample_rate)) * 0.5;
    var total_amp: f32 = 0.0;
    for (freqs) |freq| {
        if (freq < nyquist) {
            total_amp += goertzel(pcm, count, sample_rate, freq);
        }
    }
    return (total_amp / @as(f32, @floatFromInt(freqs.len))) * 1000.0;
}

export fn ds_engine_speech_ratio(samples: ?[*]const i16, count: usize, sample_rate: u32) f32 {
    const pcm = samples orelse return 0.0;
    if (count == 0 or sample_rate == 0) return 0.0;
    const sub_freqs = [_]f64{ 80.0, 120.0, 160.0, 200.0, 250.0, 300.0 };
    const speech_freqs = [_]f64{ 400.0, 700.0, 1000.0, 1400.0, 1900.0, 2400.0 };
    const nyquist = @as(f64, @floatFromInt(sample_rate)) * 0.5;
    var sub_sum: f32 = 0.0;
    for (sub_freqs) |freq| {
        if (freq < nyquist) sub_sum += goertzel(pcm, count, sample_rate, freq);
    }
    var speech_sum: f32 = 0.0;
    for (speech_freqs) |freq| {
        if (freq < nyquist) speech_sum += goertzel(pcm, count, sample_rate, freq);
    }
    const sub_mean = sub_sum / @as(f32, @floatFromInt(sub_freqs.len));
    const speech_mean = speech_sum / @as(f32, @floatFromInt(speech_freqs.len));
    if (speech_mean <= 0.00001 and sub_mean <= 0.00001) return 0.0;
    return speech_mean / (sub_mean + 0.0005);
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

export fn ds_engine_vector_word(bx: f32, by: f32, bz: f32,
                                base_x: f32, base_y: f32, base_z: f32,
                                dictionary: ?[*]const u8, dictionary_size: usize,
                                out: [*]u8, capacity: usize) usize {
    if (capacity == 0) return 0;
    out[0] = 0;
    const bytes = dictionary orelse return 0;
    if (dictionary_size < 12 or bytes[0] != 'D' or bytes[1] != 'S' or bytes[2] != 'W' or bytes[3] != 'B') return 0;
    if (read_u32(bytes, 4) != 1) return 0;
    const word_count = read_u32(bytes, 8);
    if (word_count == 0) return 0;

    if (!@import("std").math.isFinite(bx) or !@import("std").math.isFinite(by) or !@import("std").math.isFinite(bz)) return 0;
    if (!@import("std").math.isFinite(base_x) or !@import("std").math.isFinite(base_y) or !@import("std").math.isFinite(base_z)) return 0;

    const dx: f64 = @as(f64, bx) - @as(f64, base_x);
    const dy: f64 = @as(f64, by) - @as(f64, base_y);
    const dz: f64 = @as(f64, bz) - @as(f64, base_z);
    const delta_mag = @sqrt(dx * dx + dy * dy + dz * dz);
    if (delta_mag < 0.0001 or delta_mag > 1000000.0) return 0;

    // Azimuth in horizontal plane [0.0, 360.0) degrees
    var azimuth_rad = @import("std").math.atan2(dy, dx);
    if (azimuth_rad < 0.0) azimuth_rad += 6.283185307179586;
    const azimuth_deg = azimuth_rad * 57.29577951308232;

    // Elevation in vertical plane [-90.0, +90.0] degrees
    const z_ratio = if (dz / delta_mag > 1.0) 1.0 else if (dz / delta_mag < -1.0) -1.0 else dz / delta_mag;
    const elevation_deg = @import("std").math.asin(z_ratio) * 57.29577951308232;

    // High resolution spatial quantization:
    // 0.25-degree azimuth resolution = 1440 bins
    const az_val = @mod(azimuth_deg * 4.0, 1440.0);
    const az_bin: u64 = @intFromFloat(@min(@max(0.0, az_val), 1439.0));
    // 0.5-degree elevation resolution = 360 bins
    const el_val = (elevation_deg + 90.0) * 2.0;
    const el_bin: u64 = @intFromFloat(@min(@max(0.0, el_val), 360.0));
    // 0.1 uT magnitude resolution
    const mag_bin: u64 = @intFromFloat(@min(@max(0.0, delta_mag * 10.0), 100000.0));

    // Deterministic 64-bit spatial FNV-1a hash across the quantized 3D spherical coordinates
    var h: u64 = 0xcbf29ce484222325;
    const prime: u64 = 0x100000001b3;
    h = (h ^ az_bin) *% prime;
    h = (h ^ el_bin) *% prime;
    h = (h ^ mag_bin) *% prime;

    const index: usize = @intCast(h % @as(u64, word_count));
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

export fn ds_engine_next_word(magnetic_field: f32, sample: u32, dictionary: ?[*]const u8,
                              dictionary_size: usize, out: [*]u8, capacity: usize) usize {
    _ = sample;
    return ds_engine_vector_word(magnetic_field, 0.0, 0.0, 0.0, 0.0, 0.0, dictionary, dictionary_size, out, capacity);
}
