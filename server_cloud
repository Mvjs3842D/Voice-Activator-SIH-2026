# 1. Install dependencies
!pip install faster-whisper websockets nest_asyncio numpy ipywidgets
!wget -q -nc https://github.com/ekzhang/bore/releases/download/v0.5.2/bore-v0.5.2-x86_64-unknown-linux-musl.tar.gz
!tar -xzf bore-v0.5.2-x86_64-unknown-linux-musl.tar.gz

import asyncio
import websockets
import numpy as np
import json
import subprocess
import re
from faster_whisper import WhisperModel
import nest_asyncio
import ipywidgets as widgets
from IPython.display import display

nest_asyncio.apply()

# Keep track of the connected ESP32 so the buttons can send commands to it
connected_esp32 = set()

# 2. Start the Free TCP Tunnel
print("Starting Cloud Tunnel...")
bore_proc = subprocess.Popen(["./bore", "local", "8765", "--to", "bore.pub"],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

public_port = None
for line in bore_proc.stdout:
    if "listening at" in line:
        match = re.search(r"bore\.pub:(\d+)", line)
        if match:
            public_port = int(match.group(1))
            break

print("\n" + "="*55)
print(f"🌐 [CLOUD SERVER] TUNNEL ACTIVE")
print(f"👉 ESP32 WEBSOCKET SERVER : bore.pub")
print(f"👉 ESP32 WEBSOCKET PORT   : {public_port}")
print("="*55 + "\n")

# 3. Load Faster-Whisper Model
print("[🧠] Loading Faster-Whisper on T4 GPU (float16)...")
model = WhisperModel("tiny.en", device="cuda", compute_type="float16")
print("[✔️] Model loaded. Cloud Server is ready!\n")

# 4. Interactive Cloud Dashboard Buttons (Manual Override)
loop = asyncio.get_event_loop()

btn_on = widgets.Button(description="💡 Manual ON", button_style='success')
btn_off = widgets.Button(description="🌑 Manual OFF", button_style='danger')

def manual_override(action_cmd):
    if not connected_esp32:
        print("[⚠️] Cannot send command: ESP32 is not connected!")
        return
    # Send the command to the ESP32 safely from the button thread
    for ws in connected_esp32:
        asyncio.run_coroutine_threadsafe(ws.send(json.dumps({"action": action_cmd})), loop)
    print(f"\n[👆] BUTTON CLICKED: Sent manual command -> {action_cmd}")

btn_on.on_click(lambda b: manual_override("LAMP_ON"))
btn_off.on_click(lambda b: manual_override("LAMP_OFF"))

# Display the buttons in the Colab Output
print("\n👇 CLOUD DASHBOARD (Click to test actuation without speaking) 👇")
display(widgets.HBox([btn_on, btn_off]))
print("")

# 5. WebSocket Server Logic
async def audio_server(websocket):
    print("\n" + "="*50)
    print("[🟢] ESP32 CONNECTED TO CLOUD SERVER")
    print("="*50)

    connected_esp32.add(websocket)
    audio_data = bytearray()

    try:
        async for message in websocket:
            if isinstance(message, bytes):
                audio_data.extend(message)

            elif isinstance(message, str):
                if message == "START":
                    print("\n[🎙️] Receiving Audio Stream from ESP32...")
                elif message == "DONE":
                    print(f"[✅] Audio Received ({len(audio_data)} bytes). Transcribing...")

                    audio_np = np.frombuffer(audio_data, dtype=np.int16).astype(np.float32) / 32768.0

                    # GPU Transcription
                    segments, info = model.transcribe(audio_np, beam_size=5)
                    text = "".join([segment.text for segment in segments]).strip().lower()
                    print(f"[🗣️] Transcript: '{text}'")

                    # Intent Parsing
                    action = "UNKNOWN"
                    if "on" in text and ("light" in text or "lamp" in text):
                        action = "LAMP_ON"
                        print("[💡] Intent Detected: Turn ON")
                    elif "off" in text and ("light" in text or "lamp" in text):
                        action = "LAMP_OFF"
                        print("[🌑] Intent Detected: Turn OFF")
                    else:
                        print("[❓] Intent Detected: Unrecognized")

                    # Send response
                    response = json.dumps({"action": action})
                    await websocket.send(response)
                    print(f"[📤] Sent command to ESP32: {action}\n")

                    audio_data = bytearray()

    except websockets.exceptions.ConnectionClosed:
        print("\n[⚠️] ESP32 DISCONNECTED from Cloud Server.")
    finally:
        connected_esp32.remove(websocket)

# 6. Start Server
async def main():
    async with websockets.serve(audio_server, "0.0.0.0", 8765):
        await asyncio.Future()

asyncio.run(main())
