# synapse_serial

Ground-side peer for the RDD2 CSyn serial transport (`subsys/csyn_serial/`),
plus an opt-in `native_sim` harness for exercising that transport without
hardware.

The link carries bare fixed-layout `synapse_fbs` payload structs inside the
compact framing `fbs/transport.fbs` prescribes for constrained byte streams:

```
off  size  field
 0     2   sync      0x53 0x59 ('S','Y')
 2     2   len       u16 LE, payload byte count
 4     2   topic_id  u16 LE, synapse catalog TopicId
 6     1   seq       u8, wraps, for loss detection
 7     1   flags     u8, reserved, zero
 8     N   payload   bare struct, byte-identical to the Zenoh encoding
8+N    2   crc16     CRC-16/CCITT-FALSE over bytes [2, 8+N)
```

A GNSS fix is 64 bytes of payload, so 74 bytes on the wire.

## Talking to the vehicle

Needs `pyserial`. The default baud matches the SiK factory setting.

```sh
./synapse_serial.py selftest                     # framing checks, no hardware
./synapse_serial.py send-gnss /dev/ttyUSB0 --lat 37.7749 --lon -122.4194 --alt 12
./synapse_serial.py monitor   /dev/ttyUSB0       # decode what the vehicle sends
```

On the vehicle: `csyn topic echo gnss` for the fix, `csyn_serial status` for
link counters.

## Running it without a radio

`native_sim` has a second PTY UART. Pointing the transport at it runs the real
firmware framer and parser against a real serial device. This is opt-in and
changes no default image:

```sh
west build -b native_sim/native/64 -d build-loopback -p -- \
  -DEXTRA_DTC_OVERLAY_FILE=$PWD/tools/synapse_serial/native_sim.overlay \
  -DEXTRA_CONF_FILE=$PWD/tools/synapse_serial/native_sim.conf

./build-loopback/zephyr/zephyr.exe -uart_stdinout -wait_uart
```

The binary prints `uart_1 connected to pseudotty: /dev/pts/N`; point
`send-gnss` or `monitor` at that path.

`-wait_uart` matters: the transport emits its first outbound frame within one
`CONFIG_RDD2_CSYN_SERIAL_TX_MIN_INTERVAL_MS` of boot, and without the flag
that frame is gone before a client can attach.

Note the sim's control loop only advances under lockstep, so flight-state
topics stay empty and outbound traffic is limited to whatever published at
boot. Inbound injection is unaffected.
