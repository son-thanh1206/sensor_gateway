# Sensor gateway

## Data manager (Step 5)

- Valid sensors are read once at startup from `room_sensor.map` (format of
  assignment section 3.6) in the working directory. A missing or malformed
  file stops the gateway with `Gateway shutting down (init error)`.
- An unknown sensor ID, or a known ID with the wrong sensor type, is logged
  as `Received sensor data with invalid sensor node ID <id>` **once per ID**;
  later frames from that ID are dropped silently so `gateway.log` is not
  flooded.
- Alerts are logged only when a sensor enters an alert state (too cold/hot,
  too dry/humid, smoke/gas alarm), not on every measurement. Returning to
  normal is not logged. Door sensors log each open/close change, motion
  sensors every detection.
- Unit tests: `make test-datamgr`.
