# Monitoring the Aquarea proxy with Prometheus and Grafana

The proxy serves Prometheus metrics at `http://<proxy-ip>/metrics` (Basic auth, user `aquarea`,
password = `DEVICE_PASSWORD` from `secrets.env`). Quick look from a terminal:

```bash
scripts/aquarea.sh metrics               # everything
scripts/aquarea.sh metrics temperature   # only the lines containing "temperature"
```

## 1. Prometheus

First set the proxy address in [prometheus.yml](prometheus.yml) (`targets`) and give Prometheus the
password through a gitignored file (it is never written into the config):

```bash
sed -n 's/^DEVICE_PASSWORD=//p' ../secrets.env | tr -d '\n' > monitoring/aquarea_password   # from the repo root
```

Already have one? Copy the `aquarea` job from [prometheus.yml](prometheus.yml) into your config
and reload. Otherwise run the one in [docker-compose.yml](docker-compose.yml):

```bash
cd monitoring && docker compose up -d      # Prometheus on http://localhost:9090
```

Check **Status > Targets**: the `aquarea` job must be `UP`. Query `aquarea_outdoor_temperature_celsius`
to see the first value. Optional alert rules: [alerts.yml](alerts.yml) (`rule_files: ["alerts.yml"]`).

## 2. Grafana data source

In Grafana: **Connections > Data sources > Add > Prometheus**, URL = where Grafana reaches Prometheus
(for example `http://prometheus:9090`). With file provisioning use
[grafana-datasource.yml](grafana-datasource.yml).

## 3. Import the dashboard

**Dashboards > New > Import > Upload JSON file** and choose
[aquarea-dashboard.json](aquarea-dashboard.json). The `Data source` and `Job` drop-downs at the top pick
your Prometheus and the `aquarea` job. For file provisioning, copy the JSON into the folder of your
dashboards provider (as with your other dashboards).

## Metrics

| Metric | Meaning |
|---|---|
| `aquarea_power_on`, `aquarea_mode_info{mode}`, `aquarea_status_flags` | on/off, operating mode, raw flags |
| `aquarea_{outdoor,flow,return,tank}_temperature_celsius` | temperatures |
| `aquarea_compressor_frequency_hertz`, `aquarea_pump_speed_level`, `aquarea_defrost_active` | heat pump activity |
| `aquarea_power_watts{circuit}` | electric power: heating, cooling, tank |
| `aquarea_error_code{kind}` | current / last error (0 = none, 156 = H76, 38 = H72) |
| `aquarea_setting_celsius{setting}` | settings stored in the heat pump (tank target, cool set, ...) |
| `aquarea_register_raw{register,name}` | every register seen, raw (also the not-yet-understood ones) |
| `aquarea_bus_*_total` | bus health counters: bytes, valid frames, forwarded bytes, NAK, no reply, unmatched, overflow |
| `aquarea_data_age_seconds` | age of the freshest value; large = no traffic from the heat pump |
| `aquarea_uptime_seconds`, `aquarea_free_heap_bytes`, `aquarea_wifi_rssi_dbm` | the proxy itself |

Notes:
- Values change slowly and the panel polls each register about every 4 s, so a 15 s scrape interval is plenty.
- The counters restart from 0 when the proxy reboots; Prometheus handles that in `rate()` and `increase()`.
- A scrape takes a few milliseconds of the proxy's time; the bus keeps being served between the chunks of the answer.
