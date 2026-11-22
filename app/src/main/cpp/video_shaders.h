#ifndef DEADSPEAK_VIDEO_SHADERS_H
#define DEADSPEAK_VIDEO_SHADERS_H

/* Shared OpenGL ES 2.0 camera filter & anomaly overlay shaders used by both live
 * preview (main.c) and the hardware-backed MP4 encoder surface (video_capture.c). */

static const char *const DS_CAMERA_VERTEX_SHADER =
    "attribute vec2 a_position;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "}\n";

static const char *const DS_CAMERA_FRAGMENT_SHADER =
    "precision mediump float;\n"
    "uniform sampler2D u_image;\n"
    "uniform int u_filter;\n"
    "uniform float u_anomaly;\n"
    "uniform float u_time;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 uv = v_uv;\n"
    "    vec4 c = texture2D(u_image, uv);\n"
    "    if (u_anomaly > 0.01) {\n"
    "        float band = sin(uv.y * 85.0 + u_time * 18.0);\n"
    "        float jitter = (band > 0.86 ? 0.006 : 0.0015) * u_anomaly;\n"
    "        float cr = texture2D(u_image, clamp(uv + vec2(jitter, 0.0), 0.0, 1.0)).r;\n"
    "        float cb = texture2D(u_image, clamp(uv - vec2(jitter, 0.0), 0.0, 1.0)).b;\n"
    "        c.rgb = mix(c.rgb, vec3(cr, c.g, cb), clamp(u_anomaly, 0.0, 1.0));\n"
    "        float scan = 1.0 - 0.14 * u_anomaly * (0.5 + 0.5 * sin(uv.y * 240.0 + u_time * 24.0));\n"
    "        c.rgb *= scan;\n"
    "    }\n"
    "    float luma = dot(c.rgb, vec3(0.299, 0.587, 0.114));\n"
    "    if (u_filter == 0) {\n" /* NIGHT: Phosphor Green night vision */
    "        float g = clamp(luma * 1.55 + 0.08, 0.0, 1.0);\n"
    "        gl_FragColor = vec4(g * 0.12, g * 1.08, g * 0.22, 1.0);\n"
    "    } else if (u_filter == 1) {\n" /* THERMAL: False-Color Heatmap */
    "        float t = clamp(luma, 0.0, 1.0);\n"
    "        vec3 col;\n"
    "        if (t < 0.25) col = mix(vec3(0.05, 0.0, 0.25), vec3(0.0, 0.2, 0.85), t / 0.25);\n"
    "        else if (t < 0.5) col = mix(vec3(0.0, 0.2, 0.85), vec3(0.85, 0.1, 0.15), (t - 0.25) / 0.25);\n"
    "        else if (t < 0.75) col = mix(vec3(0.85, 0.1, 0.15), vec3(1.0, 0.85, 0.0), (t - 0.5) / 0.25);\n"
    "        else col = mix(vec3(1.0, 0.85, 0.0), vec3(1.0, 1.0, 1.0), (t - 0.75) / 0.25);\n"
    "        gl_FragColor = vec4(col, 1.0);\n"
    "    } else if (u_filter == 2) {\n" /* EDGE: Sobel spectral contour detection */
    "        float d = 1.0 / 480.0;\n"
    "        float c0 = dot(texture2D(u_image, uv + vec2(-d, -d)).rgb, vec3(0.299, 0.587, 0.114));\n"
    "        float c1 = dot(texture2D(u_image, uv + vec2(0.0, -d)).rgb, vec3(0.299, 0.587, 0.114));\n"
    "        float c2 = dot(texture2D(u_image, uv + vec2(d, -d)).rgb, vec3(0.299, 0.587, 0.114));\n"
    "        float c3 = dot(texture2D(u_image, uv + vec2(-d, 0.0)).rgb, vec3(0.299, 0.587, 0.114));\n"
    "        float c5 = dot(texture2D(u_image, uv + vec2(d, 0.0)).rgb, vec3(0.299, 0.587, 0.114));\n"
    "        float c6 = dot(texture2D(u_image, uv + vec2(-d, d)).rgb, vec3(0.299, 0.587, 0.114));\n"
    "        float c7 = dot(texture2D(u_image, uv + vec2(0.0, d)).rgb, vec3(0.299, 0.587, 0.114));\n"
    "        float c8 = dot(texture2D(u_image, uv + vec2(d, d)).rgb, vec3(0.299, 0.587, 0.114));\n"
    "        float gx = -c0 + c2 - 2.0 * c3 + 2.0 * c5 - c6 + c8;\n"
    "        float gy = -c0 - 2.0 * c1 - c2 + c6 + 2.0 * c7 + c8;\n"
    "        float edge = clamp(sqrt(gx * gx + gy * gy) * 3.2, 0.0, 1.0);\n"
    "        gl_FragColor = vec4(edge * 0.1, edge * 1.0, edge * 0.75, 1.0);\n"
    "    } else if (u_filter == 3) {\n" /* MONO: High Contrast IR */
    "        float m = clamp((luma - 0.08) * 1.25, 0.0, 1.0);\n"
    "        gl_FragColor = vec4(vec3(m), 1.0);\n"
    "    } else if (u_filter == 4) {\n" /* SEPIA: Vintage */
    "        vec3 s = vec3(dot(c.rgb, vec3(0.393, 0.769, 0.189)),\n"
    "                      dot(c.rgb, vec3(0.349, 0.686, 0.168)),\n"
    "                      dot(c.rgb, vec3(0.272, 0.534, 0.131)));\n"
    "        gl_FragColor = vec4(clamp(s, 0.0, 1.0), 1.0);\n"
    "    } else {\n" /* RAW */
    "        gl_FragColor = c;\n"
    "    }\n"
    "}\n";

static const char *const DS_RGBA_OVERLAY_FRAGMENT_SHADER =
    "precision mediump float;\n"
    "uniform sampler2D u_image;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_image, v_uv);\n"
    "}\n";

#endif /* DEADSPEAK_VIDEO_SHADERS_H */
