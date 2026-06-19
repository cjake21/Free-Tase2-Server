# FreeTASE2 Server

An open-source IEC 60870-6 TASE.2 / ICCP **server** simulator.

Most TASE.2 tooling is either commercial or client-only, so there was no easy way
to stand up a TASE.2 server you could point a client at and capture. This project
fills that gap. It runs a TASE.2/ICCP server that exposes the real ICCP object
model (bilateral table, version negotiation, data sets, transfer sets, indication
points, and a device control point) over the normal MMS / ISO-on-TCP stack on
TCP/102, so the TASE.2 object names and services show up on the wire.

Where it fits:

| Tool | TASE.2 client | TASE.2 server | Open source |
|------|:---:|:---:|:---:|
| MZ Automation libtase2 | yes | yes | no (commercial) |
| FreeTase2 (`aklira/FreeTase2`) | yes (Python) | no | yes |
| libIEC61850 | yes (MMS) | IEC 61850 only | yes |
| FreeTASE2 Server (this) | yes (C driver) | yes | yes |

Good for: testing TASE.2 clients, validating OT/ICS sensors and DPI parsers
(Malcolm, Zeek, Suricata, Arkime), generating labeled capture data, and protocol
research or training.

```text
Ethernet -> IP -> TCP -> TPKT -> COTP -> Session -> Presentation -> ACSE -> MMS
                                                                      ^ TASE.2 / ICCP object model
```

A note that trips people up: TASE.2/ICCP on the wire *is* MMS (ISO 9506). There
is no separate "TASE.2 PDU". What makes traffic TASE.2 is the object model and the
transfer-set / report behaviour layered on top of MMS, which is what this server
provides.

## What it implements

Following the IEC 60870-6 Conformance Building Blocks (CBBs):

- **Block 1 (basic services):** association, bilateral-table negotiation
  (`Bilateral_Table_ID`, VMD-scope `TASE2_Version` and `Supported_Features`),
  data values, data sets, and DS transfer set objects.
- **Block 2 (report by exception):** integrity and change-driven reporting of
  transfer sets as unconfirmed MMS `InformationReport` PDUs.
- **Block 5 (device control):** a control point you can operate.

Object model the server exposes:

```text
VMD scope (read with domain = NULL):
  TASE2_Version        STRUCTURE { major, minor }   -> {2000, 8}
  Supported_Features   BIT-STRING                    -> Blocks 1,2,5

ICC domain (default "TestDomain"):
  Bilateral_Table_ID   VISIBLE-STRING                -> "TestBilTab"
  Next_DSTransfer_Set  STRUCTURE { available, max, name }
  Transfer_Set_Name / Transfer_Set_Time_Stamp / DSConditions_Detected /
  Event_Code_Detected / Transfer_Report_ACK / Transfer_Report_NACK
  tm1, tm2             RealQ  STRUCTURE { Value:float, Flags:bitstring }  (live)
  ts1, ts2             StateQ STRUCTURE { Value:int,   Flags:bitstring }  (live)
  DSTransferSet01..08  STRUCTURE (standard TASE.2 transfer-set attributes)
  dev1                 STRUCTURE { Command, Tag, Status }  (control point)
```

## Layout

```text
free-tase2-server/
├── README.md
├── LICENSE                         # GPL-3.0
├── src/
│   ├── tase2_server.c              # the TASE.2/ICCP server
│   ├── tase2_client.c              # C client driver (Block 1/2/5), no extra deps
│   ├── tase2_probe.c               # small read-only client probe
│   ├── tase2_hmi_agent.c           # persistent ICCP client that backs the SCADA HMI
│   └── Makefile
├── hmi/
│   ├── bridge.py                   # stdlib HTTP/SSE bridge; drives two ICCP agents
│   └── static/                     # the single role-aware SCADA HMI page
├── bindings/
│   └── pyiec61850_tase2_wrappers.i # optional helpers for the FreeTase2 Python client
├── scripts/
│   ├── 00_install_deps.sh
│   ├── 10_build.sh                 # clone + patch + build libIEC61850, build the tools
│   ├── 20_netns_up.sh / 21_netns_down.sh
│   ├── 30_run_server.sh / 31_run_client.sh
│   ├── 32_capture.sh               # one-shot pcap capture (needs sudo)
│   ├── 40_local_test.sh            # no-sudo loopback smoke test
│   └── 50_run_hmi.sh               # start the server + SCADA HMI (no sudo)
└── docs/
    ├── proof_probe.txt             # saved tool output
    └── proof_client.txt
```

## How it's built

First, why this is C and not Python, since there are already Python TASE.2 tools
around. TASE.2 has two halves: a client that asks for data, and a server that owns
the data and hands it out. The ready-made open-source Python project (FreeTase2)
only does the client half. Nothing in Python can play the server side; that part
simply does not exist. The one mature open-source engine that can act as the
server for this protocol is libIEC61850, and it is a C library. Its Python wrapper
only exposes the library's polished, high-level mode, which is locked to fixed IEC
61850 naming and cannot present the plain ICCP names a TASE.2 client expects (such
as `Bilateral_Table_ID`). The parts we actually needed live in the library's
lower-level C internals that the Python wrapper never exposes. So the only way to
stand up a real TASE.2 server was to write it in C, directly against those
internals.

In concrete terms: the server is written in C against libIEC61850's lower-level
MMS server API (`MmsServer` / `MmsDevice` / `MmsDomain`) rather than `IedServer`.
`IedServer` derives its MMS names from a fixed IEC 61850 model and can't present
flat ICCP names like `Bilateral_Table_ID`, while the MMS API can. The tools
static-link `libiec61850.a` and `libhal.a`, so the binaries don't need
libIEC61850 installed at runtime.

`scripts/10_build.sh` clones libIEC61850 and applies two changes that a TASE.2
server needs but that aren't the upstream defaults:

1. Turns on `CONFIG_MMS_SUPPORT_VMD_SCOPE_NAMED_VARIABLES`, because TASE.2 reads
   `TASE2_Version` and `Supported_Features` at VMD scope.
2. Adds a missing argument to one call in the VMD-scope read path
   (`mms_read_service.c`). That code isn't compiled by default, and it doesn't
   build once you enable step 1. It's a small upstream bug this use case exposes.

## Quick start

Install the build and capture tools:

```bash
cd ~/free-tase2-server
./scripts/00_install_deps.sh
```

Build (this clones and patches libIEC61850, then builds the tools):

```bash
./scripts/10_build.sh
```

You'll get `src/tase2_server`, `src/tase2_client`, and `src/tase2_probe`.

Fastest way to confirm it works, no root needed:

```bash
./scripts/40_local_test.sh
```

That starts the server on `127.0.0.1:10502`, runs the probe and the full client
against it, and prints the whole Block 1/2/5 exchange including the reports it
receives.

## SCADA HMI

The server can also be driven from a small SCADA-style HMI, so you can watch the
TASE.2 exchange instead of only reading tool output. It is a single role-aware
web page with two views:

- **Station A** &mdash; the *local control centre* (the ICCP server side). It is
  interactive: set the tie-line flow (MW) or bus voltage (kV), and open/close the
  tie breaker.
- **Station B** &mdash; the *remote control centre / Bilateral Agreement* (the
  ICCP client side). It is read-only and shows **only** what actually arrived
  over ICCP in Block 2 reports, plus the reporting status and a power-flow
  threshold indicator.

```bash
./scripts/50_run_hmi.sh           # starts the server on 127.0.0.1:10502 + HMI
# then open http://127.0.0.1:8800
```

How it works, and why it is useful for capture: the HMI does not fake anything.
A Python bridge (`hmi/bridge.py`, stdlib only) runs two persistent ICCP clients
(`src/tase2_hmi_agent`) against the server. Station A actions become **real MMS
writes** to the `tm`/`ts` points (held via the server's injection-hold, `-o`) and
Block 5 `dev1` control; Station B is fed by the server's **real Block 2
`InformationReport` PDUs**. So a local change you make on Station A propagates to
Station B over genuine TASE.2/MMS traffic on the wire &mdash; which is exactly
what you can capture and inspect.

To capture the HMI's traffic, run the namespace lab (`20_netns_up.sh`,
`30_run_server.sh`, `32_capture.sh`) and point the bridge at the server
namespace:

```bash
TASE2_HOST=10.20.0.10 TASE2_PORT=102 python3 hmi/bridge.py \
    --server-host 10.20.0.10 --server-port 102
```

This is a **closed lab simulator**. Every value is synthetic, it controls no real
infrastructure, and it connects to nothing outside the lab. Station A/B, the
breaker, the tie-line flow, the voltage and all markers are training artifacts.

### CTF / lab branch

The `scada-hmi-lab` branch adds two synthetic validation markers on top of this
HMI for a proof-of-concept training workflow: a bilateral-table marker revealed
in the lab/Caldera output, and a power-flow marker revealed on the HMI once the
reported tie-line flow crosses a training threshold. See
[`docs/lab_ctf.md`](docs/lab_ctf.md). The standard branch has no hidden markers.

### Watching the loopback traffic in Wireshark

When you run `50_run_hmi.sh`, the HMI's clients (and any Caldera/C client you point
at it) all talk to the server over loopback on port 10502. Two settings trip
people up, so set both or you will see nothing:

1. Capture on the **loopback interface (`lo`)**, not a physical NIC. All the
   traffic is on `127.0.0.1`, so capturing on `eth0`/`wlan0` shows nothing.
2. Wireshark only auto-decodes MMS on TCP port 102. On 10502 you have to tell it:
   right-click any packet on that port, choose **Decode As...**, and in the
   **Current** column (not the Value column) set TCP port `10502` to **TPKT**.
   Pick TPKT, not MMS; TPKT pulls up the rest of the stack itself (COTP, Session,
   Presentation, ACSE, MMS).

Start the capture **before** launching the HMI if you want to catch the initial
association (COTP CR/CC, the MMS initiate, and the `Bilateral_Table_ID` read that
is the bilateral agreement). Then use these filters:

```text
tcp.port == 10502
mms                              # all TASE.2 / MMS PDUs
mms.unconfirmed                  # the live Block 2 report stream
mms.confirmedServiceRequest      # reads/writes you drive from the HMI or Caldera
```

(The `Capturing a pcap` section below is the namespace lab, which runs on port 102
where Wireshark auto-decodes MMS with no Decode As step.)

## Capturing a pcap

These use network namespaces, so they need sudo (the scripts call it for you).

Bring up the isolated network (`tase2_srv` at 10.20.0.10, `tase2_cli` at
10.20.0.20, joined by a veth pair):

```bash
./scripts/20_netns_up.sh
```

Capture in one command (starts tcpdump and the server, runs the client for ~20s,
then tears down):

```bash
./scripts/32_capture.sh /tmp/tase2_iccp.pcap
```

If the pcap comes out empty, your sudo password didn't go through; run it again.

Open it:

```bash
wireshark /tmp/tase2_iccp.pcap
# or
tshark -r /tmp/tase2_iccp.pcap -q -z io,phs
```

Handy Wireshark filters:

```text
tcp.port == 102
tpkt || cotp || acse || mms
mms                              # all MMS PDUs
mms.confirmedServiceResponse     # reads of TASE.2 objects
mms.unconfirmed                  # Block 2 InformationReports
```

Tear it down when you're done:

```bash
./scripts/21_netns_down.sh
```

If you'd rather watch each side, use three terminals: capture in one,
`./scripts/30_run_server.sh` in another, `./scripts/31_run_client.sh` in a third.

## Which script does what

| To... | Run |
|---|---|
| Install everything | `./scripts/00_install_deps.sh` |
| Build server + clients | `./scripts/10_build.sh` |
| Confirm it works (no sudo) | `./scripts/40_local_test.sh` |
| Run the SCADA HMI (no sudo) | `./scripts/50_run_hmi.sh` |
| Create the capture network | `./scripts/20_netns_up.sh` |
| Produce a pcap | `./scripts/32_capture.sh out.pcap` |
| Run just the server | `./scripts/30_run_server.sh` |
| Run just the client | `./scripts/31_run_client.sh` |
| Clean up the network | `./scripts/21_netns_down.sh` |

## Running the server on its own

```bash
src/tase2_server -i <bindIp> -p <port> -d <domain> -b <bltId> -t <integritySecs>
# defaults: all interfaces, port 102, domain TestDomain, blt TestBilTab, integrity 30s

# over TLS (Secure ICCP): -T turns it on, -C/-K are the server cert and key,
# -A is the CA used to validate client certificates (mutual TLS)
src/tase2_server -i <bindIp> -p <port> -T -C server.pem -K server.key -A ca.pem
```

Then point any TASE.2 client at it: the bundled `tase2_client`, a commercial test
set, or the FreeTase2 Python client.

## Proof

`docs/proof_client.txt` is real output from `tase2_client` driving the server:
the Block 1 reads, the data set creation, the transfer set being enabled, the
report-by-exception reports coming back, and a device-control operate. You can
regenerate it any time with `./scripts/40_local_test.sh`.

`docs/tase2_iccp.pcap` is a real capture from the namespace lab, with
`docs/capture_decode.txt` showing how it decodes. The full stack is present
(eth → ip → tcp → tpkt → cotp → ses → pres → acse → mms), and the TASE.2 object
names show up on the wire, for example:

```text
TASE2_Version, Supported_Features, Bilateral_Table_ID, Next_DSTransfer_Set,
Transfer_Set_Name, Transfer_Set_Time_Stamp, DSTransferSet01 (+ attributes),
tm1, tm2, ds_analog, dev1$Command, dev1$Tag

MMS PDU mix: initiate / confirmed request / confirmed response, plus
unconfirmed-PDU (the Block 2 report-by-exception reports).
```

## Limitations

- The indication-point and transfer-set value encodings follow common TASE.2
  conventions rather than the full IEC 60870-6-802 type catalogue. The
  association, object names, service types, and report framing are accurate;
  that's what matters for capture and parser work. Tightening the value
  encodings is future work.
- TLS (Secure ICCP) is optional: pass `-T` with a cert (`-C`), key (`-K`) and CA
  (`-A`) to serve over TLS. It requires libIEC61850 built with the mbedtls backend.
- The FreeTase2 (Python) client path is experimental; the C `tase2_client` is the
  supported driver.

## License

GPL-3.0, see `LICENSE`. Built on libIEC61850, which is GPL-3.0 (with a commercial
option from MZ Automation).
