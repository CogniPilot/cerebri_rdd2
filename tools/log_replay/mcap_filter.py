#!/usr/bin/env python3
"""Copy a synapse/1 MCAP, dropping whole channels selected by topic name.

The RDD2 logger writes an uncompressed, unchunked and index-less MCAP: the file
is a leading magic, a Header record, one Schema and one Channel record per
topic, a flat run of Message records, then a Metadata, DataEnd and Footer
record followed by the trailing magic. Because there is no chunk, message or
summary index that carries byte offsets, and the Footer of an index-less writer
holds summary_start = 0, dropping Message records does not invalidate anything
that remains. This makes a faithful copy possible at the record level with no
MCAP library: every record is walked by its one-byte opcode and u64 length, the
Channel records name each channel id, and the records belonging to a dropped
topic are simply not written to the output.

  mcap_filter.py INPUT OUTPUT --drop TOPIC [--drop TOPIC ...]

Example, produce a GPS-free log:
  mcap_filter.py flight.mcap flight_nogps.mcap --drop gnss_fix
"""
import argparse
import struct
import sys

MAGIC = b"\x89MCAP0\r\n"

# MCAP record opcodes used by the RDD2 writer.
OP_CHANNEL = 0x04
OP_MESSAGE = 0x05
OP_FOOTER = 0x02


def filter_log(in_path, out_path, drop_topics):
    with open(in_path, "rb") as handle:
        data = handle.read()
    if data[:8] != MAGIC:
        raise SystemExit("not an MCAP file: %s" % in_path)

    drop = set(drop_topics)
    channel_topic = {}          # channel id -> topic name
    dropped_channel_ids = set()
    dropped_channels = 0
    dropped_messages = 0
    kept_records = 0

    pos = 8
    end = len(data)
    with open(out_path, "wb") as out:
        out.write(MAGIC)
        while pos + 9 <= end:
            op = data[pos]
            length = struct.unpack_from("<Q", data, pos + 1)[0]
            record_end = pos + 9 + length
            if record_end > end:
                raise SystemExit(
                    "truncated record: opcode 0x%02x at byte %d claims length %d"
                    % (op, pos, length))
            body_start = pos + 9

            if op == OP_CHANNEL:
                cid = struct.unpack_from("<H", data, body_start)[0]
                topic_len = struct.unpack_from("<I", data, body_start + 4)[0]
                topic = data[body_start + 8:body_start + 8 + topic_len].decode()
                channel_topic[cid] = topic
                if topic in drop:
                    dropped_channel_ids.add(cid)
                    dropped_channels += 1
                    pos = record_end
                    continue
            elif op == OP_MESSAGE:
                cid = struct.unpack_from("<H", data, body_start)[0]
                if cid in dropped_channel_ids:
                    dropped_messages += 1
                    pos = record_end
                    continue

            out.write(data[pos:record_end])
            kept_records += 1
            pos = record_end

            if op == OP_FOOTER:
                # The trailing magic follows the Footer record.
                out.write(MAGIC)
                pos = end
                break

    missing = sorted(drop - set(channel_topic.values()))
    if missing:
        print("warning: topics not present in %s: %s"
              % (in_path, ", ".join(missing)), file=sys.stderr)
    print("kept %d records, dropped %d channel record(s) and %d message(s): %s"
          % (kept_records, dropped_channels, dropped_messages,
             ", ".join(sorted(drop)) or "none"))
    print("wrote %s" % out_path)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="source synapse/1 MCAP")
    ap.add_argument("output", help="destination MCAP")
    ap.add_argument("--drop", action="append", default=[], metavar="TOPIC",
                    help="topic name whose channel and messages are removed "
                    "(repeatable)")
    args = ap.parse_args()
    if not args.drop:
        raise SystemExit("nothing to drop: pass at least one --drop TOPIC")
    filter_log(args.input, args.output, args.drop)


if __name__ == "__main__":
    main()
