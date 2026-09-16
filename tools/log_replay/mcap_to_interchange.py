#!/usr/bin/env python3
"""Decode an RDD2 flight log (synapse/1 MCAP) into the replay interchange CSVs.

The writer is uncompressed, unchunked and index-less, so the file is walked
record by record without an MCAP library. Fixed-layout topic structs are read
straight out of their one-field FlatBuffer root tables using the layouts of
the synapse_fbs schema the log was recorded with.

Outputs (see README.md for the column contract):
  imu.csv, gps.csv, flow.csv, onboard.csv, origin.json

Wire topics (GNSS, optical flow) arrive from another node. While that node is
gPTP synced their timestamps are mapped onto the flight controller boot clock
through the logged TimeReference samples; before sync the logger accept time
minus the measured transport latency is used instead.

--fill-gaps bridges IMU dropouts (the logger drops frames at every session
rotation) by linear interpolation at the nominal 800 Hz spacing.
"""
import argparse, json, os, struct
import numpy as np
from scipy.signal import butter, filtfilt

from wire_replay import read_channels

LAYOUTS = {
    'control_imu': np.dtype([('ts', '<u8'), ('ax', '<f4'), ('ay', '<f4'), ('az', '<f4'), ('gx', '<f4'), ('gy', '<f4'), ('gz', '<f4'), ('temp', '<f4'), ('flags', 'u1'), ('time_status', 'u1'), ('id', 'u1'), ('pad', 'u1')]),
    'gnss_fix': np.dtype([('ts', '<u8'), ('unix_ns', '<u8'), ('lat_e7', '<i4'), ('lon_e7', '<i4'), ('alt_msl_mm', '<i4'), ('alt_ell_mm', '<i4'), ('hacc_mm', '<u2'), ('vacc_mm', '<u2'), ('sacc_mm_s', '<u2'), ('yawacc_cdeg', '<u2'), ('hdop', '<u2'), ('vdop', '<u2'), ('gspeed_cm_s', '<u2'), ('cog_cdeg', '<u2'), ('yaw_cdeg', '<u2'), ('vup_cm_s', '<i2'), ('flags', 'u1'), ('fix_type', 'u1'), ('sats_used', 'u1'), ('sats_vis', 'u1'), ('time_status', 'u1'), ('id', 'u1'), ('pad', 'V6')]),
    'optical_flow_vel': np.dtype([('ts', '<u8'), ('vx', '<f4'), ('vy', '<f4'), ('dist', '<f4'), ('roll', '<f4'), ('pitch', '<f4'), ('quality', 'u1'), ('flags', 'u1'), ('time_status', 'u1'), ('id', 'u1')]),
    'attitude_estimate': np.dtype([('ts', '<u8'), ('qw', '<f4'), ('qx', '<f4'), ('qy', '<f4'), ('qz', '<f4'), ('wx', '<f4'), ('wy', '<f4'), ('wz', '<f4'), ('flags', 'u1'), ('time_status', 'u1'), ('pad', 'V2')]),
    'time_reference': np.dtype([('ts', '<u8'), ('tai_ns', '<u8'), ('unix_ns', '<u8'), ('unc_ns', '<u4'), ('utc_off', '<i2'), ('time_status', 'u1'), ('clock_class', 'u1'), ('domain', 'u1'), ('id', 'u1'), ('pad', 'V6')]),
}
WIRE_LATENCY_NS = 2_000_000


def read_log(path):
    """Decode the fixed-layout topic streams of an MCAP log into typed arrays.

    The MCAP walk and FlatBuffer root extraction are shared with the wire
    replay through ``wire_replay.read_channels``; here each channel's raw
    payload bytes are reinterpreted through its ``LAYOUTS`` dtype and paired
    with the log accept times.
    """
    channels = read_channels(path, list(LAYOUTS))
    out = {}
    for name, dtype in LAYOUTS.items():
        frames = channels.get(name)
        if not frames:
            continue
        blob = b''.join(raw for _log_time, raw in frames)
        if len(blob) != len(frames) * dtype.itemsize:
            raise SystemExit(f'{name}: payload size {len(blob) / len(frames)} does not match layout {dtype.itemsize}')
        log_times = np.array([log_time for log_time, _raw in frames], dtype=np.int64)
        out[name] = (np.frombuffer(blob, dtype=dtype), log_times)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log'); ap.add_argument('output_dir'); ap.add_argument('--fill-gaps', action='store_true')
    args = ap.parse_args(); os.makedirs(args.output_dir, exist_ok=True); out = args.output_dir
    log = read_log(args.log)
    imu, _ = log['control_imu']; gps, gps_lt = log['gnss_fix']; flow, flow_lt = log['optical_flow_vel']
    att, _ = log['attitude_estimate']; tr, _ = log['time_reference']
    t0 = int(imu['ts'][0])
    sync = tr['time_status'] == 1
    tr_ts = tr['ts'][sync].astype(np.int64); offset = tr['unix_ns'][sync].astype(np.int64) - tr_ts

    def wire_to_boot(ts, logt, status):
        ts = ts.astype(np.int64); logt = logt.astype(np.int64); res = logt - WIRE_LATENCY_NS
        m = status == 1
        if m.any() and len(tr_ts):
            idx = np.searchsorted(tr_ts, logt[m]).clip(0, len(tr_ts) - 1); res[m] = ts[m] - offset[idx]
        return res

    ti = (imu['ts'].astype(np.int64) - t0) / 1e9
    rows = np.c_[ti, imu['gx'], imu['gy'], imu['gz'], imu['ax'], imu['ay'], imu['az']]
    if args.fill_gaps:
        nominal = 1.0 / 800.0; filled = [rows[0]]
        for k in range(1, len(rows)):
            dt = rows[k, 0] - rows[k - 1, 0]
            if dt > 3e-3:
                n = int(round(dt / nominal)) - 1
                for j in range(1, n + 1):
                    a = j / (n + 1); filled.append(rows[k - 1] * (1 - a) + rows[k] * a)
            filled.append(rows[k])
        rows = np.array(filled)
    np.savetxt(f'{out}/imu.csv', rows, delimiter=',', fmt=['%.6f'] + ['%.7f'] * 6, header='t_s,gx_rad_s,gy_rad_s,gz_rad_s,ax_m_s2,ay_m_s2,az_m_s2', comments='')

    tg = (wire_to_boot(gps['ts'], gps_lt, gps['time_status']) - t0) / 1e9
    lat = gps['lat_e7'] / 1e7; lon = gps['lon_e7'] / 1e7; alt = gps['alt_msl_mm'] / 1e3; alte = gps['alt_ell_mm'] / 1e3
    hacc = gps['hacc_mm'] / 1e3; vacc = gps['vacc_mm'] / 1e3; sacc = gps['sacc_mm_s'] / 1e3
    speed = gps['gspeed_cm_s'] / 100.0; cog = np.deg2rad(gps['cog_cdeg'] / 100.0)
    vn = speed * np.cos(cog); ve = speed * np.sin(cog)
    b, a = butter(2, 0.3 / (5.0 / 2)); vd = -np.gradient(filtfilt(b, a, alt), tg)
    pos_valid = (gps['fix_type'] >= 3) & (hacc <= 10.0) & (vacc <= 15.0)
    vel_valid = ((gps['flags'] & 2) != 0) & (sacc <= 5.0)
    first = int(np.flatnonzero(pos_valid)[0])
    origin = dict(lat_deg=float(lat[first]), lon_deg=float(lon[first]), alt_msl_m=float(alt[first]), t_s=float(tg[first]))
    json.dump(origin, open(f'{out}/origin.json', 'w'), indent=1)
    radius = 6378137.0; lat0 = np.deg2rad(origin['lat_deg'])
    e = np.deg2rad(lon - origin['lon_deg']) * radius * np.cos(lat0); n = np.deg2rad(lat - origin['lat_deg']) * radius; u = alt - origin['alt_msl_m']
    np.savetxt(f'{out}/gps.csv', np.c_[tg, lat, lon, alt, alte, vn, ve, vd, hacc, vacc, sacc, gps['fix_type'], gps['sats_used'], pos_valid, vel_valid, np.ones_like(tg), e, n, u], delimiter=',',
               fmt=['%.6f', '%.9f', '%.9f', '%.3f', '%.3f', '%.3f', '%.3f', '%.3f', '%.3f', '%.3f', '%.3f', '%d', '%d', '%d', '%d', '%d', '%.3f', '%.3f', '%.3f'],
               header='t_s,lat_deg,lon_deg,alt_msl_m,alt_ell_m,vn_m_s,ve_m_s,vd_m_s,hacc_m,vacc_m,sacc_m_s,fix_type,sats_used,pos_valid,vel_valid,vd_derived,e_m,n_m,u_m', comments='')
    tf = (wire_to_boot(flow['ts'], flow_lt, flow['time_status']) - t0) / 1e9
    np.savetxt(f'{out}/flow.csv', np.c_[tf, flow['vx'], flow['vy'], flow['dist'], flow['quality'] / 255.0, (flow['flags'] & 7) == 7], delimiter=',', fmt=['%.6f', '%.4f', '%.4f', '%.4f', '%.4f', '%d'], header='t_s,vx_flu_m_s,vy_flu_m_s,dist_m,quality,valid', comments='')
    ta = (att['ts'].astype(np.int64) - t0) / 1e9
    np.savetxt(f'{out}/onboard.csv', np.c_[ta, att['qw'], att['qx'], att['qy'], att['qz'], att['wx'], att['wy'], att['wz']], delimiter=',', fmt='%.6f', header='t_s,qw,qx,qy,qz,wx,wy,wz', comments='')
    print(f'imu {len(rows)} rows, gps {len(tg)} ({int(pos_valid.sum())} usable), flow {len(tf)}, origin {origin}')


if __name__ == '__main__':
    main()
