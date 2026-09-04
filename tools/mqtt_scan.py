import time, json, collections
import paho.mqtt.client as mqtt

HOST = "192.168.1.11"; PORT = 1883; USER = "mqtt1"; PASS = "mqtt1"

disc = collections.defaultdict(lambda: collections.Counter())  # devid -> comp -> count
empty = collections.defaultdict(lambda: collections.Counter()) # devid -> comp -> cleared
meta = {}                                                       # devid -> (base, mdl)
bases = collections.defaultdict(set)                            # base -> set(topic-suffix kinds)
status = {}                                                     # base -> payload

def on_connect(c, u, f, rc, props=None):
    print(f"connected rc={rc}")
    c.subscribe("#", qos=0)

def on_message(c, u, msg):
    if not msg.retain:
        return
    t = msg.topic; p = msg.payload
    parts = t.split("/")
    if t.startswith("homeassistant/") and t.endswith("/config") and len(parts) >= 4:
        comp, devid = parts[1], parts[2]
        if len(p) == 0:
            empty[devid][comp] += 1
        else:
            disc[devid][comp] += 1
            try:
                j = json.loads(p.decode("utf-8", "replace"))
                meta[devid] = (j.get("~", "?"), j.get("dev", {}).get("mdl", "?"))
            except Exception:
                pass
    else:
        # device state tree: base/<kind>/...
        if len(parts) >= 2 and parts[-1] in ("state", "event") or t.endswith("/status"):
            base = parts[0]
            if t.endswith("/status"):
                status[base] = p.decode("utf-8", "replace")
            else:
                bases[base].add(parts[1] if len(parts) > 1 else "?")

c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="claude-scan")
c.username_pw_set(USER, PASS)
c.on_connect = on_connect
c.on_message = on_message
c.connect(HOST, PORT, 60)
c.loop_start()
time.sleep(4)
c.loop_stop(); c.disconnect()

print("\n==== DEVICES (retained HA discovery) ====")
for devid in sorted(disc):
    base, mdl = meta.get(devid, ("?", "?"))
    d = disc[devid]
    line = ", ".join(f"{k}={d[k]}" for k in sorted(d))
    e = empty[devid]
    ex = ("  [cleared: " + ", ".join(f"{k}={e[k]}" for k in sorted(e)) + "]") if e else ""
    print(f"  {devid}  mdl={mdl:<16} base={base:<22} {line}{ex}")

print("\n==== BASE-TOPIC STATE TREES + status ====")
allbases = set(bases) | set(status)
for base in sorted(allbases):
    kinds = ",".join(sorted(bases.get(base, set()))) or "-"
    print(f"  {base:<24} status={status.get(base,'(none)'):<8} state-kinds=[{kinds}]")
