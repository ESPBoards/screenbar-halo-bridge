# Dev tools

None of these are needed to run the bridge. They're for working on the radio
and the protocol. Run the commands from the repository root.

## wire.py: stand in for the C3

Talks the bridge protocol to an S3 running the USB build, so a PC can play the
C3's part. Needs `pip install pyserial`.

```sh
(cd s3 && idf.py -B build-usb -D SDKCONFIG=build-usb/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.usb" build flash)
tools/wire.py PORT hello
tools/wire.py PORT config 1A2B3C4D
tools/wire.py PORT set on --raw 0xA5 --temp 50
tools/wire.py PORT watch 60
tools/wire.py PORT pair 30
```

With the C3 soldered on, the plain `flash` fails; build without it and flash
from `s3/build-usb` with the esptool `--before usb-reset` line from the main
README. The S3 keeps nothing across a restart, so send `config` again after
one. `tools/wire.py -h` lists every option. Flash the normal build again to
go back to the C3.

## s3-bench: bring-up firmware for the S3

A separate ESP-IDF project on the same pinned ESP-IDF. It replaces the bridge
firmware until you flash that again.

```sh
(cd tools/s3-bench && idf.py build flash monitor)
```

With the C3 soldered on, flash from `tools/s3-bench/build` with the
`--before usb-reset` esptool line instead, then run `idf.py monitor`.

At boot it prints `TIMING` lines (PHY start-up, transmit path, demodulator),
then listens. The switches are `#define`s at the top of the files:

| File | Switch | Does |
|---|---|---|
| `bench_main.c` | `RUN_TX_BENCH` | Sends a 20-command burst to the lamp at boot (`SEND`, `SENDSUMMARY`). |
| `bench_main.c` | `RING_LISTEN` | 1: the always-on listener (`FRAME`, `CORE0`, `CORE1`, `HEARD`, `FLOOR` every second). 0: one 1 ms capture at a time (`FRAME`, `STATS`). |
| `bench_capture.c` | `DUMPS_ON` | In capture mode, prints loud captures that decoded to nothing as `RAW n` ... `RAWEND n`, for `replay`. |
| `bench_ring.c` | `GATE_DETAIL`, `TASK_STATS`, `AUDIT_EVERY` | Gate histograms, CPU per task, how often to demod everything to catch what the gate missed. |

Sending needs your lamp's address in `tools/s3-bench/main/bench_local.h`,
which git ignores so the address stays out of the repository:

```c
#define BENCH_LAMP_ADDRESS 0x1A2B3C4Du
```

Without it, the burst is skipped.

## replay.c: find out why a capture didn't decode

Runs raw captures through the real demodulator and detector on a PC, and
shows near misses.

```sh
make replay
./build-host/replay serial.log
```

Give it a saved serial log from the bench's capture mode with `DUMPS_ON`; it
picks out the `RAW` blocks and skips everything else.
