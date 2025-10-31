#include <WiFi.h>              // WiFi para ESP32
#include <PubSubClient.h>      // Cliente MQTT
#include <ArduinoJson.h>       // Para construir el JSON
#include <time.h>              // Para NTP y timestamps ISO

// ---- CONFIGURACIÓN DE RED ----
const char* ssid = "cubicua";       //cubicua SSID de tu WiFi "LOPRICO_60F0"
const char* password = "";          // Password WiFi  "Sehacaalan579$$%"

// ---- CONFIGURACIÓN DEL BROKER MQTT ----
const char* mqtt_server = "test.mosquitto.org"; // 172.29.41.88 IP de tu broker Mosquitto local
const int   mqtt_port   = 1883;           // Puerto MQTT estándar

WiFiClient espClient;                  // Socket TCP/IP
PubSubClient client(espClient);        // Cliente MQTT sobre ese socket

// ---- IDENTIDAD / TOPICS ----
const char* SENSOR_ID = "LAB08JAV_G7";  // id único de tu semáforo
// DATOS DE VUESTRO GRUPO (LAB08JAV G7) sacados del Excel del profe:
// street_id: ST_0662, distrito/barrio: San Blas-Canillejas, bidireccional
const char* STREET_ID = "ST_0662";
const char* DISTRICT = "San Blas-Canillejas";
const char* NEIGHBORHOOD = "San Blas-Canillejas";

// Construimos los topics siguiendo la convención
String topic = String("sensors/") + STREET_ID + "/" + SENSOR_ID; 
// ---- PINES DEL SEMÁFORO (ESP32-WROOM-32) ----
const int PIN_RED    = 25;   // LED rojo    (GPIO25)
const int PIN_YELLOW = 26;   // LED amarillo(GPIO26)
const int PIN_GREEN  = 27;   // LED verde   (GPIO27)

// ---- BUZZER ----

// Pin del buzzer (GPIO33 es una buena opción en ESP32-WROOM-32)
const int PIN_BUZZER = 33;

// (Opcional) Botón de peatón: pull-up interno y a GND
const int PIN_PED_BUTTON = 16;    // Cambia si conectas un botón físico
bool pedestrianButtonPressed = false; // Estado latched del botón

// ---- LÓGICA DEL CICLO DE SEMÁFORO ----
// Duraciones de cada fase en segundos (ajústalas a lo que necesites)
uint16_t DURATION_GREEN  = 10;
uint16_t DURATION_YELLOW = 2;
uint16_t DURATION_RED    = 10;
// Suma total del ciclo (para "cycle_duration_seconds")
uint16_t CYCLE_DURATION = DURATION_GREEN + DURATION_YELLOW + DURATION_RED;

// Enumeración de estados del semáforo
enum State { GREEN, YELLOW, RED };
State currentState = RED;           // Estado inicial
State prevState    = RED;           // Para detectar cambios
unsigned long stateStartMillis = 0; // Cuándo empezó el estado actual (millis)
unsigned long lastPublishMillis = 0;// Para publicar cada X ms
uint32_t cycleCount = 0;            // Número de ciclos completos
String lastStateChangeISO; // última marca de cambio de estado


// ---- NTP (hora real para timestamps ISO) ----
// Zona horaria de Madrid: CET/CEST
const char* TZ_EUROPE_MADRID = "CET-1CEST,M3.5.0/2,M10.5.0/3";
// Servidores NTP
const char* NTP1 = "pool.ntp.org";
const char* NTP2 = "time.google.com";

// ---- FUNCIONES AUXILIARES ----

// Conectase a la wifi
void setup_wifi() {
  delay(10);
  Serial.println("\nConectando a la red WiFi...");
  WiFi.begin(ssid, password);   // Iniciar conexión wifi con ssid y password

  while (WiFi.status() != WL_CONNECTED) {// Espera hasta conectar
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi conectado."); // Esto para el debugging
  Serial.print("Direccion IP: ");
  Serial.println(WiFi.localIP());
}

// Callback cuando llega un mensaje MQTT 
void callback(char* topic, byte* message, unsigned int length) {
  Serial.print("Mensaje recibido en el topic: ");
  Serial.println(topic);

  // Convertir el mensaje recibido a texto legible
  String msg = "";
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)message[i];
  }
  Serial.print("Contenido: "); // Más debugging
  Serial.println(msg);

  // --- Interpretar la orden ---
  if (msg == "red") {
    currentState = RED;
    applyOutputs(RED);
    publishTrafficLightState(true);
    client.loop();
    delay(5000);
  } 
  else if (msg == "green") {
    currentState = GREEN;
    applyOutputs(GREEN);
    publishTrafficLightState(true);
    client.loop();
    delay(5000);
  } 
  else if (msg == "yellow") {
    currentState = YELLOW;
    applyOutputs(YELLOW);
    publishTrafficLightState(true);
    client.loop();
    delay(5000);
  }else if(msg =="Night_mode"){
     DURATION_GREEN  = 0;
     DURATION_YELLOW = 1;
     DURATION_RED    = 0;
  }else if(msg =="Day_mode"){
     DURATION_GREEN  = 10;
     DURATION_YELLOW = 2;
     DURATION_RED    = 10;
  }
  else {
    Serial.println("La orden no fue interpretada");
  }
}

// Reintento de conexión a MQTT
void reconnect() {
  while (!client.connected()) {
    Serial.print("Intentando conectar al broker MQTT...");
    String clientId = "LAB08JAV_G7"; // Pongo es ID por poner uno, pero es necesario
    if (client.connect(clientId.c_str())) {
      Serial.println("Conectado al broker MQTT.");
      client.subscribe(topic.c_str()); // Suscrubirme al topic para que funcione
    } else {
      Serial.print("Fallo, rc="); // Mas debugging para ver si se está conectando o pasan cosas raras
      Serial.print(client.state());
      Serial.println(" reintentando en 5 segundos");
      delay(5000);
    }
  }
}

// Arranca NTP y espera una hora válida
void setupTime() {
  configTzTime(TZ_EUROPE_MADRID, NTP1, NTP2); // Configura zona y servidores
  Serial.print("Sincronizando NTP");
  time_t now = time(nullptr);
  int retries = 0;
  while (now < 8 * 3600 * 2 && retries < 60) { // Espera hasta que haya epoch razonable
    Serial.print(".");
    delay(500);
    now = time(nullptr);
    retries++;
  }
  Serial.println();
  if (now >= 8 * 3600 * 2) {
    Serial.println("Hora NTP sincronizada.");
  } else {
    Serial.println("No se pudo sincronizar NTP (se enviarán tiempos relativos).");
  }
}


// Convierte time_t a ISO-8601 "YYYY-MM-DDThh:mm:ss.ssssss"
String toISO8601(time_t t) {
  struct tm timeinfo;
  gmtime_r(&t, &timeinfo);            // UTC para ISO canónico
  char buf[40];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
  // Añadimos microsegundos "falsos" derivados de millis() para cumplir formato
  char out[60];
  snprintf(out, sizeof(out), "%s.%06lu", buf, (unsigned long)(micros() % 1000000));
  return String(out);
}

// Obtiene ahora en ISO-8601 (si no hay NTP, usa millis() aproximado)
String nowISO() {
  time_t now = time(nullptr);
  if (now > 100000) return toISO8601(now);
  // Fallback si no hay NTP: marca relativa desde arranque (no ideal, pero válido)
  unsigned long ms = millis();
  unsigned long secs = ms / 1000;
  char buf[40];
  snprintf(buf, sizeof(buf), "1970-01-01T00:%02lu:%02lu.%06lu",
           (secs/60)%60, secs%60, (unsigned long)(ms%1000)*1000);
  return String(buf);
}

// Enciende/apaga LEDs según estado
void applyOutputs(State s) {
// Apagar todos los LEDs primero
digitalWrite(PIN_GREEN, LOW);
digitalWrite(PIN_YELLOW, LOW);
digitalWrite(PIN_RED, LOW);

// Encender solo el que corresponda al estado actual
if (s == GREEN) {
  digitalWrite(PIN_GREEN, HIGH);
}
else if (s == YELLOW) {
  digitalWrite(PIN_YELLOW, HIGH);
}
else if (s == RED) {
  digitalWrite(PIN_RED, HIGH);
}

if (s == RED) {
    buzzerOn();
  } else {
    buzzerOff();
  }

}

// Calcula en qué segundo del ciclo estamos (0..CYCLE_DURATION-1)
uint16_t cyclePositionSeconds() {
  unsigned long elapsed = (millis() - stateStartMillis) / 1000; // segs en este estado
  // Posición dentro del ciclo completo:
  uint16_t pos = 0;
  switch (currentState) {
    case GREEN:  pos = elapsed; break;
    case YELLOW: pos = DURATION_GREEN + elapsed; break;
    case RED:    pos = DURATION_GREEN + DURATION_YELLOW + elapsed; break;
  }
  // Acota por si hemos sobrepasado
  if (pos >= CYCLE_DURATION) pos = CYCLE_DURATION - 1;
  return pos;
}

// Segundos que quedan para cambiar de estado dentro del ciclo
uint16_t timeRemainingSeconds() {
  unsigned long elapsed = (millis() - stateStartMillis) / 1000;
  uint16_t rem = 0;
  switch (currentState) {
    case GREEN:  rem = (elapsed >= DURATION_GREEN ) ? 0 : (DURATION_GREEN  - elapsed); break;
    case YELLOW: rem = (elapsed >= DURATION_YELLOW) ? 0 : (DURATION_YELLOW - elapsed); break;
    case RED:    rem = (elapsed >= DURATION_RED   ) ? 0 : (DURATION_RED    - elapsed); break;
  }
  return rem;
}

// Enciende el buzzer (solo para buzzer activo)
void buzzerOn() {
  digitalWrite(PIN_BUZZER, HIGH);
}

// Apaga el buzzer
void buzzerOff() {
  digitalWrite(PIN_BUZZER, LOW);
}

// Publica el JSON en topicState respetando la estructura del sistema
void publishTrafficLightState(bool stateChanged) {
  // Construimos el JSON y lo mandamos al topic
  StaticJsonDocument<1024> doc;

  // Envoltorio común (ver práctica): sensor_id, sensor_type, street_id, timestamp, location
  doc["sensor_id"]  = SENSOR_ID;
  doc["sensor_type"] = "traffic_light";  // Tipo de sensor según práctica
  doc["street_id"]  = STREET_ID;
  doc["timestamp"]  = nowISO();          // Marca de tiempo del mensaje

  // Ubicación ampliada (puedes completar lat/long si lo necesitas)
  JsonObject loc = doc.createNestedObject("location");
  // Si quieres ser preciso, usa los del Excel de G7; aquí los dejamos como ejemplo
  loc["latitude"]  = 40,4499298;            // (opcional) podrías poner el inicio/fin medios
  loc["longitude"] = -3,6115262;
  loc["altitude_meters"] = 0.0;          // si no la conoces, 0.0
  loc["district"] = DISTRICT;            // San Blas-Canillejas (G7)
  loc["neighborhood"] = NEIGHBORHOOD;    // San Blas-Canillejas (G7)

  // ----- Bloque data específico de "traffic_light" -----
  JsonObject data = doc.createNestedObject("data");

  // current_state (green/yellow/red)
const char* stateStr;

if (currentState == GREEN) {
  stateStr = "green";
} 
else if (currentState == YELLOW) {
  stateStr = "yellow";
} 
else {
  stateStr = "red";
}
                          
  data["current_state"] = stateStr;

  // Posición dentro del ciclo y tiempos
  data["cycle_position_seconds"] = cyclePositionSeconds();
  data["time_remaining_seconds"] = timeRemainingSeconds();
  data["cycle_duration_seconds"] = CYCLE_DURATION;

  // Tipología del semáforo y circulación (de vuestro Excel: bidireccional=1)
  data["traffic_light_type"] = "vehicle_only";     // Solo vehículos en este ejemplo
  data["circulation_direction"] = "bidirectional"; // G7: is_bidirectional = 1

  // Estados de peatones (si añades botón real, actualiza estas flags)
  bool pedestrian_waiting = false;                 // lógica extra si detectas colas
  data["pedestrian_waiting"] = pedestrian_waiting;
  data["pedestrian_button_pressed"] = pedestrianButtonPressed;

  // Fallos y telemetría adicional
  bool malfunction_detected = false;               // puedes poner watchdogs/diagnóstico
  data["malfunction_detected"] = malfunction_detected;
  data["cycle_count"] = cycleCount;                // nº de ciclos completados
  data["state_changed"] = stateChanged;            // true si acaba de cambiar

  // Marca de tiempo del último cambio de estado
  static String lastChangeISO = nowISO();          // cache de la última ISO
  data["last_state_change"] = lastChangeISO;

  // Publicamos al topic y mostramos el paquete con un string
  String payload;
  serializeJson(doc, payload);
  bool ok = client.publish(topic.c_str(), payload.c_str()); 
  if (ok) {
    Serial.println("[MQTT] Estado publicado en " + topic);
    Serial.println(payload);
  } else {
    Serial.println("[MQTT] ERROR al publicar estado.");
  }
}

// Cambia el estado cuando expire su duración
void updateStateMachine() {

    if (pedestrianButtonPressed && currentState != RED) {

    // 1) Pasar a AMARILLO
    currentState = YELLOW;
    stateStartMillis = millis();
    lastStateChangeISO = nowISO();   // marca cambio de estado
    applyOutputs(YELLOW);

    bool firstSend = true;
    for (uint16_t i = 0; i < DURATION_YELLOW; i++) {
      publishTrafficLightState(firstSend);  // solo la 1ª vez: state_changed=true
      firstSend = false;
      client.loop();                         // mantiene MQTT (muy importante)
      delay(1000);
    }

    // 2) Pasar a ROJO
    currentState = RED;
    stateStartMillis = millis();
    lastStateChangeISO = nowISO();   // marca cambio de estado
    applyOutputs(RED);

    firstSend = true;
    for (uint16_t j = 0; j < DURATION_RED; j++) {
      publishTrafficLightState(firstSend);
      firstSend = false;
      client.loop();                 // mantiene MQTT vivo
      delay(1000);
    }

    Serial.println(">> Botón: Semáforo forzado a ROJO."); //Más debug
    pedestrianButtonPressed = false;
    return; // salimos de la función para seguir el ciclo normal
  }


// Este es el ciclo normal
  unsigned long elapsed = (millis() - stateStartMillis) / 1000; // segs en estado actual
  bool changed = false;

  switch (currentState) {
    case GREEN:
      if (elapsed >= DURATION_GREEN ) { // Cuando el tiempo transcurrido pasa el maximo, cambiamos de estado
        currentState = YELLOW;
        stateStartMillis = millis();
        changed = true;
      }
      break;
    case YELLOW:
      if (elapsed >= DURATION_YELLOW) {
        currentState = RED;
        stateStartMillis = millis();
        changed = true;
      }
      break;
    case RED:
      if (elapsed >= DURATION_RED) {
        currentState = GREEN;
        stateStartMillis = millis();
        changed = true;
        cycleCount++; // Completamos un ciclo cuando pasamos de rojo a verde
      }
      break;
  }

  if (changed) {
    // Guarda momento del cambio para el campo "last_state_change"
    prevState = currentState;
    applyOutputs(currentState); // Aplicamos el cambio de estadp
    publishTrafficLightState(true); // Cambiamos al estado 
  }
}

// Lee el botón de peatones (si está cableado)
void readPedestrianButton() {
  int val = digitalRead(PIN_PED_BUTTON); // Leemos -> PIN HIGH en reposo y LOW cuando pulsamos
  // Ambos son static para que se inicialize una vez y conserve el valor entre llamadas
  static unsigned long lastDebounce = 0; // Guarda la última vez en ms que hicimos una comprobación
  static int lastStable = HIGH;  // Guarda el estado del boton (Empezamos en HIGH) 
  if (millis() - lastDebounce > 30) {   // simple debounce 30ms
    if (val != lastStable) { // Si cambia el estado antiguo y la nueva leida
      lastStable = val; // Cambia al actual
      if (val == LOW) {  //Si es LOW es que se ha pulsado el boton
        pedestrianButtonPressed = true; // cambiamos el valor del campo para esta iteracion
        Serial.println("Botón de peatón PULSADO.");
      }
    }
    lastDebounce = millis();
  }
}

// ---- SETUP ----
void setup() {
  Serial.begin(115200);                  // Velocidad serie
  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_YELLOW, OUTPUT);
  pinMode(PIN_GREEN, OUTPUT);
  pinMode(PIN_PED_BUTTON, INPUT_PULLUP); // Botón con pull-up
  
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);  // Asegura que arranca apagado
  
  applyOutputs(currentState);            // Aplicamos el estado incial --> ROJO

  setup_wifi();                          // Conecta WiFi
  setupTime();                           // Sincroniza hora (para timestamps)
  lastStateChangeISO = nowISO();
  client.setServer(mqtt_server, mqtt_port); // Iniciamos el Broker (MQTT) con su puerto
  client.setCallback(callback);             // Para 
  stateStartMillis = millis();           // Marca inicio del estado inicial
}

// ---- LOOP ----
void loop() {
  // Garantiza que estamos conectados a MQTT
  if (!client.connected()) {
    reconnect(); // Para reconecatar si la sesión se cae
  }
  client.loop(); // Mantenemos la conexión (Bidireccionalidad) y atiende a publicaciones del topioc 

  // La lógica del semaforo
  readPedestrianButton(); // Comprueba que el sensor del Boton ha sido pulsado
  updateStateMachine(); // Actualizamos el estado de la maquina y los campos a enviar del estado del semaforo y aplicamos al ESP32 los cambios
                        // Aunque si cambia de estado o se pullsa el botón, también publicamos

  // Publicamos cada 1 segundo --> Millis devuelve los ms pasados desde encendido
  if (millis() - lastPublishMillis > 1000) {
    lastPublishMillis = millis();
    // Publica con state_changed=false en envío periódico
    publishTrafficLightState(false);
    // Tras publicar, reseteamos 
    pedestrianButtonPressed = false;
  }
 
}
