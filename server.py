import asyncio
import websockets
import numpy as np
from faster_whisper import WhisperModel
import io

print("Loading Faster-Whisper model... (this may take a moment)")
model = WhisperModel("tiny.en", device="cpu", compute_type="int8")
print("Model loaded. Ready to receive audio!")

async def handle_client(websocket, *args):
    print(">>> ESP32 connected via WebSocket!")
    audio_data = bytearray()
    
    try:
        async for message in websocket:
            if isinstance(message, bytes):
                # We receive binary PCM data from ESP32, accumulate it
                audio_data.extend(message)
            elif isinstance(message, str):
                if message == "START":
                    print("Wake word detected! Recording command...")
                    audio_data = bytearray()
                elif message == "DONE":
                    print("Finished recording. Transcribing...")
                    
                    if len(audio_data) > 0:
                        # Convert raw 16-bit PCM bytes to numpy float32 array
                        audio_np = np.frombuffer(audio_data, dtype=np.int16).astype(np.float32) / 32768.0
                        
                        # Transcribe audio using faster-whisper
                        segments, info = model.transcribe(audio_np, beam_size=5)
                        text = "".join([segment.text for segment in segments]).lower()
                        print(f"Transcription: {text}")
                        
                        # Command Parsing Logic for Lamp (Relay)
                        if "on" in text:
                            await websocket.send("LAMP_ON")
                            print("Action: LAMP_ON")
                        elif "off" in text:
                            await websocket.send("LAMP_OFF")
                            print("Action: LAMP_OFF")
                        else:
                            await websocket.send("UNKNOWN")
                            print("Action: UNKNOWN COMMAND")
                    else:
                        await websocket.send("UNKNOWN")
    except websockets.exceptions.ConnectionClosed:
        print("ESP32 disconnected.")

async def main():
    async with websockets.serve(handle_client, "0.0.0.0", 8765):
        print("WebSocket Server running on ws://0.0.0.0:8765")
        await asyncio.Future()  # run forever

if __name__ == "__main__":
    asyncio.run(main())
