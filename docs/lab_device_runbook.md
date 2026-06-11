# Lab Runbook: connect a real TASE.2 device to the server and view traffic

This walks a tester through standing up the FreeTASE2 Server on a Linux host and
having a **real lab SCADA / RTU / ICCP device that is configured to speak TASE.2**
associate to it as the client, then viewing the exchange, both live on the server
console and as a saved pcap.

```text
  ┌─────────────────────────┐         TCP/102 (ISO-on-TCP / MMS)        ┌──────────────────────┐
  │  Lab SCADA device        │  ───────────────────────────────────▶   │  This Linux host      │
  │  (TASE.2 CLIENT)         │   associate, read, enable transfer set   │  tase2_server         │
  │  e.g. 10.20.0.20         │   ◀───── report-by-exception ──────      │  (TASE.2 SERVER)      │
  └─────────────────────────┘                                          │  10.20.0.10:102       │
                                                                        │  tcpdump on same NIC  │
                                                                        └──────────────────────┘
```

> **Roles matter.** In ICCP the *data owner* is the MMS **server**; the peer that
> wants the data is the MMS **client**. Here the server owns the data and the lab
> device is the client. Data flows server→device via reports; the device does not
> "push" values in. See the repo README "What it implements" for the object model.

---

## 0. Prerequisites

- A Linux host (Debian/Ubuntu assumed) on the **same LAN/segment** as the device,
  or with a NIC cabled to the device.
- `sudo` on the host (needed for TCP/102, IP setup, and tcpdump).
- A lab device that can act as a **TASE.2 / ICCP client** and lets you configure
  the peer IP, bilateral table, and object names (most commercial ICCP stacks do).

---

## 1. Clone and build (on the host)

```bash
git clone <this-repo-url> free-tase2-server
cd free-tase2-server
git checkout feature/lab-device-capture     # branch with the LAN capture script

./scripts/00_install_deps.sh                # build + capture tools (apt, uses sudo)
./scripts/10_build.sh                        # clones + patches libIEC61850, builds the tools
```

`10_build.sh` produces `src/tase2_server`, `src/tase2_client`, and `src/tase2_probe`.
The binaries are statically linked, so libIEC61850 does **not** need to be installed
at runtime.

Confirm the build works before involving the device (loopback, no sudo, ~20s):

```bash
./scripts/40_local_test.sh
```

You should see Block 1 reads, a data set created, a transfer set enabled, and
report-by-exception reports come back. If that prints cleanly, the server is good.

---

## 2. Put the host and device on the same subnet

Pick the NIC cabled to the device.

**If using a dedicated lab NIC that has no IP yet** (e.g. `enp0s8`):

```bash
sudo ip addr add 10.20.0.10/24 dev enp0s8
sudo ip link set enp0s8 up
```

Then configure the device with an address on the same /24, e.g. `10.20.0.20/24`.

**If using your existing LAN NIC** (already has an IP, e.g. `enp0s3` = `192.168.1.26`):
nothing to do here; just use that NIC's IP as the server address below.

Sanity check L3 reachability (run from the host, or ping the host from the device):

```bash
ping -c2 10.20.0.20        # the device
```

---

## 3. Provision the lab device (the TASE.2 client)

ICCP access is bilateral-table-gated and this server's model is fixed in code, so
the device must be configured to match **exactly**:

| Device setting              | Value                                            |
|-----------------------------|--------------------------------------------------|
| Server / peer IP            | `10.20.0.10` (lab NIC) or `192.168.1.26` (LAN)   |
| Port                        | `102`                                            |
| **TLS / Secure ICCP**       | **OFF**. This build has no TLS; leave it on and the association fails |
| Domain / ICC name           | `TestDomain`                                      |
| Bilateral Table ID          | `TestBilTab`                                       |
| TASE2_Version (read-back)   | `{2000, 8}` at VMD scope                          |

**Objects the device can read / subscribe / control (exact names):**

| Purpose                  | Object                                                                 |
|--------------------------|-----------------------------------------------------------------------|
| VMD scope (domain = NULL)| `TASE2_Version`, `Supported_Features`                                  |
| Indication (analog)      | `tm1`, `tm2`  (RealQ: `Value` float + `Flags` bitstring)              |
| Indication (status)      | `ts1`, `ts2`  (StateQ: `Value` int + `Flags` bitstring)               |
| Block 2 transfer set     | `DSTransferSet01`: write `DataSetName`, `Interval`, `DSConditionsRequested`, then `Status`=1 to enable |
| Block 5 control point    | `dev1`: members `Command`, `Tag`, `Status` (select-before-operate)   |

> Only `DSTransferSet01` is the live, cache-backed transfer set. `DSTransferSet02..08`
> exist in the type spec but are not the one to enable for reporting.

To get **report-by-exception** traffic, point the device at a data set containing
`tm1/tm2/ts1/ts2`, bind it to `DSTransferSet01`, and enable it (`Status`=1). The
server also sends periodic integrity reports on the `-t` interval (default 30s).

---

## 4. Start the server and capture, in one command

```bash
sudo ./scripts/33_capture_lan.sh /tmp/tase2_lab.pcap
```

This binds the server to `0.0.0.0:102` and runs `tcpdump` on the lab NIC,
filtering `tcp port 102`. Defaults can be overridden by env:

```bash
# capture on your main LAN NIC instead of the lab NIC:
sudo IFACE=enp0s3 ./scripts/33_capture_lan.sh /tmp/tase2_lab.pcap

# change domain / bilateral table / integrity period:
sudo TASE2_DOMAIN=TestDomain TASE2_BLT_ID=TestBilTab TASE2_INTEGRITY=5 \
     ./scripts/33_capture_lan.sh /tmp/tase2_lab.pcap
```

Leave it running. The server console prints each event live:

```text
[tase2] server listening on 0.0.0.0:102 domain=TestDomain blt=TestBilTab integrity=30s
[tase2] <client> associated
[tase2] DSTransferSet01 bound to data set '...'
[tase2] DSTransferSet01 ENABLED
[tase2] device control operate on dev1.Command
```

Now trigger the association from the device (bring its ICCP link up / start its
transfer). When you have enough traffic, press **Ctrl-C**. The script stops the
server and tcpdump, fixes the pcap's ownership/permissions, and prints its size.

---

## 5. View the traffic

**Protocol hierarchy** confirms the full TASE.2/ICCP stack is present:

```bash
tshark -r /tmp/tase2_lab.pcap -q -z io,phs
# expect: eth → ip → tcp → tpkt → cotp → ses → pres → acse → mms
```

**The MMS / TASE.2 exchange**:

```bash
tshark -r /tmp/tase2_lab.pcap -Y mms            # every MMS PDU
tshark -r /tmp/tase2_lab.pcap -Y mms.confirmedServiceResponse   # reads of TASE.2 objects
tshark -r /tmp/tase2_lab.pcap -Y mms.unconfirmed                # Block 2 report-by-exception
```

**In Wireshark**, open the pcap and use any of:

```text
tcp.port == 102
tpkt || cotp || acse || mms
mms                                # all MMS PDUs
mms.confirmedServiceResponse       # object reads
mms.unconfirmed                    # InformationReports (transfer-set reports)
```

You should see the TASE.2 object names on the wire: `TASE2_Version`,
`Supported_Features`, `Bilateral_Table_ID`, `Next_DSTransfer_Set`,
`Transfer_Set_Name`, `DSTransferSet01`, `tm1/tm2`, `dev1$Command`, etc.

(For a reference of what a good capture looks like, see `docs/tase2_iccp.pcap` and
`docs/capture_decode.txt` on `main`.)

---

## 6. Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| pcap is empty, no packets at all | Wrong NIC (`IFACE=`), L2/cabling, or no IP on the lab NIC. `ping` the device first. |
| Device sends SYN but never associates | Firewall on 102, **or the client is offering TLS**. Turn Secure ICCP off. |
| Associates, then errors on a read | Object name / domain / BLT mismatch, or a strict client rejecting a value encoding (known limitation: encodings follow common TASE.2 conventions, not the full IEC 60870-6-802 catalogue). |
| `Address already in use` on start | Another process holds 102: `sudo lsof -iTCP:102 -sTCP:LISTEN`. |
| No reports arriving | Device didn't enable `DSTransferSet01` (`Status`=1) or its data set is empty. Lower `TASE2_INTEGRITY` to see integrity reports sooner. |
| Need to watch each side separately | Run capture in one terminal, `./scripts/30_run_server.sh`-style server in another (note: `30_*` uses the netns lab; for LAN just run `src/tase2_server -i <ip> -p 102 ...` directly). |

---

## Quick reference: server flags

```bash
src/tase2_server -i <bindIp> -p <port> -d <domain> -b <bltId> -t <integritySecs>
# defaults: all interfaces, 102, TestDomain, TestBilTab, 30
```
