import sys, time, datetime
import paho.mqtt.client as mqtt

HOST = "192.168.1.11"; PORT = 1883; USER = "mqtt1"; PASS = "mqtt1"
DUR = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0

def ts():
    return datetime.datetime.now().strftime("%H:%M:%S")

def keep(t):
    if t.startswith("tm4cio/"):
        return True
    if t.startswith("homeassistant/") and ("tm4c" in t or "/input" in t
                                           or "/relay" in t or "/cover" in t):
        return True
    return False

def on_connect(c, u, f, rc, props=None):
    print(f"[{ts()}] connected rc={rc}; subscribing '#'", flush=True)
    c.subscribe("#", qos=0)

def on_message(c, u, msg):
    t = msg.topic
    if not keep(t):
        return
    p = msg.payload.decode("utf-8", "replace")
    r = "R" if msg.retain else " "
    print(f"[{ts()}] {r} {t} = {p}", flush=True)

c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="claude-watch")
c.username_pw_set(USER, PASS)
c.on_connect = on_connect
c.on_message = on_message
c.connect(HOST, PORT, 60)
c.loop_start()
time.sleep(DUR)
c.loop_stop()
print(f"[{ts()}] done", flush=True)
