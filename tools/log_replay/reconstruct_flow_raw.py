#!/usr/bin/env python3
"""Reconstruct an APPROXIMATE raw optical-flow interchange (flow_raw.csv).

Some flight logs carry only the flow node's derived velocity topic
(optical_flow_vel) and not its raw product (optical_flow / OpticalFlowData).
This helper synthesizes a flow_raw.csv from the velocity messages plus the
flight controller IMU so the tightly coupled raw pipeline (replay --flow-raw,
flow_calibrate.py) can be exercised on such a log.

The reconstruction is APPROXIMATE and is only for exercising the pipeline, not
for metric calibration. It:

  - reuses the flight controller IMU (control_imu, via the interchange
    imu.csv) instead of the flow node's own ICM45686, so the integrated delta
    angle is the FC gyro, not the sensor gyro the node actually differenced;
  - inverts the node's tilt and range scaling, which already discarded
    structure (the node collapsed the 2-D flow field to a planar velocity and
    threw away the per-axis measurement geometry), so the recovered flow angles
    carry only what survived that collapse;
  - assumes the integration window equals the velocity inter-sample interval
    (with a nominal fallback), which is not the sensor's true exposure window.

Per velocity message at time t with body FLU velocity (vx forward, vy left),
distance d, roll r, pitch p and flags:

  dt          = interval to the previous velocity message (fallback nominal).
  v_perp      = in-plane velocity with the node tilt compensation removed:
                if TiltCompensated: v_perp_left = vy / cos(r),
                                    v_perp_fwd  = (vx - sin(p)*sin(r)*v_perp_left) / cos(p)
                else:               v_perp      = (vx, vy).
  tau_y       = v_perp_fwd * dt / d      (translation flow, forward)
  tau_x       = -v_perp_left * dt / d    (translation flow, left)
  delta_angle = integral of the FC gyro (body FLU) over [t - dt, t].
  flow_rad    = tau + delta_angle[x, y]  (node body-sense: flow_rad - delta = tau).

The emitted flags are FlowValid|DeltaAngleValid|DistanceValid (7) and the
distance quality mirrors the flow quality.
"""
import argparse
import os

import numpy as np

from mcap_to_interchange import LAYOUTS, WIRE_LATENCY_NS, read_log

NOMINAL_WINDOW_S = 0.029


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log', help='synapse/1 MCAP flight log carrying optical_flow_vel')
    ap.add_argument('interchange_dir', help='interchange directory containing imu.csv (FC gyro)')
    ap.add_argument('output', help='output flow_raw.csv path')
    args = ap.parse_args()

    log = read_log(args.log)
    if 'optical_flow_vel' not in log:
        raise SystemExit('log has no optical_flow_vel topic to reconstruct from')
    if 'control_imu' not in log:
        raise SystemExit('log has no control_imu topic')

    imu, _ = log['control_imu']
    flow, flow_lt = log['optical_flow_vel']
    tr = log.get('time_reference')
    t0 = int(imu['ts'][0])

    if tr is not None:
        tr_arr, _ = tr
        sync = tr_arr['time_status'] == 1
        tr_ts = tr_arr['ts'][sync].astype(np.int64)
        offset = tr_arr['unix_ns'][sync].astype(np.int64) - tr_ts
    else:
        tr_ts = np.array([], dtype=np.int64)
        offset = np.array([], dtype=np.int64)

    def wire_to_boot(ts, logt, status):
        ts = ts.astype(np.int64); logt = logt.astype(np.int64); res = logt - WIRE_LATENCY_NS
        m = status == 1
        if m.any() and len(tr_ts):
            idx = np.searchsorted(tr_ts, logt[m]).clip(0, len(tr_ts) - 1)
            res[m] = ts[m] - offset[idx]
        return res

    tf = (wire_to_boot(flow['ts'], flow_lt, flow['time_status']) - t0) / 1e9

    # Dense FC gyro for the delta-angle integral. Prefer the interchange
    # imu.csv (gap filled), fall back to the raw control_imu stream.
    imu_csv = os.path.join(args.interchange_dir, 'imu.csv')
    if os.path.exists(imu_csv):
        cols = np.loadtxt(imu_csv, delimiter=',', skiprows=1)
        ti = cols[:, 0]
        gx = cols[:, 1]; gy = cols[:, 2]; gz = cols[:, 3]
    else:
        ti = (imu['ts'].astype(np.int64) - t0) / 1e9
        gx = imu['gx']; gy = imu['gy']; gz = imu['gz']

    order = np.argsort(ti)
    ti = ti[order]; gx = gx[order]; gy = gy[order]; gz = gz[order]
    # Cumulative angle integrals so a window integral is a difference.
    cix = np.concatenate(([0.0], np.cumsum(0.5 * (gx[1:] + gx[:-1]) * np.diff(ti))))
    ciy = np.concatenate(([0.0], np.cumsum(0.5 * (gy[1:] + gy[:-1]) * np.diff(ti))))
    ciz = np.concatenate(([0.0], np.cumsum(0.5 * (gz[1:] + gz[:-1]) * np.diff(ti))))

    def window_delta(t_end, dt):
        t_beg = t_end - dt
        return (
            np.interp(t_end, ti, cix) - np.interp(t_beg, ti, cix),
            np.interp(t_end, ti, ciy) - np.interp(t_beg, ti, ciy),
            np.interp(t_end, ti, ciz) - np.interp(t_beg, ti, ciz),
        )

    dts = np.diff(tf, prepend=tf[0] - NOMINAL_WINDOW_S)
    dts[dts <= 0] = NOMINAL_WINDOW_S

    tilt_compensated = 1 << 1  # RDD2_OPTICAL_FLOW_TILT_COMPENSATED

    rows = []
    for i in range(len(tf)):
        t = tf[i]; dt = float(dts[i])
        vx = float(flow['vx'][i]); vy = float(flow['vy'][i])
        d = float(flow['dist'][i])
        r = float(flow['roll'][i]); p = float(flow['pitch'][i])
        flags = int(flow['flags'][i])
        quality = int(flow['quality'][i])
        if d <= 1e-3:
            continue
        if flags & tilt_compensated:
            cr = np.cos(r); cp = np.cos(p)
            v_perp_left = vy / cr if abs(cr) > 1e-6 else vy
            v_perp_fwd = (vx - np.sin(p) * np.sin(r) * v_perp_left) / cp if abs(cp) > 1e-6 else vx
        else:
            v_perp_left = vy
            v_perp_fwd = vx
        tau_y = v_perp_fwd * dt / d
        tau_x = -v_perp_left * dt / d
        da_x, da_y, da_z = window_delta(t, dt)
        flow_x = tau_x + da_x
        flow_y = tau_y + da_y
        rows.append((t, flow_x, flow_y, da_x, da_y, da_z, dt, d, quality, quality, 7))

    arr = np.array(rows, dtype=float)
    header = ('t_s,flow_rad_x,flow_rad_y,delta_angle_x,delta_angle_y,delta_angle_z,'
              'integration_s,dist_m,dist_quality,quality,flags')
    banner = ('# APPROXIMATE reconstruction for pipeline exercise only, not a metric\n'
              '# calibration source. Built from the optical_flow_vel topic plus the FC\n'
              '# gyro (control_imu), inverting the node tilt/range scaling and assuming\n'
              '# the integration window equals the velocity inter-sample interval. The\n'
              '# delta angle is the flight controller IMU, not the flow node ICM45686.')
    with open(args.output, 'w') as handle:
        handle.write(banner + '\n')
        np.savetxt(handle, arr, delimiter=',',
                   fmt=['%.6f', '%.7f', '%.7f', '%.7f', '%.7f', '%.7f', '%.6f', '%.4f', '%d', '%d', '%d'],
                   header=header, comments='')
    print('reconstructed flow_raw rows: %d -> %s' % (len(arr), args.output))


if __name__ == '__main__':
    main()
