#!/usr/bin/env python3
"""Calibrate the raw optical-flow node against a GPS-aided navigation estimate.

Given a raw flow interchange file (flow_raw.csv, the columns emitted by
mcap_to_interchange.py) and an estimate.csv produced by ./replay with GPS
aiding enabled and flow disabled, this fits the flow node's mount and
sensitivity:

  (a) a scalar sensitivity scale applied to flow_rad,
  (b) the mount yaw among {0, 90, 180, 270} degrees,
  (c) a sign flip.

Predicted body velocity from flow (matching navigation_optical_flow_raw.c and
the ESKF model):

    comp = R_yaw(scale * sign * flow_rad) - R_yaw(delta_angle_xy)
    v_forward = range * comp_y / dt
    v_left    = -range * comp_x / dt

with R_yaw the mount rotation from carrier FLU to vehicle FLU, dt the
integration time and range the ground distance. The reference body velocity
comes from the GPS-aided estimate: the estimate's world ENU velocity rotated
into body FLU through the estimate attitude (quaternionWorldBody), sampled at
each flow time.

For each of the four yaws and two signs the sensitivity scale is fit by least
squares over both body-velocity axes; the combination with the smallest RMS
residual and a positive scale is chosen. The velocity residual of the chosen
fit is then regressed on the body angular rate (delta_angle / dt) to expose a
rotation leak, in particular a roll-rate leak into the forward axis.

The reported VOF_SENS correction is multiplicative: the node's VOF_SENS is in
counts per radian, so a fitted flow scale s means the true flow is s times the
reported flow, hence new_VOF_SENS = old_VOF_SENS / s. The old value is not
recoverable from the log, so only the correction factor (1 / s) is reported.
"""
import argparse
import numpy as np

YAWS = (0, 90, 180, 270)


def load_named(path):
    """Read a CSV that may carry leading '#' comment lines before the header."""
    header = None
    body = []
    with open(path) as handle:
        for line in handle:
            if line.startswith('#') or not line.strip():
                continue
            if header is None:
                header = line.strip().split(',')
            else:
                body.append(line)
    if header is None:
        raise SystemExit('%s: no header row' % path)
    if not body:
        raise SystemExit('%s: no data rows' % path)
    data = np.array([[float(v) for v in row.split(',')] for row in body])
    return {name: data[:, i] for i, name in enumerate(header)}


def rotate_yaw(x, y, yaw_deg):
    """Rotate an in-plane vector from carrier FLU into vehicle FLU by the mount
    yaw, matching apply_mount_yaw in navigation_optical_flow_raw.c."""
    if yaw_deg == 90:
        return -y, x
    if yaw_deg == 180:
        return -x, -y
    if yaw_deg == 270:
        return y, -x
    return x, y


def world_enu_to_body_flu(v_enu, quat):
    """Rotate world ENU velocities into body FLU using quaternionWorldBody
    (Hamilton, body-to-world). Body velocity is R^T @ v_world."""
    w, x, y, z = quat[:, 0], quat[:, 1], quat[:, 2], quat[:, 3]
    # Rows of the world-from-body rotation matrix.
    r00 = 1 - 2 * (y * y + z * z); r01 = 2 * (x * y - w * z); r02 = 2 * (x * z + w * y)
    r10 = 2 * (x * y + w * z); r11 = 1 - 2 * (x * x + z * z); r12 = 2 * (y * z - w * x)
    r20 = 2 * (x * z - w * y); r21 = 2 * (y * z + w * x); r22 = 1 - 2 * (x * x + y * y)
    ve, vn, vu = v_enu[:, 0], v_enu[:, 1], v_enu[:, 2]
    # Body = R^T @ world, so body_x is the first column of R dotted with world.
    bx = r00 * ve + r10 * vn + r20 * vu
    by = r01 * ve + r11 * vn + r21 * vu
    bz = r02 * ve + r12 * vn + r22 * vu
    return np.column_stack((bx, by, bz))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('flow_raw', help='flow_raw.csv from mcap_to_interchange.py')
    ap.add_argument('estimate', help='estimate.csv from ./replay with GPS aiding, flow off')
    ap.add_argument('--min-speed', type=float, default=0.3,
                    help='only fit samples whose reference body speed exceeds this (m/s)')
    ap.add_argument('--min-quality', type=float, default=100.0,
                    help='minimum flow quality (raw 0-255 counts)')
    args = ap.parse_args()

    flow = load_named(args.flow_raw)
    est = load_named(args.estimate)

    tf = flow['t_s']
    te = est['t_s']

    # Match the estimate to each flow time by interpolation, restricted to the
    # estimate's valid, GPS-aided span.
    att_valid = est['att_valid'] > 0.5
    if not att_valid.any():
        raise SystemExit('estimate has no attitude-valid rows')
    te_v = te[att_valid]
    lo, hi = te_v[0], te_v[-1]

    ve = np.interp(tf, te, est['ve_m_s'])
    vn = np.interp(tf, te, est['vn_m_s'])
    vu = np.interp(tf, te, est['vu_m_s'])
    qw = np.interp(tf, te, est['qw']); qx = np.interp(tf, te, est['qx'])
    qy = np.interp(tf, te, est['qy']); qz = np.interp(tf, te, est['qz'])
    quat = np.column_stack((qw, qx, qy, qz))
    quat /= np.linalg.norm(quat, axis=1, keepdims=True)

    v_body = world_enu_to_body_flu(np.column_stack((ve, vn, vu)), quat)
    ref_speed = np.hypot(v_body[:, 0], v_body[:, 1])

    dt = flow['integration_s']
    dist = flow['dist_m']
    flags = flow['flags'].astype(int)
    quality = flow['quality']

    valid = (
        (tf >= lo) & (tf <= hi)
        & (dt > 1e-4) & (dist > 1e-3)
        & ((flags & 0x7) == 0x7)
        & (quality >= args.min_quality)
        & np.isfinite(v_body[:, 0]) & np.isfinite(v_body[:, 1])
    )
    fit = valid & (ref_speed >= args.min_speed)
    if fit.sum() < 10:
        raise SystemExit('too few overlapping samples to calibrate (%d)' % int(fit.sum()))

    fx = flow['flow_rad_x'][fit]; fy = flow['flow_rad_y'][fit]
    dax = flow['delta_angle_x'][fit]; day = flow['delta_angle_y'][fit]
    daz = flow['delta_angle_z'][fit]
    k = dist[fit] / dt[fit]
    dt_fit = dt[fit]
    vbx = v_body[fit, 0]; vby = v_body[fit, 1]

    best = None
    for yaw in YAWS:
        rfx, rfy = rotate_yaw(fx, fy, yaw)          # rotated flow
        rdx, rdy = rotate_yaw(dax, day, yaw)        # rotated delta angle
        # Predicted body velocity is linear in the signed scale s:
        #   pred_forward = s * (k*rfy) - k*rdy
        #   pred_left    = -s * (k*rfx) + k*rdx
        for sign in (+1.0, -1.0):
            a_fwd = sign * k * rfy
            b_fwd = -k * rdy
            a_left = -sign * k * rfx
            b_left = k * rdx
            a = np.concatenate((a_fwd, a_left))
            b = np.concatenate((b_fwd, b_left))
            target = np.concatenate((vbx, vby))
            denom = float(np.dot(a, a))
            if denom <= 0.0:
                continue
            scale = float(np.dot(a, target - b) / denom)
            if scale <= 0.0:
                # Wrong sign for this yaw; the paired sign covers it.
                continue
            resid = scale * a + b - target
            rms = float(np.sqrt(np.mean(resid ** 2)))
            cand = dict(yaw=yaw, sign=sign, scale=scale, rms=rms)
            if best is None or rms < best['rms']:
                best = cand

    if best is None:
        raise SystemExit('no positive-scale fit found')

    # Rotation-leak regression: residual of the chosen fit versus body rate.
    yaw = best['yaw']; sign = best['sign']; scale = best['scale']
    rfx, rfy = rotate_yaw(fx, fy, yaw)
    rdx, rdy = rotate_yaw(dax, day, yaw)
    comp_x = scale * sign * rfx - rdx
    comp_y = scale * sign * rfy - rdy
    pred_fwd = k * comp_y
    pred_left = -k * comp_x
    res_fwd = pred_fwd - vbx
    res_left = pred_left - vby

    # Body angular rate from the (unrotated) delta angle over the window.
    wx = dax / dt_fit; wy = day / dt_fit; wz = daz / dt_fit
    design = np.column_stack((wx, wy, wz, np.ones_like(wx)))
    coef_fwd, *_ = np.linalg.lstsq(design, res_fwd, rcond=None)
    coef_left, *_ = np.linalg.lstsq(design, res_left, rcond=None)

    yaw_config = {0: 'RDD2_OPTICAL_FLOW_RAW_MOUNT_YAW_0',
                  90: 'RDD2_OPTICAL_FLOW_RAW_MOUNT_YAW_90',
                  180: 'RDD2_OPTICAL_FLOW_RAW_MOUNT_YAW_180',
                  270: 'RDD2_OPTICAL_FLOW_RAW_MOUNT_YAW_270'}[yaw]

    print('flow calibration')
    print('  fit samples          : %d (of %d valid, speed >= %.2f m/s)'
          % (int(fit.sum()), int(valid.sum()), args.min_speed))
    print('  chosen mount yaw     : %d deg' % yaw)
    print('  chosen sign          : %+d' % int(sign))
    print('  fitted flow scale    : %.6f' % scale)
    print('  residual RMS         : %.4f m/s' % best['rms'])
    print('  per-yaw/sign search  :')
    for y in YAWS:
        rfx2, rfy2 = rotate_yaw(fx, fy, y)
        rdx2, rdy2 = rotate_yaw(dax, day, y)
        for s in (+1.0, -1.0):
            a = np.concatenate((s * k * rfy2, -s * k * rfx2))
            b = np.concatenate((-k * rdy2, k * rdx2))
            target = np.concatenate((vbx, vby))
            denom = float(np.dot(a, a))
            sc = float(np.dot(a, target - b) / denom) if denom > 0 else float('nan')
            resid = sc * a + b - target
            rms = float(np.sqrt(np.mean(resid ** 2)))
            mark = ' *' if (y == yaw and s == sign) else ''
            print('      yaw %3d sign %+d : scale %+9.5f  rms %.4f%s' % (y, int(s), sc, rms, mark))
    print('  rotation-leak coefficients (velocity residual = C . [wx wy wz 1], w = delta_angle/dt):')
    print('      forward axis (x) : wx %+.5f  wy %+.5f  wz %+.5f  bias %+.5f  [m/s per rad/s]'
          % (coef_fwd[0], coef_fwd[1], coef_fwd[2], coef_fwd[3]))
    print('      left axis    (y) : wx %+.5f  wy %+.5f  wz %+.5f  bias %+.5f  [m/s per rad/s]'
          % (coef_left[0], coef_left[1], coef_left[2], coef_left[3]))
    print('      roll-rate (wx) leak into forward axis : %+.5f m/s per rad/s' % coef_fwd[0])
    print('  VOF_SENS correction  : new_VOF_SENS = old_VOF_SENS / %.6f = old_VOF_SENS * %.6f'
          % (scale, 1.0 / scale))
    print('                         (old VOF_SENS is not recoverable from the log; apply the factor above)')
    print('  mount-yaw to set     : CONFIG_%s' % yaw_config)


if __name__ == '__main__':
    main()
