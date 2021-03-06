#ifndef _TRAIT_POLLABLE_H
#define _TRAIT_POLLABLE_H


class Pollable 
{
protected:
  bool changed;

  unsigned long last_sample = 0;
  unsigned long last_report = 0;
  int sample_interval_ms = -1;
  int report_interval_sec = -1;

  virtual void status_pub()=0;
  virtual bool poll()=0;
	
  void pollable_loop() 
  {
    //ENTER(L_INFO);
    
    if (sample_interval_ms < 0) {
      DEBUG("pollable has no sample interval");
      return;
    }
    
    unsigned long now = millis();
    if ((mqttConnected && (last_sample == 0)) ||
	((sample_interval_ms + last_sample) <= now)
      ) {
      // time to take a new sample
      changed = poll();
      last_sample = now; 
    }
    
    if ( (mqttConnected && (last_report == 0)) ||
	 changed ||
	 ((last_report + report_interval_sec * 1000) <= now)
      ) {
      // Publish a report every N seconds, or if changed by more than d%
      status_pub();
      last_report = now;
      changed = false;
    }

    //LEAF_LEAVE;
  }
};


#endif
// local Variables:
// mode: C++
// c-basic-offset: 2
// End:
