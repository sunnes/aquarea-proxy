# Changelog

## v0.1.1

- Settings limits from the Panasonic service manual (WH-SDC03/05E3E5): heating curve outdoor points
  -15..15 C, flow temperatures 25..55 C, backup heater -15..20 C, cooling 5..20 C
- Heating curve writes that would fold the curve are refused (low outdoor point must stay below the high
  one, the colder point must not get the cooler flow)
- Timing guards: power ON waits 3 minutes after a power OFF, mode changes while running are 3 minutes
  apart, error reset at most once per 30 minutes (H76 exempt)
- Refused commands answer HTTP 409 with `"refused":true`; the CLI shows them as REFUSED

## v0.1.0

First public release.

- In-line proxy between an Aquarea heat pump and its remote panel, forwarding both directions unchanged
- Register decoding with a live telnet log (changes, writes, anomalies)
- Prometheus metrics, Grafana dashboard and alert rules
- HTTP API and CLI: state, power, operating mode, reset error
- Whitelisted temperature settings (heating curve, heat curve shift, tank and cooling targets) with limits
- Secrets kept out of the repository: `secrets.env` injected at build time, `gitleaks` guard
