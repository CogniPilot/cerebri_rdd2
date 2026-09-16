#!/usr/bin/env python3
"""Bench the raw optical-flow product (OpticalFlowData) against the IMU.

For an indoor (no GPS) hand-carry test this checks the flow node's raw channel
for axis convention, sign, gyro compensation and scale without any external
position reference. It consumes the replay interchange (imu.csv, flow_raw.csv);
given an MCAP it first decodes it with mcap_to_interchange.py.

flow_raw.csv columns (mcap_to_interchange.py):
  t_s,flow_rad_x,flow_rad_y,delta_angle_x,delta_angle_y,delta_angle_z,
  integration_s,dist_m,dist_quality,quality,flags
imu.csv columns:
  t_s,gx_rad_s,gy_rad_s,gz_rad_s,ax_m_s2,ay_m_s2,az_m_s2

Both streams are on the flight-controller control-IMU boot clock: the decoder
re-stamps the wire flow payloads onto that clock (wire_to_boot) and rebases IMU
and flow to the first IMU sample, so flow_raw.csv timestamps and imu.csv
timestamps share one domain and can be differenced directly.

Convention checked (authoritative, matching navigation_optical_flow_raw.c):
  flow_rad is the integrated angular image flow in body FLU, in the SAME
  rotational sense as the body rotation, so a pure body rotation about +x by
  angle a gives flow_rad.x ~= +a. delta_angle_flu is the genuine +integral of
  the body FLU rate. The translation flow is tau = flow_rad - delta_angle (x,y);
  v_forward = range*tau_y/dt, v_left = -range*tau_x/dt. So for a pure-rotation
  window flow_rad = delta_angle and tau = 0, and for a pure +x (forward)
  translation flow_rad.y > 0, for a pure +y (left) translation flow_rad.x < 0.

The node reports flow angles from PAA3905 counts through VOF_SENS = 480.24
counts/rad; a scale error there shows as a distance-scale error on the walk.
"""
import argparse
import json
import os
import subprocess
import sys

import numpy as np

VOF_SENS_COUNTS_PER_RAD = 480.24

RAW_COLUMNS = (
    "t_s", "flow_rad_x", "flow_rad_y", "delta_angle_x", "delta_angle_y",
    "delta_angle_z", "integration_s", "dist_m", "dist_quality", "quality",
    "flags",
)


def load_named(path):
    """Read a CSV that may carry leading '#' comment lines before the header."""
    header = None
    body = []
    with open(path) as handle:
        for line in handle:
            if line.startswith("#") or not line.strip():
                continue
            if header is None:
                header = line.strip().split(",")
            else:
                body.append(line)
    if header is None:
        raise SystemExit("%s: no header row" % path)
    if not body:
        raise SystemExit("%s: no data rows" % path)
    data = np.array([[float(v) for v in row.split(",")] for row in body])
    return {name: data[:, i] for i, name in enumerate(header)}


def ensure_interchange(source, tool_dir):
    """Return an interchange directory, decoding an MCAP first if needed.

    A directory is used as is. A file is treated as an MCAP and decoded into a
    sibling ``<name>.interchange`` directory with mcap_to_interchange.py.
    """
    if os.path.isdir(source):
        return source
    if not os.path.isfile(source):
        raise SystemExit("input %s is neither a directory nor a file" % source)
    out_dir = os.path.splitext(source)[0] + ".interchange"
    decoder = os.path.join(tool_dir, "mcap_to_interchange.py")
    print("decoding %s -> %s" % (source, out_dir))
    subprocess.run([sys.executable, decoder, source, out_dir, "--fill-gaps"],
                   check=True)
    return out_dir


def cumulative_integral(t, y):
    """Cumulative trapezoidal integral of y over t, starting at zero."""
    dt = np.diff(t)
    seg = 0.5 * (y[1:] + y[:-1]) * dt
    return np.concatenate(([0.0], np.cumsum(seg)))


def gyro_delta_over_windows(imu, t_end, integration_s, lag_s=0.0):
    """Integrate the FC gyro over each flow window [t_end-integ, t_end]+lag.

    Returns an (N, 3) array of body FLU delta angles per flow sample.
    """
    ti = imu["t_s"]
    cg = np.column_stack([
        cumulative_integral(ti, imu["gx_rad_s"]),
        cumulative_integral(ti, imu["gy_rad_s"]),
        cumulative_integral(ti, imu["gz_rad_s"]),
    ])
    end = t_end + lag_s
    start = end - integration_s
    out = np.zeros((len(t_end), 3))
    for axis in range(3):
        out[:, axis] = np.interp(end, ti, cg[:, axis]) - \
            np.interp(start, ti, cg[:, axis])
    return out


def body_velocity_reference(imu, t0, t1):
    """AC body-frame velocity over [t0, t1] from the accelerometer.

    Gravity and accelerometer bias are removed as the segment mean of each
    axis (valid for a roughly level segment), the specific force is integrated
    to velocity and the velocity is linearly detrended (high-pass) to drop the
    residual integration ramp. Only the horizontal (x forward, y left) axes are
    returned, resampled onto a dense grid; the result carries direction, not
    absolute scale.
    """
    ti = imu["t_s"]
    m = (ti >= t0) & (ti <= t1)
    t = ti[m]
    if len(t) < 8:
        return None
    ax = imu["ax_m_s2"][m] - np.mean(imu["ax_m_s2"][m])
    ay = imu["ay_m_s2"][m] - np.mean(imu["ay_m_s2"][m])
    vx = cumulative_integral(t, ax)
    vy = cumulative_integral(t, ay)

    def detrend(v):
        c = np.polyfit(t - t[0], v, 1)
        return v - np.polyval(c, t - t[0])

    return dict(t=t, vfwd=detrend(vx), vleft=detrend(vy))


def signed_permutations():
    """The eight 2x2 signed permutation matrices."""
    out = []
    for perm in ((0, 1), (1, 0)):
        for sx in (1.0, -1.0):
            for sy in (1.0, -1.0):
                p = np.zeros((2, 2))
                p[0, perm[0]] = sx
                p[1, perm[1]] = sy
                out.append(p)
    return out


def describe_permutation(p):
    """Human-readable form of a signed permutation as flow<-body relations."""
    labels = ("vfwd(+x)", "vleft(+y)")
    rows = []
    for r, name in enumerate(("tau_x", "tau_y")):
        col = int(np.argmax(np.abs(p[r])))
        sign = "+" if p[r, col] > 0 else "-"
        rows.append("%s=%s%s" % (name, sign, labels[col]))
    return ", ".join(rows)


# --------------------------------------------------------------------------
# segmentation
# --------------------------------------------------------------------------

def moving_average(x, n):
    if n <= 1:
        return x
    kernel = np.ones(n) / n
    return np.convolve(x, kernel, mode="same")


def close_gaps(mask, radius):
    """Binary closing: fill gaps in a boolean mask up to 2*radius wide.

    This keeps an oscillatory rotation, whose rate dips to zero twice per
    cycle, as a single active segment.
    """
    if radius <= 0 or not mask.any():
        return mask
    n = len(mask)
    dil = mask.copy()
    for i in np.flatnonzero(mask):
        dil[max(0, i - radius):min(n, i + radius + 1)] = True
    ero = dil.copy()
    for i in np.flatnonzero(~dil):
        ero[max(0, i - radius):min(n, i + radius + 1)] = False
    return ero


def segment_log(imu, flow, cfg):
    """Classify each flow sample and merge contiguous runs into segments.

    Rest, translation and rotation are told apart by the FC gyro rate and the
    translation-flow speed at each flow time; both features are smoothed over a
    short window so that an oscillatory roll/pitch/yaw motion (which crosses
    zero rate twice per cycle) stays one segment. Rotation segments are further
    labelled by the dominant body axis.
    """
    tf = flow["t_s"]
    integ = np.clip(flow["integration_s"], 1e-3, None)
    delta_fc = gyro_delta_over_windows(imu, tf, integ)
    gyro_rate = np.linalg.norm(delta_fc, axis=1) / integ
    tau = np.column_stack([
        flow["flow_rad_x"] - delta_fc[:, 0],
        flow["flow_rad_y"] - delta_fc[:, 1],
    ])
    speed = flow["dist_m"] * np.linalg.norm(tau, axis=1) / integ

    dt_flow = np.median(np.diff(tf)) if len(tf) > 1 else 0.02
    win = max(1, int(round(cfg["smooth_s"] / max(dt_flow, 1e-6))))
    radius = int(round(cfg["bridge_s"] / max(dt_flow, 1e-6)))
    gyro_s = moving_average(gyro_rate, win)
    speed_s = moving_average(speed, win)

    rot_mask = close_gaps(gyro_s > cfg["rot_gyro"], radius)
    trans_mask = close_gaps((speed_s > cfg["trans_speed"]) & ~rot_mask,
                            radius) & ~rot_mask
    rest_mask = ((gyro_s < cfg["rest_gyro"]) & (speed_s < cfg["rest_speed"]) &
                 ~rot_mask & ~trans_mask)

    label = np.full(len(tf), "none", dtype=object)
    label[rot_mask] = "rot"
    label[trans_mask] = "trans"
    label[rest_mask] = "rest"

    segments = []
    i = 0
    n = len(tf)
    while i < n:
        j = i
        while j < n and label[j] == label[i]:
            j += 1
        t0 = tf[i]
        t1 = tf[j - 1]
        kind = label[i]
        if kind != "none" and (t1 - t0) >= cfg["min_seg_s"]:
            sub = slice(i, j)
            if kind == "rot":
                axis_energy = np.sum(np.abs(delta_fc[sub]), axis=0)
                axis = ("roll_x", "pitch_y", "yaw_z")[int(np.argmax(axis_energy))]
                name = "rot_%s" % axis
            else:
                name = kind
            segments.append(dict(kind=kind, name=name, t0=float(t0),
                                 t1=float(t1), i0=i, i1=j))
        i = j
    return segments, dict(delta_fc=delta_fc, gyro_rate=gyro_rate, tau=tau,
                          speed=speed)


def label_segments(segments):
    """Give repeated segments of a kind an ordinal suffix for reference."""
    counts = {}
    for seg in segments:
        counts.setdefault(seg["name"], 0)
        counts[seg["name"]] += 1
    seen = {}
    for seg in segments:
        if counts[seg["name"]] > 1:
            seen.setdefault(seg["name"], 0)
            seen[seg["name"]] += 1
            seg["label"] = "%s_%d" % (seg["name"], seen[seg["name"]])
        else:
            seg["label"] = seg["name"]


# --------------------------------------------------------------------------
# rotation analysis
# --------------------------------------------------------------------------

def analyze_rotation(imu, flow, seg, lag_grid_s):
    """Compare node delta angle and flow angle with the FC gyro on rotation.

    Sweeps a lag, fits the node delta angle as A @ delta_fc (3x3) to check the
    node's own gyro integration, and for an in-plane rotation (roll about x,
    pitch about y) fits the flow angle on that axis against the FC gyro to see
    whether flow_rad carries the rotation the adapter subtracts. Reports the
    adapter compensation residual tau = flow_rad - delta_node before and after
    compensation. For a yaw rotation the in-plane flow should be near zero, so
    the residual itself is the check.
    """
    sub = slice(seg["i0"], seg["i1"])
    tf = flow["t_s"][sub]
    integ = np.clip(flow["integration_s"][sub], 1e-3, None)
    delta_node = np.column_stack([
        flow["delta_angle_x"][sub], flow["delta_angle_y"][sub],
        flow["delta_angle_z"][sub],
    ])
    flow_xy = np.column_stack([flow["flow_rad_x"][sub], flow["flow_rad_y"][sub]])

    best = None
    for lag in lag_grid_s:
        delta_fc = gyro_delta_over_windows(imu, tf, integ, lag)
        resid = delta_node - _fit_linear(delta_fc, delta_node)[1]
        rms = float(np.sqrt(np.mean(np.sum(resid ** 2, axis=1))))
        if best is None or rms < best[0]:
            best = (rms, lag)
    lag = best[1]
    delta_fc = gyro_delta_over_windows(imu, tf, integ, lag)

    a_node, _ = _fit_linear(delta_fc, delta_node)          # 3x3
    node_diag = [float(a_node[k, k]) for k in range(3)]

    axis = int(np.argmax(np.sum(np.abs(delta_fc), axis=0)))
    axis_name = ("roll_x", "pitch_y", "yaw_z")[axis]
    node_gain_axis = float(a_node[axis, axis])

    tau = flow_xy - delta_node[:, :2]
    before = float(np.sqrt(np.mean(np.sum(flow_xy ** 2, axis=1))))
    after = float(np.sqrt(np.mean(np.sum(tau ** 2, axis=1))))

    comp_gain = None
    cross_gain = None
    if axis in (0, 1):
        drive = delta_fc[:, axis]
        denom = float(np.dot(drive, drive))
        if denom > 0:
            comp_gain = float(np.dot(drive, flow_xy[:, axis]) / denom)
            other = 1 - axis
            cross_gain = float(np.dot(drive, flow_xy[:, other]) / denom)
        verdict = _compensation_verdict(comp_gain, cross_gain)
    else:
        verdict = ("yaw: node gyro-z gain %.3f (ideal 1); in-plane flow rms "
                   "%.5f rad after comp %.5f rad (ideal ~0)" % (
                       node_diag[2], before, after))
    return dict(
        label=seg["label"], axis=axis_name, t0=seg["t0"], t1=seg["t1"],
        n=int(len(tf)), lag_ms=float(lag * 1e3),
        a_node=a_node.tolist(), node_delta_diag=node_diag,
        node_gain_axis=node_gain_axis,
        comp_gain=comp_gain, cross_gain=cross_gain,
        resid_before_rad=before, resid_after_rad=after,
        compensation_verdict=verdict,
    )


def _compensation_verdict(comp_gain, cross_gain):
    if comp_gain is None:
        return "insufficient rotation on this axis"
    if cross_gain is not None and abs(cross_gain) > 0.4 * max(abs(comp_gain),
                                                              1e-6):
        return ("cross-leaked (in-plane flow gain %.2f, cross-axis %.2f)" %
                (comp_gain, cross_gain))
    if comp_gain < 0:
        return "sign-flipped (flow tracks -gyro, gain %.2f)" % comp_gain
    if comp_gain < 0.4:
        return "missing (flow carries little rotation, gain %.2f)" % comp_gain
    if comp_gain > 1.6:
        return "doubled (flow ~2x rotation, gain %.2f)" % comp_gain
    return "consistent (flow ~ +gyro, gain %.2f)" % comp_gain


def _fit_linear(x, y):
    """Least-squares fit y ~ x @ C (no intercept). Returns (C.T, prediction)."""
    coef, _, _, _ = np.linalg.lstsq(x, y, rcond=None)
    return coef.T, x @ coef


# --------------------------------------------------------------------------
# translation analysis
# --------------------------------------------------------------------------

def analyze_translation(imu, flow, feats, trans_segments, lag_grid_s,
                        exclude_labels=()):
    """Recover the sensor-to-body axis map from translation segments.

    The reference is AC body velocity from the accelerometer; the measurement
    is the translation flow tau/integration. A common lag is swept and every
    signed permutation is fit with a positive scale. Constant-velocity walk
    segments are excluded because the high-pass AC reference cancels a steady
    velocity, leaving no direction to match; use the oscillating forward-back
    and left-right segments (B and C) for the map.
    """
    ref_t = []
    ref_v = []
    meas_t = []
    meas_v = []
    for seg in trans_segments:
        if seg["label"] in exclude_labels:
            continue
        ref = body_velocity_reference(imu, seg["t0"], seg["t1"])
        if ref is None:
            continue
        sub = slice(seg["i0"], seg["i1"])
        integ = np.clip(flow["integration_s"][sub], 1e-3, None)
        tau = feats["tau"][sub]
        ref_t.append(ref["t"])
        ref_v.append(np.column_stack([ref["vfwd"], ref["vleft"]]))
        meas_t.append(flow["t_s"][sub])
        meas_v.append(tau / integ[:, None])
    if not meas_t:
        return None

    def resample_ref(lag):
        r = []
        for k in range(len(ref_t)):
            vf = np.interp(meas_t[k] + lag, ref_t[k], ref_v[k][:, 0])
            vl = np.interp(meas_t[k] + lag, ref_t[k], ref_v[k][:, 1])
            r.append(np.column_stack([vf, vl]))
        return np.vstack(r)

    m = np.vstack(meas_v)
    m = m - np.mean(m, axis=0)
    cands = signed_permutations()

    best_overall = None
    for lag in lag_grid_s:
        r = resample_ref(lag)
        r = r - np.mean(r, axis=0)
        for p in cands:
            pr = r @ p.T
            denom = float(np.sum(pr * pr))
            if denom <= 0:
                continue
            s = float(np.sum(m * pr) / denom)
            if s <= 0:
                continue
            resid = float(np.sqrt(np.mean(np.sum((m - s * pr) ** 2, axis=1))))
            if best_overall is None or resid < best_overall["resid"]:
                best_overall = dict(resid=resid, scale=s, lag=lag,
                                    p=p.copy())

    lag = best_overall["lag"]
    r = resample_ref(lag)
    r = r - np.mean(r, axis=0)
    ranking = []
    for p in cands:
        pr = r @ p.T
        denom = float(np.sum(pr * pr))
        if denom <= 0:
            continue
        s = float(np.sum(m * pr) / denom)
        resid = float(np.sqrt(np.mean(np.sum((m - s * pr) ** 2, axis=1))))
        ranking.append(dict(matrix=p.tolist(), det=float(np.linalg.det(p)),
                            scale=s, resid=resid,
                            describe=describe_permutation(p)))
    ranking.sort(key=lambda d: d["resid"])

    expected = np.array([[0.0, -1.0], [1.0, 0.0]])
    best_p = np.array(best_overall["p"])
    is_expected = np.allclose(best_p, expected)
    reflection = np.linalg.det(best_p) < 0
    return dict(
        n=int(len(m)), lag_ms=float(lag * 1e3),
        best=dict(matrix=best_p.tolist(), scale=best_overall["scale"],
                  resid=best_overall["resid"],
                  describe=describe_permutation(best_p),
                  det=float(np.linalg.det(best_p))),
        matches_convention=bool(is_expected),
        reflection=bool(reflection),
        ranking=ranking,
    )


# --------------------------------------------------------------------------
# walk analysis and dead reckoning
# --------------------------------------------------------------------------

def analyze_walk(imu, flow, feats, seg, walk_length_m, out_dir):
    """Dead-reckon a walk segment and compare its extent with a paced length."""
    sub = slice(seg["i0"], seg["i1"])
    tf = flow["t_s"][sub]
    integ = np.clip(flow["integration_s"][sub], 1e-3, None)
    dist = flow["dist_m"][sub]
    tau = feats["tau"][sub]

    # Body-frame incremental displacement over each window.
    dfwd = dist * tau[:, 1]                # range * tau_y = v_fwd * dt
    dleft = -dist * tau[:, 0]              # -range * tau_x = v_left * dt

    # Yaw from the integrated FC gyro z, sampled at flow times.
    yaw_full = cumulative_integral(imu["t_s"], imu["gz_rad_s"])
    yaw = np.interp(tf, imu["t_s"], yaw_full)
    yaw = yaw - yaw[0]

    fwd_cum = np.cumsum(dfwd)
    left_cum = np.cumsum(dleft)
    x = np.zeros(len(tf))
    y = np.zeros(len(tf))
    px = 0.0
    py = 0.0
    for i in range(len(tf)):
        c = np.cos(yaw[i])
        s = np.sin(yaw[i])
        px += c * dfwd[i] - s * dleft[i]
        py += s * dfwd[i] + c * dleft[i]
        x[i] = px
        y[i] = py

    forward_extent = float(np.max(fwd_cum) - np.min(fwd_cum))
    path_length = float(np.sum(np.hypot(dfwd, dleft)))
    net = float(np.hypot(x[-1], y[-1]))

    correction = None
    new_vof = None
    if walk_length_m and forward_extent > 1e-3:
        correction = float(walk_length_m / forward_extent)
        new_vof = float(VOF_SENS_COUNTS_PER_RAD * correction)

    track_csv = os.path.join(out_dir, "flow_bench_track.csv")
    np.savetxt(track_csv,
               np.column_stack([tf, x, y, yaw, fwd_cum, left_cum]),
               delimiter=",", fmt="%.6f",
               header="t_s,x_m,y_m,yaw_rad,fwd_cum_m,left_cum_m", comments="")

    png = os.path.join(out_dir, "flow_bench_track.png")
    plotted = _plot_track(x, y, png, seg["label"])

    return dict(
        label=seg["label"], t0=seg["t0"], t1=seg["t1"], n=int(len(tf)),
        forward_extent_m=forward_extent, path_length_m=path_length,
        net_displacement_m=net, walk_length_m=walk_length_m,
        sensitivity_correction=correction, implied_vof_sens=new_vof,
        track_csv=track_csv, track_png=png if plotted else None,
    )


def _plot_track(x, y, path, title):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception:
        return False
    fig, ax = plt.subplots(figsize=(6, 6))
    ax.plot(y, x, "-", lw=1.2)
    ax.plot(y[0], x[0], "go", label="start")
    ax.plot(y[-1], x[-1], "rs", label="end")
    ax.set_xlabel("left y (m)")
    ax.set_ylabel("forward x (m)")
    ax.set_title("dead-reckoned track: %s" % title)
    ax.axis("equal")
    ax.grid(True, alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    plt.close(fig)
    return True


# --------------------------------------------------------------------------
# static analysis
# --------------------------------------------------------------------------

def analyze_static(flow, feats, rest_segments):
    if not rest_segments:
        return None
    idx = np.concatenate([np.arange(s["i0"], s["i1"]) for s in rest_segments])
    tau = feats["tau"][idx]
    integ = np.clip(flow["integration_s"][idx], 1e-3, None)
    speed = feats["speed"][idx]
    false_motion = float(np.mean(speed > 0.05))
    return dict(
        n=int(len(idx)),
        flow_rad_std=[float(np.std(flow["flow_rad_x"][idx])),
                      float(np.std(flow["flow_rad_y"][idx]))],
        tau_std_rad=[float(np.std(tau[:, 0])), float(np.std(tau[:, 1]))],
        false_motion_speed_mean=float(np.mean(speed)),
        false_motion_rate=false_motion,
        quality_mean=float(np.mean(flow["quality"][idx])),
        quality_min=float(np.min(flow["quality"][idx])),
        dist_mean_m=float(np.mean(flow["dist_m"][idx])),
        dist_std_m=float(np.std(flow["dist_m"][idx])),
        integration_mean_s=float(np.mean(integ)),
    )


# --------------------------------------------------------------------------
# driver
# --------------------------------------------------------------------------

def parse_overrides(items):
    out = []
    for item in items or []:
        parts = item.split(":")
        if len(parts) != 3:
            raise SystemExit("--segment expects label:t0:t1, got %r" % item)
        out.append(dict(label=parts[0], t0=float(parts[1]), t1=float(parts[2])))
    return out


def apply_overrides(flow, imu, overrides):
    segments = []
    for ov in overrides:
        tf = flow["t_s"]
        i0 = int(np.searchsorted(tf, ov["t0"]))
        i1 = int(np.searchsorted(tf, ov["t1"]))
        if i1 <= i0:
            continue
        name = ov["label"].lower()
        if any(k in name for k in ("rot", "roll", "pitch", "yaw", "spin")):
            kind = "rot"
        elif any(k in name for k in ("rest", "static", "still")):
            kind = "rest"
        else:
            kind = "trans"
        segments.append(dict(kind=kind, name=name, label=name,
                             t0=ov["t0"], t1=ov["t1"], i0=i0, i1=i1))
    return segments


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="interchange directory or MCAP log")
    ap.add_argument("--out-dir", default=None,
                    help="directory for the summary, track CSV and PNG "
                         "(default: the interchange directory)")
    ap.add_argument("--walk-length", type=float, default=None,
                    help="paced hallway length in metres for segment F")
    ap.add_argument("--walk-segment", default=None,
                    help="label of the segment to treat as the hallway walk "
                         "(default: the longest translation segment)")
    ap.add_argument("--segment", action="append", default=[],
                    help="explicit override label:t0:t1 (repeatable); when "
                         "given, auto segmentation is replaced")
    ap.add_argument("--rest-gyro", type=float, default=0.03,
                    help="rest gyro-rate threshold rad/s")
    ap.add_argument("--rest-speed", type=float, default=0.05,
                    help="rest flow-speed threshold m/s")
    ap.add_argument("--rot-gyro", type=float, default=0.15,
                    help="rotation gyro-rate threshold rad/s")
    ap.add_argument("--trans-speed", type=float, default=0.10,
                    help="translation flow-speed threshold m/s")
    ap.add_argument("--min-seg", type=float, default=3.0,
                    help="minimum segment duration s")
    ap.add_argument("--smooth", type=float, default=0.5,
                    help="feature smoothing window s for segmentation")
    ap.add_argument("--bridge", type=float, default=1.0,
                    help="gap-close radius s so an oscillating rotation stays "
                         "one segment")
    ap.add_argument("--lag-min-ms", type=float, default=-200.0)
    ap.add_argument("--lag-max-ms", type=float, default=200.0)
    ap.add_argument("--lag-step-ms", type=float, default=5.0)
    args = ap.parse_args()

    tool_dir = os.path.dirname(os.path.abspath(__file__))
    interchange = ensure_interchange(args.input, tool_dir)
    out_dir = args.out_dir or interchange
    os.makedirs(out_dir, exist_ok=True)

    imu_path = os.path.join(interchange, "imu.csv")
    raw_path = os.path.join(interchange, "flow_raw.csv")
    if not os.path.isfile(imu_path):
        raise SystemExit("no imu.csv in %s" % interchange)
    if not os.path.isfile(raw_path):
        print("ERROR: no flow_raw.csv in %s: the node did not send the raw "
              "OpticalFlowData topic (STREAM_OPTICAL_RAW). Enable the raw "
              "channel and re-record." % interchange, file=sys.stderr)
        sys.exit(2)

    imu = load_named(imu_path)
    flow = load_named(raw_path)
    missing = [c for c in RAW_COLUMNS if c not in flow]
    if missing:
        raise SystemExit("flow_raw.csv missing columns: %s" % missing)

    cfg = dict(rest_gyro=args.rest_gyro, rest_speed=args.rest_speed,
               rot_gyro=args.rot_gyro, trans_speed=args.trans_speed,
               min_seg_s=args.min_seg, smooth_s=args.smooth,
               bridge_s=args.bridge)

    segments, feats = segment_log(imu, flow, cfg)
    if args.segment:
        overrides = apply_overrides(flow, imu, parse_overrides(args.segment))
        segments = overrides
    label_segments(segments)

    lag_grid = np.arange(args.lag_min_ms, args.lag_max_ms + 1e-6,
                         args.lag_step_ms) * 1e-3

    lines = []

    def emit(text=""):
        print(text)
        lines.append(text)

    emit("flow_bench: raw optical-flow check against IMU")
    emit("interchange: %s" % interchange)
    emit("flow_raw samples: %d over %.1f s (%.1f Hz)" % (
        len(flow["t_s"]), flow["t_s"][-1] - flow["t_s"][0],
        len(flow["t_s"]) / max(flow["t_s"][-1] - flow["t_s"][0], 1e-6)))
    emit("quality range %d..%d, distance %.2f..%.2f m" % (
        int(np.min(flow["quality"])), int(np.max(flow["quality"])),
        float(np.min(flow["dist_m"])), float(np.max(flow["dist_m"]))))
    emit("")
    emit("timeline (%d segments):" % len(segments))
    emit("  %-16s %8s %8s %8s" % ("label", "t0_s", "t1_s", "dur_s"))
    for seg in segments:
        emit("  %-16s %8.2f %8.2f %8.2f" % (
            seg["label"], seg["t0"], seg["t1"], seg["t1"] - seg["t0"]))
    emit("")

    summary = dict(interchange=interchange,
                   flow_samples=int(len(flow["t_s"])),
                   segments=[dict(label=s["label"], kind=s["kind"],
                                  t0=s["t0"], t1=s["t1"]) for s in segments])

    rot_segments = [s for s in segments if s["kind"] == "rot"]
    trans_segments = [s for s in segments if s["kind"] == "trans"]
    rest_segments = [s for s in segments if s["kind"] == "rest"]

    # Pick the hallway-walk segment first so it can be held out of the map fit.
    walk_seg = None
    if args.walk_segment:
        for seg in segments:
            if seg["label"] == args.walk_segment:
                walk_seg = seg
    elif trans_segments:
        walk_seg = max(trans_segments, key=lambda s: s["t1"] - s["t0"])
    walk_label = walk_seg["label"] if walk_seg is not None else None

    # Rotation.
    summary["rotation"] = []
    if rot_segments:
        emit("rotation segments (node delta and flow angle vs FC gyro):")
        for seg in rot_segments:
            r = analyze_rotation(imu, flow, seg, lag_grid)
            summary["rotation"].append(r)
            emit("  %s [%s]  lag %.0f ms  n=%d" % (
                r["label"], r["axis"], r["lag_ms"], r["n"]))
            emit("    node gyro gain on %s = %.3f (ideal 1)" % (
                r["axis"], r["node_gain_axis"]))
            if r["comp_gain"] is not None:
                emit("    flow-vs-gyro gain on %s = %.3f (ideal 1), "
                     "cross-axis = %.3f" %
                     (r["axis"], r["comp_gain"], r["cross_gain"]))
            emit("    residual before comp %.5f rad, after comp %.5f rad" % (
                r["resid_before_rad"], r["resid_after_rad"]))
            emit("    verdict: %s" % r["compensation_verdict"])
        emit("")

    # Translation (the constant-velocity walk is held out of the map fit).
    if trans_segments:
        exclude = (walk_label,) if walk_label else ()
        t = analyze_translation(imu, flow, feats, trans_segments, lag_grid,
                                exclude_labels=exclude)
        summary["translation"] = t
        if t:
            emit("translation axis map (flow tau vs accel body velocity):")
            emit("  lag %.0f ms  n=%d" % (t["lag_ms"], t["n"]))
            emit("  best map: %s" % t["best"]["describe"])
            emit("  scale %.4f  resid %.5f  det %.0f  matches convention: %s" % (
                t["best"]["scale"], t["best"]["resid"], t["best"]["det"],
                t["matches_convention"]))
            if t["reflection"]:
                emit("  REFLECTION detected (mirrored axis, det = -1)")
            emit("  all candidates (resid ascending):")
            for c in t["ranking"]:
                emit("    resid %.5f  scale %+.4f  det %+.0f  %s" % (
                    c["resid"], c["scale"], c["det"], c["describe"]))
            emit("")
    else:
        summary["translation"] = None

    # Walk.
    summary["walk"] = None
    if walk_seg is not None:
        w = analyze_walk(imu, flow, feats, walk_seg, args.walk_length, out_dir)
        summary["walk"] = w
        emit("hallway walk (%s):" % w["label"])
        emit("  forward extent %.3f m, path length %.3f m, net %.3f m" % (
            w["forward_extent_m"], w["path_length_m"], w["net_displacement_m"]))
        if w["sensitivity_correction"] is not None:
            emit("  paced length %.3f m -> sensitivity correction %.4f, "
                 "implied VOF_SENS %.2f counts/rad (baseline %.2f)" % (
                     w["walk_length_m"], w["sensitivity_correction"],
                     w["implied_vof_sens"], VOF_SENS_COUNTS_PER_RAD))
        else:
            emit("  no --walk-length given: scale not resolved")
        emit("  track CSV %s" % w["track_csv"])
        if w["track_png"]:
            emit("  track PNG %s" % w["track_png"])
        emit("")

    # Static.
    s = analyze_static(flow, feats, rest_segments)
    summary["static"] = s
    if s:
        emit("static rest (%d samples):" % s["n"])
        emit("  flow_rad std (x,y) = %.6f %.6f rad" % tuple(s["flow_rad_std"]))
        emit("  false-motion speed mean %.4f m/s, rate above 0.05 m/s = %.1f%%" %
             (s["false_motion_speed_mean"], 100.0 * s["false_motion_rate"]))
        emit("  quality mean %.1f (min %.0f), distance %.3f +/- %.3f m" % (
            s["quality_mean"], s["quality_min"], s["dist_mean_m"],
            s["dist_std_m"]))
        emit("")

    summary_path = os.path.join(out_dir, "flow_bench_summary.json")
    with open(summary_path, "w") as handle:
        json.dump(summary, handle, indent=1)
    report_path = os.path.join(out_dir, "flow_bench_report.txt")
    with open(report_path, "w") as handle:
        handle.write("\n".join(lines) + "\n")
    print("summary JSON: %s" % summary_path)
    print("text report: %s" % report_path)


if __name__ == "__main__":
    main()
