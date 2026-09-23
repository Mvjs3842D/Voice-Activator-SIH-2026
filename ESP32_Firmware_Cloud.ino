#include <WiFi.h>
#include <WebSocketsClient.h>
#include <driver/i2s.h>
#include <driver/adc.h>
#include <Mvjs3842D-project-1_inferencing.h>

// ==========================================
// WiFi & Cloud Server Settings
// ==========================================
const char* ssid     = "SAM";
const char* password = "12345678";

const char* websocket_server = "bore.pub"; 
const uint16_t websocket_port = 53454; // <-- UPDATE THIS FROM COLAB!

WebSocketsClient webSocket;

// Relay Pins
#define RELAY_CH1 16
#define RELAY_CH2 17

// Audio Settings
#define I2S_SAMPLE_RATE 48000
#define DECIMATION_FACTOR 3 
#define I2S_PORT I2S_NUM_0

// Inference parameters
#define BUFFER_SIZE (EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE)
int16_t i2s_buffer[256];
int16_t inference_buffer[BUFFER_SIZE]; 
int buffer_index = 0;

enum State {
  LISTENING_WAKEWORD,
  STREAMING_AUDIO,
  WAITING_FOR_COMMAND
};

State currentState = LISTENING_WAKEWORD;
unsigned long streamingStartTime = 0;
unsigned long lastDashboardPrint = 0;

// Status Tracking Variables
bool isServerConnected = false;
bool isMicConnected = true;
float lastMarvinScore = 0.0;
float lastNoiseScore = 0.0;
String lastCommand = "None";

// Edge impulse callback function
int raw_feature_get_data(size_t offset, size_t length, float *out_ptr) {
    for (size_t i = 0; i < length; i++) {
        out_ptr[i] = (float)inference_buffer[offset + i];
    }
    return 0;
}

// Custom Dashboard Print
void printDashboard(bool force = false) {
    if (!force && (millis() - lastDashboardPrint < 2000)) return; // Print every 2s unless forced
    lastDashboardPrint = millis();

    String wifiStatus = (WiFi.status() == WL_CONNECTED) ? "[✔️] WiFi" : "[❌] WiFi";
    String serverStatus = isServerConnected ? "[✔️] Server" : "[❌] Server";
    String micStatus = isMicConnected ? "[✔️] Mic" : "[❌] Mic";

    String stateStr = "";
    if (currentState == LISTENING_WAKEWORD) stateStr = "IDLE (Listening)";
    else if (currentState == STREAMING_AUDIO) stateStr = "STREAMING (Sending Audio)";
    else stateStr = "WAITING (For Cloud Result)";

    uint32_t freeHeap = ESP.getFreeHeap() / 1024;
    uint32_t totalHeap = ESP.getHeapSize() / 1024;

    Serial.println("\n==================================================");
    Serial.printf("%s   %s   %s\n", wifiStatus.c_str(), serverStatus.c_str(), micStatus.c_str());
    Serial.printf("CPU: %u MHz | RAM: %u KB Free / %u KB Total\n", ESP.getCpuFreqMHz(), freeHeap, totalHeap);
    Serial.printf("State: %s\n", stateStr.c_str());
    Serial.printf("AI Scores -> Marvin: %.2f | Noise: %.2f\n", lastMarvinScore, lastNoiseScore);
    if (lastCommand != "None") {
        Serial.printf("Last Action: %s\n", lastCommand.c_str());
    }
    Serial.println("==================================================");
}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      if (isServerConnected) {
          isServerConnected = false;
          Serial.println("\n[⚠️] Disconnected from Cloud Server!");
          printDashboard(true);
      }
      break;

    case WStype_CONNECTED:
      isServerConnected = true;
      Serial.println("\n[🟢] CONNECTED successfully to Cloud Server!");
      printDashboard(true);
      break;

    case WStype_TEXT: {
      String msg = (char*)payload;
      
      Serial.println("\n==================================================");
      Serial.println("📩 COMMAND RECEIVED FROM CLOUD:");
      Serial.println(msg);
      
      if (msg.indexOf("LAMP_ON") >= 0) {
          digitalWrite(RELAY_CH1, LOW); 
          lastCommand = "💡 TURNED LIGHT ON";
          Serial.println(">>> Physical Relay 1 -> ON");
      }
      else if (msg.indexOf("LAMP_OFF") >= 0) {
          digitalWrite(RELAY_CH1, HIGH);  
          lastCommand = "🌑 TURNED LIGHT OFF";
          Serial.println(">>> Physical Relay 1 -> OFF");
      }
      else {
          lastCommand = "❓ Unrecognized Command";
      }
      Serial.println("==================================================\n");
      
      currentState = LISTENING_WAKEWORD;
      buffer_index = 0; 
      printDashboard(true);
      break;
    }

    case WStype_ERROR:
      Serial.printf("[❌] WebSocket Error!\n");
      break;
  }
}

void setup() {
  Serial.begin(115200);
  
  pinMode(RELAY_CH1, OUTPUT);
  pinMode(RELAY_CH2, OUTPUT);
  
  digitalWrite(RELAY_CH1, HIGH); 
  digitalWrite(RELAY_CH2, LOW);
  
  Serial.print("\nConnecting to WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[✔️] WiFi Connected.");
  
  webSocket.begin(websocket_server, websocket_port, "/");
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
  
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
  i2s_set_adc_mode(ADC_UNIT_1, ADC1_CHANNEL_4); 
  adc1_config_channel_atten(ADC1_CHANNEL_4, ADC_ATTEN_DB_11);
  i2s_adc_enable(I2S_PORT);
}

void loop() {
  webSocket.loop();
  
  size_t bytes_read;
  static float dc_offset = 1450.0;

  if (currentState == LISTENING_WAKEWORD) {
    printDashboard(); // Will only print every 2 seconds
    
    i2s_read(I2S_PORT, (void*)i2s_buffer, sizeof(i2s_buffer), &bytes_read, portMAX_DELAY);
    int samples = bytes_read / sizeof(int16_t);
    
    for (int i = 0; i < samples; i += DECIMATION_FACTOR) {
        float raw = (float)(i2s_buffer[i] & 0x0FFF);
        dc_offset = (dc_offset * 0.99) + (raw * 0.01);
        float centered = (raw - dc_offset) * 12.0;
        int16_t sample = (int16_t)constrain(centered, -32768.0, 32767.0);
        
        // Quick Mic Diagnostic every 16000 samples
        static int debug_counter = 0;
        if (++debug_counter >= 16000) {
            isMicConnected = !(raw < 500.0 || raw > 4000.0);
            debug_counter = 0;
        }
        
        inference_buffer[buffer_index++] = sample;
        
        if (buffer_index >= BUFFER_SIZE) {
            signal_t features_signal;
            features_signal.total_length = BUFFER_SIZE;
            features_signal.get_data = &raw_feature_get_data;
            
            ei_impulse_result_t result = { 0 };
            EI_IMPULSE_ERROR res = run_classifier(&features_signal, &result, false);
            
            if (res == EI_IMPULSE_OK) {
                // Update AI Scores for the Dashboard
                for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
                    if (strcasecmp(result.classification[ix].label, "marvin") == 0) {
                        lastMarvinScore = result.classification[ix].value;
                    } else if (strcasecmp(result.classification[ix].label, "noise") == 0) {
                        lastNoiseScore = result.classification[ix].value;
                    }
                }

                // Check Wake Word Trigger
                if (lastMarvinScore > 0.35) {
                    Serial.println("\n[🎙️] WAKE WORD DETECTED! 'Marvin'");
                    currentState = STREAMING_AUDIO;
                    streamingStartTime = millis();
                    webSocket.sendTXT("START");
                    printDashboard(true);
                    break;
                }
            }

            memmove(&inference_buffer[0], &inference_buffer[4000], (BUFFER_SIZE - 4000) * sizeof(int16_t));
            buffer_index = BUFFER_SIZE - 4000;
        }
    }
  } 
  else if (currentState == STREAMING_AUDIO) {
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
    
    if (millis() - streamingStartTime > 3000) {
        Serial.println("[📤] Finished recording. Sent audio to cloud. Waiting for command...");
        webSocket.sendTXT("DONE");
        currentState = WAITING_FOR_COMMAND;
        printDashboard(true);
    }
  }
}
