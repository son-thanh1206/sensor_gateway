# Sensor gateway

A Linux sensor gateway written in C. It receives measurements from many
sensor nodes over TCP, checks them against a list of known sensors, raises
alerts, stores every measurement in SQLite and logs system events through a
separate log process.

Five sensor types are supported (temperature, humidity, motion, door,
smoke/gas) over four wire protocols, each on its own TCP port.

## Architecture

```
 sensor nodes ──TCP──▶ ┌──────────────────── main process ───────────────────┐
  (port P..P+3)        │ Connection manager ──▶ sbuffer ──▶ Data manager     │
                       │   (poll, 4 adapters)        └────▶ Storage manager ─┼──▶ gateway.db
                       └──────────────┬──────────────────────────────────────┘
                                      │ logFifo (FIFO)
                                      ▼
                                 log process ──▶ gateway.log
```

| Component | Runs as | Role |
|---|---|---|
| Connection manager | thread | Listens on ports P..P+3, decodes frames with the protocol adapter of the port, writes `sensor_data_t` into the sbuffer. One thread serves all sockets with `poll()`. |
| sbuffer | shared queue | The single queue between the three threads (mutex + condition variable). A measurement is removed once both readers have read it. |
| Data manager | thread | Validates sensor ID and type against `room_sensor.map`, computes running averages, logs alerts. |
| Storage manager | thread | Inserts every measurement into SQLite; reconnects on failure. |
| Log process | child process (`fork`) | Reads `<SRC> <message>` lines from the FIFO and writes them to `gateway.log` with a sequence number and timestamp. |

## Requirements

- Linux, `gcc`, `make`
- SQLite development files: `sudo apt install libsqlite3-dev`
- Optional: `sqlite3` (to inspect the database), `valgrind` (memory and
  thread checks)

## Build and run

```bash
make                    # builds bin/server and bin/sensor_node
make debug              # rebuild with -DDEBUG -g (diagnostics on stderr)

./bin/server <TCP base port P> <HTTP port>
./bin/server 1234 8080  # or: make run PORT=1234 HTTP_PORT=8080
```

The gateway listens on four TCP ports, one per protocol:

| Port | Protocol | Sensors | Framing |
|---|---|---|---|
| P | TBP | temperature | fixed 18 bytes, binary, little-endian |
| P+1 | HLP | humidity | one ASCII line `HUM <id> <value> <ts>\n` |
| P+2 | EVP | motion, door | TLV, binary, big-endian |
| P+3 | SGP | smoke/gas | fixed 11 bytes, CRC-16, ACK/NAK reply |

Stop it with Ctrl+C (SIGINT) or SIGTERM.

Files used in the working directory:

| File | Purpose |
|---|---|
| `room_sensor.map` | Valid sensors, read once at startup (input) |
| `gateway.log` | Event log, recreated on each run |
| `logFifo` | FIFO between the gateway and the log process (created if missing) |
| `gateway.db` | SQLite database, table `SensorData(id, type, value, ts)` |

### Sensor simulator

```bash
./bin/sensor_node <type> <id> <host> <base port P> <period ms> [options]

./bin/sensor_node temperature 10 127.0.0.1 1234 1000
./bin/sensor_node smoke_gas   50 127.0.0.1 1234 2000 --corrupt
```

- `type`: `temperature | humidity | motion | door | smoke_gas`
- `--fragment`: split each frame over several `send()` calls
- `--glue`: pack several frames into one `send()`
- `--corrupt`: send a bad frame now and then (bad CRC, magic, length)

To start a whole set of sensors from a CSV file
([test/sensors.csv](test/sensors.csv), 17 sensors of every type):

```bash
make test-sensors                                 # until Ctrl+C
PORT=1234 ./bin/test_sensors test/sensors.csv 60  # for 60 seconds
```

### Configuration: `room_sensor.map`

One sensor per line: room, sensor ID, type. Lines starting with `#` are
comments. A room may have several sensors.

```
# room  sensor_id  type
501     10         temperature
501     20         humidity
602     40         door
```

At most 64 sensors (`MAX_SENSORS`). Alert thresholds are in
[include/sensor_config.h](include/sensor_config.h).

## Behaviour

**Connections**
- The sensor ID of a connection is known after its first valid frame; that
  is when `A sensor node with <id> has opened a new connection` is logged.
- A malformed first frame closes the connection. After that, 5 malformed
  frames in a row close it; a valid frame resets the count, so a noisy link
  stays connected.
- SGP: a resent frame (same ID, timestamp and value, sent again because its
  ACK was lost) is ACKed again but stored only once.
- The number of nodes is not fixed. The connection table grows as needed;
  the real limit is the descriptor limit (the soft `RLIMIT_NOFILE` is raised
  to the hard limit, 16 descriptors are kept for the gateway itself). A node
  beyond it is closed at once and logged as `too many connections`.

**Data processing**
- A missing or malformed `room_sensor.map` stops the gateway at startup.
- An unknown sensor ID, or a known ID with the wrong type, is logged as
  `Received sensor data with invalid sensor node ID <id>` once per ID; later
  frames from that ID are dropped silently.
- Temperature and humidity use the average of the last 5 measurements;
  smoke/gas is compared to its threshold immediately.
- Alerts are logged when a sensor enters an alert state, not on every
  measurement. Returning to normal is not logged. Door sensors log every
  open/close change, motion sensors every detection.

**Storage**
- Every measurement is stored, including those with an invalid sensor ID.
- When an INSERT fails (file renamed, locked by another process for more
  than 1 s, …) the storage manager logs `Connection to SQL server lost.`
  and keeps the pending measurements in the sbuffer. It retries 3 times,
  5 s apart. On success nothing is lost; after the third failure the
  gateway stops with `Gateway shutting down (SQL server unreachable)` and
  exit code 1.
- A reconnection never creates a new database file, so a moved file does not
  silently split the history into two databases.

**Shutdown**

On SIGINT/SIGTERM, or when the storage manager gives up:
1. the connection manager is woken up, closes every sensor connection and
   is joined first (it is the only writer of the sbuffer);
2. the sbuffer is shut down; the data and storage managers process every
   remaining measurement, then exit and are joined;
3. sockets are closed and memory freed;
4. the FIFO is closed, the log process writes the last lines and is reaped.

### Log format

```
<seq> <YYYY-MM-DD HH:MM:SS> <MAIN|CONN|DATA|STOR|WEB> <message>

1 2026-10-06 09:15:02 MAIN Gateway started (TCP base port 1234, HTTP port 8080)
2 2026-10-06 09:15:02 STOR Connection to SQL server established.
3 2026-10-06 09:15:02 STOR New table SensorData created.
4 2026-10-06 09:15:07 CONN A sensor node with 10 has opened a new connection
5 2026-10-06 09:15:31 DATA The sensor node with 10 reports it's too hot (running avg temperature = 21.4)
```

Each event is one `write()` of at most `PIPE_BUF` bytes to the FIFO, so lines
from concurrent threads never interleave.

## Testing

```bash
make test               # all unit tests
make test-sbuffer       # concurrent readers/writer on the sbuffer
make test-adapters      # each protocol parser, incl. the CRC-16 test vector
make test-datamgr       # room-sensor file, validation, alert handlers
make test-storagemgr    # SQLite connect, insert, lost connection

make soak                         # integration test, 180 s under valgrind
make soak SOAK_SECONDS=60
HELGRIND=1 make soak SOAK_SECONDS=60
```

`make soak` ([test/soak.sh](test/soak.sh)) runs the gateway under valgrind
with 117 sensor nodes, stops it with SIGINT and checks: exit code 0, no
memory leak or valgrind error, no descriptor left open, log process reaped,
`gateway.log` sequence and format, every connection closed, and as many rows
in the database as measurements received. A second round with `ulimit -n 32`
checks the descriptor limit. Output goes to `test/out/soak/`.

Manual checks with valgrind (run sensors from another terminal, then Ctrl+C):

```bash
make debug
valgrind --leak-check=full --track-fds=yes --log-file=valgrind.%p.txt bin/server 1234 8080
valgrind --tool=helgrind --log-file=helgrind.%p.txt bin/server 1234 8080
```

Valgrind writes one report per process (main and log process).

## Project layout

```
sensor_gateway/
├── Makefile                  build, run, test and soak targets
├── README.md
├── room_sensor.map           list of valid sensors (room, ID, type)
├── include/
│   ├── adapters.h            adapter instances and frame constants
│   ├── byteorder.h           read/write little/big-endian frame fields
│   ├── connmgr.h
│   ├── crc16.h               CRC-16/CCITT-FALSE (SGP)
│   ├── datamgr.h
│   ├── debug.h               DEBUG_PRINT, enabled by -DDEBUG
│   ├── gateway.h             data structures of all components
│   ├── logger.h
│   ├── protocol_adapter.h    adapter interface: parse(), build_reply()
│   ├── sbuffer.h
│   ├── sensor_config.h       alert thresholds, running average length
│   ├── sensor_types.h        sensor_data_t, sensor_type_t
│   └── storagemgr.h
├── src/
│   ├── main.c                startup, signals, shutdown order
│   ├── gateway.c             stop request and failure signalling between threads
│   ├── connmgr.c             connection manager: poll(), framing, ACK/NAK
│   ├── adapter/
│   │   ├── tbp_adapter.c     temperature, port P
│   │   ├── hlp_adapter.c     humidity, port P+1
│   │   ├── evp_adapter.c     motion and door, port P+2
│   │   ├── sgp_adapter.c     smoke/gas, port P+3
│   │   └── adapter_error.c   reason of the last malformed frame
│   ├── sbuffer.c             shared measurement queue
│   ├── datamgr.c             validation, running average, alerts
│   ├── storagemgr.c          SQLite storage and reconnection
│   ├── logger.c              log process and printlog()
│   └── sensor_node.c         sensor simulator (separate program)
└── test/
    ├── test_sbuffer.c        unit tests (make test-*)
    ├── test_adapters.c
    ├── test_datamgr.c
    ├── test_storagemgr.c
    ├── test_sensors.c        starts the sensor nodes listed in a CSV file
    ├── sensors.csv           17 sensors of every type
    └── soak.sh               integration test under valgrind (make soak)
```

Generated at build or run time (ignored by git): `build/` (object files),
`bin/` (`server`, `sensor_node`, test programs), `test/out/` (test output),
and `gateway.log`, `logFifo`, `gateway.db` in the working directory.
