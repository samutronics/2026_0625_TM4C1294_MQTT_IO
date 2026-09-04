import time, datetime
import paho.mqtt.client as mqtt

HOST = "192.168.1.11"; PORT = 1883; USER = "mqtt1"; PASS = "mqtt1"
TEST_DEVID = "tm4c1294_02ffb4"     # this test's device only
TEST_BASE  = "tm4cio/"             # this test's base topic only
KEEP = ("02d44e", "02f903")        # real devices — never touch

def ts():
    return datetime.datetime.now().strftime("%H:%M:%S")

seen = {}

def is_test(t):
    if any(k in t for k in KEEP):
        return False
    return (TEST_DEVID in t) or t.startswith(TEST_BASE)

def on_connect(c, u, f, rc, props=None):
    print(f"[{ts()}] connected rc={rc}", flush=True)
    c.subscribe("homeassistant/#", qos=0)
    c.subscribe(TEST_BASE + "#", qos=0)

def on_message(c, u, msg):
    # Only act on retained messages (the persisted ones we want to clear).
    if msg.retain and is_test(msg.topic):
        seen[msg.topic] = True

c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="claude-cleanup")
c.username_pw_set(USER, PASS)
c.on_connect = on_connect
c.on_message = on_message
c.connect(HOST, PORT, 60)
c.loop_start()
time.sleep(4)   # collect retained
topics = sorted(seen.keys())
print(f"[{ts()}] clearing {len(topics)} retained test topics", flush=True)
for t in topics:
    c.publish(t, payload=b"", qos=0, retain=True)  # empty retained => delete
    print(f"[{ts()}] cleared {t}", flush=True)
time.sleep(2)   # let clears flush
c.loop_stop()
c.disconnect()
print(f"[{ts()}] done ({len(topics)} cleared)", flush=True)
