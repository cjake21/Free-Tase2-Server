# FreeTASE2 CTF / Lab Branch

This is the **`scada-hmi-lab`** branch: the standard SCADA HMI plus two synthetic
validation markers for a proof-of-concept training workflow. It shares all of the
standard branch's code and UI; it only adds the markers, a power-flow threshold,
and the integration points for the Caldera automation workflow.

> **Closed lab only.** Every value, marker, and flag is a synthetic training
> artifact. The server controls no real infrastructure and connects to nothing
> outside the lab.

## The two markers

| | Marker 1 | Marker 2 |
|---|---|---|
| Object | `Bilateral_Marker` | `PowerFlow_Marker` |
| Revealed in | lab output / observable traffic (Caldera read result, on the wire) | the SCADA HMI (Station B) |
| Unlocked when | a client **enumerates the domain object list** (the "Enumerate Bilateral Table and Objects" recon step) | the watched flow point (`tm1`) **crosses the threshold** (default 100 MW) |
| Default flag | `flag{tase2_bilateral_table_recon}` | `flag{tase2_powerflow_overflow}` |
| Until unlocked | returns `LOCKED - enumerate the bilateral table object list first` | returns `LOCKED - tie-line flow within limits` |

Both marker objects are part of the TASE.2 object model (they show up in the
object list and introspection), but they are deliberately **not cached**: reads
route through the server's read handler, which only returns the flag once the
matching milestone has been reached. The gates are global and latch once, so they
work across the separate associations that each Caldera ability step opens.

Marker 1 is gated on the *object-list enumeration* rather than on a
`Bilateral_Table_ID` read, specifically so the monitoring HMI (which reads the
BLT but never enumerates names) does not pre-unlock it — the gate reflects the
learner's workflow, not the HMI.

Both flags and the threshold are configurable:

```bash
src/tase2_server --flag1 'flag{...}' --flag2 'flag{...}' \
                 --flow-threshold 100 --flow-point tm1
```

## Running the lab end-to-end

1. **Start the lab server + HMI** (loopback, no sudo):
   ```bash
   ./scripts/50_run_hmi.sh        # server on 127.0.0.1:10502 + HMI on :8800
   ```
   Open `http://127.0.0.1:8800` and confirm Station A / Station B are live.
   (`50_run_hmi.sh` already passes a long injection-hold so an injected overload
   stays visible.)

2. **Run the Caldera workflow** against the same server. In Caldera, load the
   `tase2` plugin, use the **TASE.2 CTF Lab Facts (FreeTASE2 SCADA HMI)** fact
   source and the **TASE.2 CTF Lab (Bilateral Recon + Power-Flow FDI)** adversary
   (both shipped in the plugin's `lab-ctf` branch). The chain runs:

   | Step | Ability | What it does | Marker |
   |---|---|---|---|
   | 1 | Enumerate Domains | finds the ICC domain | |
   | 2 | Enumerate Bilateral Table and Objects | reads the BLT id + lists objects | **unlocks marker 1** |
   | 3 | Read Arbitrary Object (`Bilateral_Marker`) | reads the marker object | **marker 1 in the output** |
   | 4 | Inject False Telemetry (`tm1=137.5`) | drives the tie-line flow over the threshold | **unlocks marker 2** |
   | 5 | Manipulate Transfer Set Reporting | makes the false value report upstream | |
   | 6 | Capture Transfer Set Reports | verifies the false data on the wire | |

3. **Marker 1** appears in step 3's link output as the `tase2.read.value` fact
   (`flag{tase2_bilateral_table_recon}`), and is visible on the wire in the MMS
   read response.

4. **Marker 2** appears on the **SCADA HMI**: once step 4 pushes `tm1` over the
   threshold, Station B shows the changed/overflow flow state and reveals
   `flag{tase2_powerflow_overflow}`. (You can also trigger it manually from
   Station A by setting the tie-line flow above the threshold.)

5. **Capture** the traffic with the namespace scripts or any TCP/102 capture to
   keep PCAP evidence of the whole sequence.

## Trying the marker mechanics without Caldera

The Caldera abilities just run the `tase2_actions` client, so you can exercise the
same gates directly:

```bash
ACT=../caldera-tase2/src/tase2_actions          # the plugin's client
$ACT read     127.0.0.1 10502 TestDomain 'Bilateral_Marker' --id-spec none   # LOCKED
$ACT discover 127.0.0.1 10502 TestDomain --id-spec none                       # enumerate -> unlocks
$ACT read     127.0.0.1 10502 TestDomain 'Bilateral_Marker' --id-spec none   # flag 1
$ACT inject   127.0.0.1 10502 TestDomain tm1 137.5 --id-spec none             # over threshold
$ACT read     127.0.0.1 10502 TestDomain 'PowerFlow_Marker' --id-spec none   # flag 2 (also shown on the HMI)
```
