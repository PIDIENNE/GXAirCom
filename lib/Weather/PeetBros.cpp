/*
Peet Bros. (PRO) anemometer interface.

Read the wind speed and direction interrupts from two input pins, do some math,
and output this

The anemometer itself is designed so that the two inputs does not signal
at the same time. We can use simple non-interruptable interrupts and still
be pretty sure not to lose any events.

Useful resources:
    https://github.com/guywithaview/peet-bros-wind/tree/master
    http://arduino.cc/en/Tutorial/DigitalPins
    http://learn.parallax.com/reed-switch-arduino-demo
    http://www.agrolan.co.il/UploadProductFiles/AWVPRO.pdf

http://www.peetbros.com/shop/item.aspx?itemid=137

*/

#include <PeetBros.h>


const unsigned long DEBOUNCE = 10000ul;      // Minimum switch time in microseconds
//const unsigned long DEBOUNCE = 50000ul;      // Minimum switch time in microseconds
const unsigned long TIMEOUT = 2000000ul;       // Maximum time allowed between speed pulses in microseconds

// speed is actually stored as (km/h). Deviations below should match these units.
const int BAND_0 =  20;
const int BAND_1 =  150;

const float filterGain = 0.25;               // Filter gain on direction output filter. Range: 0.0 to 1.0
                                             // 1.0 means no filtering. A smaller number increases the filtering


const int SPEED_DEV_LIMIT_0 =  10;     // Deviation from last measurement to be valid. Band_0: 0 to 20 kmh
const int SPEED_DEV_LIMIT_1 = 20;     // Deviation from last measurement to be valid. Band_1: 20 to 150 kmh
const int SPEED_DEV_LIMIT_2 = 55;     // Deviation from last measurement to be valid. Band_2: 150+ kmh

// Should be larger limits as lower speed, as the direction can change more per speed update
const int DIR_DEV_LIMIT_0 = 25;     // Deviation from last measurement to be valid. Band_0: 0 to 20 kmh
const int DIR_DEV_LIMIT_1 = 18;     // Deviation from last measurement to be valid. Band_1: 20 to 150 kmh
const int DIR_DEV_LIMIT_2 = 10;     // Deviation from last measurement to be valid. Band_2: 150+ kmh

volatile unsigned long speedPulse = 0ul;    // Time capture of speed pulse
volatile unsigned long dirPulse = 0ul;      // Time capture of direction pulse
volatile unsigned long speedTime = 0ul;     // Time between speed pulses (microseconds)
volatile unsigned long directionTime = 0ul; // Time between direction pulses (microseconds)
volatile boolean PB_newData = false;           // New speed pulse received
volatile uint32_t PB_actPulseCount = 0;
volatile uint32_t PB_speedpulsecount = 0;
volatile uint8_t PB_timerIrq = 0;
hw_timer_t * pb_timer = NULL;
int8_t _wspeedPin = -1;
int8_t _wDirPin = -1;

void IRAM_ATTR isr_rotated();
void IRAM_ATTR isr_direction();
float PB_actSpeed;
float PB_actDir;
uint32_t tsPB_valid =  0;
uint8_t PB_valid = false;
volatile float dirOut = 0.0;      // Direction output in degrees

void IRAM_ATTR pb_onTimer() {
  if (PB_timerIrq) return;
  PB_actPulseCount = PB_speedpulsecount;
  PB_speedpulsecount = 0;
  PB_timerIrq = 1;
}

void pb_attachInterrupts(int8_t speedPin,int8_t dirPin){
  attachInterrupt(digitalPinToInterrupt(speedPin), isr_rotated, FALLING);
  attachInterrupt(digitalPinToInterrupt(dirPin), isr_direction, FALLING);
}

void pb_detachInterrupts(int8_t speedPin,int8_t dirPin){
  detachInterrupt(digitalPinToInterrupt(speedPin));
  detachInterrupt(digitalPinToInterrupt(dirPin));
}



void peetBros_init(int8_t windSpeedPin,int8_t windDirPin){
  PB_valid = 0;
  PB_actSpeed = 0.0;
  PB_actDir = 0.0;
  _wspeedPin = windSpeedPin;
  _wDirPin = windDirPin;
  pinMode(_wspeedPin, INPUT);
  pinMode(_wDirPin, INPUT);
  log_i("speedPin=%d,dirPin=%d",_wspeedPin,_wDirPin);
  pb_attachInterrupts(_wspeedPin,_wDirPin);
  pb_timer = timerBegin(0, 80, true);
  timerAttachInterrupt(pb_timer, &pb_onTimer, true);
  timerAlarmWrite(pb_timer, 2000000, true); //every 2 seconds
  timerAlarmEnable(pb_timer);      
}

boolean checkDirDev(float kmh, float dev)
{
    if (kmh < BAND_0)
    {
        if ((abs(dev) < DIR_DEV_LIMIT_0) || (abs(dev) > 360 - DIR_DEV_LIMIT_0)) return true;
    }
    else if (kmh < BAND_1)
    {
        if ((abs(dev) < DIR_DEV_LIMIT_1) || (abs(dev) > 360 - DIR_DEV_LIMIT_1)) return true;
    }
    else
    {
        if ((abs(dev) < DIR_DEV_LIMIT_2) || (abs(dev) > 360 - DIR_DEV_LIMIT_2)) return true;
    }
    //log_e("no valid dir change dev=%.0f;speed=%.2f",dev,kmh);
    return false;
}

boolean checkSpeedDev(float kmh, float dev)
{
    if (kmh < BAND_0)
    {
        if (abs(dev) < SPEED_DEV_LIMIT_0) return true;
    }
    else if (kmh < BAND_1)
    {
        if (abs(dev) < SPEED_DEV_LIMIT_1) return true;
    }
    else
    {
        if (abs(dev) < SPEED_DEV_LIMIT_2) return true;
    }
    //log_e("no valid speed change dev=%.0f;speed=%.2f",dev,kmh);
    return false;
}

float peetBrosWindSpeed(float rps){ 
  float mph;
  //log_i("rps=%.2f",rps);
  if (rps < 0.010){
    return 0.0;
  }  
  if (rps < 3.229){
    mph = -0.1095*(rps * rps) + (2.9318*rps) - 0.1412; 
  }else if (rps < 54.362){
    mph = 0.0052*(rps * rps) + (2.1980*rps) + 1.1091;
  }else{
    mph = 0.1104*(rps * rps) - (9.5685*rps) + 329.87;    
  } 
  //log_i("mph=%.2f",mph);   
  return mph * 1.6094; //change to kmh
}

void peetBrosRun(){
  unsigned long speedPulse_;
  unsigned long speedTime_;
  unsigned long directionTime_; 
  bool _newData;
  float rps,kmh = 0,dev;
  static float prevKmh = 0;
  int16_t windDirection = 0;
  static int16_t prevDir = 0;
  static uint32_t pulseCount = 0;
  static float timeDivider = 2.0;

  if (PB_timerIrq){
    tsPB_valid = millis();
    pulseCount += PB_actPulseCount;
    PB_timerIrq = 0;
    if ((pulseCount < 10) & (timeDivider < 10.0)){ //when we have low wind-speed, we count longer to be more precise.
      timeDivider += 2.0;
      return;
    }
    if (pulseCount == 0){
      PB_actSpeed = 0.0;
      PB_valid = 1;  
      timeDivider = 2.0;
      return;      
    }
    float rps = (float)pulseCount / timeDivider; //get rps
    rps /= 2;
    PB_actSpeed =  peetBrosWindSpeed(rps);
    //log_i("cnt=%d,rps=%.2f,kmh=%.2f,td=%.2f",pulseCount,rps,PB_actSpeed,timeDivider);    
    PB_valid = 1;  
    timeDivider = 2.0;
    pulseCount = 0;
    //return;
  } 
  if (PB_newData == false){
    //log_i("no new Data");
    return;
  }
  pb_detachInterrupts(_wspeedPin,_wDirPin);
  speedPulse_ = speedPulse;
  speedTime_ = speedTime;
  directionTime_ = directionTime;
  _newData = PB_newData;
  PB_newData = false;
  pb_attachInterrupts(_wspeedPin,_wDirPin);

  if (speedTime_ <= 0){   
    log_e("speedTime=0"); 
    return;
  }
  if (directionTime_ > speedTime_) return;
  // Calculate direction from captured pulse times
  windDirection = ((directionTime_ * 360) / speedTime_) % 360; 
  PB_actDir = float(windDirection); 
  //PB_valid = 1;
  /* 
  rps = 1000000.0/float(speedTime_);
  if (rps < 0.010){
    //log_e("rps to small < 0.01");
    return;
  }

  // Make speed zero, if the pulse delay is too long
  if (micros() - speedPulse_ > TIMEOUT){
    log_e("rotation timeout rps=%.2f",rps);
    return;
  }
  if (rps < 3.229){
    kmh = (-0.1095*(rps * rps) + 2.9318*rps - 0.1412) * 1.6094; 
  }else if (rps < 54.362){
    kmh = (0.0052*(rps * rps) + 2.1980*rps + 1.1091) * 1.6094;
  }else{
    kmh = (0.1104*(rps * rps) - 9.5685*rps + 329.87) * 1.6094;    
  }  
  dev = (kmh - prevKmh);
  prevKmh = kmh;
  //log_i("dt=%d,st=%d,speed=%.2f,rps=%.2f,dev=%.1f",directionTime_,speedTime_,PB_actSpeed,rps,dev);
  if (checkSpeedDev(kmh,dev) == false){    
    return; //not valid speed change
  } 
    
  if (directionTime_ > speedTime_) return;
  // Calculate direction from captured pulse times
  windDirection = ((directionTime_ * 360) / speedTime_) % 360;  
  dev = float(windDirection - prevDir);  
  prevDir = windDirection;
  //log_i("dir=%d,speed=%.2f,rps=%.2ff",windDirection,PB_actSpeed,rps);
  if (checkDirDev(kmh, dev) == false){    
    return; //not valid dir change
  }
  float delta = (windDirection - dirOut);
  if (delta < -180)
  {
    delta = delta + 360;    // Take the shortest path when filtering
  }
  else if (delta > +180)
  {
    delta = delta - 360;
  }
  // Perform filtering to smooth the direction output
  dirOut = (int)(dirOut + (round(filterGain * delta))) % 360;
  if (dirOut < 0) dirOut = dirOut + 360;
  PB_actSpeed = kmh; 
  PB_actDir = float(dirOut);
  tsPB_valid = millis();
  PB_valid = 1;
  */
}

uint8_t peetBrosgetNewData(float *Dir, float *Speed){
  //PB_valid 0 ... init, 1 .. new Data, 2.. no new data, 255 ... timeout
  if ((PB_valid != 255) && (timeOver(millis(),tsPB_valid,4000))){
    PB_valid = 255;
  }
  uint8_t ret = PB_valid;
  if (PB_valid == 255){
    *Speed = 0.0;
  }else if (PB_valid == 1){
    *Dir = PB_actDir;
    *Speed = PB_actSpeed;
    PB_valid = 2; //set to no new data yet
  }
  return ret;
}

/*
 * Interrupt called when the rotating part of the wind instrument
 * has completed one rotation.
 *
 * We can find the wind speed by calculating how long the complete
 * rotation took.
*/
void isr_rotated() {
  // Despite the interrupt being set to FALLING edge, double check the pin is now LOW
  if (((micros() - speedPulse) > DEBOUNCE) && (digitalRead(_wspeedPin) == LOW))
  {
      PB_speedpulsecount ++;
      // Work out time difference between last pulse and now
      speedTime = micros() - speedPulse;
      // Direction pulse should have occured after the last speed pulse
      if (dirPulse - speedPulse >= 0){
        directionTime = dirPulse - speedPulse;
        PB_newData = true;
      }
      speedPulse = micros();    // Capture time of the new speed pulse
  }
}

/*
 * After the rotation interrupt happened, the rotating section continues
 * into the next round. Somewhere within that rotation we get a different
 * interrupt when a certain point of the wind wane/direction indicator is
 * passed. By counting how far into the rotation (assumed to be == last rotation)
 * we can compute the angle.
 *
*/
void IRAM_ATTR isr_direction() { 
  if (((micros() - dirPulse) > DEBOUNCE) && (digitalRead(_wDirPin) == LOW))
  {
    dirPulse = micros();        // Capture time of direction pulse
  }
}

