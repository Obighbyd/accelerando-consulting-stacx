#include "abstract_pubsub.h"

//
//@********************** class PubsubSim7000MQTTLeaf ***********************
//
// This class encapsulates an Mqtt connection using the AT commands of the
// Sim7000 LTE modem
//

class PubsubSim7000MQTTLeaf : public AbstractPubsubLeaf
{
public:
  PubsubSim7000MQTTLeaf(String name, String target, bool use_ssl=true, bool use_device_topic=true, bool run = true) : AbstractPubsubLeaf(name, target, use_ssl, use_device_topic) {
    LEAF_ENTER(L_INFO);
    this->target = target;
    this->run = run;
    // all the setup happens in the superclass
    LEAF_LEAVE;
  }

  virtual void setup();
  virtual void loop(void);
  virtual uint16_t _mqtt_publish(String topic, String payload, int qos=0, bool retain=false);
  virtual void _mqtt_subscribe(String topic, int qos=0);
  virtual void _mqtt_unsubscribe(String topic);
  virtual bool mqtt_receive(String type, String name, String topic, String payload);
  virtual bool connect(void);
  virtual void disconnect(bool deliberate=true);
  virtual void initiate_sleep_ms(int ms);
  virtual void pre_sleep(int duration=0);
protected:
  //
  // Network resources
  //
  IpSim7000Leaf *modemLeaf = NULL;
  Sim7000Modem *modem=NULL;
  unsigned long mqttReconnectAt=0;
  char username[40]="";
  char password[40]="";
  char keepalive[40] = "120";
  bool use_client_cert = true;
  uint32_t connect_time = 0;
  uint32_t disconnect_time = 0;
  bool was_connected = false;
  bool enter_sleep = false;
  char lwt_topic[80];

  void handle_connect_event(bool do_subscribe=true);
  bool install_cert();

  bool netStatus()
  {
    return modemLeaf?modemLeaf->netStatus():false;
  }
  bool connStatus()
  {
    return modemLeaf?modemLeaf->connStatus():false;
  }

};

void PubsubSim7000MQTTLeaf::setup()
{
  AbstractPubsubLeaf::setup();
  ENTER(L_INFO);
  mqttConnected = _connected = false;

  StorageLeaf *prefs_leaf = (StorageLeaf *)get_tap("prefs");

  if (prefs_leaf) {
    String value;

    value = prefs_leaf->get(String("pubsub_autoconnect"));
    if (value.length()) autoconnect = (value=="on");

    value = prefs_leaf->get("use_get");
    if (value.length()) use_get = (value=="on");
    value = prefs_leaf->get("use_set");
    if (value.length()) use_set = (value=="on");
    value = prefs_leaf->get("use_cmd");
    if (value.length()) use_cmd = (value=="on");
    value = prefs_leaf->get("use_flat_topic");
    if (value.length()) use_flat_topic = (value=="on");
    value = prefs_leaf->get("use_status");
    if (value.length()) use_status = (value=="on");
    value = prefs_leaf->get("use_event");
    if (value.length()) use_event = (value=="on");
    value = prefs_leaf->get("use_wildcard_topic");
    if (value.length()) use_wildcard_topic = (value=="on");
    value = prefs_leaf->get("use_client_cert");
    if (value.length()) use_client_cert = (value=="on");
    value = prefs_leaf->get("use_ssl");
    if (value.length()) use_ssl = (value=="on");

    value = prefs_leaf->get("mqtt_host");
    if (value.length()) {
      // there's a preference, overwrite the default
      strlcpy(mqtt_host, value.c_str(), sizeof(mqtt_host));
    }
    else {
      // nothing saved, save the default
      prefs_leaf->put("mqtt_host", mqtt_host);
    }

    value = prefs_leaf->get("mqtt_port");
    if (value.length()) {
      strlcpy(mqtt_port, value.c_str(), sizeof(mqtt_port));
    }
    else {
      // nothing saved, save the default
      prefs_leaf->put("mqtt_port", mqtt_port);
    }

    value = prefs_leaf->get("mqtt_user");
    if (value.length()) {
      strlcpy(mqtt_user, value.c_str(), sizeof(mqtt_user));
    }
    else {
      prefs_leaf->put("mqtt_user", mqtt_user);
    }


    value = prefs_leaf->get("mqtt_pass");
    if (value.length()) {
      strlcpy(mqtt_pass, value.c_str(), sizeof(mqtt_pass));
    }
    else {
      prefs_leaf->put("mqtt_pass", mqtt_pass);
    }
  }

  //
  // Set up the MQTT Client
  //
  modemLeaf = (IpSim7000Leaf *)ipLeaf;
  if (modemLeaf == NULL) {
    LEAF_ALERT("Modem leaf not found");
  }
  LEAF_ALERT("MQTT Setup [%s:%s] %s", mqtt_host, mqtt_port, base_topic.c_str());
  strlcpy(username, mqtt_user, sizeof(username));
  strlcpy(password, mqtt_pass, sizeof(password));

  LEAVE;
}

bool PubsubSim7000MQTTLeaf::mqtt_receive(String type, String name, String topic, String payload)
{
  LEAF_ENTER(L_DEBUG);
  bool handled = Leaf::mqtt_receive(type, name, topic, payload);
  LEAF_INFO("%s, %s", topic.c_str(), payload.c_str());

  if (topic == "_ip_connect") {
    if (ipLeaf) {
      if (autoconnect) {
	LEAF_NOTICE("IP is online, autoconnecting MQTT");
	connect();
      }
    }
    handled = true;
  }
  else if (topic == "_ip_disconnect") {
    if (_connected) {
      disconnect();
    }
  }
  else if (topic == "cmd/pubsub_status") {
    char status[32];
    uint32_t secs;
    if (_connected) {
      secs = (millis() - connect_time)/1000;
      snprintf(status, sizeof(status), "online %d:%02d", secs/60, secs%60);
    }
    else {
      secs = (millis() - disconnect_time)/1000;
      snprintf(status, sizeof(status), "offline %d:%02d", secs/60, secs%60);
    }
    mqtt_publish("status/pubsub_status", status);
  }

  return handled;
}

void PubsubSim7000MQTTLeaf::loop()
{
  AbstractPubsubLeaf::loop();
  //LEAF_ENTER(L_DEBUG);

  unsigned long now = millis();

  if (mqttReconnectAt && (millis() >= mqttReconnectAt)) {
    mqttReconnectAt=0;
    LEAF_NOTICE("Attempting reconenct");
    connect();
  }

  if (!was_connected && _connected) {
    handle_connect_event(true);
  }
  was_connected = _connected;

  //
  // Handle MQTT Events
  //
  if (_connected) {
    //
    // MQTT is active, process any pending events
    //
    // no need for heartbeat here, the leaf superclass handles that
  }
  //LEAF_LEAVE;
}

void PubsubSim7000MQTTLeaf::pre_sleep(int duration)
{
  enter_sleep = true;
  disconnect(true);
}

void PubsubSim7000MQTTLeaf::disconnect(bool deliberate) {
  LEAF_ENTER(L_NOTICE);

  idle_pattern(1000,50);

  mqttConnected = _connected = false;

  if (deliberate) {
    if (!enter_sleep) {
      modem->MQTT_connect(false);
      publish("_pubsub_disconnect", "deliberate");
    }
  }
  else {
    //post_error(POST_ERROR_PUBSUB, 3);
    post_error(POST_ERROR_PUBSUB_LOST, 0);
    ERROR("MQTT disconnect");
    if (autoconnect) {
      mqttReconnectAt = millis() + (MQTT_RECONNECT_SECONDS*1000);
      publish("_pubsub_disconnect", "will-retry");
    }
    else {
      publish("_pubsub_disconnect", "no-retry");
    }
  }

  LEAF_LEAVE;
}

bool PubsubSim7000MQTTLeaf::install_cert()
{
  const char *cacert =
    "-----BEGIN CERTIFICATE-----\r\n"
    "MIICdzCCAeCgAwIBAgIJAK3TxzrHW8SsMA0GCSqGSIb3DQEBCwUAMFMxCzAJBgNV\r\n"
    "BAYTAkFVMRMwEQYDVQQIDApxdWVlbnNsYW5kMRwwGgYDVQQKDBNTZW5zYXZhdGlv\r\n"
    "biBQdHkgTHRkMREwDwYDVQQDDAhzZW5zYWh1YjAeFw0yMDAzMjEyMzMzMTJaFw0y\r\n"
    "NTAzMjAyMzMzMTJaMFMxCzAJBgNVBAYTAkFVMRMwEQYDVQQIDApxdWVlbnNsYW5k\r\n"
    "MRwwGgYDVQQKDBNTZW5zYXZhdGlvbiBQdHkgTHRkMREwDwYDVQQDDAhzZW5zYWh1\r\n"
    "YjCBnzANBgkqhkiG9w0BAQEFAAOBjQAwgYkCgYEAtAag8LBkdk+QmMJWxT/yDtqH\r\n"
    "iwFKIpIoz4PwFlPHi1bisRM1VB3IajU/bhMLc8AdhSIhG6GuSo4abfesYsFdEmTd\r\n"
    "Z0es5TTDNZWj+dPOLEBDkKyi4RDrRmzh/N8axZ3Yhoc/k4QuzGhnUKOvA6z07Sg5\r\n"
    "XsNUfIYGatxPl8JYSScCAwEAAaNTMFEwHQYDVR0OBBYEFD7Ad200vd05FMewsGsW\r\n"
    "WJy09X+dMB8GA1UdIwQYMBaAFD7Ad200vd05FMewsGsWWJy09X+dMA8GA1UdEwEB\r\n"
    "/wQFMAMBAf8wDQYJKoZIhvcNAQELBQADgYEAOad6RxxSFaG8heBXY/0/WNLudt/W\r\n"
    "WLeigKMPXZmY72Y4U8/FQzxDj4bP+AOE+xoVVFcmZURmX3V80g+ti6Y/d9QFDQ+t\r\n"
    "YsHyzwrWsMusM5sRlmfrxlExrRjw1oEwdLefAM8L5WDEuhCdXrLxwFjUK2TcJ9u0\r\n"
    "rQ09npAQ1MgeaRo=\r\n"
    "-----END CERTIFICATE-----\r\n";

  const char *clientcert =
    "-----BEGIN CERTIFICATE-----\r\n"
    "MIIC+zCCAmSgAwIBAgICEAAwDQYJKoZIhvcNAQELBQAwUzELMAkGA1UEBhMCQVUx\r\n"
    "EzARBgNVBAgMCnF1ZWVuc2xhbmQxHDAaBgNVBAoME1NlbnNhdmF0aW9uIFB0eSBM\r\n"
    "dGQxETAPBgNVBAMMCHNlbnNhaHViMB4XDTIwMDcxNzAwMDAxMloXDTIxMDcyNzAw\r\n"
    "MDAxMlowazELMAkGA1UEBhMCQVUxEzARBgNVBAgMClF1ZWVuc2xhbmQxDzANBgNV\r\n"
    "BAcMBlN1bW5lcjEUMBIGA1UECgwLQWNjZWxlcmFuZG8xDDAKBgNVBAsMA1BVQzES\r\n"
    "MBAGA1UEAwwJcHVjMDAwMDAxMIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDW\r\n"
    "siRHo4hVgYz6EEINbWraXnouvKJ5qTb+xARdOIsCnZxx4A2nEf//VXUhz+uAffpo\r\n"
    "+p3YtQ42wG/j0G0uWxOqgGjGom6KhF7Bt4n8AtSJeoDfZV1imGsY+mL+PqsLjJhx\r\n"
    "85gnhFgC4ii38V9bwQU7WjTSO/1TfHw+vFjVd0AkDwIDAQABo4HFMIHCMAkGA1Ud\r\n"
    "EwQCMAAwEQYJYIZIAYb4QgEBBAQDAgWgMDMGCWCGSAGG+EIBDQQmFiRPcGVuU1NM\r\n"
    "IEdlbmVyYXRlZCBDbGllbnQgQ2VydGlmaWNhdGUwHQYDVR0OBBYEFN9SYA+m3ZlF\r\n"
    "eR81YxXf9CbNOFw4MB8GA1UdIwQYMBaAFD7Ad200vd05FMewsGsWWJy09X+dMA4G\r\n"
    "A1UdDwEB/wQEAwIF4DAdBgNVHSUEFjAUBggrBgEFBQcDAgYIKwYBBQUHAwQwDQYJ\r\n"
    "KoZIhvcNAQELBQADgYEAqIOpCct75VymZctHb98U1leUdG1eHYubP0YLc9ae3Sph\r\n"
    "T6R7JnwFynSFgXeAbTzC57O1onQwNq1fJ+LsyaeTWmxEb3PeVzoVqitO92Pw/kgj\r\n"
    "IXrFxaaEYEBIPsk3Ez5KUEhQicH8Y1zyneAAvxzKhSRwoJZ1YPfeGnp7SvTlNC0=\r\n"
    "-----END CERTIFICATE-----\r\n";

  const char *clientkey =
    "-----BEGIN RSA PRIVATE KEY-----\r\n"
    "MIICXQIBAAKBgQDWsiRHo4hVgYz6EEINbWraXnouvKJ5qTb+xARdOIsCnZxx4A2n\r\n"
    "Ef//VXUhz+uAffpo+p3YtQ42wG/j0G0uWxOqgGjGom6KhF7Bt4n8AtSJeoDfZV1i\r\n"
    "mGsY+mL+PqsLjJhx85gnhFgC4ii38V9bwQU7WjTSO/1TfHw+vFjVd0AkDwIDAQAB\r\n"
    "AoGBALZoehyHm3CSdjWLlKMV4KARfxuwVxaopzoDTnXpcWnSgTXbF55n06mbcL4+\r\n"
    "iicMYbHJpEyXX7EzBJ142xp0dRpr51mOCF9pLtLsDSOslA87X74pffnY6boMvW2+\r\n"
    "Tiou1AP5XXlemTmKiT3vMLno+JKfxqu+DhLyCdV5zHyeyw4hAkEA9PjtgN6Xaru0\r\n"
    "BFdBYlURllGq2J11ZioM1HlhNUX1UA6WR7jC6ZLXxFSbrZkLKgInuwiJxUn6j2mb\r\n"
    "/ZypzrOo8QJBAOBcTmHlqTWSK6r32A6+1WVN1QdSU7Nif/lIAUG+Y4XBMij3mJgX\r\n"
    "decI/qGQI/6P3LhSErbUOZVlsHh7zUzYnP8CQQDp6mRHIMUu+qrrVjIt5hMUGUls\r\n"
    "6/W1J0P3AywqRXH4DuW6+JbNmBUF+NBqlG/Pnh03//Al/f0OQgbcxWJz6KPRAkB+\r\n"
    "M23jo0uK1q25fbAKm01trlolxClQvhc+IUKTuIRCuGl+occzxf6L9oNEXc/hYQrG\r\n"
    "o2Pjc3zwjEK3guv4TeABAkBXEi5Vair5yvU+3dV3+21tbnWnDM5UXmwh4PRgyoHQ\r\n"
    "ifrMHbpTscUNv+3Alc9gJJrUhZO4MxnebIRmKn2DzO87\r\n"
    "-----END RSA PRIVATE KEY-----\r\n";

  if (!modem->writeFileVerify("cacert.pem", cacert)) {
    Serial.println("CA cert write failed");
  }

  if (use_client_cert) {
    if (!modem->writeFileVerify("client.crt", clientcert)) {
      Serial.println("Client cert write failed");
    }

    if (!modem->writeFileVerify("client.key", clientkey)) {
      Serial.println("Client key write failed");
    }
  }


/*
  //Serial.println(F("Contents to write"));
  //Serial.println(cacert);

  static char cmd[80]="";
  int len = strlen(cacert)-2;
  Serial.println(F("Write to flash filesystem (cmd)"));
  snprintf(cmd, sizeof(cmd), "AT+CFSWFILE=3,\"cacert.pem\",0,%d,10000", len);
  modem->sendCheckReply(cmd, F("DOWNLOAD"));

  Serial.println(F("Write to flash filesystem (data)"));
  modem->send(cacert);
  modem->expectReply(F("OK"),5000);

  // resynchronise
  //int tries=5;
  //while (tries && !modem->sendCheckReply("AT", "OK")) --tries;

  modem->sendCheckReply("AT+CFSTERM","OK",5000);

  snprintf(cmd, sizeof(cmd), "AT+CFSGFIS=3,\"cacert.pem\"");
  modem->send(cmd);
  modem->expectReplyWithInt("+CFSGFIS: ", &len);

  modem->sendCheckReply("AT+CFSINIT","OK");
  snprintf(cmd, sizeof(cmd), "AT+CFSRFILE=3,\"cacert.pem\",0,%d,0", len);
  modem->send(cmd);
  if (modem->expectReplyWithInt("+CFSRFILE: ", &len)) {
    static char buf[10240];
    modem->getReplyOfSize(buf, len, 5000, true);
  }
  modem->expectReply(F("OK"),5000);

  modem->sendCheckReply("AT+CFSTERM","OK");
*/

  return false;


}



//
// Initiate connection to MQTT server
//
bool PubsubSim7000MQTTLeaf::connect() {
  LEAF_ENTER(L_INFO);
  static char buf[2048];

  if (!modem) {
    modem = modemLeaf->get_modem();
  }
  if (!modem) {
    LEAF_ALERT("Modem leaf not found");
    return false;
  }

  if (!connStatus()) {
    LEAF_ALERT("Not connected to cell network, wait till later");
    return false;
  }

  /* force disconnect - TESTING ONLY */
  //modem->MQTT_connect(false);

  // If not already connected, connect to MQTT
  int is_connected = modem->MQTT_connectionStatus();

  if (is_connected) {
    LEAF_NOTICE("Already connected to MQTT broker.");
    was_connected = true;
    mqttConnected = _connected = true;
    handle_connect_event(false);
    idle_pattern(5000,5);
    LEAF_RETURN(true);
  }

  LEAF_NOTICE("Establishing connection to MQTT broker %s => %s:%s",
	      device_id, mqtt_host, mqtt_port);
  mqttConnected = _connected = false;
  idle_pattern(100,50);
  modem->MQTT_setParameter("CLEANSS", "0");
  modem->MQTT_setParameter("CLIENTID", device_id);
  // Set up MQTT parameters (see MQTT app note for explanation of parameter values)

  int port = atoi(mqtt_port);
  if (port == 1883) {
    LEAF_INFO("Using default MQTT port number");
    modem->MQTT_setParameter("URL", mqtt_host);
  }
  else {
    LEAF_INFO("Using custom MQTT port number %s", mqtt_port);
    modem->MQTT_setParameter("URL", mqtt_host, atoi(mqtt_port));
  }


  // Set up MQTT username and password if necessary (or if blank!)
  if (strlen(username) > 0) {
    if (strcmp(username,"[none]")==0) {
      modem->MQTT_setParameter("USERNAME", "");
      modem->MQTT_setParameter("PASSWORD", "");
    }
    else{
      modem->MQTT_setParameter("USERNAME", username);
      modem->MQTT_setParameter("PASSWORD", password);
    }
  }

  if (keepalive > 0) {
    modem->MQTT_setParameter("KEEPTIME", keepalive); // Time to connect to server, 60s by default
  }
  if (use_status) {
    snprintf(lwt_topic, sizeof(lwt_topic), "%sstatus/presence", base_topic.c_str());
    modem->MQTT_setParameter("TOPIC", lwt_topic);
    modem->MQTT_setParameter("MESSAGE", "offline");
    modem->MQTT_setParameter("RETAIN", "1");
  }

  if (use_ssl) {

    LEAF_INFO("Configuring MQTT for SSL...");
    modem->sendCheckReply(F("AT+CSSLCFG=\"ctxindex\", 0"), F("OK")); // use index 1
    //modem->sendCheckReply(F("AT+CSSLCFG=\"ignorertctime\", 1"), F("OK"));
    modem->sendCheckReply(F("AT+CSSLCFG=\"sslversion\",0,3"), F("OK")); // TLS 1.2
    modem->sendCheckReply(F("AT+CSSLCFG=\"convert\",2,\"cacert.pem\""), F("OK"));
    if (use_client_cert) {
      modem->sendCheckReply(F("AT+CSSLCFG=\"convert\",1,\"client.crt\",\"client.key\""), F("OK"));
      modem->sendCheckReply(F("AT+SMSSL=1,cacert.pem,client.crt"), F("OK"));
    }
    else {
      modem->sendCheckReply(F("AT+SMSSL=0,cacert.pem"), F("OK"));
    }
    modem->sendCheckReply(F("AT+SMSSL?"), F("OK"));
  }
  else {
    modem->sendCheckReply(F("AT+SMSSL=0"), F("OK"));
  }

  char cmdbuffer[80];
  char replybuffer[80];
#if 0
  modem->sendExpectStringReply("AT+CDNSCFG?","PrimaryDns: ", replybuffer, 30000, sizeof(replybuffer),2);
  //if (strcmp(replybuffer,"0.0.0.0")==0) {
  //LEAF_NOTICE("Modem does not have DNS.  Let's use teh googles");
  //modem->sendCheckReply("AT+CDNSCFG=8.8.8.8","OK");
  //}

  snprintf(cmdbuffer, sizeof(cmdbuffer), "AT+CDNSGIP=%s", mqtt_host);
  modem->sendExpectStringReply(cmdbuffer,"+CDNSGIP: ", replybuffer, 30000, sizeof(replybuffer),2);
#endif

  LEAF_INFO("Initiating MQTT connect");
  int retry = 1;
  const int max_retries = 1;
  int initialState;

  while (!_connected && (retry <= max_retries)) {

    if (! modem->MQTT_connect(true,10000)) {
      LEAF_ALERT("ERROR: Failed to connect to broker.");
      post_error(POST_ERROR_PUBSUB, 3);
      ERROR("MQTT connect fail");
      ++retry;

      if (modem->waitfor("AT+SMDISC",5000,replybuffer, sizeof(replybuffer)) &&
	  (strstr(replybuffer, "+CME ERROR: operation not allowed")==0) ) {
	LEAF_ALERT("Probable loss of LTE carrier.  Reconnect");
	modemLeaf->disconnect();
	modemLeaf->schedule_reconnect(NETWORK_RECONNECT_SECONDS);
	return false;
      }
    }
    else {
      LEAF_NOTICE("Connected to broker.");
      mqttConnected = _connected = true;
      handle_connect_event(true);
    }
  }
  if (!_connected) {
    disconnect(false);
#if 0
    LEAF_ALERT("Broker is not answering.   Rebooting..");
    modem->sendCheckReply("AT+CFUN=1,1","OK");
#ifdef ESP8266
    ESP.reset();
#else
    ESP.restart();
#endif
#endif
  }

  LEAF_LEAVE;
  return true;
}

void PubsubSim7000MQTTLeaf::handle_connect_event(bool do_subscribe)
{
  LEAF_ENTER(L_INFO);

  LEAF_INFO("Connected to MQTT");

  // Once connected, publish an announcement...
  mqttConnected = true;
  mqtt_publish("status/presence", "online", 0, true);
  if (wake_reason.length()) {
    mqtt_publish("status/wake", wake_reason, 0, true);
  }
  mqtt_publish("status/ip", ip_addr_str, 0, true);
  for (int i=0; leaves[i]; i++) {
    leaves[i]->mqtt_connect();
  }

  if (do_subscribe) {
    // we skip this if the modem told us "already connected, dude", which
    // can happen after sleep.

    //_mqtt_subscribe("ping");
    if (use_wildcard_topic) {
      _mqtt_subscribe(base_topic+"cmd/+");
      _mqtt_subscribe(base_topic+"get/+");
      _mqtt_subscribe(base_topic+"set/+");
      if (!use_flat_topic) {
	_mqtt_subscribe(base_topic+"set/pref/+");
      }
    }

    mqtt_subscribe("cmd/restart");
    mqtt_subscribe("cmd/setup");
#ifdef _OTA_OPS_H
    mqtt_subscribe("cmd/update");
    mqtt_subscribe("cmd/rollback");
    mqtt_subscribe("cmd/bootpartition");
    mqtt_subscribe("cmd/nextpartition");
#endif
    mqtt_subscribe("cmd/ping");
    mqtt_subscribe("cmd/leaves");
    mqtt_subscribe("cmd/format");
    mqtt_subscribe("cmd/status");
    mqtt_subscribe("cmd/subscriptions");
    mqtt_subscribe("set/name");
    mqtt_subscribe("set/debug");
    mqtt_subscribe("set/debug_wait");
    mqtt_subscribe("set/debug_lines");
    mqtt_subscribe("set/debug_flush");

    LEAF_INFO("Set up leaf subscriptions");

#if 0
    _mqtt_subscribe(base_topic+"devices/*/+/#");
    _mqtt_subscribe(base_topic+"devices/+/*/#");
#endif
    for (int i=0; leaves[i]; i++) {
      Leaf *leaf = leaves[i];
      LEAF_INFO("Initiate subscriptions for %s", leaf->get_name().c_str());
      leaf->mqtt_do_subscribe();
    }
  }
  LEAF_INFO("MQTT Connection setup complete");

  publish("_pubsub_connect", mqtt_host);
  idle_pattern(5000,5);
  last_external_input = millis();

  LEAF_LEAVE;
}


uint16_t PubsubSim7000MQTTLeaf::_mqtt_publish(String topic, String payload, int qos, bool retain)
{
  LEAF_ENTER(L_DEBUG);
  LEAF_INFO("PUB %s => [%s]", topic.c_str(), payload.c_str());
  const char *t = topic.c_str();
  const char *p = payload.c_str();
  int i;

  if (mqttLoopback) {
    LEAF_NOTICE("LOOPBACK PUB %s => %s", t, p);
    loopback_buffer += topic + ' ' + payload + '\n';
    return 0;
  }

  if (_connected) {
    modem->sendExpectIntReply("AT+SMSTATE?","+SMSTATE: ", &i);
    if (i==0) {
      LEAF_ALERT("Lost MQTT connection");
      if (ipLeaf->isConnected()) {
	LEAF_ALERT("Try MQTT reconnection");
	if (! modem->MQTT_connect(true,75000)) {
	  post_error(POST_ERROR_LTE, 3);
	  ALERT("Unable to reconnect");
	  ERROR("MQTT reconn fail");
	  return 0;
	}
      }
    }

    if (!modem->MQTT_publish(t, p, payload.length(), qos, retain)) {
      LEAF_ALERT("ERROR: Failed to publish to %s", t);
      // lets check if the connection dropped
      if (!modem->sendExpectIntReply("AT+SMSTATE?","+SMSTATE: ", &i)) {
	i = 0;
      }
      if (i==0) {
	LEAF_ALERT("Lost MQTT connection");
	if (ipLeaf->isConnected()) {
	  LEAF_ALERT("Try MQTT reconnect");
	  connect();
	}
      }
    }
  }
  else {
    LEAF_ALERT("Publish skipped while MQTT connection is down: %s=>%s", t, p);
  }
  LEAF_LEAVE;
  return 1;
}

void PubsubSim7000MQTTLeaf::_mqtt_subscribe(String topic, int qos)
{
  LEAF_ENTER(L_INFO);
  const char *t = topic.c_str();

  LEAF_NOTICE("MQTT SUB %s", t);
  if (_connected) {

    if (modem->MQTT_subscribe(t, qos,10000)) {
      LEAF_NOTICE("Subscription initiated for topic=%s", t);
      if (mqttSubscriptions) {
	mqttSubscriptions->put(topic, qos);
      }

    }
    else {
      LEAF_ALERT("Subscription FAILED for topic=%s (maybe already subscribed?)", t);
#if 0
      if (modem->MQTT_unsubscribe(t, 10000)) {
	  if (mqttSubscriptions) {
	    mqttSubscriptions->remove(topic);
	  }

	if (modem->MQTT_subscribe(t, qos, 20000)) {
	  LEAF_NOTICE("Subscription retry succeeded for topic=%s", t);
	  if (mqttSubscriptions) {
	    mqttSubscriptions->put(topic, qos);
	  }
	}
	else {
	  LEAF_ALERT("Subscription retry failed for topic=%s", t);
	}
      }
#endif
    }
  }
  else {
    LEAF_ALERT("Warning: Subscription attempted while MQTT connection is down (%s)", t);
  }
  LEAF_LEAVE;
}

void PubsubSim7000MQTTLeaf::_mqtt_unsubscribe(String topic)
{
  const char *t = topic.c_str();
  LEAF_NOTICE("MQTT UNSUB %s", t);

  if (_connected) {
    if (modem->MQTTunsubscribe(t)) {
      LEAF_ALERT("Unsubscription FAILED for topic=%s", t);
    }
    else {
      LEAF_DEBUG("UNSUBSCRIPTION initiated topic=%s", t);
      mqttSubscriptions->remove(topic);
    }
  }
  else {
    LEAF_ALERT("Warning: Unsubscription attempted while MQTT connection is down (%s)", t);
  }
}

void PubsubSim7000MQTTLeaf::initiate_sleep_ms(int ms)
{
  LEAF_NOTICE("Prepare for deep sleep");

  mqtt_publish("event/sleep",String(millis()/1000,10));

  // Apply sleep in reverse order, highest level leaf first
  int leaf_index;
  for (leaf_index=0; leaves[leaf_index]; leaf_index++);
  for (leaf_index--; leaf_index<=0; leaf_index--) {
    leaves[leaf_index]->pre_sleep(ms/1000);
  }
  
  if (ms == 0) {
    LEAF_ALERT("Initiating indefinite deep sleep (wake source GPIO0)");
  }
  else {
    LEAF_ALERT("Initiating deep sleep (wake sources GPIO0 plus timer %dms)", ms);
  }

  Serial.flush();
  if (ms != 0) {
    // zero means forever
    esp_sleep_enable_timer_wakeup(ms * 1000ULL);
  }
  esp_sleep_enable_ext0_wakeup((gpio_num_t)0, 0);

  esp_deep_sleep_start();
}
// Local Variables:
// mode: C++
// c-basic-offset: 2
// End:
