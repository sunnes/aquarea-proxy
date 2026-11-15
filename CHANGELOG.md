# Changelog

Each release has a section named after its version tag, for example `## v0.1.0`.

## v0.1.0

First public release.

- In-line proxy between an Aquarea heat pump and its remote panel, forwarding both directions unchanged
- Register decoding with a live telnet log (changes, writes, anomalies)
- Prometheus metrics, Grafana dashboard and alert rules
- HTTP API and CLI: state, power, operating mode, reset error
- Whitelisted temperature settings (heating curve, heat curve shift, tank and cooling targets) with limits
- Secrets kept out of the repository: `secrets.env` injected at build time, `gitleaks` guard
