#include "config.h"

#if defined(ESP8266)
#include <FS.h> // must be first
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESP8266WebServer.h>
#undef USE_NTP
#ifdef USE_NTP
#include <TimeLib.h>
#include <NtpClientLib.h>
#endif
#define helloPin 2
#define HELLO_ON 0
#define HELLO_OFF 1
#else // ESP32

#define ASYNC_TCP_SSL_ENABLED 0
#define CONFIG_ASYNC_TCP_RUNNING_CORE -1
#define CONFIG_ASYNC_TCP_USE_WDT 0
#include <soc/rtc_cntl_reg.h>

#include <WiFi.h>          //https://github.com/esp8266/Arduino
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>

#if defined(DEVICE_ID_PREFERENCE_GROUP) && defined(DEVICE_ID_PREFERENCE_KEY)
#include <Preferences.h>
Preferences global_preferences;
#endif

#endif

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SimpleMap.h>
#include <Ticker.h>
#include <time.h>


#ifndef USE_BT_CONSOLE
  #define USE_BT_CONSOLE 0
#endif

#ifndef USE_OLED
  #define USE_OLED 0
#endif

#if USE_BT_CONSOLE
  #include "BluetoothSerial.h"
  BluetoothSerial *SerialBT = NULL;
#endif

#ifndef USE_STATUS
#define USE_STATUS true
#endif

#ifndef USE_EVENT
#define USE_EVENT true
#endif

#ifndef USE_SET
#define USE_SET true
#endif

#ifndef USE_GET
#define USE_GET true
#endif

#ifndef USE_CMD
#define USE_CMD true
#endif

#ifndef USE_FLAT_TOPIC
#define USE_FLAT_TOPIC false
#endif

#ifndef USE_WILDCARD_TOPIC
#define USE_WILDCARD_TOPIC false
#endif

#ifndef SHELL_DELAY
#define SHELL_DELAY 1000
#endif

#ifndef LEAF_SETUP_DELAY
#define LEAF_SETUP_DELAY 0
#endif

#include "accelerando_trace.h"

//@******************************* constants *********************************
// you can override these by defining them in config.h

#ifndef HEARTBEAT_INTERVAL_SECONDS
#define HEARTBEAT_INTERVAL_SECONDS (600)
#endif

#ifndef NETWORK_RECONNECT_SECONDS
#define NETWORK_RECONNECT_SECONDS 20
#endif

#ifndef MQTT_RECONNECT_SECONDS
#define MQTT_RECONNECT_SECONDS 5
#endif

//@******************************* globals *********************************

int blink_rate = 100;
int blink_duty = 0;
bool identify = false;
bool blink_enable = true;


#ifdef ESP8266
int boot_count = 0;
#else 
RTC_DATA_ATTR int boot_count = 0;
#endif
String wake_reason=""; // will be filled in during startup
String _ROOT_TOPIC="";

bool wifiConnected = false;
char ip_addr_str[20] = "unset";
char mac_short[7] = "unset";
char mac[19];

bool use_status = USE_STATUS;
bool use_event = USE_EVENT;
bool use_set = USE_SET;
bool use_get = USE_GET;
bool use_cmd = USE_CMD;
bool use_wildcard_topic = USE_WILDCARD_TOPIC;
bool use_flat_topic = USE_FLAT_TOPIC;
int heartbeat_interval_seconds = HEARTBEAT_INTERVAL_SECONDS;
unsigned long last_external_input = 0;

int leaf_setup_delay = LEAF_SETUP_DELAY;

char post_error_history[POST_ERROR_HISTORY_LEN];
static bool post_error_display = false;

void idle_pattern(int cycle, int duty);
void post_error(enum post_error, int count);
void post_error_history_update(enum post_device dev, uint8_t err);
void post_error_history_reset();
uint8_t post_error_history_entry(enum post_device dev, int pos);
void disable_bod();
void enable_bod();

//@********************************* leaves **********************************

#if USE_OLED
#include "oled.h"
#endif

#include "leaf.h"
#include "leaves.h"

//
//@********************************* setup ***********************************
#undef SLEEP_SHOTGUN
#undef CAMERA_SHOTGUN

#ifdef SLEEP_SHOTGUN
RTC_DATA_ATTR int sleep_duration_sec = 15;
unsigned long sleep_timeout_sec = 30;
const char *planets[]={"Netpune","Uranus","Jupiter","Mars","Earth"};
#endif

#ifdef CAMERA_SHOTGUN
#include <esp_camera.h>

// AI_THINKER ESP-32 cam pinout
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

esp_err_t camera_ok;
static camera_config_t camera_config;

static esp_err_t init_camera()
{
  // set up config
  camera_config.pin_pwdn = PWDN_GPIO_NUM;
  camera_config.pin_reset = RESET_GPIO_NUM;
  camera_config.pin_xclk = XCLK_GPIO_NUM;
  camera_config.pin_sscb_sda = SIOD_GPIO_NUM;
  camera_config.pin_sscb_scl = SIOC_GPIO_NUM;

  camera_config.pin_d0 = Y2_GPIO_NUM;
  camera_config.pin_d1 = Y3_GPIO_NUM;
  camera_config.pin_d2 = Y4_GPIO_NUM;
  camera_config.pin_d3 = Y5_GPIO_NUM;
  camera_config.pin_d4 = Y6_GPIO_NUM;
  camera_config.pin_d5 = Y7_GPIO_NUM;
  camera_config.pin_d6 = Y8_GPIO_NUM;
  camera_config.pin_d7 = Y9_GPIO_NUM;

  camera_config.pin_vsync = VSYNC_GPIO_NUM;
  camera_config.pin_href = HREF_GPIO_NUM;
  camera_config.pin_pclk = PCLK_GPIO_NUM;

  camera_config.xclk_freq_hz = 20000000;
  camera_config.ledc_timer = LEDC_TIMER_0;
  camera_config.ledc_channel = LEDC_CHANNEL_0;

  camera_config.pixel_format = PIXFORMAT_JPEG;
  camera_config.frame_size = FRAMESIZE_VGA;    //QQVGA-UXGA Do not use sizes above QVGA when not JPEG

  camera_config.jpeg_quality = 12; //0-63 lower number means higher quality
  camera_config.fb_count = 1;       //if more than one, i2s runs in continuous mode. Use only with JPEG

  //initialize the camera
  esp_err_t err = esp_camera_init(&camera_config);
  if (err != ESP_OK)
  {
    Serial.printf("Camera Init Failed (%d)", (int)err);
    esp_camera_deinit();
    esp_restart();
    // notreached
    return err;
  }

  return ESP_OK;
}
#endif

void setup(void)
{
#if defined(helloPin)
  pinMode(helloPin, OUTPUT);
  for (int i=0; i<3;i++) {
    digitalWrite(helloPin, HELLO_ON);
    delay(250);
    digitalWrite(helloPin, HELLO_OFF);
    delay(250);
  }
#endif
  post_error_history_reset();

  //
  // Set up the serial port for diagnostic trace
  //
  Serial.begin(115200);
  Serial.printf("boot_latency %lu",millis());
  Serial.println("\n\n\n");
  Serial.print("Accelerando.io Multipurpose IoT Backplane, build "); Serial.println(BUILD_NUMBER);
  Serial.println();

  uint8_t baseMac[6];
  // Get MAC address for WiFi station
#ifdef ESP8266
  WiFi.macAddress(baseMac);
#else
  esp_read_mac(baseMac, ESP_MAC_WIFI_STA);
#endif
  char baseMacChr[18] = {0};
  snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", baseMac[0], baseMac[1], baseMac[2], baseMac[3], baseMac[4], baseMac[5]);
  snprintf(mac_short, sizeof(mac_short), "%02x%02x%02x", baseMac[3], baseMac[4], baseMac[5]);
#ifdef DEVICE_ID_APPEND_MAC
  strlcat(device_id, "-", sizeof(device_id));
  strlcat(device_id, mac_short, sizeof(device_id));
#endif
  Serial.printf("MAC address is %s which makes default device ID %s\n",
		mac, device_id);

#if defined(DEVICE_ID_PREFERENCE_GROUP) && defined(DEVICE_ID_PREFERENCE_KEY)
    global_preferences.begin(DEVICE_ID_PREFERENCE_GROUP, true);
    global_preferences.getString(DEVICE_ID_PREFERENCE_KEY, device_id, sizeof(device_id));
    Serial.printf("Load configured device ID from preferences: %s\n", device_id);
    String s = global_preferences.getString("heartbeat_interval");
    if (s.length()>0) heartbeat_interval_seconds = s.toInt();
#endif


#ifdef APP_TOPIC
  // define APP_TOPIC this to use an application prefix address on all topics
  //
  // without APP_TOPIC set topics will resemble
  //		eg. devices/TYPE/INSTANCE/VALUE

  // with APP_TOPIC set topics will resemble
  //		eg. APP_TOPIC/MACADDR/devices/TYPE/INSTANCE/VALUE
  _ROOT_TOPIC = String(APP_TOPIC)+"/";
#elif defined(APP_TOPIC_BASE)
  // define APP_TOPIC_BASE this to use an application prefix plus mac address on all topics
  //
  _ROOT_TOPIC = String(APP_TOPIC)+"/"+mac_short+"/";
#endif

  NOTICE("Device ID is %s", device_id);

  //WiFi.mode(WIFI_OFF);

#if USE_BT_CONSOLE
  SerialBT = new BluetoothSerial();
  SerialBT->begin(device_id); //Bluetooth device name
#endif

  // It is now safe to use accelerando_trace ALERT NOTICE INFO DEBUG macros

#ifdef ESP8266
  wake_reason = ESP.getResetReason();
  system_rtc_mem_read(64, &boot_count, sizeof(boot_count));
  ++boot_count;
  system_rtc_mem_write(64, &boot_count, sizeof(boot_count));
#else
  esp_reset_reason_t reset_reason = esp_reset_reason();
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  switch (reset_reason) {
  case ESP_RST_UNKNOWN: wake_reason="other"; break;
  case ESP_RST_POWERON: wake_reason="poweron"; break;
  case ESP_RST_EXT: wake_reason="external"; break;
  case ESP_RST_SW: wake_reason="software"; break;
  case ESP_RST_PANIC: wake_reason="panic"; break;
  case ESP_RST_WDT: wake_reason="watchdog"; break;
  case ESP_RST_DEEPSLEEP:
    switch(wakeup_reason)
    {
    case ESP_SLEEP_WAKEUP_EXT0 : wake_reason="deepsleep/rtc io"; break;
    case ESP_SLEEP_WAKEUP_EXT1 : wake_reason ="deepsleep/rtc cntl"; break;
    case ESP_SLEEP_WAKEUP_TIMER : wake_reason = "deepsleep/timer"; break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD : wake_reason = "deepsleep/touchpad"; break;
    case ESP_SLEEP_WAKEUP_ULP : wake_reason="deepsleep/ulp"; break;
    case ESP_SLEEP_WAKEUP_GPIO: wake_reason="deepsleep/gpio"; break;
    case ESP_SLEEP_WAKEUP_UART: wake_reason="deepsleep/uart"; break;
    default: wake_reason="deepsleep/other"; break;
    }
    break;
  case ESP_RST_BROWNOUT: wake_reason="brownout"; break;
  case ESP_RST_SDIO: wake_reason="sdio reset"; break;
  default:
    wake_reason="other"; break;
  }
  ++boot_count;
#endif
  NOTICE("ESP Wakeup #%d reason: %s", boot_count, wake_reason.c_str());


#if USE_OLED
  oled_setup();
#endif

#ifdef CAMERA_SHOTGUN
  camera_ok = init_camera();
  if (camera_ok != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", (int)camera_ok);
    esp_camera_deinit();
    esp_restart();
    //notreached
  }
#endif
  
  Leaf *leaf = Leaf::get_leaf_by_name(leaves, "shell");
  if (leaf != NULL) {
    NOTICE("Press any key for shell");
    unsigned long wait_until = millis();
    if (!wake_reason.startsWith("deepsleep")) {
#ifdef SHELL_DELAY_COLD
      if (SHELL_DELAY_COLD) wait_until += SHELL_DELAY_COLD;
#endif
    }
    else {
#ifdef SHELL_DELAY
      if (SHELL_DELAY) wait_until += SHELL_DELAY;
#endif
    }
    do {
      delay(5);
      if (Serial.available()) {
	ALERT("Disabling all leaves, and dropping into shell.  Use 'cmd restart' to resume");
	for (int i=0; leaves[i]; i++) {
	  if (leaves[i] != leaf) {
	    leaves[i]->preventRun();
	  }
	}
	break;
      }
    } while (millis() <= wait_until);
  }

  //
  // Do any post-sleep hooks if waking from sleep
  //
  if (wake_reason.startsWith("deepsleep/")) {
    ALERT("Woke from deep sleep (%s), trigger post-sleep hooks", wake_reason.c_str());
    for (int i=0; leaves[i]; i++) {
      leaf = leaves[i];
      if (leaf->canRun()) {
	leaf->post_sleep();
      }
    }
  }

  //
  // Set up the IO leaves
  //
  // TODO: pass a 'was asleep' flag
  //
  // disable_bod();
  for (int i=0; leaves[i]; i++) {
    leaf = leaves[i];
    if (leaf->canRun()) {
      NOTICE("LEAF %d SETUP: %s", i+1, leaf->get_name().c_str());
      leaf->setup();
      if (leaf_setup_delay) delay(leaf_setup_delay);
    }
  }
  // enable_bod();

  // summarise the connections between leaves
  for (int i=0; leaves[i]; i++) {
    leaf = leaves[i];
    if (leaf->canRun()) {
      leaf->describe_taps();
      leaf->describe_output_taps();
    }
  }

  // call the start method on active leaves
  // (this can be used to do one-off actions after all leaves and taps are configured)
  for (int i=0; leaves[i]; i++) {
    leaf = leaves[i];
    if (leaf->canRun()) {
      NOTICE("LEAF %d START: %s", i+1, leaf->get_name().c_str());
      leaf->start();
    }
  }

  mqttConfigured = true;
  ALERT("Setup complete");
}

void disable_bod()
{
#ifdef ESP32
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); //disable brownout detector
#endif
}
void enable_bod()
{
#ifdef ESP32
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 1); //reenable brownout detector
#endif
}

void post_error_history_reset()
{
  memset(post_error_history, 0, POST_ERROR_HISTORY_LEN);
}

uint8_t post_error_history_entry(enum post_device dev, int pos)
{
	if (pos >= POST_ERROR_HISTORY_PERDEV) {
		pos = POST_ERROR_HISTORY_PERDEV-1;
	}
	return post_error_history[(POST_ERROR_HISTORY_PERDEV*dev)+pos]-'0';
}

void post_error_history_update(enum post_device dev, uint8_t err)
{
	// rotate the buffer of POST code history
	// (each device has 5 codes recorded in a fifo)
	int devpos = (dev*POST_ERROR_HISTORY_PERDEV);
	for (int i=POST_ERROR_HISTORY_PERDEV-1; i>0;i--) {
		post_error_history[devpos+i] = post_error_history[devpos+i-1];
	}
	post_error_history[devpos] = '0'+err;
}

void post_error(enum post_error err, int count)
{
  ALERT("POST error %d: %s, repeated %dx",
	(int)err,
	((err < POST_ERROR_MAX) && post_error_names[err])?post_error_names[err]:"",
	count);
#if USE_OLED
  ERROR("ERROR: %s", post_error_names);
#endif
#ifdef helloPin
  post_error_display = true;

  post_error_history_update(POST_DEV_ESP, (uint8_t)err);

  if (count == 0) return;

  digitalWrite(helloPin, HELLO_OFF);
  delay(500);

  int blinks = (int)err;

  // Deliver ${blinks} short blinks, then wait two seconds.
  // Repeat ${count} times
  for (int i = 0; i< count ; i++) {
    for (int j = 0; j<blinks; j++) {
      digitalWrite(helloPin, HELLO_ON);
      delay(125);
      digitalWrite(helloPin, HELLO_OFF);
      delay(125);
    }
    delay(1000);
  }
  post_error_display = false;
#endif
  return;
}

void idle_pattern(int cycle, int duty)
{
  blink_rate = cycle;
  blink_duty = duty;
}

//
//@********************************** loop ***********************************

void loop(void)
{
  //ENTER(L_DEBUG);

  unsigned long now = millis();

#ifdef helloPin
  static int hello = HELLO_OFF;

  int pos = now % (identify?250:blink_rate);
  int flip = blink_rate * (identify?50:blink_duty) / 100;
  int led = blink_enable?(pos < flip):0;
  //DEBUG("now = %lu pos=%d flip=%d led=%d hello=%d", now, pos, flip, led, hello);
  if (led != hello) {
    //NOTICE("writing helloPin <= %d", led);
    hello = led;
    if (!post_error_display) digitalWrite(helloPin, hello?HELLO_ON:HELLO_OFF);
  }
#endif

#ifdef SLEEP_SHOTGUN
  static int warps = 3;
  static unsigned long last_warp = 0;
  const char *planet = planets[(boot_count-1)%5];

  if (now > (last_warp + 5000)) {
    last_warp = now;
    Serial.printf("%d warp%s to %s\n", warps, (warps==1)?"":"s", planet);

#ifdef CAMERA_SHOTGUN  

    if (camera_ok == ESP_OK) {
      Serial.printf("Taking picture...");
      
      camera_fb_t *pic = esp_camera_fb_get();
      
      // use pic->buf to access the image
      Serial.printf("Picture taken! Its size was: %zu bytes\n", pic->len);
    }
#endif
    
    --warps;
    if (warps <= 0) {
      Serial.printf("%s!  Bonus Sleep Stage (%d sec).\n", planet, sleep_duration_sec);
      Serial.printf("Deinit camera...");
      esp_camera_deinit();
      esp_sleep_enable_timer_wakeup(sleep_duration_sec * 1000000ULL);
      sleep_duration_sec *= 2; // sleep longer next time
      //esp_sleep_enable_ext0_wakeup((gpio_num_t)0, 0);
      Serial.printf("Deep sleep start\n");
      esp_deep_sleep_start();
      //notreached
    }
  }
  
#endif

  //
  // Handle Leaf events
  //
  for (int i=0; leaves[i]; i++) {
    Leaf *leaf = leaves[i];
    if (leaf->canRun()) {
      leaf->loop();
    }
  }
  //LEAVE;
}

// Local Variables:
// mode: C++
// c-basic-offset: 2
// End:
