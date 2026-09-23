#include <WiFi.h>
#include <WebSocketsClient.h>
#include <driver/i2s.h>
#include <driver/adc.h>

// ==========================================
// Edge Impulse Library Header
// ==========================================
#include <Mvjs3842D-project-1_inferencing.h>

// WiFi and WebSocket Settings
const char* ssid = "Airtel_Murugan's Airtel Airfiber";
const char* password = "Murugan@1234";
const char* websocket_server = "192.168.1.6"; // Laptop's IP address
const uint16_t websocket_port = 8765;

WebSocketsClient webSocket;

// Relay Pins (2-channel relay)
#define RELAY_CH1 16
#define RELAY_CH2 17

// Audio Settings
#define I2S_SAMPLE_RATE 48000
#define DECIMATION_FACTOR 3 // 48000 / 3 = 16000 Hz
#define I2S_PORT I2S_NUM_0

// Inference parameters
#define BUFFER_SIZE (EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE)
int16_t i2s_buffer[256];
int16_t inference_buffer[BUFFER_SIZE]; // Use int16_t instead of float to save 32KB RAM
int buffer_index = 0;

enum State {
  LISTENING_WAKEWORD,
  STREAMING_AUDIO,
  WAITING_FOR_COMMAND
};

State currentState = LISTENING_WAKEWORD;
unsigned long streamingStartTime = 0;

// Edge impulse callback function (converts int16 to float on-the-fly)
int raw_feature_get_data(size_t offset, size_t length, float *out_ptr) {
    for (size_t i = 0; i < length; i++) {
        out_ptr[i] = (float)inference_buffer[offset + i];
    }
    return 0;
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.printf("[WebSocket] Disconnected from server (ws://%s:%u)!\n", websocket_server, websocket_port);
      break;

    case WStype_CONNECTED:
      Serial.printf("[WebSocket] CONNECTED successfully to ws://%s:%u/ !\n", websocket_server, websocket_port);
      break;

    case WStype_TEXT: {
      String msg = (char*)payload;
      Serial.println("[WebSocket] Server said: " + msg);
      
      // Testing with Relay on Channel 1 (Pin 16) - Active LOW
      if(msg == "LAMP_ON") {
          digitalWrite(RELAY_CH1, LOW); // Turn Relay ON (Active LOW)
          Serial.println(">>> Action executed: LAMP TURNED ON");
      }
      else if(msg == "LAMP_OFF") {
          digitalWrite(RELAY_CH1, HIGH);  // Turn Relay OFF (Active LOW)
          Serial.println(">>> Action executed: LAMP TURNED OFF");
      }
      
      // Go back to listening for wake word
      currentState = LISTENING_WAKEWORD;
      buffer_index = 0; 
      Serial.println("Resuming wake word detection...");
      break;
    }

    case WStype_ERROR:
      Serial.printf("[WebSocket] Error occurred on connection.\n");
      break;
  }
}

void setup() {
  Serial.begin(115200);
  
  pinMode(RELAY_CH1, OUTPUT);
  pinMode(RELAY_CH2, OUTPUT);
  
  // Initialize relays to OFF state (Active LOW means HIGH is OFF)
  digitalWrite(RELAY_CH1, HIGH); 
  digitalWrite(RELAY_CH2, LOW);
  
  // Test click the Relay 3 times on boot so user knows wiring is correct!
  Serial.println("Testing Relay wiring...");
  for(int i=0; i<3; i++) {
      digitalWrite(RELAY_CH1, LOW);  // Turn ON
      delay(200);
      digitalWrite(RELAY_CH1, HIGH); // Turn OFF
      delay(200);
  }
  
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected. IP: " + WiFi.localIP().toString());
  
  // Setup WebSocket
  webSocket.begin(websocket_server, websocket_port, "/");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
  
  // Configure I2S in ADC mode for analog microphone (MAX9814)
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN),
    .sample_rate = I2S_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 2,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };
  
  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  // Configure ADC to read from GPIO 32 (ADC1 Channel 4)
  i2s_set_adc_mode(ADC_UNIT_1, ADC1_CHANNEL_4); 
  // Set ADC attenuation to 11dB to measure up to ~3.3V (avoids clipping MAX9814's 1.25V bias)
  adc1_config_channel_atten(ADC1_CHANNEL_4, ADC_ATTEN_DB_11);
  i2s_adc_enable(I2S_PORT);
  
  Serial.println("System Ready. Listening for 'marvin'...");
}

void loop() {
  webSocket.loop();
  
  size_t bytes_read;
  
  static float dc_offset = 1450.0;

  if (currentState == LISTENING_WAKEWORD) {
    // Read audio chunk from microphone
    i2s_read(I2S_PORT, (void*)i2s_buffer, sizeof(i2s_buffer), &bytes_read, portMAX_DELAY);
    int samples = bytes_read / sizeof(int16_t);
    
    // Decimate from 48kHz down to 16kHz (take every 3rd sample)
    for (int i = 0; i < samples; i += DECIMATION_FACTOR) {
        // Remove DC bias from analog microphone and center to 0
        float raw = (float)(i2s_buffer[i] & 0x0FFF);
        dc_offset = (dc_offset * 0.99) + (raw * 0.01);
        float centered = (raw - dc_offset) * 12.0;
        int16_t sample = (int16_t)constrain(centered, -32768.0, 32767.0);
        
        static int debug_counter = 0;
        if (++debug_counter >= 16000) {
            Serial.printf("DEBUG MIC -> Raw: %.1f | DC: %.1f | Centered: %.1f\n", raw, dc_offset, centered);
            
            // Hardware Diagnostic Check
            if (raw < 500.0 || raw > 4000.0) {
                Serial.println("=====================================================");
                Serial.println("⚠️ HARDWARE WARNING: Microphone signal is DEAD or FLOATING!");
                Serial.println("⚠️ Please check that MAX9814 OUT is connected to GPIO 32");
                Serial.println("⚠️ and VCC/GND are securely plugged in.");
                Serial.println("=====================================================");
            }
            debug_counter = 0;
        }
        
        inference_buffer[buffer_index++] = sample;
        
        // Once our 1-second buffer is full, run the Edge Impulse model
        if (buffer_index >= BUFFER_SIZE) {
            signal_t features_signal;
            features_signal.total_length = BUFFER_SIZE;
            features_signal.get_data = &raw_feature_get_data;
            
            unsigned long t_start = millis();
            ei_impulse_result_t result = { 0 };
            EI_IMPULSE_ERROR res = run_classifier(&features_signal, &result, false);
            unsigned long total_infer_time = millis() - t_start;
            
            if (res == EI_IMPULSE_OK) {
                // Live RAM & CPU Statistics
                uint32_t totalHeap = ESP.getHeapSize();
                uint32_t freeHeap = ESP.getFreeHeap();
                uint32_t usedHeap = totalHeap - freeHeap;
                float heapUsagePercent = ((float)usedHeap / (float)totalHeap) * 100.0;
                uint32_t maxAlloc = ESP.getMaxAllocHeap();
                uint32_t cpuFreq = ESP.getCpuFreqMHz();

                Serial.println("\n--- [ESP32 Live Performance Monitor] ---");
                Serial.printf("CPU Frequency : %u MHz | Inference Time: %lu ms (DSP: %d ms, NN: %d ms)\n", 
                              cpuFreq, total_infer_time, result.timing.dsp, result.timing.classification);
                Serial.printf("RAM Usage     : %.1f KB / %.1f KB (%.1f%%) | Free: %.1f KB | Max Block: %.1f KB\n",
                              usedHeap / 1024.0, totalHeap / 1024.0, heapUsagePercent, freeHeap / 1024.0, maxAlloc / 1024.0);
                
                Serial.print("Predictions   : ");
                for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
                    Serial.printf("%s: %.2f  ", result.classification[ix].label, result.classification[ix].value);
                }
                Serial.println("\n----------------------------------------");

                // Check if the "Marvin" / "marvin" label has high confidence
                for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
                    if (strcasecmp(result.classification[ix].label, "marvin") == 0) {
                        if (result.classification[ix].value > 0.35) {
                            Serial.println(">>> Wake word 'marvin' detected! Streaming audio to server...");
                            
                            // Switch state to streaming
                            currentState = STREAMING_AUDIO;
                            streamingStartTime = millis();
                            webSocket.sendTXT("START");
                            break;
                        }
                    }
                }
            } else {
                Serial.printf("ERR: run_classifier failed (%d)\n", res);
            }

            // Sliding window: Shift buffer by 4000 samples (250ms step) so we test every 250ms seamlessly
            memmove(&inference_buffer[0], &inference_buffer[4000], (BUFFER_SIZE - 4000) * sizeof(int16_t));
            buffer_index = BUFFER_SIZE - 4000;
        }
    }
  } 
  else if (currentState == STREAMING_AUDIO) {
    // Continuously read audio and stream over WiFi at 16kHz
    i2s_read(I2S_PORT, (void*)i2s_buffer, sizeof(i2s_buffer), &bytes_read, portMAX_DELAY);
    int samples = bytes_read / sizeof(int16_t);
    
    int16_t stream_buf[128];
    int out_samples = 0;
    for (int i = 0; i < samples && out_samples < 128; i += DECIMATION_FACTOR) {
        float raw = (float)(i2s_buffer[i] & 0x0FFF);
        dc_offset = (dc_offset * 0.99) + (raw * 0.01);
        float centered = (raw - dc_offset) * 12.0;
        stream_buf[out_samples++] = (int16_t)constrain(centered, -32768.0, 32767.0);
    }
    
    if (out_samples > 0) {
        webSocket.sendBIN((uint8_t*)stream_buf, out_samples * sizeof(int16_t));
    }
    
    // Stream for exactly 3 seconds
    if (millis() - streamingStartTime > 3000) {
        Serial.println("Finished recording command. Sending DONE to server...");
        webSocket.sendTXT("DONE");
        currentState = WAITING_FOR_COMMAND;
    }
  }
}
