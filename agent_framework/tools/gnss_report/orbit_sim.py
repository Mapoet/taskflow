"""
北斗GEO卫星掩星观测窗口分析
基于简化轨道模型的模拟计算
"""
import math
import json
from datetime import datetime, timedelta

# ============ 常量 ============
MU = 3.986004418e14  # 地球引力常数 m^3/s^2
RE = 6378.137e3       # 地球赤道半径 m
OMEGA_E = 7.292115e-5 # 地球自转角速度 rad/s

# ============ 时间基准 ============
now = datetime(2026, 8, 18, 14, 55, 12)
base_t = now - timedelta(days=3)  # 2026-08-15T14:55:12Z
window_hours = 6
end_t = base_t + timedelta(hours=window_hours)

def to_jd(dt):
    y = dt.year
    m = dt.month
    d = dt.day + dt.hour/24.0 + dt.minute/1440.0 + dt.second/86400.0
    if m <= 2:
        y -= 1
        m += 12
    A = math.floor(y/100)
    B = 2 - A + math.floor(A/4)
    JD = math.floor(365.25*(y+4716)) + math.floor(30.6001*(m+1)) + d + B - 1524.5
    return JD

def mjd_of(dt):
    return to_jd(dt) - 2400000.5

# ============ 地面站 ============
STATION = {"name": "Sanya", "lon": 109.5, "lat": 18.25, "alt": 30.0}

# ============ GEO 卫星 C01 ============
GEO_LON = 140.0  # 东经
GEO_ALT = 35786.0e3
GEO_R = RE + GEO_ALT

def geo_ecef(geo_lon=GEO_LON):
    lon_rad = math.radians(geo_lon)
    return (GEO_R * math.cos(lon_rad), GEO_R * math.sin(lon_rad), 0.0)

# ============ LEO 卫星 C2E1 ============
LEO_ALT = 817.0e3
LEO_R = RE + LEO_ALT
LEO_INCL = math.radians(98.7)
LEO_PERIOD = 2*math.pi*math.sqrt(LEO_R**3/MU)
print(f"LEO轨道周期: {LEO_PERIOD/60:.2f} 分钟")

def leo_position_eci(dt, t_epoch, raan0, phase0):
    t_sec = (dt - t_epoch).total_seconds()
    n = 2*math.pi/LEO_PERIOD
    M = phase0 + n*t_sec
    x_orbit = LEO_R * math.cos(M)
    y_orbit = LEO_R * math.sin(M)
    y2 = y_orbit*math.cos(LEO_INCL)
    z2 = y_orbit*math.sin(LEO_INCL)
    x3 = x_orbit*math.cos(raan0) - y2*math.sin(raan0)
    y3 = x_orbit*math.sin(raan0) + y2*math.cos(raan0)
    z3 = z2
    return (x3, y3, z3)

def eci_to_ecef(dt, x, y, z):
    jd = to_jd(dt)
    T = (jd - 2451545.0)/36525.0
    gmst = 280.46061837 + 360.98564736629*(jd - 2451545.0) + 0.000387933*T*T
    gmst_rad = math.radians(gmst % 360.0)
    x_e = x*math.cos(gmst_rad) + y*math.sin(gmst_rad)
    y_e = -x*math.sin(gmst_rad) + y*math.cos(gmst_rad)
    return (x_e, y_e, z)

def station_ecef(lon, lat, alt):
    lon_r = math.radians(lon)
    lat_r = math.radians(lat)
    e2 = 0.00669437999014
    N = RE / math.sqrt(1 - e2*math.sin(lat_r)**2)
    x = (N + alt)*math.cos(lat_r)*math.cos(lon_r)
    y = (N + alt)*math.cos(lat_r)*math.sin(lon_r)
    z = (N*(1-e2) + alt)*math.sin(lat_r)
    return (x, y, z)

def elevation_azimuth(sat_ecef, st_ecef, ref_lon, ref_lat):
    dx = sat_ecef[0] - st_ecef[0]
    dy = sat_ecef[1] - st_ecef[1]
    dz = sat_ecef[2] - st_ecef[2]
    lon = math.radians(ref_lon)
    lat = math.radians(ref_lat)
    e = -math.sin(lon)*dx + math.cos(lon)*dy
    n = -math.sin(lat)*math.cos(lon)*dx - math.sin(lat)*math.sin(lon)*dy + math.cos(lat)*dz
    u = math.cos(lat)*math.cos(lon)*dx + math.cos(lat)*math.sin(lon)*dy + math.sin(lat)*dz
    dist = math.sqrt(e*e + n*n + u*u)
    elev = math.degrees(math.asin(u/dist))
    az = math.degrees(math.atan2(e, n))
    if az < 0:
        az += 360
    return elev, az, dist

def occultation_angle(geo, leo, st):
    v1 = [geo[i]-st[i] for i in range(3)]
    v2 = [leo[i]-st[i] for i in range(3)]
    dot = sum(v1[i]*v2[i] for i in range(3))
    m1 = math.sqrt(sum(v1[i]**2 for i in range(3)))
    m2 = math.sqrt(sum(v2[i]**2 for i in range(3)))
    cos_a = max(-1.0, min(1.0, dot/(m1*m2)))
    return math.degrees(math.acos(cos_a))

# ============ 主模拟 ============
st_ecef = station_ecef(STATION["lon"], STATION["lat"], STATION["alt"])
geo_e = geo_ecef()
print(f"GEO C01 ECEF: {geo_e}")

t0 = base_t
raan0 = math.radians(30.0)
phase0 = 0.0

step_sec = 60.0
n_steps = int(window_hours * 3600 / step_sec)

print(f"\n模拟窗口: {base_t.strftime('%Y-%m-%dT%H:%M:%SZ')} 至 {end_t.strftime('%Y-%m-%dT%H:%M:%SZ')}")
print(f"模拟步数: {n_steps}")

leo_visibility = []
occultation_events = []
min_angle = 999.0
min_angle_time = None

for i in range(n_steps):
    t_i = base_t + timedelta(seconds=i*step_sec)
    leo_eci = leo_position_eci(t_i, t0, raan0, phase0)
    leo_ec = eci_to_ecef(t_i, *leo_eci)
    
    elev, az, dist = elevation_azimuth(leo_ec, st_ecef, STATION["lon"], STATION["lat"])
    geo_elev, geo_az, _ = elevation_azimuth(geo_e, st_ecef, STATION["lon"], STATION["lat"])
    occ_angle = occultation_angle(geo_e, leo_ec, st_ecef)
    
    if occ_angle < min_angle:
        min_angle = occ_angle
        min_angle_time = t_i
    
    if elev > 10.0:
        leo_visibility.append({
            "time": t_i.strftime("%H:%M:%S"),
            "elev": round(elev, 2),
            "az": round(az, 2),
            "dist_km": round(dist/1000, 1),
            "occ_angle": round(occ_angle, 2)
        })
    
    if occ_angle < 2.0 and elev > 5.0:
        occultation_events.append({
            "time": t_i.strftime("%H:%M:%S"),
            "occ_angle": round(occ_angle, 3),
            "leo_elev": round(elev, 2),
            "leo_az": round(az, 2),
            "leo_dist_km": round(dist/1000, 1),
            "geo_elev": round(geo_elev, 2)
        })

print(f"\n最小掩星夹角: {min_angle:.3f}° at {min_angle_time.strftime('%H:%M:%S')}")
print(f"LEO可见样本数: {len(leo_visibility)}")
print(f"掩星事件数: {len(occultation_events)}")

if occultation_events:
    print("\n=== 掩星事件列表 ===")
    for ev in occultation_events:
        print(f"  {ev['time']} 夹角={ev['occ_angle']}° LEO仰角={ev['leo_elev']}°")

result = {
    "base_time": base_t.strftime("%Y-%m-%dT%H:%M:%SZ"),
    "end_time": end_t.strftime("%Y-%m-%dT%H:%M:%SZ"),
    "mjd_start": mjd_of(base_t),
    "mjd_end": mjd_of(end_t),
    "gps_week": 2431,
    "gps_seconds": 572112.0,
    "station": STATION,
    "geo_sat": {"id": "C01", "orbit": "GEO", "lon": GEO_LON, "alt_km": GEO_ALT/1e3},
    "leo_sat": {"id": "C2E1", "orbit": "LEO", "alt_km": LEO_ALT/1e3, "incl": 98.7, "period_min": round(LEO_PERIOD/60,1)},
    "min_occultation_angle": min_angle,
    "min_occultation_time": min_angle_time.strftime("%H:%M:%S"),
    "leo_visibility_count": len(leo_visibility),
    "occultation_events": occultation_events,
    "leo_visibility_sample": leo_visibility[:24]
}

with open('/workspace/gnss_report/sim_results.json', 'w') as f:
    json.dump(result, f, indent=2, ensure_ascii=False)

print("\n结果已保存到 sim_results.json")
