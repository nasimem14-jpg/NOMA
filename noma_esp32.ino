/*
  NOMA - Firmware v1 para CrowPanel 1.46" (ESP32-S3, DHR55146D)
  =================================================================
  Primera versión: incluye solo lo 100% viable con este hardware.
  NO incluye (necesitan piezas extra o no son posibles en esta placa,
  ver conversación): notificaciones/mensajes del iPad (Bluetooth ANCS),
  clima (API externa), calendario (sincronización), música (sin altavoz).

  Pantallas incluidas:
    HOME         -> reloj + estado, pulsa la corona para hablarle
    ESCUCHANDO   -> grabando tu voz
    PROCESANDO   -> esperando respuesta de Supabase/gpt-6-astra
    RESPUESTA    -> texto de la respuesta de Noma
    WIFI         -> red conectada
    TEMPORIZADOR -> cuenta atrás simple (fijo a 5 min, pulsa para iniciar/pausar)
    ALARMAS      -> una alarma on/off (hora fija por ahora, editar hora = v2)
    COMANDOS     -> lista estática de lo que puedes preguntarle
    AJUSTES      -> info del dispositivo (IP, memoria libre)

  Gira la corona para moverte entre pantallas. Pulsa la corona:
    - en HOME: empieza a escucharte
    - en TEMPORIZADOR: inicia/pausa la cuenta atrás
    - en ALARMAS: activa/desactiva la alarma

  AVISO HONESTO sobre "Batería": esta placa no tiene, según la
  documentación de Elecrow, un pin de lectura de batería confirmado,
  así que esa pantalla se deja fuera de esta v1 hasta confirmar si tu
  placa tiene un divisor de voltaje conectado a algún ADC.

  LIBRERÍAS NECESARIAS: LovyanGFX, ArduinoJson
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <SPIFFS.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ---------------------- CONFIGURACIÓN ----------------------

const char* WIFI_SSID     = "PON_TU_WIFI";
const char* WIFI_PASSWORD = "PON_TU_CONTRASEÑA";

const char* SUPABASE_FUNCTION_URL = "https://TU_PROYECTO.supabase.co/functions/v1/noma-chat";
const char* SUPABASE_ANON_KEY     = "PON_TU_ANON_KEY";
const char* DEVICE_ID              = "reloj-1";

#define SEGUNDOS_GRABAR 5

// Pines confirmados (wiki oficial de Elecrow, modelo DHR55146D)
#define PIN_SWITCH_CORONA 41
#define PIN_ENCODER_A     45
#define PIN_ENCODER_B     42
#define PIN_MIC_CLK       39   // antes "I2C_SCL"
#define PIN_MIC_DATA      38   // antes "I2C_SDA"

const int FRECUENCIA_MUESTREO = 16000;

// Paleta de color estilo Noma (azul/cian sobre negro-azulado)
#define COLOR_FONDO   0x0006
#define COLOR_ACCENT  0x07FF   // cian
#define COLOR_TEXTO   0xFFFF
#define COLOR_MUTED   0x4A69

// ---------------------- PANTALLA ----------------------

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel_instance; // si la pantalla no enciende bien, prueba Panel_ST7789
  lgfx::Bus_SPI _bus_instance;

public:
  LGFX(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 80000000;
      cfg.freq_read = 20000000;
      cfg.spi_3wire = true;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 10;
      cfg.pin_mosi = 11;
      cfg.pin_miso = -1;
      cfg.pin_dc = 3;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs = 9;
      cfg.pin_rst = 14;
      cfg.pin_busy = -1;
      cfg.memory_width = 360;
      cfg.memory_height = 360;
      cfg.panel_width = 360;
      cfg.panel_height = 360;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.readable = false;
      cfg.invert = false;
      cfg.rgb_order = true;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel_instance.config(cfg);
    }
    setPanel(&_panel_instance);
  }
};

LGFX pantalla;

// ---------------------- ESTADO ----------------------

enum Pantalla {
  HOME, ESCUCHANDO, PROCESANDO, RESPUESTA,
  WIFI_INFO, TEMPORIZADOR, ALARMAS, COMANDOS, AJUSTES,
  NUM_PANTALLAS_MENU // marcador: cuántas hay en el menú navegable (desde WIFI_INFO)
};

Pantalla pantallaActual = HOME;
String textoRespuesta = "";

// Temporizador
bool temporizadorCorriendo = false;
unsigned long temporizadorInicio = 0;
const unsigned long TEMPORIZADOR_DURACION_MS = 5UL * 60UL * 1000UL; // 5 min fijo (v1)

// Alarma (única, hora fija 08:00 -- editar hora será v2)
bool alarmaActiva = false;

// Encoder
volatile int posicionEncoder = 0;
int ultimaPosicionEncoder = 0;

void IRAM_ATTR onEncoderA() {
  bool a = digitalRead(PIN_ENCODER_A);
  bool b = digitalRead(PIN_ENCODER_B);
  posicionEncoder += (a == b) ? 1 : -1;
}

// ---------------------- DIBUJO DE CADA PANTALLA ----------------------

void dibujarCabecera(const char* titulo) {
  pantalla.fillScreen(COLOR_FONDO);
  pantalla.setTextColor(COLOR_ACCENT);
  pantalla.setTextDatum(lgfx::top_center);
  pantalla.setTextSize(2);
  pantalla.drawString("NOMA", 180, 30);
  pantalla.setTextColor(COLOR_MUTED);
  pantalla.setTextSize(1);
  pantalla.drawString(titulo, 180, 60);
}

void dibujarAnillo(int progresoPorcentaje, uint32_t color) {
  // Anillo decorativo tipo el de la maqueta, usando arcos
  pantalla.drawArc(180, 190, 90, 80, 0, 360, COLOR_MUTED);
  if (progresoPorcentaje > 0) {
    float grados = 360.0 * progresoPorcentaje / 100.0;
    pantalla.drawArc(180, 190, 90, 80, 0, grados, color);
  }
}

void pantallaHome() {
  dibujarCabecera("TU ASISTENTE, SIEMPRE CONTIGO");
  dibujarAnillo(100, COLOR_ACCENT);
  pantalla.setTextColor(COLOR_TEXTO);
  pantalla.setTextDatum(lgfx::middle_center);
  pantalla.setTextSize(3);
  pantalla.drawString("Pulsa para hablar", 180, 190);
  pantalla.setTextColor(COLOR_MUTED);
  pantalla.setTextSize(1);
  pantalla.drawString(WiFi.status() == WL_CONNECTED ? "Wi-Fi conectado" : "Sin Wi-Fi", 180, 320);
}

void pantallaEscuchando() {
  dibujarCabecera("ESCUCHANDO...");
  dibujarAnillo(60, COLOR_ACCENT);
  pantalla.setTextColor(COLOR_TEXTO);
  pantalla.setTextDatum(lgfx::middle_center);
  pantalla.setTextSize(2);
  pantalla.drawString("Habla ahora", 180, 190);
}

void pantallaProcesando() {
  dibujarCabecera("PROCESANDO TU SOLICITUD...");
  dibujarAnillo(30, 0xFFE0); // amarillo
  pantalla.setTextColor(COLOR_TEXTO);
  pantalla.setTextDatum(lgfx::middle_center);
  pantalla.setTextSize(2);
  pantalla.drawString("Un momento...", 180, 190);
}

void pantallaRespuesta() {
  pantalla.fillScreen(COLOR_FONDO);
  pantalla.setTextColor(COLOR_ACCENT);
  pantalla.setTextDatum(lgfx::top_center);
  pantalla.setTextSize(2);
  pantalla.drawString("NOMA", 180, 30);
  pantalla.setTextColor(COLOR_TEXTO);
  pantalla.setTextDatum(lgfx::top_left);
  pantalla.setTextSize(2);
  pantalla.setTextWrap(true);
  pantalla.setCursor(45, 110);
  pantalla.print(textoRespuesta);
  pantalla.setTextColor(COLOR_MUTED);
  pantalla.setTextDatum(lgfx::bottom_center);
  pantalla.setTextSize(1);
  pantalla.drawString("Pulsa para volver", 180, 340);
}

void pantallaWifi() {
  dibujarCabecera("WI-FI");
  dibujarAnillo(100, COLOR_ACCENT);
  pantalla.setTextColor(COLOR_TEXTO);
  pantalla.setTextDatum(lgfx::middle_center);
  pantalla.setTextSize(2);
  if (WiFi.status() == WL_CONNECTED) {
    pantalla.drawString("Conectado", 180, 175);
    pantalla.setTextSize(1);
    pantalla.drawString(WiFi.SSID(), 180, 210);
  } else {
    pantalla.drawString("Sin conexion", 180, 190);
  }
}

void pantallaTemporizador() {
  dibujarCabecera("TEMPORIZADOR");
  long restanteMs = TEMPORIZADOR_DURACION_MS;
  if (temporizadorCorriendo) {
    long transcurrido = millis() - temporizadorInicio;
    restanteMs = max(0L, (long)(TEMPORIZADOR_DURACION_MS - transcurrido));
  }
  int minutos = (restanteMs / 1000) / 60;
  int segundos = (restanteMs / 1000) % 60;
  char buffer[8];
  snprintf(buffer, sizeof(buffer), "%02d:%02d", minutos, segundos);

  int progreso = 100 - (restanteMs * 100 / TEMPORIZADOR_DURACION_MS);
  dibujarAnillo(progreso, COLOR_ACCENT);

  pantalla.setTextColor(COLOR_TEXTO);
  pantalla.setTextDatum(lgfx::middle_center);
  pantalla.setTextSize(4);
  pantalla.drawString(buffer, 180, 190);
  pantalla.setTextColor(COLOR_MUTED);
  pantalla.setTextSize(1);
  pantalla.drawString(temporizadorCorriendo ? "Pulsa para pausar" : "Pulsa para iniciar", 180, 250);
}

void pantallaAlarmas() {
  dibujarCabecera("ALARMAS");
  dibujarAnillo(100, alarmaActiva ? COLOR_ACCENT : COLOR_MUTED);
  pantalla.setTextColor(COLOR_TEXTO);
  pantalla.setTextDatum(lgfx::middle_center);
  pantalla.setTextSize(3);
  pantalla.drawString("08:00", 180, 175);
  pantalla.setTextColor(alarmaActiva ? COLOR_ACCENT : COLOR_MUTED);
  pantalla.setTextSize(1);
  pantalla.drawString(alarmaActiva ? "ACTIVADA" : "DESACTIVADA", 180, 210);
  pantalla.drawString("Pulsa para cambiar", 180, 250);
}

void pantallaComandos() {
  dibujarCabecera("COMANDOS DE VOZ");
  pantalla.setTextColor(COLOR_TEXTO);
  pantalla.setTextDatum(lgfx::top_left);
  pantalla.setTextSize(1);
  pantalla.setCursor(55, 110);
  pantalla.println("- Que puedes hacer?");
  pantalla.println("");
  pantalla.println("- Que hora es?");
  pantalla.println("");
  pantalla.println("- Ayudame a decidir X");
  pantalla.println("");
  pantalla.println("- Cualquier pregunta libre");
}

void pantallaAjustes() {
  dibujarCabecera("AJUSTES");
  pantalla.setTextColor(COLOR_TEXTO);
  pantalla.setTextDatum(lgfx::top_left);
  pantalla.setTextSize(1);
  pantalla.setCursor(45, 110);
  pantalla.print("IP: ");
  pantalla.println(WiFi.localIP());
  pantalla.print("Red: ");
  pantalla.println(WiFi.SSID());
  pantalla.print("Memoria libre: ");
  pantalla.print(ESP.getFreeHeap() / 1024);
  pantalla.println(" KB");
  pantalla.print("Dispositivo: ");
  pantalla.println(DEVICE_ID);
}

void dibujarPantallaActual() {
  switch (pantallaActual) {
    case HOME:          pantallaHome(); break;
    case ESCUCHANDO:     pantallaEscuchando(); break;
    case PROCESANDO:     pantallaProcesando(); break;
    case RESPUESTA:      pantallaRespuesta(); break;
    case WIFI_INFO:      pantallaWifi(); break;
    case TEMPORIZADOR:   pantallaTemporizador(); break;
    case ALARMAS:        pantallaAlarmas(); break;
    case COMANDOS:       pantallaComandos(); break;
    case AJUSTES:        pantallaAjustes(); break;
    default: break;
  }
}

// ---------------------- MICRÓFONO PDM ----------------------

void iniciarI2SPDM() {
  i2s_config_t config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
    .sample_rate = FRECUENCIA_MUESTREO,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = 0,
    .dma_buf_count = 4,
    .dma_buf_len = 1024,
  };
  i2s_pin_config_t pines = {
    .bck_io_num = I2S_PIN_NO_CHANGE,
    .ws_io_num = PIN_MIC_CLK,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = PIN_MIC_DATA,
  };
  i2s_driver_install(I2S_NUM_0, &config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pines);
}

void escribirCabeceraWav(File &f, uint32_t tamanoDatos) {
  uint32_t tamanoArchivo = tamanoDatos + 36;
  uint32_t byteRate = FRECUENCIA_MUESTREO * 2;
  f.seek(0);
  f.write((const uint8_t*)"RIFF", 4);
  f.write((uint8_t*)&tamanoArchivo, 4);
  f.write((const uint8_t*)"WAVE", 4);
  f.write((const uint8_t*)"fmt ", 4);
  uint32_t subchunk1Size = 16;
  uint16_t audioFormat = 1, numCanales = 1;
  f.write((uint8_t*)&subchunk1Size, 4);
  f.write((uint8_t*)&audioFormat, 2);
  f.write((uint8_t*)&numCanales, 2);
  uint32_t sampleRate = FRECUENCIA_MUESTREO;
  f.write((uint8_t*)&sampleRate, 4);
  f.write((uint8_t*)&byteRate, 4);
  uint16_t blockAlign = 2, bitsPerSample = 16;
  f.write((uint8_t*)&blockAlign, 2);
  f.write((uint8_t*)&bitsPerSample, 2);
  f.write((const uint8_t*)"data", 4);
  f.write((uint8_t*)&tamanoDatos, 4);
}

void grabarAudio() {
  pantallaActual = ESCUCHANDO;
  dibujarPantallaActual();

  File archivo = SPIFFS.open("/pregunta.wav", FILE_WRITE);
  escribirCabeceraWav(archivo, 0);

  const int TAM_BUFFER = 512;
  int16_t buffer[TAM_BUFFER];
  size_t bytesLeidos;
  uint32_t totalEscrito = 0;

  unsigned long inicio = millis();
  while (millis() - inicio < SEGUNDOS_GRABAR * 1000) {
    i2s_read(I2S_NUM_0, buffer, sizeof(buffer), &bytesLeidos, portMAX_DELAY);
    archivo.write((uint8_t*)buffer, bytesLeidos);
    totalEscrito += bytesLeidos;
  }

  escribirCabeceraWav(archivo, totalEscrito);
  archivo.close();
}

void enviarYMostrar() {
  pantallaActual = PROCESANDO;
  dibujarPantallaActual();

  File f = SPIFFS.open("/pregunta.wav", FILE_READ);
  if (!f) {
    textoRespuesta = "Error al leer el audio grabado.";
    pantallaActual = RESPUESTA;
    dibujarPantallaActual();
    return;
  }

  HTTPClient http;
  http.begin(SUPABASE_FUNCTION_URL);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);

  String limite = "----NomaBoundary";
  String inicioParte =
    "--" + limite + "\r\n"
    "Content-Disposition: form-data; name=\"device_id\"\r\n\r\n" +
    String(DEVICE_ID) + "\r\n" +
    "--" + limite + "\r\n"
    "Content-Disposition: form-data; name=\"audio\"; filename=\"audio.wav\"\r\n"
    "Content-Type: audio/wav\r\n\r\n";
  String finParte = "\r\n--" + limite + "--\r\n";

  size_t tamanoAudio = f.size();
  size_t tamanoTotal = inicioParte.length() + tamanoAudio + finParte.length();

  uint8_t* cuerpo = (uint8_t*)malloc(tamanoTotal);
  if (!cuerpo) {
    textoRespuesta = "Sin memoria suficiente.";
    f.close();
    pantallaActual = RESPUESTA;
    dibujarPantallaActual();
    return;
  }

  memcpy(cuerpo, inicioParte.c_str(), inicioParte.length());
  f.read(cuerpo + inicioParte.length(), tamanoAudio);
  memcpy(cuerpo + inicioParte.length() + tamanoAudio, finParte.c_str(), finParte.length());
  f.close();

  http.addHeader("Content-Type", "multipart/form-data; boundary=" + limite);
  int codigo = http.POST(cuerpo, tamanoTotal);
  free(cuerpo);

  if (codigo != 200) {
    textoRespuesta = "Error de conexion con Noma.";
  } else {
    String respuestaJson = http.getString();
    JsonDocument doc;
    deserializeJson(doc, respuestaJson);
    textoRespuesta = String((const char*)(doc["respuesta"] | "Sin respuesta"));
  }
  http.end();

  pantallaActual = RESPUESTA;
  dibujarPantallaActual();
}

// ---------------------- NAVEGACIÓN ----------------------

Pantalla menuPantallas[] = { HOME, WIFI_INFO, TEMPORIZADOR, ALARMAS, COMANDOS, AJUSTES };
const int NUM_MENU = sizeof(menuPantallas) / sizeof(menuPantallas[0]);
int indiceMenu = 0;

void manejarPulsacionCorona() {
  switch (pantallaActual) {
    case HOME:
      grabarAudio();
      enviarYMostrar();
      break;
    case RESPUESTA:
      pantallaActual = HOME;
      indiceMenu = 0;
      dibujarPantallaActual();
      break;
    case TEMPORIZADOR:
      if (!temporizadorCorriendo) {
        temporizadorCorriendo = true;
        temporizadorInicio = millis();
      } else {
        temporizadorCorriendo = false;
      }
      dibujarPantallaActual();
      break;
    case ALARMAS:
      alarmaActiva = !alarmaActiva;
      dibujarPantallaActual();
      break;
    default:
      break;
  }
}

// ---------------------- SETUP / LOOP ----------------------

void setup() {
  Serial.begin(115200);
  SPIFFS.begin(true);

  pantalla.init();
  pantalla.setRotation(0);
  pantalla.fillScreen(COLOR_FONDO);
  pantalla.setTextColor(COLOR_TEXTO);
  pantalla.setTextDatum(lgfx::middle_center);
  pantalla.setTextSize(2);
  pantalla.drawString("Conectando WiFi...", 180, 180);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
  }

  pinMode(PIN_SWITCH_CORONA, INPUT_PULLUP);
  pinMode(PIN_ENCODER_A, INPUT_PULLUP);
  pinMode(PIN_ENCODER_B, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ENCODER_A), onEncoderA, CHANGE);

  iniciarI2SPDM();

  pantallaActual = HOME;
  dibujarPantallaActual();
}

void loop() {
  // Navegación con la corona (solo si no estamos en medio del flujo de voz)
  if (pantallaActual != ESCUCHANDO && pantallaActual != PROCESANDO) {
    int delta = posicionEncoder - ultimaPosicionEncoder;
    if (abs(delta) >= 4) { // 4 pasos del encoder = 1 "click" en la mayoría de encoders
      if (pantallaActual != RESPUESTA) {
        indiceMenu = (indiceMenu + (delta > 0 ? 1 : -1) + NUM_MENU) % NUM_MENU;
        pantallaActual = menuPantallas[indiceMenu];
        dibujarPantallaActual();
      }
      ultimaPosicionEncoder = posicionEncoder;
    }
  }

  // Pulsador de la corona
  if (digitalRead(PIN_SWITCH_CORONA) == LOW) {
    delay(50);
    if (digitalRead(PIN_SWITCH_CORONA) == LOW) {
      manejarPulsacionCorona();
      while (digitalRead(PIN_SWITCH_CORONA) == LOW) delay(10);
    }
  }

  // Refrescar el temporizador mientras corre
  if (pantallaActual == TEMPORIZADOR && temporizadorCorriendo) {
    static unsigned long ultimoRefresco = 0;
    if (millis() - ultimoRefresco > 500) {
      dibujarPantallaActual();
      ultimoRefresco = millis();
    }
  }
}
