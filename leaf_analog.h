//
//@**************************** class DHTLeaf ******************************
//
// This class encapsulates an analog input sensor publishes measured
// voltage values to MQTT
//

class AnalogInputLeaf : public Leaf
{
protected:
  int value;
  unsigned long last_sample;
  unsigned long last_report;
  int sample_interval_ms;
  int report_interval_sec;
  int dp;
  String unit;
  int delta;
  int fromLow, fromHigh;
  float toLow, toHigh;

public:
  AnalogInputLeaf(String name, pinmask_t pins, int in_min=0, int in_max=1023, float out_min=0, float out_max=100, bool asBackplane = false) : Leaf("analog", name, pins)
  {
    LEAF_ENTER(L_INFO);
    value = -1;
    report_interval_sec = 60;
    sample_interval_ms = 1000;
    delta = 5;
    last_sample = 0;
    last_report = 0;
    dp = 2;
    unit = "";
    fromLow = in_min;
    fromHigh = in_max;
    toLow = out_min;
    toHigh = out_max;
    impersonate_backplane = asBackplane;

    LEAF_LEAVE;
  };

  virtual float get_value()
  {
    if (fromLow < 0) {
      return value;
    }
    // This is the floating point version of Arduino's map() function
    float mv = (value - fromLow) * (toHigh - toLow) / (fromHigh - fromLow) + toLow;
    return mv;
  };

  virtual String get_value_string()
  {
    return String(get_value(), dp);
  };

  virtual void status_pub()
  {
    mqtt_publish("status/raw", String(value, DEC));
    if (value >= 0) {
      mqtt_publish("status/value", get_value_string());
    }
  };

  virtual bool sample(void)
  {
      int inputPin;
      FOR_PINS({inputPin=pin;});
      // time to take a new sample
      int new_value = analogRead(inputPin);
      bool changed =
	(last_sample == 0) ||
	(value < 0) ||
	((value > 0) && (abs(100*(value-new_value)/value) > delta));
      LEAF_DEBUG("Sampling Analog input on pin %d => %d", inputPin, new_value);
      if (changed) {
	value = new_value;
	LEAF_NOTICE("Analog input on pin %d => %d", inputPin, value);
      }

      return changed;
  }

  virtual void loop(void) {
    Leaf::loop();
    bool changed = false;
    unsigned long now = millis();

    if ((mqttConnected && (last_sample == 0)) ||
	(now >= (last_sample + sample_interval_ms))
      ) {
      LEAF_DEBUG("taking a sample");
      changed = sample();
      last_sample = now;
    }

    //
    // Reasons to report are:
    //   * significant change in value
    //   * report timer has elapsed
    //   * this is the first poll after connecting to MQTT
    //
    if ( changed ||
	 (now >= (last_report + (report_interval_sec * 1000))) ||
	 (mqttConnected && (last_report == 0))
      ) {
      // Publish a report every N seconds, or if changed by more than d%
      status_pub();
      last_report = now;
    }

    //LEAF_LEAVE;
  };

};


// local Variables:
// mode: C++
// c-basic-offset: 2
// End:
